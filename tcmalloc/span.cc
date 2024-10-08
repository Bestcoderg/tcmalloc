// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/span.h"

#include <stdint.h>

#include <atomic>
#include <cstddef>

#include "absl/base/optimization.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void Span::Sample(SampledAllocation* sampled_allocation) {
  TC_CHECK(!sampled_ && sampled_allocation);
  sampled_ = 1;
  sampled_allocation_ = sampled_allocation;

  // The cast to value matches Unsample.
  tcmalloc_internal::StatsCounter::Value allocated_bytes =
      static_cast<tcmalloc_internal::StatsCounter::Value>(
          AllocatedBytes(sampled_allocation->sampled_stack));
  tc_globals.sampled_objects_size_.Add(allocated_bytes);
  tc_globals.total_sampled_count_.Add(1);
}

SampledAllocation* Span::Unsample() {
  if (!sampled_) {
    return nullptr;
  }
  TC_CHECK(sampled_ && sampled_allocation_);
  sampled_ = 0;
  SampledAllocation* sampled_allocation = sampled_allocation_;
  sampled_allocation_ = nullptr;

  // The cast to Value ensures no funny business happens during the negation if
  // sizeof(size_t) != sizeof(Value).
  tcmalloc_internal::StatsCounter::Value neg_allocated_bytes =
      -static_cast<tcmalloc_internal::StatsCounter::Value>(
          AllocatedBytes(sampled_allocation->sampled_stack));
  tc_globals.sampled_objects_size_.Add(neg_allocated_bytes);
  return sampled_allocation;
}

double Span::Fragmentation(size_t object_size) const {
  if (object_size == 0) {
    // Avoid crashes in production mode code, but report in tests.
    TC_ASSERT_NE(object_size, 0);
    return 0;
  }
  const size_t span_objects = bytes_in_span() / object_size;
  const size_t live = allocated_.load(std::memory_order_relaxed);
  if (live == 0) {
    // Avoid crashes in production mode code, but report in tests.
    TC_ASSERT_NE(live, 0);
    return 0;
  }
  // Assume that all in-use objects in this span are spread evenly
  // through this span.  So charge the free space in span evenly
  // to each of the live objects.
  // A note on units here: StackTraceTable::AddTrace(1, *t)
  // represents usage (of whatever kind: heap space, allocation,
  // fragmentation) of 1 object of size t->allocated_size.
  // So we want to report here the number of objects we are "responsible"
  // for pinning - NOT bytes.
  return static_cast<double>(span_objects - live) / live;
}

// Freelist organization.
//
// Partially full spans in CentralFreeList contain a list of free objects
// (freelist). We could use the free objects as linked list nodes and form
// a stack, but since the free objects are not likely to be cache-hot the
// chain of dependent misses is very cache-unfriendly. The current
// organization reduces number of cache misses during push/pop.
// (我们需要组织一个freelist,我们可以使用空闲的object来组织.注意这里我们会将这个链表组织为
//  栈stack的形式,因为我们希望freelist更加地cache友好,为了达到这个目的,我们会将好几个free object
// 索引放到一个object中,这样可以令cache的命中率更高.但因为一个object中存储了好几个free
// object 的 index 所以我们只能将freelist组织为stack的形式)
//
// Objects in the freelist are represented by 2-byte indices. The index is
// object offset from the span start divided by a constant. For small objects
// (<512) divider is 8, for larger -- 64. This allows to fit all indices into
// 2 bytes.
//
// The freelist has two components. First, we have a small array-based cache
// (4 objects) embedded directly into the Span (cache_ and cache_size_). We can
// access this without touching any objects themselves.
// (freelist 分为两个部分,首先我们做一个高速的缓存,我们可以通过缓存快速取到free
// object)
//
// The rest of the freelist is stored as arrays inside free objects themselves.
// We can store object_size / 2 indexes in any object, but this is not always
// sufficient to store the entire contents of a Span in a single object. So we
// reserve the first index slot in an object to form a linked list. 
// (我们保留第一个槽位的index 'freelist_',以此来形成一个链表).
// We use the first object in that list (freelist_) as an array to push/pop from; any
// subsequent objects in the list's arrays are guaranteed to be full.
// (我们将freelist_的第一个对象用于链接这个链表,实际上object中的每一个idx都是一个free object index
//  不过为了组成一个 链表/stack 我们赋予第一个index特殊意义,即认为第一个index是freelist指向下一个list array
//  的index,这样就将我们的链表串联起来了)
// 
// Graphically this can be depicted as follows:
//
//         freelist_  embed_count_         cache_        cache_size_
// Span: [  |idx|         4          |idx|idx|---|---|        2      ]
//            |
//            \/
//            [idx|idx|idx|idx|idx|---|---|---]  16-byte object
//              |
//              \/
//              [---|idx|idx|idx|idx|idx|idx|idx]  16-byte object
//

void* Span::BitmapIdxToPtr(ObjIdx idx, size_t size) const {
  uintptr_t off = first_page_.start_uintptr() + idx * size;
  return reinterpret_cast<ObjIdx*>(off);
}

size_t Span::BitmapPopBatch(void** __restrict batch, size_t N, size_t size) {
  size_t before = bitmap_.CountBits(0, bitmap_.size());
  size_t count = 0;
  // Want to fill the batch either with N objects, or the number of objects
  // remaining in the span.
  while (!bitmap_.IsZero() && count < N) {
    size_t offset = bitmap_.FindSet(0);
    TC_ASSERT_LT(offset, bitmap_.size());
    batch[count] = BitmapIdxToPtr(offset, size);
    bitmap_.ClearLowestBit();
    count++;
  }

  TC_ASSERT_EQ(bitmap_.CountBits(0, bitmap_.size()) + count, before);
  allocated_.store(allocated_.load(std::memory_order_relaxed) + count,
                   std::memory_order_relaxed);
  return count;
}

size_t Span::FreelistPopBatch(void** __restrict batch, size_t N, size_t size) {
  // Handle spans with bitmap_.size() or fewer objects using a bitmap. We expect
  // spans to frequently hold smaller objects.
  if (ABSL_PREDICT_FALSE(UseBitmapForSize(size))) {
    return BitmapPopBatch(batch, N, size);
  }
  return ListPopBatch(batch, N, size);
}

size_t Span::ListPopBatch(void** __restrict batch, size_t N, size_t size) {
  size_t result = 0;

  // Pop from cache.
  auto csize = cache_size_;
  // TODO(b/304135905):  Complete experiment and update kCacheSize.
  ASSUME(csize <= kLargeCacheSize);
  auto cache_reads = csize < N ? csize : N;
  const uintptr_t span_start = first_page_.start_uintptr();
  for (; result < cache_reads; result++) {
    batch[result] = IdxToPtr(cache_[csize - result - 1], size, span_start);
  }

  // Store this->cache_size_ one time.
  cache_size_ = csize - result;

  while (result < N) {
    if (freelist_ == kListEnd) {
      break;
    }

    ObjIdx* const host = IdxToPtr(freelist_, size, span_start);
    uint16_t embed_count = embed_count_;

    size_t iter = embed_count;
    if (result + embed_count > N) {
      iter = N - result;
    }
    for (size_t i = 0; i < iter; i++) {
      // Pop from the first object on freelist.
      batch[result + i] = IdxToPtr(host[embed_count - i], size, span_start);
    }
    embed_count -= iter;
    result += iter;

    if (result == N) {
      embed_count_ = embed_count;
      break;
    }

    // The first object on the freelist is empty, pop it.
    TC_ASSERT_EQ(embed_count, 0);

    batch[result] = host;
    result++;

    freelist_ = host[0];
    embed_count_ = size / sizeof(ObjIdx) - 1;
  }
  allocated_.store(allocated_.load(std::memory_order_relaxed) + result,
                   std::memory_order_relaxed);
  return result;
}

uint32_t Span::CalcReciprocal(size_t size) {
  // Calculate scaling factor. We want to avoid dividing by the size of the
  // object. Instead we'll multiply by a scaled version of the reciprocal.
  // We divide kBitmapScalingDenominator by the object size, so later we can
  // multiply by this reciprocal, and then divide this scaling factor out.
  return kBitmapScalingDenominator / size;
}

void Span::BuildBitmap(size_t size, size_t count) {
  // We are using a bitmap to indicate whether objects are used or not. The
  // maximum capacity for the bitmap is bitmap_.size() objects.
  TC_ASSERT_LE(count, bitmap_.size());
  allocated_.store(0, std::memory_order_relaxed);
  bitmap_.Clear();  // bitmap_ can be non-zero from a previous use.
  bitmap_.SetRange(0, count);
  TC_ASSERT_EQ(bitmap_.CountBits(0, bitmap_.size()), count);
}

int Span::BuildFreelist(size_t size, size_t count, void** batch, int N,
                        uint32_t max_cache_size) {
  TC_ASSERT_GT(count, 0);
  freelist_ = kListEnd;

  if (UseBitmapForSize(size)) {
    BuildBitmap(size, count);
    return BitmapPopBatch(batch, N, size);
  }

  // First, push as much as we can into the batch.
  const uintptr_t start = first_page_.start_uintptr();
  char* ptr = reinterpret_cast<char*>(start);
  int result = N <= count ? N : count;
  // 先将申请的空间给batch
  for (int i = 0; i < result; ++i) {
    batch[i] = ptr;
    ptr += size;
  }
  allocated_.store(result, std::memory_order_relaxed);

  const ObjIdx idxStep = size / static_cast<size_t>(kAlignment);
  // Valid objects are {0, idxStep, idxStep * 2, ..., idxStep * (count - 1)}.
  ObjIdx idx = idxStep * result;

  // Verify that the end of the useful portion of the span (and the beginning of
  // the span waste) has an index that doesn't overflow or risk confusion with
  // kListEnd. This is slightly stronger than we actually need (see comment in
  // PtrToIdx for that) but rules out some bugs and weakening it wouldn't
  // actually help. One example of the potential bugs that are ruled out is the
  // possibility of idxEnd (below) overflowing.
  TC_ASSERT_LT(count * idxStep, kListEnd);

  // The index of the end of the useful portion of the span.
  ObjIdx idxEnd = count * idxStep;

  // Then, push as much as we can into the cache_.
  // 优先将对象放入cache中,cache 的索引性能优于 freelist
  TC_ASSERT_GE(max_cache_size, kCacheSize);
  TC_ASSERT_LE(max_cache_size, kLargeCacheSize);
  int cache_size = 0;
  for (; idx < idxEnd && cache_size < max_cache_size; idx += idxStep) {
    cache_[cache_size] = idx;
    cache_size++;
  }
  cache_size_ = cache_size;

  // Now, build freelist and stack other objects onto freelist objects.
  // Note: we take freelist objects from the beginning and stacked objects
  // from the end. This has a nice property of not paging in whole span at once
  // and not draining whole cache.
  // 剩余的object加入freelist_的索引,以栈的形成索引,详细的组织形式看 span.cc:100
  ObjIdx* host = nullptr;  // cached first object on freelist
  const size_t max_embed = size / sizeof(ObjIdx) - 1;
  int embed_count = 0;
  while (idx < idxEnd) {
    // Check the no idx can be confused with kListEnd.
    TC_ASSERT_NE(idx, kListEnd);
    if (host && embed_count != max_embed) {
      // Push onto first object on the freelist.
      embed_count++;
      idxEnd -= idxStep;
      host[embed_count] = idxEnd;
    } else {
      // The first object is full, push new object onto freelist.
      host = IdxToPtr(idx, size, start);
      host[0] = freelist_;
      freelist_ = idx;
      embed_count = 0;
      idx += idxStep;
    }
  }
  embed_count_ = embed_count;
  return result;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
