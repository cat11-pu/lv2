// Copyright 2019 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "atom_test_utils.c"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/event/event-helpers.h>
#include <lv2/urid/urid.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
test_string_overflow(void)
{
#define MAX_CHARS 15

  static const size_t capacity = sizeof(LV2_Atom_String) + MAX_CHARS + 1;
  static const char*  str      = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

  uint8_t*       buf = (uint8_t*)malloc(capacity);
  LV2_URID_Map   map = {NULL, urid_map};
  LV2_Atom_Forge forge;
  lv2_atom_forge_init(&forge, &map);

  // Check that writing increasingly long strings fails at the right point
  for (unsigned count = 0; count < MAX_CHARS; ++count) {
    lv2_atom_forge_set_buffer(&forge, buf, capacity);

    const LV2_Atom_Forge_Ref ref = lv2_atom_forge_string(&forge, str, count);
    if (!ref) {
      return test_fail("Failed to write %u byte string\n", count);
    }
  }

  // Failure writing to an exactly full forge
  if (lv2_atom_forge_string(&forge, str, MAX_CHARS + 1)) {
    return test_fail("Successfully wrote past end of buffer\n");
  }

  // Failure writing body after successfully writing header
  lv2_atom_forge_set_buffer(&forge, buf, sizeof(LV2_Atom) + 1);
  if (lv2_atom_forge_string(&forge, "AB", 2)) {
    return test_fail("Successfully wrote atom header past end\n");
  }

  free(buf);
  return 0;
}

static int
test_literal_overflow(void)
{
  static const size_t capacity = sizeof(LV2_Atom_Literal) + 2;

  uint8_t*       buf = (uint8_t*)malloc(capacity);
  LV2_URID_Map   map = {NULL, urid_map};
  LV2_Atom_Forge forge;
  lv2_atom_forge_init(&forge, &map);

  // Failure in atom header
  lv2_atom_forge_set_buffer(&forge, buf, 1);
  if (lv2_atom_forge_literal(&forge, "A", 1, 0, 0)) {
    return test_fail("Successfully wrote atom header past end\n");
  }

  // Failure in literal header
  lv2_atom_forge_set_buffer(&forge, buf, sizeof(LV2_Atom) + 1);
  if (lv2_atom_forge_literal(&forge, "A", 1, 0, 0)) {
    return test_fail("Successfully wrote literal header past end\n");
  }

  // Success (only room for one character + null terminator)
  lv2_atom_forge_set_buffer(&forge, buf, capacity);
  if (!lv2_atom_forge_literal(&forge, "A", 1, 0, 0)) {
    return test_fail("Failed to write small enough literal\n");
  }

  // Failure in body
  lv2_atom_forge_set_buffer(&forge, buf, capacity);
  if (lv2_atom_forge_literal(&forge, "AB", 2, 0, 0)) {
    return test_fail("Successfully wrote literal body past end\n");
  }

  free(buf);
  return 0;
}

static int
test_sequence_overflow(void)
{
  static const size_t size = sizeof(LV2_Atom_Sequence) + (6 * sizeof(LV2_Atom));
  LV2_URID_Map        map  = {NULL, urid_map};

  // Test over a range that fails in the sequence header and event components
  for (size_t capacity = 1; capacity < size; ++capacity) {
    uint8_t* buf = (uint8_t*)malloc(capacity);

    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, buf, capacity);

    LV2_Atom_Forge_Frame frame;
    LV2_Atom_Forge_Ref   ref = lv2_atom_forge_sequence_head(&forge, &frame, 0);

    assert(capacity >= sizeof(LV2_Atom_Sequence) || !frame.ref);
    assert(capacity >= sizeof(LV2_Atom_Sequence) || !ref);
    (void)ref;

    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_int(&forge, 42);
    lv2_atom_forge_pop(&forge, &frame);

    free(buf);
  }

  return 0;
}

static int
test_vector_head_overflow(void)
{
  static const size_t size = sizeof(LV2_Atom_Vector) + (3 * sizeof(LV2_Atom));
  LV2_URID_Map        map  = {NULL, urid_map};

  // Test over a range that fails in the vector header and elements
  for (size_t capacity = 1; capacity < size; ++capacity) {
    uint8_t* buf = (uint8_t*)malloc(capacity);

    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, buf, capacity);

    LV2_Atom_Forge_Frame frame;
    LV2_Atom_Forge_Ref   ref =
      lv2_atom_forge_vector_head(&forge, &frame, sizeof(int32_t), forge.Int);

    assert(capacity >= sizeof(LV2_Atom_Vector) || !frame.ref);
    assert(capacity >= sizeof(LV2_Atom_Vector) || !ref);
    (void)ref;

    lv2_atom_forge_int(&forge, 1);
    lv2_atom_forge_int(&forge, 2);
    lv2_atom_forge_int(&forge, 3);
    lv2_atom_forge_pop(&forge, &frame);

    free(buf);
  }

  return 0;
}

static int
test_vector_overflow(void)
{
  static const size_t  size  = sizeof(LV2_Atom_Vector) + (3 * sizeof(LV2_Atom));
  static const int32_t vec[] = {1, 2, 3};
  LV2_URID_Map         map   = {NULL, urid_map};

  // Test over a range that fails in the vector header and elements
  for (size_t capacity = 1; capacity < size; ++capacity) {
    uint8_t* buf = (uint8_t*)malloc(capacity);

    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, buf, capacity);

    LV2_Atom_Forge_Ref ref =
      lv2_atom_forge_vector(&forge, sizeof(int32_t), forge.Int, 3, vec);

    assert(capacity >= sizeof(LV2_Atom_Vector) || !ref);
    (void)ref;

    free(buf);
  }

  return 0;
}

static int
test_tuple_overflow(void)
{
  static const size_t size = sizeof(LV2_Atom_Tuple) + (3 * sizeof(LV2_Atom));
  LV2_URID_Map        map  = {NULL, urid_map};

  // Test over a range that fails in the tuple header and elements
  for (size_t capacity = 1; capacity < size; ++capacity) {
    uint8_t* buf = (uint8_t*)malloc(capacity);

    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, buf, capacity);

    LV2_Atom_Forge_Frame frame;
    LV2_Atom_Forge_Ref   ref = lv2_atom_forge_tuple(&forge, &frame);

    assert(capacity >= sizeof(LV2_Atom_Tuple) || !frame.ref);
    assert(capacity >= sizeof(LV2_Atom_Tuple) || !ref);
    (void)ref;

    lv2_atom_forge_int(&forge, 1);
    lv2_atom_forge_float(&forge, 2.0f);
    lv2_atom_forge_string(&forge, "three", 5);
    lv2_atom_forge_pop(&forge, &frame);

    free(buf);
  }

  return 0;
}

static int
test_missing_resource_id(void)
{
  uint8_t        buf[256];
  LV2_URID_Map   map = {NULL, urid_map};
  LV2_Atom_Forge forge;

  lv2_atom_forge_init(NULL, &map);
  lv2_atom_forge_init(&forge, NULL);
  if (lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT ||
      !lv2_atom_forge_status_string(LV2_ATOM_FORGE_ERR_INVALID_RESOURCE)) {
    return test_fail("Invalid forge initialisation did not report an error\n");
  }

  lv2_atom_forge_init(&forge, &map);
  lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));

  const uint32_t first = forge.offset;
  if (lv2_atom_forge_key(&forge, 0) ||
      lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_RESOURCE) {
    return test_fail("Accepted a property key with a missing URID\n");
  }

  if (lv2_atom_forge_urid(&forge, 0) ||
      lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_RESOURCE) {
    return test_fail("Accepted a URID atom with a missing URID\n");
  }

  LV2_Atom_Forge_Frame frame;
  if (lv2_atom_forge_object(&forge, &frame, 0, 0) ||
      lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_RESOURCE) {
    return test_fail("Accepted an object with a missing type URID\n");
  }

  if (forge.offset != first) {
    return test_fail("Rejected writes modified the output position\n");
  }

  if (!lv2_atom_forge_object(&forge, &frame, 1, 1) ||
      !lv2_atom_forge_pop_checked(&forge, &frame)) {
    return test_fail("Failed to build a valid object after rejecting input\n");
  }

  return 0;
}

static int
test_lifecycle_recovery(void)
{
  uint8_t        buf[256];
  LV2_URID_Map   map = {NULL, urid_map};
  LV2_Atom_Forge forge;
  lv2_atom_forge_init(&forge, &map);
  lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));

  LV2_Atom_Forge_Frame outer;
  LV2_Atom_Forge_Frame inner;
  if (!lv2_atom_forge_object(&forge, &outer, 1, 1) ||
      !lv2_atom_forge_object(&forge, &inner, 2, 1)) {
    return test_fail("Failed to push nested objects\n");
  }

  if (lv2_atom_forge_pop_checked(&forge, &outer) ||
      lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE ||
      forge.stack != &inner) {
    return test_fail("Out-of-order pop did not fail cleanly\n");
  }

  if (!lv2_atom_forge_pop_checked(&forge, &inner) ||
      !lv2_atom_forge_pop_checked(&forge, &outer) ||
      forge.stack != NULL ||
      !lv2_atom_forge_ok(&forge)) {
    return test_fail("Failed to recover by popping the correct frames\n");
  }

  if (!lv2_atom_forge_object(&forge, &outer, 3, 1)) {
    return test_fail("Failed to start an object for recovery test\n");
  }

  lv2_atom_forge_recover(&forge);
  if (forge.stack || !lv2_atom_forge_ok(&forge)) {
    return test_fail("Forge recovery left an invalid stack or status\n");
  }

  return 0;
}

static int
test_index_bounds(void)
{
  uint8_t        buf[256];
  LV2_URID_Map   map = {NULL, urid_map};
  LV2_Atom_Forge forge;
  lv2_atom_forge_init(&forge, &map);

  lv2_atom_forge_set_buffer(&forge, NULL, 0);
  if (lv2_atom_forge_int(&forge, 1) ||
      lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT) {
    return test_fail("Accepted a write through a null buffer\n");
  }

  lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));
  if (lv2_atom_forge_deref(&forge, (LV2_Atom_Forge_Ref)(uintptr_t)1) ||
      lv2_atom_forge_get_status(&forge) !=
        LV2_ATOM_FORGE_ERR_INVALID_INDEX) {
    return test_fail("Accepted an out-of-range forge reference\n");
  }

#if SIZE_MAX > UINT32_MAX
  lv2_atom_forge_set_buffer(&forge, buf, (size_t)UINT32_MAX + 1U);
  if (lv2_atom_forge_get_status(&forge) !=
      LV2_ATOM_FORGE_ERR_INVALID_INDEX) {
    return test_fail("Accepted a buffer size that cannot be indexed\n");
  }
#endif

  lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));
  if (!lv2_atom_forge_int(&forge, 1) || !lv2_atom_forge_ok(&forge)) {
    return test_fail("Failed to write after recovering the forge\n");
  }

  return 0;
}

static int
test_sequence_bounds(void)
{
  uint8_t            storage[sizeof(LV2_Atom_Sequence) + 128];
  LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)storage;
  struct {
    LV2_Atom_Event event;
    uint8_t        body[4];
  } packet;
  LV2_Atom_Event* ev = &packet.event;
  memset(storage, 0, sizeof(storage));
  memset(&packet, 0, sizeof(packet));
  ev->body.size = sizeof(packet.body);

  seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
  if (!lv2_atom_sequence_append_event(seq, sizeof(storage), ev)) {
    return test_fail("Failed to append a valid event\n");
  }

  const uint32_t valid_size = seq->atom.size;
  seq->atom.size            = sizeof(storage) + 1U;
  if (lv2_atom_sequence_append_event(seq, sizeof(storage), ev) ||
      seq->atom.size != sizeof(storage) + 1U) {
    return test_fail("Accepted an event sequence index past capacity\n");
  }

  seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
  ev->body.size  = UINT32_MAX;
  if (lv2_atom_sequence_append_event(seq, sizeof(storage), ev)) {
    return test_fail("Accepted an event size that overflows\n");
  }

  lv2_atom_sequence_clear(seq);
  ev->body.size = sizeof(packet.body);
  if (!lv2_atom_sequence_append_event(seq, sizeof(storage), ev) ||
      seq->atom.size != valid_size) {
    return test_fail("Failed to recover the sequence after an error\n");
  }

  return 0;
}

LV2_DISABLE_DEPRECATION_WARNINGS
static int
test_event_bounds(void)
{
  uint8_t           storage[sizeof(LV2_Event_Buffer) + 64];
  LV2_Event_Buffer* buf      = (LV2_Event_Buffer*)storage;
  uint8_t           data[4]  = {1, 2, 3, 4};
  LV2_Event_Iterator iter;

  lv2_event_buffer_reset(buf, 0, storage + sizeof(*buf));
  buf->capacity = sizeof(storage) - sizeof(*buf);

  if (lv2_event_begin(&iter, buf) ||
      iter.status != LV2_EVENT_SUCCESS) {
    return test_fail("Non-empty result from an empty event buffer\n");
  }

  if (!lv2_event_write(&iter, 0, 0, 1, sizeof(data), data)) {
    return test_fail("Failed to write a valid event\n");
  }

  if (!lv2_event_begin(&iter, buf) ||
      !lv2_event_get(&iter, NULL) ||
      iter.status != LV2_EVENT_SUCCESS) {
    return test_fail("Failed to read back a valid event\n");
  }

  const uint32_t valid_size = buf->size;
  iter.offset               = 4;
  if (lv2_event_reserve(&iter, 0, 0, 1, sizeof(data)) ||
      iter.status != LV2_EVENT_ERR_INVALID_INDEX ||
      buf->size != valid_size) {
    return test_fail("Accepted an event offset inside an existing event\n");
  }

  iter.offset = valid_size + 1U;
  if (lv2_event_reserve(&iter, 0, 0, 1, sizeof(data)) ||
      iter.status != LV2_EVENT_ERR_INVALID_INDEX ||
      buf->size != valid_size) {
    return test_fail("Accepted an event offset past the buffer end\n");
  }

  iter.offset   = buf->size;
  buf->capacity = buf->size;
  if (lv2_event_write(&iter, 0, 0, 1, sizeof(data), data) ||
      iter.status != LV2_EVENT_ERR_NO_SPACE ||
      buf->size != valid_size) {
    return test_fail("Accepted an event that does not fit\n");
  }

  lv2_event_buffer_reset(buf, 0, storage + sizeof(*buf));
  buf->capacity = sizeof(storage) - sizeof(*buf);
  lv2_event_begin(&iter, buf);
  if (iter.status != LV2_EVENT_SUCCESS || iter.offset ||
      !lv2_event_write(&iter, 0, 0, 1, sizeof(data), data)) {
    return test_fail("Failed to recover the event buffer after an error\n");
  }

  return 0;
}
LV2_RESTORE_WARNINGS

int
main(void)
{
  const int ret = test_string_overflow() || test_literal_overflow() ||
                  test_sequence_overflow() || test_vector_head_overflow() ||
                  test_vector_overflow() || test_tuple_overflow() ||
                  test_missing_resource_id() || test_lifecycle_recovery() ||
                  test_index_bounds() || test_sequence_bounds() ||
                  test_event_bounds();

  free_urid_map();

  return ret;
}
