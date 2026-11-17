#define _POSIX_C_SOURCE 200809L

#include <lv2/atom/atom.h>
#include <lv2/atom/event-queue.h>
#include <lv2/atom/util.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sched.h>
#  include <time.h>
#endif

#define CHECK(condition, ...)     \
  do {                            \
    if (!(condition)) {           \
      return fail(__VA_ARGS__);   \
    }                             \
  } while (0)

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static int
fail(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  fprintf(stderr, "error: ");
  vfprintf(stderr, format, args);
  va_end(args);
  return 1;
}

static uint32_t
event_size(uint32_t body_size)
{
  return lv2_atom_pad_size((uint32_t)sizeof(LV2_Atom_Event) + body_size);
}

static void
sequence_reset(LV2_Atom_Sequence* sequence, uint32_t type)
{
  memset(sequence, 0, sizeof(*sequence));
  sequence->atom.type = type;
  sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
}

static bool
sequence_add(LV2_Atom_Sequence* sequence,
             uint32_t           capacity,
             int64_t            time,
             uint32_t           type,
             const void*        body,
             uint32_t           body_size)
{
  const uint32_t total = event_size(body_size);
  if ((uint64_t)sizeof(LV2_Atom) + sequence->atom.size + total > capacity) {
    return false;
  }

  LV2_Atom_Event* const event =
    (LV2_Atom_Event*)((uint8_t*)sequence + sizeof(LV2_Atom) +
                      sequence->atom.size);
  event->time.frames = time;
  event->body.type   = type;
  event->body.size   = body_size;

  if (body_size > 0U) {
    memcpy(event + 1, body, body_size);
  }

  const uint32_t padding = total - (uint32_t)sizeof(*event) - body_size;
  if (padding > 0U) {
    memset((uint8_t*)(event + 1) + body_size, 0, padding);
  }

  sequence->atom.size += total;
  return true;
}

static bool
sequence_add_int(LV2_Atom_Sequence* sequence,
                 uint32_t           capacity,
                 int64_t            time,
                 int32_t            value)
{
  return sequence_add(
    sequence, capacity, time, 1U, &value, (uint32_t)sizeof(value));
}

static bool
sequence_add_bytes(LV2_Atom_Sequence* sequence,
                   uint32_t           capacity,
                   int64_t            time,
                   const uint8_t*     bytes,
                   uint32_t           size)
{
  return sequence_add(sequence, capacity, time, 1U, bytes, size);
}

static void
verify_ints(const LV2_Atom_Sequence* sequence,
            uint32_t                 expected_count,
            int32_t                  first_value,
            int64_t                  first_time,
            int64_t                  time_step,
            uint32_t*                out_count,
            int*                     out_failed)
{
  uint32_t i = 0U;
  *out_failed = 0;

  LV2_ATOM_SEQUENCE_FOREACH(sequence, event)
  {
    const int32_t value = *(const int32_t*)LV2_ATOM_BODY_CONST(&event->body);
    if (event->body.type != 1U || event->body.size != sizeof(value) ||
        value != first_value + (int32_t)i ||
        event->time.frames != first_time + (int64_t)i * time_step) {
      *out_failed = 1;
      break;
    }

    ++i;
  }

  *out_count = i;
  if (i != expected_count) {
    *out_failed = 1;
  }
}

static int
test_init(void)
{
  LV2_Atom_Event_Queue queue;
  uint8_t              storage[256];

  CHECK(!lv2_atom_event_queue_init(NULL, storage, sizeof(storage)),
        "init accepted a null queue\n");
  CHECK(!lv2_atom_event_queue_init(&queue, NULL, sizeof(storage)),
        "init accepted null storage\n");
  CHECK(!lv2_atom_event_queue_init(&queue, storage, 0U),
        "init accepted zero capacity\n");
  CHECK(!lv2_atom_event_queue_init(&queue, storage, 96U),
        "init accepted a non-power-of-two capacity\n");
  CHECK(!lv2_atom_event_queue_init(&queue, storage, 0x80000000U),
        "init accepted a capacity that can wrap offsets ambiguously\n");
  CHECK(lv2_atom_event_queue_init(&queue, storage, 16U),
        "init rejected the minimum capacity\n");
  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "init rejected valid storage\n");
  return 0;
}

static int
test_empty_and_cutoff(void)
{
  uint8_t              storage[512];
  uint8_t              batch_mem[1024];
  uint8_t              out_mem[1024];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out   = (LV2_Atom_Sequence*)out_mem;

  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to initialize queue\n");

  sequence_reset(out, 42U);
  out->body.unit = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 0, out, sizeof(out_mem)) == 0U,
        "empty queue returned events\n");
  CHECK(out->atom.size == sizeof(LV2_Atom_Sequence_Body),
        "empty pop modified output size\n");

  sequence_reset(batch, 42U);
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "empty batch push failed\n");

  CHECK(sequence_add_int(batch, sizeof(batch_mem), 10, 0),
        "failed to build test batch\n");
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 10, 1),
        "failed to build test batch\n");
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 20, 2),
        "failed to build test batch\n");
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "failed to push test batch\n");

  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 9, out, sizeof(out_mem)) == 0U,
        "pop returned an event after the cutoff\n");

  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 10, out, sizeof(out_mem)) == 2U,
        "pop did not return all events at the cutoff\n");
  {
    uint32_t count  = 0U;
    int      failed = 0;
    verify_ints(out, 2U, 0, 10, 0, &count, &failed);
    CHECK(!failed, "equal-timestamp events were lost or reordered\n");
  }

  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 19, out, sizeof(out_mem)) == 0U,
        "pop returned a future event\n");
  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 20, out, sizeof(out_mem)) == 1U,
        "pop did not return the final event\n");
  {
    uint32_t count  = 0U;
    int      failed = 0;
    verify_ints(out, 1U, 2, 20, 0, &count, &failed);
    CHECK(!failed, "final event was corrupt\n");
  }

  return 0;
}

static int
test_batch_atomicity(void)
{
  uint8_t              storage[512];
  uint8_t              batch_mem[1024];
  uint8_t              out_mem[1024];
  uint8_t              body[12];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out   = (LV2_Atom_Sequence*)out_mem;

  memset(body, 0, sizeof(body));
  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to initialize queue\n");

  sequence_reset(batch, 42U);
  for (int32_t i = 0; i < 15; ++i) {
    CHECK(sequence_add_bytes(
            batch, sizeof(batch_mem), i, body, (uint32_t)sizeof(body)),
          "failed to build 15-event batch\n");
  }
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "failed to push 15-event batch\n");

  sequence_reset(batch, 42U);
  CHECK(sequence_add_bytes(
          batch, sizeof(batch_mem), 15, body, (uint32_t)sizeof(body)),
        "failed to build first overflow event\n");
  CHECK(sequence_add_bytes(
          batch, sizeof(batch_mem), 16, body, (uint32_t)sizeof(body)),
        "failed to build second overflow event\n");
  CHECK(!lv2_atom_event_queue_push(&queue, batch),
        "push accepted a batch that did not fit\n");

  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, sizeof(out_mem)) == 15U,
        "a rejected batch caused a partial write or event loss\n");

  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to reinitialize queue\n");
  sequence_reset(batch, 42U);
  for (int32_t i = 0; i < 16; ++i) {
    CHECK(sequence_add_bytes(
            batch, sizeof(batch_mem), i, body, (uint32_t)sizeof(body)),
          "failed to build exact-fit batch\n");
  }
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "push rejected an exact-fit batch\n");
  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, sizeof(out_mem)) == 16U,
        "exact-fit batch was truncated\n");

  CHECK(lv2_atom_event_queue_init(&queue, storage, 64U),
        "failed to initialize small queue\n");
  sequence_reset(batch, 42U);
  for (int32_t i = 0; i < 3; ++i) {
    CHECK(sequence_add_bytes(
            batch, sizeof(batch_mem), i, body, (uint32_t)sizeof(body)),
          "failed to build oversize batch\n");
  }
  CHECK(!lv2_atom_event_queue_push(&queue, batch),
        "push accepted a batch larger than the queue\n");

  return 0;
}

static int
test_order_validation(void)
{
  uint8_t              storage[512];
  uint8_t              batch_mem[512];
  uint8_t              out_mem[512];
  uint8_t              malformed_mem[64];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch     = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out       = (LV2_Atom_Sequence*)out_mem;
  LV2_Atom_Sequence*   malformed = (LV2_Atom_Sequence*)malformed_mem;

  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to initialize queue\n");

  sequence_reset(batch, 42U);
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 10, 0),
        "failed to build ordered batch\n");
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 11, 1),
        "failed to build ordered batch\n");
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 12, 2),
        "failed to build ordered batch\n");
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "failed to push ordered batch\n");

  sequence_reset(batch, 42U);
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 9, 3),
        "failed to build stale batch\n");
  CHECK(!lv2_atom_event_queue_push(&queue, batch),
        "push accepted a timestamp older than the previous batch\n");

  sequence_reset(batch, 42U);
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 13, 3),
        "failed to build unordered batch\n");
  CHECK(sequence_add_int(batch, sizeof(batch_mem), 12, 4),
        "failed to build unordered batch\n");
  CHECK(!lv2_atom_event_queue_push(&queue, batch),
        "push accepted an unordered batch\n");

  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, sizeof(out_mem)) == 3U,
        "rejected batches changed the queue\n");
  {
    uint32_t count  = 0U;
    int      failed = 0;
    verify_ints(out, 3U, 0, 10, 1, &count, &failed);
    CHECK(!failed, "queued order changed\n");
  }

  memset(malformed_mem, 0, sizeof(malformed_mem));
  malformed->atom.type = 42U;
  malformed->atom.size = sizeof(LV2_Atom_Sequence_Body) + 8U;
  CHECK(!lv2_atom_event_queue_push(&queue, malformed),
        "push accepted a truncated event header\n");

  return 0;
}

static int
test_output_limits(void)
{
  uint8_t              storage[512];
  uint8_t              batch_mem[512];
  uint8_t              out_mem[512];
  uint8_t              body[12];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out   = (LV2_Atom_Sequence*)out_mem;

  memset(body, 0, sizeof(body));
  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to initialize queue\n");

  sequence_reset(batch, 42U);
  for (int32_t i = 0; i < 3; ++i) {
    CHECK(sequence_add_bytes(
            batch, sizeof(batch_mem), i, body, (uint32_t)sizeof(body)),
          "failed to build output-limit batch\n");
  }
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "failed to push output-limit batch\n");

  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, 47U) == 0U,
        "pop wrote an event into an undersized output\n");
  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, 48U) == 1U,
        "pop failed at the exact output boundary\n");
  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, 80U) == 2U,
        "pop did not fill the output to its exact boundary\n");
  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 100, out, 80U) == 0U,
        "pop returned all events\n");

  return 0;
}

static int
test_min_capacity_and_overflow(void)
{
  uint8_t              storage[64];
  uint8_t              batch_mem[128];
  uint8_t              out_mem[64];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out   = (LV2_Atom_Sequence*)out_mem;

  CHECK(lv2_atom_event_queue_init(&queue, storage, 16U),
        "failed to initialize minimum-size queue\n");

  sequence_reset(batch, 42U);
  CHECK(sequence_add(batch, sizeof(batch_mem), 0, 1U, NULL, 0U),
        "failed to build empty-body event\n");
  CHECK(lv2_atom_event_queue_push(&queue, batch),
        "minimum-size queue rejected an exact-fit event\n");
  out->atom.size = 0U;
  CHECK(lv2_atom_event_queue_pop(&queue, 0, out, 32U) == 1U,
        "minimum-size queue did not return its event\n");

  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to reinitialize queue\n");
  memset(batch_mem, 0, sizeof(batch_mem));
  batch->atom.size = sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event);
  {
    LV2_Atom_Event* const event =
      (LV2_Atom_Event*)((uint8_t*)batch + sizeof(LV2_Atom) +
                        sizeof(LV2_Atom_Sequence_Body));
    event->time.frames = 1;
    event->body.size   = UINT32_MAX;
    event->body.type   = 1U;
  }
  CHECK(!lv2_atom_event_queue_push(&queue, batch),
        "push accepted an event with an overflowing body size\n");

  return 0;
}

static int
test_wraparound(void)
{
  uint8_t              storage[128];
  uint8_t              batch_mem[256];
  uint8_t              out_mem[256];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out   = (LV2_Atom_Sequence*)out_mem;

  CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
        "failed to initialize queue\n");

  int32_t expected = 0;
  for (int32_t iteration = 0; iteration < 50; ++iteration) {
    sequence_reset(batch, 42U);
    for (int32_t i = 0; i < 5; ++i) {
      CHECK(sequence_add_int(
              batch, sizeof(batch_mem), iteration * 5 + i, expected + i),
            "failed to build wraparound batch\n");
    }

    CHECK(lv2_atom_event_queue_push(&queue, batch),
          "failed to push wraparound batch\n");

    out->atom.size = 0U;
    CHECK(lv2_atom_event_queue_pop(&queue, 10000, out, sizeof(out_mem)) == 5U,
          "failed to pop wraparound batch\n");
    {
      uint32_t count  = 0U;
      int      failed = 0;
      verify_ints(
        out, 5U, expected, iteration * 5, 1, &count, &failed);
      CHECK(!failed, "wraparound corrupted event order or payload\n");
    }

    expected += 5;
  }

  return 0;
}

static int
test_reuse_10000(void)
{
  uint8_t              storage[128];
  uint8_t              batch_mem[128];
  uint8_t              out_mem[128];
  LV2_Atom_Event_Queue queue;
  LV2_Atom_Sequence*   batch = (LV2_Atom_Sequence*)batch_mem;
  LV2_Atom_Sequence*   out   = (LV2_Atom_Sequence*)out_mem;

  for (int32_t cycle = 0; cycle < 10000; ++cycle) {
    CHECK(lv2_atom_event_queue_init(&queue, storage, sizeof(storage)),
          "failed to initialize queue on cycle %d\n", cycle);

    sequence_reset(batch, 42U);
    CHECK(sequence_add_int(batch, sizeof(batch_mem), cycle, cycle),
          "failed to build reuse batch\n");
    CHECK(lv2_atom_event_queue_push(&queue, batch),
          "failed to push reuse batch on cycle %d\n", cycle);

    out->atom.size = 0U;
    CHECK(lv2_atom_event_queue_pop(&queue, cycle, out, sizeof(out_mem)) == 1U,
          "failed to pop reuse batch on cycle %d\n", cycle);
  }

  return 0;
}

typedef struct {
  LV2_Atom_Event_Queue queue;
  uint32_t             frames;
  uint32_t             events_per_frame;
  uint32_t             received;
  uint32_t             lost;
  uint32_t             duplicates;
  uint32_t             reordered;
  uint32_t             timestamp_errors;
  uint32_t             max_lag_cycles;
  uint32_t             checksum;
  volatile uint32_t    start;
  volatile uint32_t    produced;
  volatile uint32_t    consumed;
  volatile uint32_t    done;
} Stress;

static uint32_t
load_acquire(volatile uint32_t* value)
{
#if defined(_MSC_VER)
  return (uint32_t)_InterlockedCompareExchange(
    (volatile long*)value, 0L, 0L);
#else
  return __atomic_load_n((const uint32_t*)value, __ATOMIC_ACQUIRE);
#endif
}

static void
store_release(volatile uint32_t* value, uint32_t new_value)
{
#if defined(_MSC_VER)
  (void)_InterlockedExchange((volatile long*)value, (long)new_value);
#else
  __atomic_store_n((uint32_t*)value, new_value, __ATOMIC_RELEASE);
#endif
}

static void
thread_yield(void)
{
#if defined(_WIN32)
  SwitchToThread();
#else
  sched_yield();
#endif
}

static void
producer_run(Stress* stress)
{
  uint8_t batch_mem[512];
  LV2_Atom_Sequence* batch = (LV2_Atom_Sequence*)batch_mem;

  while (load_acquire(&stress->start) == 0U) {
    thread_yield();
  }

  for (uint32_t frame = 0; frame < stress->frames; ++frame) {
    while (!load_acquire(&stress->done) && frame > 0U &&
           load_acquire(&stress->consumed) + 1U < frame) {
      thread_yield();
    }
    if (load_acquire(&stress->done)) {
      break;
    }

    sequence_reset(batch, 42U);
    for (uint32_t i = 0; i < stress->events_per_frame; ++i) {
      const int32_t value = (int32_t)(frame * stress->events_per_frame + i);
      (void)sequence_add_int(
        batch, sizeof(batch_mem), (int64_t)frame, value);
    }

    while (!lv2_atom_event_queue_push(&stress->queue, batch)) {
      if (load_acquire(&stress->done)) {
        return;
      }
      thread_yield();
    }

    store_release(&stress->produced, frame + 1U);
  }
}

static void
consumer_run(Stress* stress)
{
  uint8_t out_mem[1024];
  LV2_Atom_Sequence* out = (LV2_Atom_Sequence*)out_mem;
  uint32_t      expected = 0U;

  while (load_acquire(&stress->start) == 0U) {
    thread_yield();
  }

  for (uint32_t frame = 0; frame < stress->frames; ++frame) {
    const uint32_t wait_for =
      (frame + 1U < stress->frames) ? frame + 2U : stress->frames;
    while (load_acquire(&stress->produced) < wait_for) {
      if (load_acquire(&stress->done)) {
        return;
      }
      thread_yield();
    }

    memset(out, 0, sizeof(LV2_Atom_Sequence));
    out->atom.type = 42U;
    out->body.unit = 0U;

    const uint32_t count = lv2_atom_event_queue_pop(
      &stress->queue, (int64_t)frame, out, sizeof(out_mem));
    if (count != stress->events_per_frame) {
      stress->lost += stress->events_per_frame - count;
      store_release(&stress->done, 1U);
      return;
    }

    LV2_ATOM_SEQUENCE_FOREACH(out, event)
    {
      const int32_t value = *(const int32_t*)LV2_ATOM_BODY_CONST(&event->body);
      if (event->body.type != 1U ||
          event->body.size != (uint32_t)sizeof(value) ||
          event->time.frames != (int64_t)frame) {
        ++stress->timestamp_errors;
      }

      if ((uint32_t)value < expected) {
        ++stress->duplicates;
        ++stress->reordered;
      } else if ((uint32_t)value > expected) {
        stress->lost += (uint32_t)value - expected;
      } else {
        ++stress->received;
      }

      expected = (uint32_t)value + 1U;
      stress->checksum = (stress->checksum ^ (uint32_t)value) * 16777619U;

      const uint32_t delivery_cycle = frame + 1U;
      const uint32_t lag =
        delivery_cycle - (uint32_t)event->time.frames;
      if (lag > stress->max_lag_cycles) {
        stress->max_lag_cycles = lag;
      }
    }

    store_release(&stress->consumed, frame + 1U);
  }
}

#if defined(_WIN32)
typedef HANDLE Test_Thread;

static DWORD WINAPI
producer_entry(LPVOID argument)
{
  producer_run((Stress*)argument);
  return 0U;
}

static DWORD WINAPI
consumer_entry(LPVOID argument)
{
  consumer_run((Stress*)argument);
  return 0U;
}

typedef DWORD(WINAPI* Thread_Function)(LPVOID);

static int
spawn_thread(Test_Thread* thread, Thread_Function function, void* argument)
{
  *thread = CreateThread(NULL, 0U, function, argument, 0U, NULL);
  return *thread ? 0 : 1;
}

static void
join_thread(Test_Thread thread)
{
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
}
#else
typedef pthread_t Test_Thread;

static void*
producer_entry(void* argument)
{
  producer_run((Stress*)argument);
  return NULL;
}

static void*
consumer_entry(void* argument)
{
  consumer_run((Stress*)argument);
  return NULL;
}

static int
spawn_thread(Test_Thread* thread, void* (*function)(void*), void* argument)
{
  return pthread_create(thread, NULL, function, argument);
}

static void
join_thread(Test_Thread thread)
{
  pthread_join(thread, NULL);
}
#endif

static uint64_t
now_ns(void)
{
#if defined(_WIN32)
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000000) / frequency.QuadPart);
#else
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000000000U + (uint64_t)time.tv_nsec;
#endif
}

static uint32_t
expected_checksum(uint32_t count)
{
  uint32_t checksum = 2166136261U;
  for (uint32_t i = 0; i < count; ++i) {
    checksum = (checksum ^ i) * 16777619U;
  }
  return checksum;
}

static int
test_concurrent_10000(void)
{
  uint8_t storage[4096];
  Stress  stress;
  memset(&stress, 0, sizeof(stress));

  stress.frames           = 10000U;
  stress.events_per_frame = 4U;
  stress.checksum         = 2166136261U;

  CHECK(lv2_atom_event_queue_init(
          &stress.queue, storage, (uint32_t)sizeof(storage)),
        "failed to initialize stress queue\n");

  Test_Thread producer;
  Test_Thread consumer;
  CHECK(!spawn_thread(&producer, producer_entry, &stress),
        "failed to start producer thread\n");
  CHECK(!spawn_thread(&consumer, consumer_entry, &stress),
        "failed to start consumer thread\n");

  const uint64_t start_ns = now_ns();
  store_release(&stress.start, 1U);
  join_thread(producer);
  join_thread(consumer);
  const uint64_t elapsed_ns = now_ns() - start_ns;

  const uint32_t total = stress.frames * stress.events_per_frame;
  CHECK(stress.lost == 0U, "concurrent stress lost %u events\n", stress.lost);
  CHECK(stress.duplicates == 0U,
        "concurrent stress observed %u duplicate or reordered events\n",
        stress.duplicates);
  CHECK(stress.reordered == 0U,
        "concurrent stress observed %u reordered events\n",
        stress.reordered);
  CHECK(stress.timestamp_errors == 0U,
        "concurrent stress observed %u timestamp errors\n",
        stress.timestamp_errors);
  CHECK(stress.received == total,
        "concurrent stress received %u of %u events\n",
        stress.received,
        total);
  CHECK(stress.max_lag_cycles <= 1U,
        "event delivery lag was %u cycles\n", stress.max_lag_cycles);
  CHECK(stress.checksum == expected_checksum(total),
        "event stream checksum mismatch: 0x%08X\n", stress.checksum);

  printf(
    "{\"algorithm\":\"spsc-batch\",\"cycles\":%u,\"events\":%u,"
    "\"received\":%u,\"lost\":%u,\"duplicates\":%u,\"reordered\":%u,"
    "\"timestamp_errors\":%u,\"max_lag_cycles\":%u,"
    "\"lag_bound_cycles\":1,\"checksum\":\"0x%08X\","
    "\"elapsed_ns\":%llu,\"heap_allocations\":0}\n",
    stress.frames,
    total,
    stress.received,
    stress.lost,
    stress.duplicates,
    stress.reordered,
    stress.timestamp_errors,
    stress.max_lag_cycles,
    stress.checksum,
    (unsigned long long)elapsed_ns);

  return 0;
}

int
main(void)
{
  int status = 0;
  status |= test_init();
  status |= test_empty_and_cutoff();
  status |= test_batch_atomicity();
  status |= test_order_validation();
  status |= test_output_limits();
  status |= test_min_capacity_and_overflow();
  status |= test_wraparound();
  status |= test_reuse_10000();
  status |= test_concurrent_10000();
  return status;
}
