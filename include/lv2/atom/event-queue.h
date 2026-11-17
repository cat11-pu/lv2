#ifndef LV2_ATOM_EVENT_QUEUE_H
#define LV2_ATOM_EVENT_QUEUE_H

/**
   @defgroup atom_event_queue Event Queue
   @ingroup atom

   A bounded lock-free single-producer/single-consumer queue for
   time-stamped Atom events.

   This is a non-normative convenience API.  One thread at a time may call
   lv2_atom_event_queue_push(), and one other thread at a time may call
   lv2_atom_event_queue_pop().  The queue uses acquire/release atomics and
   performs no allocation after initialization, so it is suitable for passing
   events from an audio thread to a UI thread.

   A push is atomic with respect to consumers: either the entire batch is
   queued, or nothing is queued and false is returned.  This makes overflow
   explicit and prevents partially written batches from being observed.

   The queue is bounded, so no-loss delivery depends on the caller respecting
   backpressure.  The intended protocol is:

   - The producer calls push() for every batch and never discards a batch for
     which push() returned false.
   - The consumer calls pop() at least once per audio cycle, using the end of
     that cycle as `max_time`.
   - Capacity is at least the maximum number of bytes that can be pending
     across two consecutive cycles.

   Under that protocol, every event published in cycle N is available no
   later than the consumer's cycle N+1 poll.

   Events must be in non-decreasing timestamp order, both within a batch and
   across successive pushes.  Events with equal timestamps retain FIFO order.
   Event bodies are copied byte-for-byte, so they must be self-contained POD
   data; references and pointers inside atoms are not followed.

   A pop copies all queued events with a timestamp less than or equal to the
   caller's limit into an Atom sequence.  It stops early if the output buffer
   is too small, leaving the remaining events queued.

   @{
*/

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#  define LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE(ptr) \
    ((uint32_t)_InterlockedCompareExchange((volatile long*)(ptr), 0L, 0L))
#  define LV2_ATOM_EVENT_QUEUE_STORE_RELEASE(ptr, value) \
    ((void)_InterlockedExchange((volatile long*)(ptr), (long)(value)))
#elif defined(__GNUC__) || defined(__clang__)
#  define LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE(ptr) \
    __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
#  define LV2_ATOM_EVENT_QUEUE_STORE_RELEASE(ptr, value) \
    __atomic_store_n((ptr), (value), __ATOMIC_RELEASE)
#else
#  error "LV2 Atom event queue requires GCC, Clang, or MSVC atomics"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
   A bounded SPSC queue of time-stamped Atom events.

   The fields are implementation details and must not be modified directly.
   The queue must be initialized with lv2_atom_event_queue_init() before use.
*/
typedef struct {
  uint8_t* data;          /**< Caller-provided event storage. */
  uint32_t capacity;      /**< Storage size in bytes, a power of two. */
  uint32_t mask;          /**< Capacity minus one. */
  uint32_t head;          /**< Read offset, written by the consumer. */
  uint32_t tail;          /**< Write offset, written by the producer. */
  int64_t  last_time;     /**< Timestamp of the last queued event. */
  bool     have_last_time;/**< Whether last_time is set. */
} LV2_Atom_Event_Queue;

/**
   Initialize a queue using caller-provided storage.

   `capacity` must be a power of two of at least sizeof(LV2_Atom_Event) and
   at most 2^30.  No memory is allocated.  `storage` must remain valid for
   the lifetime of the queue and must not be accessed by the application
   while the queue is in use.

   @return True on success, or false if the arguments are invalid.
*/
static inline bool
lv2_atom_event_queue_init(LV2_Atom_Event_Queue* queue,
                          uint8_t*              storage,
                          uint32_t              capacity)
{
  if (!queue || !storage || capacity < sizeof(LV2_Atom_Event) ||
      capacity > 0x40000000U || (capacity & (capacity - 1U)) != 0U) {
    return false;
  }

  queue->data           = storage;
  queue->capacity       = capacity;
  queue->mask           = capacity - 1U;
  queue->head           = 0U;
  queue->tail           = 0U;
  queue->last_time      = 0;
  queue->have_last_time = false;
  return true;
}

/**
   Push all events in `batch` as one atomic batch.

   The batch is expected to be an Atom sequence whose events use audio frame
   timestamps and are sorted by timestamp.  The timestamp order is checked
   against both the batch and the previously pushed event.

   If the queue does not have room for the entire batch, no events are queued
   and false is returned.  The caller retains ownership of `batch` and may
   retry it later.  `batch` must not alias the queue storage.

   This function must only be called by the producer thread.

   @return True if the batch was queued, otherwise false.
*/
static inline bool
lv2_atom_event_queue_push(LV2_Atom_Event_Queue*     queue,
                          const LV2_Atom_Sequence* batch)
{
  if (!queue || !queue->data || !batch ||
      batch->atom.size < sizeof(LV2_Atom_Sequence_Body)) {
    return false;
  }

  const uint8_t* const body = (const uint8_t*)&batch->body;
  const uint8_t* const end  = body + batch->atom.size;
  const uint8_t*       pos  = body + sizeof(LV2_Atom_Sequence_Body);

  uint64_t batch_size = 0U;
  int64_t  last_time  = 0;
  bool     have_time  = false;

  while (pos < end) {
    if ((size_t)(end - pos) < sizeof(LV2_Atom_Event)) {
      return false;
    }

    const LV2_Atom_Event* const event = (const LV2_Atom_Event*)pos;
    const uint64_t total =
      (uint64_t)sizeof(LV2_Atom_Event) + (uint64_t)event->body.size;
    if (total > 0xFFFFFFF8U) {
      return false;
    }

    const uint32_t event_size = lv2_atom_pad_size((uint32_t)total);
    if ((uint64_t)(end - pos) < event_size || event_size == 0U) {
      return false;
    }

    if (have_time && event->time.frames < last_time) {
      return false;
    }

    last_time = event->time.frames;
    have_time = true;
    batch_size += event_size;
    pos += event_size;
  }

  if ((queue->have_last_time && have_time && last_time < queue->last_time) ||
      batch_size > queue->capacity) {
    return false;
  }

  const uint32_t head = LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE(&queue->head);
  const uint32_t tail = LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE(&queue->tail);
  const uint32_t used = tail - head;
  if (used > queue->capacity ||
      batch_size > (uint64_t)(queue->capacity - used)) {
    return false;
  }

  uint32_t offset = tail;
  pos             = body + sizeof(LV2_Atom_Sequence_Body);
  while (pos < end) {
    const LV2_Atom_Event* const event = (const LV2_Atom_Event*)pos;
    const uint32_t total =
      lv2_atom_pad_size((uint32_t)(sizeof(LV2_Atom_Event) + event->body.size));
    const uint32_t index = offset & queue->mask;
    const uint32_t first = (total < queue->capacity - index)
                             ? total
                             : queue->capacity - index;

    memcpy(queue->data + index, pos, first);
    if (first < total) {
      memcpy(queue->data, pos + first, total - first);
    }

    pos += total;
    offset += total;
  }

  LV2_ATOM_EVENT_QUEUE_STORE_RELEASE(&queue->tail, offset);
  if (have_time) {
    queue->last_time      = last_time;
    queue->have_last_time = true;
  }

  return true;
}

/**
   Pop events with timestamps up to and including `max_time`.

   Events are appended to `out`, which must point to at least `capacity`
   bytes.  The caller must initialize `out->atom.type` and `out->body.unit`;
   this function initializes the sequence size and padding fields.

   The function stops at the first event later than `max_time`, or when the
   next event does not fit in `out`.  Events that are not popped remain at the
   front of the queue in their original order.  `out` must not alias the queue
   storage.

   This function must only be called by the consumer thread.

   @return The number of events appended to `out`.
*/
static inline uint32_t
lv2_atom_event_queue_pop(LV2_Atom_Event_Queue* queue,
                         int64_t               max_time,
                         LV2_Atom_Sequence*    out,
                         uint32_t              capacity)
{
  if (!queue || !queue->data || !out ||
      capacity < sizeof(LV2_Atom_Sequence)) {
    return 0U;
  }

  out->atom.size = sizeof(LV2_Atom_Sequence_Body);
  out->body.pad  = 0U;

  const uint32_t tail  = LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE(&queue->tail);
  uint32_t       head  = LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE(&queue->head);
  uint32_t       count = 0U;

  while (tail != head) {
    const uint32_t used = tail - head;
    if (used < sizeof(LV2_Atom_Event)) {
      break;
    }

    LV2_Atom_Event event;
    {
      const uint32_t index = head & queue->mask;
      const uint32_t first =
        (sizeof(event) < queue->capacity - index)
          ? (uint32_t)sizeof(event)
          : queue->capacity - index;

      memcpy(&event, queue->data + index, first);
      if (first < sizeof(event)) {
        memcpy((uint8_t*)&event + first, queue->data, sizeof(event) - first);
      }
    }

    const uint64_t total64 =
      (uint64_t)sizeof(LV2_Atom_Event) + (uint64_t)event.body.size;
    if (total64 > 0xFFFFFFF8U) {
      break;
    }

    const uint32_t total = lv2_atom_pad_size((uint32_t)total64);
    if (total > used || total == 0U || event.time.frames > max_time) {
      break;
    }

    const uint32_t out_used = out->atom.size;
    if (total > capacity - sizeof(LV2_Atom) - out_used) {
      break;
    }

    uint8_t* const out_body = (uint8_t*)&out->body + sizeof(out->body);
    const uint32_t index     = head & queue->mask;
    const uint32_t first =
      (total < queue->capacity - index) ? total : queue->capacity - index;

    memcpy(out_body + out_used - sizeof(out->body),
           queue->data + index,
           first);
    if (first < total) {
      memcpy(out_body + out_used - sizeof(out->body) + first,
             queue->data,
             total - first);
    }

    head += total;
    out->atom.size += total;
    ++count;
  }

  if (count > 0U) {
    LV2_ATOM_EVENT_QUEUE_STORE_RELEASE(&queue->head, head);
  }

  return count;
}

#undef LV2_ATOM_EVENT_QUEUE_LOAD_ACQUIRE
#undef LV2_ATOM_EVENT_QUEUE_STORE_RELEASE

#ifdef __cplusplus
} /* extern "C" */
#endif

/**
   @}
*/

#endif // LV2_ATOM_EVENT_QUEUE_H
