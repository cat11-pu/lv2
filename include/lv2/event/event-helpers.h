// Copyright 2008-2015 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef LV2_EVENT_EVENT_HELPERS_H
#define LV2_EVENT_EVENT_HELPERS_H

/**
   @file event-helpers.h Helper functions for the LV2 Event extension
   <http://lv2plug.in/ns/ext/event>.
*/

#include <lv2/core/attributes.h>
#include <lv2/event/event.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

LV2_DISABLE_DEPRECATION_WARNINGS

/** @file
 * Helper functions for the LV2 Event extension
 * <http://lv2plug.in/ns/ext/event>.
 *
 * These functions are provided for convenience only, use of them is not
 * required for supporting lv2ev (i.e. the events extension is defined by the
 * raw buffer format described in lv2_event.h and NOT by this API).
 *
 * Note that these functions are all static inline which basically means:
 * do not take the address of these functions. */

/** Pad a size to 64 bits (for event sizes) */
static inline uint16_t
lv2_event_pad_size(uint16_t size)
{
  return (uint16_t)((size + 7U) & ~7U);
}

/** Result of a deprecated LV2 Event buffer operation. */
typedef enum {
  LV2_EVENT_SUCCESS = 0,           /**< Operation succeeded. */
  LV2_EVENT_ERR_INVALID_ARGUMENT,  /**< A required pointer was null. */
  LV2_EVENT_ERR_INVALID_INDEX,     /**< The buffer index is out of range. */
  LV2_EVENT_ERR_NO_SPACE           /**< The event buffer is full. */
} LV2_Event_Status;

/** Return a human-readable description of an event status. */
static inline const char*
lv2_event_status_string(LV2_Event_Status status)
{
  switch (status) {
  case LV2_EVENT_SUCCESS:
    return "success";
  case LV2_EVENT_ERR_INVALID_ARGUMENT:
    return "invalid argument";
  case LV2_EVENT_ERR_INVALID_INDEX:
    return "invalid buffer index";
  case LV2_EVENT_ERR_NO_SPACE:
    return "insufficient space";
  }

  return "unknown error";
}

/** Initialize (empty, reset..) an existing event buffer.
 * The contents of buf are ignored entirely and overwritten, except capacity
 * which is unmodified. */
static inline void
lv2_event_buffer_reset(LV2_Event_Buffer* buf,
                       uint16_t          stamp_type,
                       uint8_t*          data)
{
  buf->data        = data;
  buf->header_size = sizeof(LV2_Event_Buffer);
  buf->stamp_type  = stamp_type;
  buf->event_count = 0;
  buf->size        = 0;
}

/** Allocate a new, empty event buffer. */
static inline LV2_Event_Buffer*
lv2_event_buffer_new(uint32_t capacity, uint16_t stamp_type)
{
#if SIZE_MAX == UINT32_MAX
  if (capacity > SIZE_MAX - sizeof(LV2_Event_Buffer)) {
    return NULL;
  }
#endif

  const size_t      size = sizeof(LV2_Event_Buffer) + capacity;
  LV2_Event_Buffer* buf  = (LV2_Event_Buffer*)malloc(size);
  if (buf != NULL) {
    buf->capacity = capacity;
    lv2_event_buffer_reset(buf, stamp_type, (uint8_t*)(buf + 1));
    return buf;
  }
  return NULL;
}

/** An iterator over an LV2_Event_Buffer.
 *
 * Multiple simultaneous read iterators over a single buffer is fine,
 * but changing the buffer invalidates all iterators. */
typedef struct {
  LV2_Event_Buffer* buf;
  uint32_t          offset;
  LV2_Event_Status  status;
} LV2_Event_Iterator;

/** Return true iff an event buffer is internally consistent. */
static inline bool
lv2_event_buffer_is_valid(const LV2_Event_Buffer* buf)
{
  return buf && buf->size <= buf->capacity &&
         (!buf->capacity || buf->data);
}

static inline bool
lv2_event_offset_is_boundary(const LV2_Event_Buffer* buf, uint32_t offset);

/** Return true iff `iter` points to a complete event in its buffer. */
static inline bool
lv2_event_iterator_is_valid(LV2_Event_Iterator* iter)
{
  if (!iter) {
    return false;
  }

  if (!lv2_event_buffer_is_valid(iter->buf)) {
    iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    return false;
  }

  if (iter->offset > iter->buf->size) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return false;
  }

  if (iter->offset % sizeof(uint32_t)) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return false;
  }

  if (iter->offset == iter->buf->size) {
    return false;
  }

  const uint32_t remaining = iter->buf->size - iter->offset;
  if (remaining < sizeof(LV2_Event)) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return false;
  }

  const LV2_Event* const ev =
    (const LV2_Event*)(iter->buf->data + iter->offset);
  if (ev->size > remaining - sizeof(LV2_Event)) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return false;
  }

  iter->status = LV2_EVENT_SUCCESS;
  return true;
}

/** Return true iff `offset` is at an event boundary in `buf`. */
static inline bool
lv2_event_offset_is_boundary(const LV2_Event_Buffer* buf, uint32_t offset)
{
  if (!lv2_event_buffer_is_valid(buf) || offset > buf->size) {
    return false;
  }

  if (offset == buf->size) {
    return true;
  }

  uint32_t pos = 0;
  while (pos < offset) {
    const uint32_t remaining = buf->size - pos;
    if (remaining < sizeof(LV2_Event)) {
      return false;
    }

    const LV2_Event* const ev =
      (const LV2_Event*)(buf->data + pos);
    const uint32_t total_size = (uint32_t)sizeof(LV2_Event) + ev->size;
    const uint32_t padded_size = (total_size + 7U) & ~7U;
    if (padded_size > remaining) {
      return false;
    }

    pos += padded_size;
  }

  return pos == offset;
}

/** Reset an iterator to point to the start of `buf`.
 * @return True if `iter` is valid, otherwise false (buffer is empty) */
static inline bool
lv2_event_begin(LV2_Event_Iterator* iter, LV2_Event_Buffer* buf)
{
  if (!iter) {
    return false;
  }

  iter->buf    = buf;
  iter->offset = 0;
  iter->status = LV2_EVENT_SUCCESS;

  if (!lv2_event_buffer_is_valid(buf)) {
    iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    return false;
  }

  return buf->size > 0;
}

/** Check if `iter` is valid.
 * @return True if `iter` is valid, otherwise false (past end of buffer) */
static inline bool
lv2_event_is_valid(LV2_Event_Iterator* iter)
{
  return lv2_event_iterator_is_valid(iter);
}

/** Advance `iter` forward one event.
 * `iter` must be valid.
 * @return True if `iter` is valid, otherwise false (reached end of buffer) */
static inline bool
lv2_event_increment(LV2_Event_Iterator* iter)
{
  if (!lv2_event_is_valid(iter)) {
    return false;
  }

  const LV2_Event* const ev =
    (const LV2_Event*)(iter->buf->data + iter->offset);

  const uint32_t total_size = (uint32_t)sizeof(LV2_Event) + ev->size;
  const uint32_t padded_size = (total_size + 7U) & ~7U;
  iter->offset += padded_size;

  return true;
}

/** Dereference an event iterator (get the event currently pointed at).
 * `iter` must be valid.
 * `data` if non-NULL, will be set to point to the contents of the event
 *         returned.
 * @return A Pointer to the event `iter` is currently pointing at, or NULL
 *         if the end of the buffer is reached (in which case `data` is
 *         also set to NULL). */
static inline LV2_Event*
lv2_event_get(LV2_Event_Iterator* iter, uint8_t** data)
{
  if (!lv2_event_is_valid(iter)) {
    if (data) {
      *data = NULL;
    }
    return NULL;
  }

  LV2_Event* const ev = (LV2_Event*)(iter->buf->data + iter->offset);

  if (data) {
    *data = (uint8_t*)ev + sizeof(LV2_Event);
  }

  return ev;
}

/** Write an event at `iter`.
 * The event (if any) pointed to by `iter` will be overwritten, and `iter`
 * incremented to point to the following event (i.e. several calls to this
 * function can be done in sequence without twiddling iter in-between).
 * @return True if event was written, otherwise false (buffer is full). */
static inline bool
lv2_event_write(LV2_Event_Iterator* iter,
                uint32_t            frames,
                uint32_t            subframes,
                uint16_t            type,
                uint16_t            size,
                const uint8_t*      data)
{
  if (!iter || !iter->buf || (size && !data)) {
    if (iter) {
      iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    }
    return false;
  }

  if (!lv2_event_buffer_is_valid(iter->buf)) {
    iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    return false;
  }

  if (iter->offset > iter->buf->size ||
      !lv2_event_offset_is_boundary(iter->buf, iter->offset) ||
      (iter->offset < iter->buf->size && !lv2_event_is_valid(iter))) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return false;
  }

  const bool     append     = iter->offset == iter->buf->size;
  const uint32_t total_size = (uint32_t)sizeof(LV2_Event) + size;
  const uint32_t padded_size = (total_size + 7U) & ~7U;
  if (iter->offset > iter->buf->capacity ||
      padded_size > iter->buf->capacity - iter->offset) {
    iter->status = LV2_EVENT_ERR_NO_SPACE;
    return false;
  }

  LV2_Event* const ev = (LV2_Event*)(iter->buf->data + iter->offset);

  ev->frames    = frames;
  ev->subframes = subframes;
  ev->type      = type;
  ev->size      = size;
  memcpy((uint8_t*)ev + sizeof(LV2_Event), data, size);
  if (append) {
    ++iter->buf->event_count;
  }

  if (iter->offset + padded_size > iter->buf->size) {
    iter->buf->size = iter->offset + padded_size;
  }
  iter->offset += padded_size;
  iter->status = LV2_EVENT_SUCCESS;

  return true;
}

/** Reserve space for an event in the buffer and return a pointer to
    the memory where the caller can write the event data, or NULL if there
    is not enough room in the buffer. */
static inline uint8_t*
lv2_event_reserve(LV2_Event_Iterator* iter,
                  uint32_t            frames,
                  uint32_t            subframes,
                  uint16_t            type,
                  uint16_t            size)
{
  if (!iter || !iter->buf) {
    if (iter) {
      iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    }
    return NULL;
  }

  if (!lv2_event_buffer_is_valid(iter->buf)) {
    iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    return NULL;
  }

  if (iter->offset > iter->buf->size ||
      !lv2_event_offset_is_boundary(iter->buf, iter->offset) ||
      (iter->offset < iter->buf->size && !lv2_event_is_valid(iter))) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return NULL;
  }

  const bool     append     = iter->offset == iter->buf->size;
  const uint32_t total_size = (uint32_t)sizeof(LV2_Event) + size;
  const uint32_t padded_size = (total_size + 7U) & ~7U;
  if (iter->offset > iter->buf->capacity ||
      padded_size > iter->buf->capacity - iter->offset) {
    iter->status = LV2_EVENT_ERR_NO_SPACE;
    return NULL;
  }

  LV2_Event* const ev = (LV2_Event*)(iter->buf->data + iter->offset);

  ev->frames    = frames;
  ev->subframes = subframes;
  ev->type      = type;
  ev->size      = size;
  if (append) {
    ++iter->buf->event_count;
  }

  if (iter->offset + padded_size > iter->buf->size) {
    iter->buf->size = iter->offset + padded_size;
  }
  iter->offset += padded_size;
  iter->status = LV2_EVENT_SUCCESS;

  return (uint8_t*)ev + sizeof(LV2_Event);
}

/** Write an event at `iter`.
 * The event (if any) pointed to by `iter` will be overwritten, and `iter`
 * incremented to point to the following event (i.e. several calls to this
 * function can be done in sequence without twiddling iter in-between).
 * @return True if event was written, otherwise false (buffer is full). */
static inline bool
lv2_event_write_event(LV2_Event_Iterator* iter,
                      const LV2_Event*    ev,
                      const uint8_t*      data)
{
  if (!iter || !iter->buf || !ev || (ev->size && !data)) {
    if (iter) {
      iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    }
    return false;
  }

  if (!lv2_event_buffer_is_valid(iter->buf)) {
    iter->status = LV2_EVENT_ERR_INVALID_ARGUMENT;
    return false;
  }

  if (iter->offset > iter->buf->size ||
      !lv2_event_offset_is_boundary(iter->buf, iter->offset) ||
      (iter->offset < iter->buf->size && !lv2_event_is_valid(iter))) {
    iter->status = LV2_EVENT_ERR_INVALID_INDEX;
    return false;
  }

  const bool     append     = iter->offset == iter->buf->size;
  const uint32_t total_size = (uint32_t)sizeof(LV2_Event) + ev->size;
  const uint32_t padded_size = (total_size + 7U) & ~7U;
  if (iter->offset > iter->buf->capacity ||
      padded_size > iter->buf->capacity - iter->offset) {
    iter->status = LV2_EVENT_ERR_NO_SPACE;
    return false;
  }

  LV2_Event* const write_ev = (LV2_Event*)(iter->buf->data + iter->offset);

  *write_ev = *ev;
  memcpy((uint8_t*)write_ev + sizeof(LV2_Event), data, ev->size);
  if (append) {
    ++iter->buf->event_count;
  }

  if (iter->offset + padded_size > iter->buf->size) {
    iter->buf->size = iter->offset + padded_size;
  }
  iter->offset += padded_size;
  iter->status = LV2_EVENT_SUCCESS;

  return true;
}

LV2_RESTORE_WARNINGS

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // LV2_EVENT_EVENT_HELPERS_H
