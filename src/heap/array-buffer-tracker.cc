// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/array-buffer-tracker.h"
#include "src/heap/array-buffer-tracker-inl.h"
#include "src/heap/heap.h"

namespace v8 {
namespace internal {

LocalArrayBufferTracker::~LocalArrayBufferTracker() {
  CHECK(array_buffers_.empty());
}

template <LocalArrayBufferTracker::FreeMode free_mode>
void LocalArrayBufferTracker::Free() {
  size_t freed_memory = 0;
  for (TrackingMap::iterator it = array_buffers_.begin();
       it != array_buffers_.end();) {
    if ((free_mode == kFreeAll) ||
        Marking::IsWhite(Marking::MarkBitFrom(it->first))) {
      heap_->isolate()->array_buffer_allocator()->Free(it->second.first,
                                                       it->second.second);
      freed_memory += it->second.second;
      it = array_buffers_.erase(it);
    } else {
      it++;
    }
  }
  if (freed_memory > 0) {
    heap_->update_external_memory_concurrently_freed(
        static_cast<intptr_t>(freed_memory));
  }
}

template <typename Callback>
void LocalArrayBufferTracker::Process(Callback callback) {
  JSArrayBuffer* new_buffer = nullptr;
  size_t freed_memory = 0;
  for (TrackingMap::iterator it = array_buffers_.begin();
       it != array_buffers_.end();) {
    const CallbackResult result = callback(it->first, &new_buffer);
    if (result == kKeepEntry) {
      it++;
    } else if (result == kUpdateEntry) {
      DCHECK_NOT_NULL(new_buffer);
      Page* target_page = Page::FromAddress(new_buffer->address());
      // We need to lock the target page because we cannot guarantee
      // exclusive access to new space pages.
      if (target_page->InNewSpace()) target_page->mutex()->Lock();
      LocalArrayBufferTracker* tracker = target_page->local_tracker();
      if (tracker == nullptr) {
        target_page->AllocateLocalTracker();
        tracker = target_page->local_tracker();
      }
      DCHECK_NOT_NULL(tracker);
      tracker->Add(new_buffer, it->second);
      if (target_page->InNewSpace()) target_page->mutex()->Unlock();
      it = array_buffers_.erase(it);
    } else if (result == kRemoveEntry) {
      heap_->isolate()->array_buffer_allocator()->Free(it->second.first,
                                                       it->second.second);
      freed_memory += it->second.second;
      it = array_buffers_.erase(it);
    } else {
      UNREACHABLE();
    }
  }
  if (freed_memory > 0) {
    heap_->update_external_memory_concurrently_freed(
        static_cast<intptr_t>(freed_memory));
  }
}

void ArrayBufferTracker::FreeDeadInNewSpace(Heap* heap) {
  DCHECK_EQ(heap->gc_state(), Heap::HeapState::SCAVENGE);
  NewSpacePageIterator from_it(heap->new_space()->FromSpaceStart(),
                               heap->new_space()->FromSpaceEnd());
  while (from_it.has_next()) {
    Page* page = from_it.next();
    bool empty = ProcessBuffers(page, kUpdateForwardedRemoveOthers);
    CHECK(empty);
  }
  heap->account_external_memory_concurrently_freed();
}

void ArrayBufferTracker::FreeDead(Page* page) {
  // Callers need to ensure having the page lock.
  LocalArrayBufferTracker* tracker = page->local_tracker();
  if (tracker == nullptr) return;
  DCHECK(!page->SweepingDone());
  tracker->Free<LocalArrayBufferTracker::kFreeDead>();
  if (tracker->IsEmpty()) {
    page->ReleaseLocalTracker();
  }
}

void ArrayBufferTracker::FreeAll(Page* page) {
  LocalArrayBufferTracker* tracker = page->local_tracker();
  if (tracker == nullptr) return;
  tracker->Free<LocalArrayBufferTracker::kFreeAll>();
  if (tracker->IsEmpty()) {
    page->ReleaseLocalTracker();
  }
}

bool ArrayBufferTracker::ProcessBuffers(Page* page, ProcessingMode mode) {
  LocalArrayBufferTracker* tracker = page->local_tracker();
  if (tracker == nullptr) return true;

  DCHECK(page->SweepingDone());
  tracker->Process(
      [mode](JSArrayBuffer* old_buffer, JSArrayBuffer** new_buffer) {
        MapWord map_word = old_buffer->map_word();
        if (map_word.IsForwardingAddress()) {
          *new_buffer = JSArrayBuffer::cast(map_word.ToForwardingAddress());
          return LocalArrayBufferTracker::kUpdateEntry;
        }
        return mode == kUpdateForwardedKeepOthers
                   ? LocalArrayBufferTracker::kKeepEntry
                   : LocalArrayBufferTracker::kRemoveEntry;
      });
  return tracker->IsEmpty();
}

bool ArrayBufferTracker::IsTracked(JSArrayBuffer* buffer) {
  Page* page = Page::FromAddress(buffer->address());
  {
    base::LockGuard<base::Mutex> guard(page->mutex());
    LocalArrayBufferTracker* tracker = page->local_tracker();
    if (tracker == nullptr) return false;
    return tracker->IsTracked(buffer);
  }
}

}  // namespace internal
}  // namespace v8
