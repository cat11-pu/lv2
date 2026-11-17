// Copyright 2026 LV2 contributors
// SPDX-License-Identifier: ISC

#define _POSIX_C_SOURCE 200809L

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/hotswap/hotswap.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

#define TEST_URI        "urn:test:hotswap"
#define OTHER_URI       "urn:test:other"
#define REFERENCE_RATE  48000U
#define BLOCK_FRAMES    256U
#define SWITCH_COUNT    1000U
#define EVENT_COUNT     2000U
#define EVENT_INTERVAL  128U
#define MAX_STATE_SIZE  256U
#define MAX_RETIRED     8U
#define SEQ_BUFFER_SIZE 4096U

#define URID_SEQUENCE 1U
#define URID_INT      2U

#define TYPE_AUDIO   100U
#define TYPE_CONTROL 101U
#define TYPE_ATOM    102U

#define TEST_PI 3.14159265358979323846264338327950288

typedef enum {
  ROLE_IN_L,
  ROLE_IN_R,
  ROLE_OUT_L,
  ROLE_OUT_R,
  ROLE_GAIN,
  ROLE_EVENTS_IN,
  ROLE_EVENTS_OUT,
  ROLE_COUNT
} Role;

static const uint32_t layouts[2][ROLE_COUNT] = {
  {0U, 1U, 2U, 3U, 4U, 5U, 6U},
  {5U, 3U, 4U, 0U, 2U, 1U, 6U},
};

typedef union {
  size_t size;
  double align_double;
  void*  align_pointer;
} AllocHeader;

static size_t n_allocations = 0U;
static size_t n_frees       = 0U;
static size_t live_bytes    = 0U;

static void*
test_malloc(size_t size)
{
  AllocHeader* header = (AllocHeader*)malloc(sizeof(AllocHeader) + size);
  if (!header) {
    return NULL;
  }

  header->size = size;
  ++n_allocations;
  live_bytes += size;
  return header + 1;
}

static void
test_free(void* pointer)
{
  if (!pointer) {
    return;
  }

  AllocHeader* header = (AllocHeader*)pointer - 1;
  live_bytes -= header->size;
  ++n_frees;
  free(header);
}

static uint64_t
now_ns(void)
{
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  LARGE_INTEGER        counter;
  if (frequency.QuadPart == 0) {
    QueryPerformanceFrequency(&frequency);
  }
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000000) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000U) + (uint64_t)ts.tv_nsec;
#endif
}

typedef struct {
  uint64_t host_frame;
  uint32_t id;
} QueuedEvent;

typedef struct {
  int64_t  time;
  uint32_t size;
  uint32_t type;
  int32_t  value;
} IntEvent;

typedef struct {
  uint32_t version;
  uint32_t size;
  double   phase;
  double   gain;
  uint8_t  reserved[192];
} FakeState;

typedef struct {
  double sample_rate;
  double phase;
  double gain;
  void*  ports[ROLE_COUNT];
  uint32_t kind;
  uint32_t state_version;
} FakePlugin;

typedef struct {
  LV2_Descriptor descriptor;
  uint32_t       kind;
} TestDescriptor;

static uint32_t
fake_state_size(LV2_Handle instance)
{
  (void)instance;
  return (uint32_t)sizeof(FakeState);
}

static LV2_Hotswap_Status
fake_save_state(LV2_Handle instance, void* state, uint32_t size)
{
  FakePlugin* plugin = (FakePlugin*)instance;
  if (!plugin || !state || size < sizeof(FakeState)) {
    return LV2_HOTSWAP_ERR_BAD_STATE;
  }

  FakeState snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  snapshot.version = plugin->state_version;
  snapshot.size    = (uint32_t)sizeof(FakeState);
  snapshot.phase   = plugin->phase;
  snapshot.gain    = plugin->gain;
  memcpy(state, &snapshot, sizeof(snapshot));
  return LV2_HOTSWAP_SUCCESS;
}

static LV2_Hotswap_Status
fake_restore_state(LV2_Handle instance, const void* state, uint32_t size)
{
  FakePlugin* plugin = (FakePlugin*)instance;
  if (!plugin || !state || size < sizeof(FakeState)) {
    return LV2_HOTSWAP_ERR_BAD_STATE;
  }

  FakeState snapshot;
  memcpy(&snapshot, state, sizeof(snapshot));
  if (snapshot.version != plugin->state_version ||
      snapshot.size != sizeof(FakeState)) {
    return LV2_HOTSWAP_ERR_BAD_STATE;
  }

  plugin->phase = snapshot.phase;
  plugin->gain  = snapshot.gain;
  return LV2_HOTSWAP_SUCCESS;
}

static const LV2_Hotswap_Plugin_Interface migration_v1 = {
  1U,
  fake_state_size,
  fake_save_state,
  fake_restore_state,
};

static const LV2_Hotswap_Plugin_Interface migration_v2 = {
  2U,
  fake_state_size,
  fake_save_state,
  fake_restore_state,
};

static const void*
extension_data_v1(const char* uri)
{
  return strcmp(uri, LV2_HOTSWAP__interface) ? NULL : &migration_v1;
}

static const void*
extension_data_v2(const char* uri)
{
  return strcmp(uri, LV2_HOTSWAP__interface) ? NULL : &migration_v2;
}

static const void*
extension_data_none(const char* uri)
{
  (void)uri;
  return NULL;
}

static LV2_Handle
fake_instantiate(const LV2_Descriptor* descriptor,
                 double                sample_rate,
                 const char*           bundle_path,
                 const LV2_Feature* const* features)
{
  (void)bundle_path;
  (void)features;

  const TestDescriptor* test_descriptor = (const TestDescriptor*)descriptor;
  FakePlugin* plugin =
    (FakePlugin*)test_malloc(sizeof(*plugin));
  if (!plugin) {
    return NULL;
  }

  memset(plugin, 0, sizeof(*plugin));
  plugin->sample_rate   = sample_rate;
  plugin->gain          = 1.0;
  plugin->kind          = test_descriptor->kind;
  plugin->state_version = test_descriptor->kind == 2U ? 2U : 1U;
  return plugin;
}

static void
fake_connect_port(LV2_Handle instance, uint32_t port, void* data)
{
  FakePlugin* plugin = (FakePlugin*)instance;
  const uint32_t kind = plugin->kind;
  for (uint32_t role = 0U; role < ROLE_COUNT; ++role) {
    if (layouts[kind][role] == port) {
      plugin->ports[role] = data;
      return;
    }
  }
}

static void
fake_activate(LV2_Handle instance)
{
  (void)instance;
}

static void
fake_run(LV2_Handle instance, uint32_t sample_count)
{
  FakePlugin* plugin = (FakePlugin*)instance;
  if (!plugin) {
    return;
  }

  LV2_Atom_Sequence* input =
    (LV2_Atom_Sequence*)plugin->ports[ROLE_EVENTS_IN];
  LV2_Atom_Sequence* output =
    (LV2_Atom_Sequence*)plugin->ports[ROLE_EVENTS_OUT];
  if (output) {
    const uint32_t capacity = output->atom.size;
    output->atom.type       = URID_SEQUENCE;
    output->atom.size       = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
    output->body.unit       = 0U;
    output->body.pad        = 0U;

    if (input) {
      LV2_ATOM_SEQUENCE_FOREACH(input, event)
      {
        if (event->body.type == URID_INT &&
            event->body.size >= sizeof(int32_t)) {
          IntEvent copy;
          memset(&copy, 0, sizeof(copy));
          copy.time = event->time.frames;
          copy.size = (uint32_t)sizeof(int32_t);
          copy.type = URID_INT;
          memcpy(&copy.value,
                 LV2_ATOM_BODY_CONST(&event->body),
                 sizeof(int32_t));
          lv2_atom_sequence_append_event(
            output, capacity, (const LV2_Atom_Event*)&copy);
        }
      }
    }
  }

  float*       out_l = (float*)plugin->ports[ROLE_OUT_L];
  float*       out_r = (float*)plugin->ports[ROLE_OUT_R];
  const float* in_l  = (const float*)plugin->ports[ROLE_IN_L];
  const float* in_r  = (const float*)plugin->ports[ROLE_IN_R];
  const double rate  = plugin->sample_rate;
  const double gain  = plugin->gain;

  for (uint32_t i = 0U; i < sample_count; ++i) {
    const double signal =
      0.25 * sin(plugin->phase * 2.0 * TEST_PI) * gain;
    out_l[i] = in_l[i] + (float)signal;
    out_r[i] = in_r[i] - (float)signal;

    plugin->phase += 440.0 / rate;
    if (plugin->phase >= 1.0) {
      plugin->phase -= 1.0;
    }
  }
}

static void
fake_deactivate(LV2_Handle instance)
{
  (void)instance;
}

static void
fake_cleanup(LV2_Handle instance)
{
  test_free(instance);
}

static TestDescriptor descriptor_old = {
  {TEST_URI,
   fake_instantiate,
   fake_connect_port,
   fake_activate,
   fake_run,
   fake_deactivate,
   fake_cleanup,
   extension_data_v1},
  0U,
};

static TestDescriptor descriptor_new = {
  {TEST_URI,
   fake_instantiate,
   fake_connect_port,
   fake_activate,
   fake_run,
   fake_deactivate,
   fake_cleanup,
   extension_data_v1},
  1U,
};

static TestDescriptor descriptor_bad_state = {
  {TEST_URI,
   fake_instantiate,
   fake_connect_port,
   fake_activate,
   fake_run,
   fake_deactivate,
   fake_cleanup,
   extension_data_v2},
  1U,
};

static TestDescriptor descriptor_other = {
  {OTHER_URI,
   fake_instantiate,
   fake_connect_port,
   fake_activate,
   fake_run,
   fake_deactivate,
   fake_cleanup,
   extension_data_v1},
  1U,
};

static TestDescriptor descriptor_stateless = {
  {TEST_URI,
   fake_instantiate,
   fake_connect_port,
   fake_activate,
   fake_run,
   fake_deactivate,
   fake_cleanup,
   extension_data_none},
  1U,
};

typedef struct {
  const char* symbol;
  uint32_t    type;
  uint32_t    flags;
  void*       data;
} Binding;

typedef struct {
  const LV2_Descriptor*                 descriptor;
  LV2_Handle                            handle;
  const LV2_Hotswap_Plugin_Interface*   migration;
  double                                rate;
  uint32_t                              kind;
  uint32_t                              state_size;
  void*                                 state;
  bool                                  active;
} Slot;

typedef struct Host {
  Slot                    current;
  Slot                    pending;
  bool                    has_pending;
  Slot                    retired[MAX_RETIRED];
  uint32_t                n_retired;
  Binding                 bindings[ROLE_COUNT];
  const LV2_Feature*      features[2];
  LV2_Hotswap_Host_Interface interface;
  uint64_t                host_frame;
  uint32_t                reference_rate;
  float                   in_l[BLOCK_FRAMES];
  float                   in_r[BLOCK_FRAMES];
  float                   out_l[BLOCK_FRAMES];
  float                   out_r[BLOCK_FRAMES];
  float                   gain;
  uint8_t                 seq_in_buffer[SEQ_BUFFER_SIZE];
  uint8_t                 seq_out_buffer[SEQ_BUFFER_SIZE];
  QueuedEvent             events[EVENT_COUNT];
  uint32_t                n_events;
  uint32_t                event_cursor;
  uint32_t                next_event_id;
  uint64_t                events_received;
  double                  expected_phase;
  bool                    have_last;
  float                   last_out_l;
  uint64_t                total_frames;
  double                  max_error;
  double                  max_jump;
  double                  max_boundary_jump;
  uint64_t                max_run_ns;
  uint64_t                last_run_ns;
} Host;

typedef struct {
  LV2_Hotswap_Target target;
  LV2_Hotswap_Port   ports[ROLE_COUNT];
} TargetBuffer;

static void
host_reap(Host* host);

static LV2_Hotswap_Status
host_prepare(void* handle, const LV2_Hotswap_Target* target)
{
  Host* host = (Host*)handle;
  if (!host || !target) {
    return LV2_HOTSWAP_ERR_BAD_ARGUMENT;
  }

  if (host->has_pending) {
    return LV2_HOTSWAP_ERR_BUSY;
  }

  if (host->n_retired >= MAX_RETIRED) {
    return LV2_HOTSWAP_ERR_BUSY;
  }

  if (!target->descriptor || !target->features ||
      !(target->sample_rate > 0.0) || target->sample_rate > 768000.0) {
    return LV2_HOTSWAP_ERR_BAD_ARGUMENT;
  }

  if (strcmp(target->descriptor->URI, host->current.descriptor->URI)) {
    return LV2_HOTSWAP_ERR_BAD_PLUGIN;
  }

  if (!target->ports || target->num_ports == 0U ||
      target->num_ports > ROLE_COUNT) {
    return LV2_HOTSWAP_ERR_BAD_PORT;
  }

  const Binding* matched[ROLE_COUNT];
  bool           used_binding[ROLE_COUNT]   = {false};
  bool           used_index[ROLE_COUNT]     = {false};
  bool           used_old_index[ROLE_COUNT] = {false};

  for (uint32_t i = 0U; i < target->num_ports; ++i) {
    const LV2_Hotswap_Port* port = &target->ports[i];
    if (!port->symbol) {
      return LV2_HOTSWAP_ERR_BAD_PORT;
    }

    uint32_t binding_index = ROLE_COUNT;
    for (uint32_t j = 0U; j < ROLE_COUNT; ++j) {
      if (!strcmp(port->symbol, host->bindings[j].symbol)) {
        binding_index = j;
        break;
      }
    }

    if (binding_index == ROLE_COUNT || used_binding[binding_index] ||
        port->old_index >= ROLE_COUNT || port->new_index >= ROLE_COUNT ||
        used_index[port->new_index] || used_old_index[port->old_index]) {
      return LV2_HOTSWAP_ERR_BAD_PORT;
    }

    const Binding* binding = &host->bindings[binding_index];
    if (port->old_type && port->new_type &&
        port->old_type != port->new_type) {
      return LV2_HOTSWAP_ERR_BAD_PORT;
    }

    if ((port->old_type && port->old_type != binding->type) ||
        (port->new_type && port->new_type != binding->type)) {
      return LV2_HOTSWAP_ERR_BAD_PORT;
    }

    matched[binding_index]          = binding;
    used_binding[binding_index]     = true;
    used_index[port->new_index]     = true;
    used_old_index[port->old_index] = true;
  }

  for (uint32_t i = 0U; i < ROLE_COUNT; ++i) {
    if (!used_binding[i]) {
      return LV2_HOTSWAP_ERR_BAD_PORT;
    }
  }

  Slot candidate;
  memset(&candidate, 0, sizeof(candidate));
  candidate.descriptor = target->descriptor;
  candidate.rate       = target->sample_rate;
  candidate.kind       = ((const TestDescriptor*)target->descriptor)->kind;

  candidate.handle = target->descriptor->instantiate(
    target->descriptor,
    target->sample_rate,
    target->bundle_path ? target->bundle_path : "",
    target->features);
  if (!candidate.handle) {
    return LV2_HOTSWAP_ERR_NO_MEMORY;
  }

  if (target->descriptor->extension_data) {
    candidate.migration = (const LV2_Hotswap_Plugin_Interface*)
      target->descriptor->extension_data(LV2_HOTSWAP__interface);
  }

  const bool require_state =
    (target->flags & LV2_HOTSWAP_F_REQUIRE_STATE) != 0U;
  if (require_state && (!host->current.migration || !candidate.migration)) {
    if (target->descriptor->cleanup) {
      target->descriptor->cleanup(candidate.handle);
    }
    return LV2_HOTSWAP_ERR_UNSUPPORTED;
  }

  if (host->current.migration && candidate.migration) {
    if (host->current.migration->state_version !=
          candidate.migration->state_version) {
      if (target->descriptor->cleanup) {
        target->descriptor->cleanup(candidate.handle);
      }
      return LV2_HOTSWAP_ERR_BAD_STATE;
    }

    if (!candidate.migration->state_size ||
        !host->current.migration->state_size) {
      if (target->descriptor->cleanup) {
        target->descriptor->cleanup(candidate.handle);
      }
      return LV2_HOTSWAP_ERR_BAD_STATE;
    }

    const uint32_t old_size =
      host->current.migration->state_size(host->current.handle);
    const uint32_t new_size =
      candidate.migration->state_size(candidate.handle);
    if (old_size != new_size ||
        (target->max_state_size && new_size > target->max_state_size)) {
      if (target->descriptor->cleanup) {
        target->descriptor->cleanup(candidate.handle);
      }
      return LV2_HOTSWAP_ERR_BAD_STATE;
    }

    candidate.state_size = new_size;
    if (new_size > 0U) {
      candidate.state = test_malloc(new_size);
      if (!candidate.state) {
        if (target->descriptor->cleanup) {
          target->descriptor->cleanup(candidate.handle);
        }
        return LV2_HOTSWAP_ERR_NO_MEMORY;
      }
    }
  }

  for (uint32_t i = 0U; i < target->num_ports; ++i) {
    const LV2_Hotswap_Port* port = &target->ports[i];
    uint32_t binding_index = ROLE_COUNT;
    for (uint32_t j = 0U; j < ROLE_COUNT; ++j) {
      if (!strcmp(port->symbol, host->bindings[j].symbol)) {
        binding_index = j;
        break;
      }
    }
    target->descriptor->connect_port(
      candidate.handle, port->new_index, matched[binding_index]->data);
  }

  if (target->descriptor->activate) {
    target->descriptor->activate(candidate.handle);
  }
  candidate.active = true;

  host->pending     = candidate;
  host->has_pending = true;
  return LV2_HOTSWAP_SUCCESS;
}

static LV2_Hotswap_Status
host_commit(void* handle, const LV2_Hotswap_Position* position)
{
  Host* host = (Host*)handle;
  if (!host || !position || position->rate == 0U) {
    return LV2_HOTSWAP_ERR_BAD_ARGUMENT;
  }

  if (!host->has_pending) {
    return LV2_HOTSWAP_ERR_NOT_PREPARED;
  }

  if (position->rate != host->reference_rate ||
      position->frame < host->host_frame) {
    return LV2_HOTSWAP_ERR_BAD_ARGUMENT;
  }

  if (host->current.migration && host->pending.migration &&
      host->pending.state_size > 0U) {
    LV2_Hotswap_Status status = host->current.migration->save_state(
      host->current.handle, host->pending.state, host->pending.state_size);
    if (status != LV2_HOTSWAP_SUCCESS) {
      return status;
    }

    status = host->pending.migration->restore_state(
      host->pending.handle, host->pending.state, host->pending.state_size);
    if (status != LV2_HOTSWAP_SUCCESS) {
      return status;
    }
  }

  const Slot old = host->current;
  host->current = host->pending;
  memset(&host->pending, 0, sizeof(host->pending));
  host->has_pending = false;
  host->host_frame  = position->frame;
  host->retired[host->n_retired++] = old;
  return LV2_HOTSWAP_SUCCESS;
}

static void
host_abort(void* handle)
{
  Host* host = (Host*)handle;
  if (!host) {
    return;
  }

  if (host->has_pending) {
    if (host->pending.active && host->pending.descriptor->deactivate) {
      host->pending.descriptor->deactivate(host->pending.handle);
    }
    if (host->pending.descriptor->cleanup) {
      host->pending.descriptor->cleanup(host->pending.handle);
    }
    test_free(host->pending.state);
    memset(&host->pending, 0, sizeof(host->pending));
    host->has_pending = false;
  }

  host_reap(host);
}

static void
host_reap(Host* host)
{
  for (uint32_t i = 0U; i < host->n_retired; ++i) {
    Slot* slot = &host->retired[i];
    if (slot->active && slot->descriptor->deactivate) {
      slot->descriptor->deactivate(slot->handle);
    }
    if (slot->descriptor->cleanup) {
      slot->descriptor->cleanup(slot->handle);
    }
    test_free(slot->state);
  }
  memset(host->retired, 0, sizeof(host->retired));
  host->n_retired = 0U;
}

static int
host_init(Host* host, const TestDescriptor* descriptor, double sample_rate)
{
  memset(host, 0, sizeof(*host));

  host->features[0] = NULL;
  host->features[1] = NULL;
  host->interface.handle  = host;
  host->interface.prepare = host_prepare;
  host->interface.commit  = host_commit;
  host->interface.abort   = host_abort;

  host->host_frame     = 0U;
  host->reference_rate = REFERENCE_RATE;
  host->gain           = 1.0f;
  host->next_event_id  = 1U;

  host->bindings[ROLE_IN_L].symbol      = "in_l";
  host->bindings[ROLE_IN_L].type        = TYPE_AUDIO;
  host->bindings[ROLE_IN_L].flags       = LV2_HOTSWAP_PORT_INPUT;
  host->bindings[ROLE_IN_L].data        = host->in_l;
  host->bindings[ROLE_IN_R].symbol      = "in_r";
  host->bindings[ROLE_IN_R].type        = TYPE_AUDIO;
  host->bindings[ROLE_IN_R].flags       = LV2_HOTSWAP_PORT_INPUT;
  host->bindings[ROLE_IN_R].data        = host->in_r;
  host->bindings[ROLE_OUT_L].symbol     = "out_l";
  host->bindings[ROLE_OUT_L].type       = TYPE_AUDIO;
  host->bindings[ROLE_OUT_L].flags      = LV2_HOTSWAP_PORT_OUTPUT;
  host->bindings[ROLE_OUT_L].data       = host->out_l;
  host->bindings[ROLE_OUT_R].symbol     = "out_r";
  host->bindings[ROLE_OUT_R].type       = TYPE_AUDIO;
  host->bindings[ROLE_OUT_R].flags      = LV2_HOTSWAP_PORT_OUTPUT;
  host->bindings[ROLE_OUT_R].data       = host->out_r;
  host->bindings[ROLE_GAIN].symbol      = "gain";
  host->bindings[ROLE_GAIN].type        = TYPE_CONTROL;
  host->bindings[ROLE_GAIN].flags       = LV2_HOTSWAP_PORT_INPUT;
  host->bindings[ROLE_GAIN].data        = &host->gain;
  host->bindings[ROLE_EVENTS_IN].symbol = "events_in";
  host->bindings[ROLE_EVENTS_IN].type   = TYPE_ATOM;
  host->bindings[ROLE_EVENTS_IN].flags  = LV2_HOTSWAP_PORT_INPUT;
  host->bindings[ROLE_EVENTS_IN].data   = host->seq_in_buffer;
  host->bindings[ROLE_EVENTS_OUT].symbol = "events_out";
  host->bindings[ROLE_EVENTS_OUT].type   = TYPE_ATOM;
  host->bindings[ROLE_EVENTS_OUT].flags  = LV2_HOTSWAP_PORT_OUTPUT;
  host->bindings[ROLE_EVENTS_OUT].data   = host->seq_out_buffer;

  for (uint32_t i = 0U; i < EVENT_COUNT; ++i) {
    host->events[i].host_frame = (uint64_t)i * EVENT_INTERVAL;
    host->events[i].id         = i + 1U;
  }
  host->n_events = EVENT_COUNT;

  host->current.descriptor = &descriptor->descriptor;
  host->current.rate       = sample_rate;
  host->current.kind       = descriptor->kind;
  host->current.handle     = descriptor->descriptor.instantiate(
    &descriptor->descriptor, sample_rate, "", host->features);
  if (!host->current.handle) {
    return 1;
  }

  if (descriptor->descriptor.extension_data) {
    host->current.migration = (const LV2_Hotswap_Plugin_Interface*)
      descriptor->descriptor.extension_data(LV2_HOTSWAP__interface);
  }

  for (uint32_t role = 0U; role < ROLE_COUNT; ++role) {
    descriptor->descriptor.connect_port(
      host->current.handle,
      layouts[descriptor->kind][role],
      host->bindings[role].data);
  }

  if (descriptor->descriptor.activate) {
    descriptor->descriptor.activate(host->current.handle);
  }
  host->current.active = true;
  return 0;
}

static void
host_destroy(Host* host)
{
  host_abort(host);
  if (host->current.active && host->current.descriptor->deactivate) {
    host->current.descriptor->deactivate(host->current.handle);
  }
  if (host->current.descriptor && host->current.descriptor->cleanup) {
    host->current.descriptor->cleanup(host->current.handle);
  }
  test_free(host->current.state);
  memset(host, 0, sizeof(*host));
}

static void
make_target(Host* host, const TestDescriptor* descriptor, double rate,
            uint32_t flags, TargetBuffer* target_buffer)
{
  memset(target_buffer, 0, sizeof(*target_buffer));
  target_buffer->target.descriptor      = &descriptor->descriptor;
  target_buffer->target.bundle_path     = "";
  target_buffer->target.features        = host->features;
  target_buffer->target.sample_rate     = rate;
  target_buffer->target.ports           = target_buffer->ports;
  target_buffer->target.num_ports       = ROLE_COUNT;
  target_buffer->target.max_state_size  = MAX_STATE_SIZE;
  target_buffer->target.flags           = flags;

  for (uint32_t role = 0U; role < ROLE_COUNT; ++role) {
    target_buffer->ports[role].symbol =
      host->bindings[role].symbol;
    target_buffer->ports[role].old_index =
      layouts[host->current.kind][role];
    target_buffer->ports[role].new_index =
      layouts[descriptor->kind][role];
    target_buffer->ports[role].old_type =
      host->bindings[role].type;
    target_buffer->ports[role].new_type =
      host->bindings[role].type;
    target_buffer->ports[role].flags =
      host->bindings[role].flags;
  }
}

static void
build_input_sequence(Host* host, uint32_t frames)
{
  LV2_Atom_Sequence* input =
    (LV2_Atom_Sequence*)host->seq_in_buffer;
  const uint32_t capacity =
    (uint32_t)(SEQ_BUFFER_SIZE - sizeof(LV2_Atom));

  input->atom.type = URID_SEQUENCE;
  input->atom.size = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
  input->body.unit = 0U;
  input->body.pad  = 0U;

  const uint64_t end = host->host_frame + BLOCK_FRAMES;
  while (host->event_cursor < host->n_events &&
         host->events[host->event_cursor].host_frame < end) {
    const QueuedEvent* queued = &host->events[host->event_cursor];
    uint32_t local_frame = (uint32_t)lv2_hotswap_scale_frame(
      queued->host_frame - host->host_frame,
      REFERENCE_RATE,
      (uint32_t)host->current.rate);
    if (local_frame >= frames) {
      local_frame = frames - 1U;
    }

    IntEvent event;
    memset(&event, 0, sizeof(event));
    event.time  = (int64_t)local_frame;
    event.size  = (uint32_t)sizeof(int32_t);
    event.type  = URID_INT;
    event.value = (int32_t)queued->id;
    lv2_atom_sequence_append_event(
      input, capacity, (const LV2_Atom_Event*)&event);
    ++host->event_cursor;
  }
}

static int
host_run_block(Host* host)
{
  const uint32_t frames = (uint32_t)lv2_hotswap_scale_frame(
    BLOCK_FRAMES, REFERENCE_RATE, (uint32_t)host->current.rate);
  if (frames == 0U) {
    return 1;
  }

  build_input_sequence(host, frames);

  LV2_Atom_Sequence* output =
    (LV2_Atom_Sequence*)host->seq_out_buffer;
  output->atom.type = URID_SEQUENCE;
  output->atom.size =
    (uint32_t)(SEQ_BUFFER_SIZE - sizeof(LV2_Atom));
  output->body.unit = 0U;
  output->body.pad  = 0U;

  memset(host->out_l, 0, sizeof(host->out_l));
  memset(host->out_r, 0, sizeof(host->out_r));

  const uint64_t start = now_ns();
  host->current.descriptor->run(host->current.handle, frames);
  const uint64_t finish = now_ns();
  host->last_run_ns = finish - start;
  if (host->last_run_ns > host->max_run_ns) {
    host->max_run_ns = host->last_run_ns;
  }

  for (uint32_t i = 0U; i < frames; ++i) {
    const float expected_l =
      host->in_l[i] +
      (float)(0.25 * sin(host->expected_phase * 2.0 * TEST_PI) *
              (double)host->gain);
    const float expected_r =
      host->in_r[i] -
      (float)(0.25 * sin(host->expected_phase * 2.0 * TEST_PI) *
              (double)host->gain);
    const double error_l = fabs((double)host->out_l[i] - expected_l);
    const double error_r = fabs((double)host->out_r[i] - expected_r);
    if (error_l > host->max_error) {
      host->max_error = error_l;
    }
    if (error_r > host->max_error) {
      host->max_error = error_r;
    }

    if (host->have_last) {
      const double jump = fabs((double)host->out_l[i] -
                               (double)host->last_out_l);
      if (jump > host->max_jump) {
        host->max_jump = jump;
      }
      if (i == 0U && jump > host->max_boundary_jump) {
        host->max_boundary_jump = jump;
      }
    }

    host->last_out_l = host->out_l[i];
    host->have_last  = true;
    host->expected_phase += 440.0 / host->current.rate;
    if (host->expected_phase >= 1.0) {
      host->expected_phase -= 1.0;
    }
  }

  LV2_ATOM_SEQUENCE_FOREACH(output, event)
  {
    if (event->body.type == URID_INT &&
        event->body.size >= sizeof(int32_t)) {
      int32_t id = 0;
      memcpy(&id, LV2_ATOM_BODY_CONST(&event->body), sizeof(id));
      if ((uint32_t)id != host->next_event_id) {
        fprintf(stderr,
                "event order error: got %d expected %u\n",
                id,
                host->next_event_id);
        return 1;
      }

      const QueuedEvent* queued = &host->events[(uint32_t)id - 1U];
      const uint32_t expected_frame = (uint32_t)lv2_hotswap_scale_frame(
        queued->host_frame - host->host_frame,
        REFERENCE_RATE,
        (uint32_t)host->current.rate);
      if ((uint32_t)event->time.frames != expected_frame) {
        fprintf(stderr,
                "event time error: got %lld expected %u\n",
                (long long)event->time.frames,
                expected_frame);
        return 1;
      }

      ++host->next_event_id;
      ++host->events_received;
    }
  }

  host->total_frames += frames;
  host->host_frame += BLOCK_FRAMES;
  return 0;
}

static int
test_scale_frame(void)
{
  int failures = 0;
  if (lv2_hotswap_scale_frame(48000U, 48000U, 44100U) != 44100U) {
    fprintf(stderr, "scale_frame equality test failed\n");
    ++failures;
  }
  if (lv2_hotswap_scale_frame(0U, 0U, 48000U) != 0U) {
    fprintf(stderr, "scale_frame zero-rate test failed\n");
    ++failures;
  }
  if (lv2_hotswap_scale_frame(UINT64_MAX, 1U, 2U) != UINT64_MAX) {
    fprintf(stderr, "scale_frame overflow test failed\n");
    ++failures;
  }
  return failures;
}

static int
test_rejections(void)
{
  Host         host;
  TargetBuffer target_buffer;
  int          failures = 0;
  LV2_Hotswap_Position position = {0U, REFERENCE_RATE};

  if (host_init(&host, &descriptor_old, 44100.0)) {
    fprintf(stderr, "failed to initialize rejection test host\n");
    return 1;
  }

  if (host_commit(&host, &position) != LV2_HOTSWAP_ERR_NOT_PREPARED) {
    fprintf(stderr, "commit without prepare was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  target_buffer.target.sample_rate = 0.0;
  if (host_prepare(&host, &target_buffer.target) !=
      LV2_HOTSWAP_ERR_BAD_ARGUMENT) {
    fprintf(stderr, "invalid sample rate was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_other, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  if (host_prepare(&host, &target_buffer.target) !=
      LV2_HOTSWAP_ERR_BAD_PLUGIN) {
    fprintf(stderr, "plugin URI change was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  target_buffer.ports[0].new_type = TYPE_CONTROL;
  if (host_prepare(&host, &target_buffer.target) != LV2_HOTSWAP_ERR_BAD_PORT) {
    fprintf(stderr, "port type change was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  target_buffer.ports[1].new_index = target_buffer.ports[0].new_index;
  if (host_prepare(&host, &target_buffer.target) != LV2_HOTSWAP_ERR_BAD_PORT) {
    fprintf(stderr, "duplicate port index was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  target_buffer.ports[1].old_index = target_buffer.ports[0].old_index;
  if (host_prepare(&host, &target_buffer.target) != LV2_HOTSWAP_ERR_BAD_PORT) {
    fprintf(stderr, "duplicate old port index was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  target_buffer.target.num_ports = ROLE_COUNT - 1U;
  if (host_prepare(&host, &target_buffer.target) != LV2_HOTSWAP_ERR_BAD_PORT) {
    fprintf(stderr, "missing port was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_bad_state, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  if (host_prepare(&host, &target_buffer.target) !=
      LV2_HOTSWAP_ERR_BAD_STATE) {
    fprintf(stderr, "state version mismatch was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  target_buffer.target.max_state_size = 4U;
  if (host_prepare(&host, &target_buffer.target) !=
      LV2_HOTSWAP_ERR_BAD_STATE) {
    fprintf(stderr, "oversized state was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_stateless, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  if (host_prepare(&host, &target_buffer.target) !=
      LV2_HOTSWAP_ERR_UNSUPPORTED) {
    fprintf(stderr, "missing required state interface was not rejected\n");
    ++failures;
  }

  make_target(&host, &descriptor_new, 48000.0,
              LV2_HOTSWAP_F_REQUIRE_STATE, &target_buffer);
  if (host_prepare(&host, &target_buffer.target) != LV2_HOTSWAP_SUCCESS) {
    fprintf(stderr, "valid prepare failed\n");
    ++failures;
    host_abort(&host);
    host_destroy(&host);
    return failures;
  }

  if (host_prepare(&host, &target_buffer.target) != LV2_HOTSWAP_ERR_BUSY) {
    fprintf(stderr, "second prepare was not rejected\n");
    ++failures;
  }

  LV2_Hotswap_Position bad_position = {host.host_frame, 0U};
  if (host_commit(&host, &bad_position) != LV2_HOTSWAP_ERR_BAD_ARGUMENT) {
    fprintf(stderr, "invalid commit position was not rejected\n");
    ++failures;
  }

  host_abort(&host);
  if (host_commit(&host, &position) != LV2_HOTSWAP_ERR_NOT_PREPARED) {
    fprintf(stderr, "aborted swap was still committed\n");
    ++failures;
  }
  host_abort(&host);

  if (host_run_block(&host)) {
    fprintf(stderr, "old instance did not survive rejected swaps\n");
    ++failures;
  }

  host_destroy(&host);
  return failures;
}

static int
compare_u64(const void* lhs, const void* rhs)
{
  const uint64_t a = *(const uint64_t*)lhs;
  const uint64_t b = *(const uint64_t*)rhs;
  return (a > b) - (a < b);
}

static int
test_switches_1000(void)
{
  Host host;
  if (host_init(&host, &descriptor_old, 44100.0)) {
    fprintf(stderr, "failed to initialize switch test host\n");
    return 1;
  }

  uint64_t prepare_ns[SWITCH_COUNT];
  uint64_t commit_ns[SWITCH_COUNT];
  uint64_t switch_ns[SWITCH_COUNT];
  uint64_t callback_ns[SWITCH_COUNT];
  uint64_t run_ns[SWITCH_COUNT];
  int      failures = 0;

  const uint64_t deadline_ns =
    ((uint64_t)BLOCK_FRAMES * 1000000000U) / REFERENCE_RATE;
  uint32_t xruns = 0U;
  uint64_t total_prepare_ns = 0U;
  uint64_t total_commit_ns  = 0U;

  for (uint32_t i = 0U; i < SWITCH_COUNT; ++i) {
    const TestDescriptor* next =
      host.current.kind == 0U ? &descriptor_new : &descriptor_old;
    const double rate = next->kind == 0U ? 44100.0 : 48000.0;

    TargetBuffer target_buffer;
    make_target(&host, next, rate, LV2_HOTSWAP_F_REQUIRE_STATE,
                &target_buffer);

    const uint64_t prepare_start = now_ns();
    const LV2_Hotswap_Status prepare_status =
      host.interface.prepare(host.interface.handle, &target_buffer.target);
    const uint64_t prepare_end = now_ns();
    prepare_ns[i] = prepare_end - prepare_start;
    total_prepare_ns += prepare_ns[i];
    if (prepare_status != LV2_HOTSWAP_SUCCESS) {
      fprintf(stderr, "prepare failed at switch %u: %u\n",
              i, (unsigned)prepare_status);
      ++failures;
      break;
    }

    const size_t alloc_before  = n_allocations;
    const size_t free_before   = n_frees;
    const uint64_t callback_start = now_ns();
    LV2_Hotswap_Position position = {host.host_frame, host.reference_rate};
    const uint64_t commit_start = now_ns();
    const LV2_Hotswap_Status commit_status =
      host.interface.commit(host.interface.handle, &position);
    const uint64_t commit_end = now_ns();
    commit_ns[i] = commit_end - commit_start;
    total_commit_ns += commit_ns[i];

    if (commit_status != LV2_HOTSWAP_SUCCESS) {
      fprintf(stderr, "commit failed at switch %u: %u\n",
              i, (unsigned)commit_status);
      ++failures;
      break;
    }

    if (n_allocations != alloc_before || n_frees != free_before) {
      fprintf(stderr, "commit allocated memory at switch %u\n", i);
      ++failures;
    }

    if (host_run_block(&host)) {
      fprintf(stderr, "run failed at switch %u\n", i);
      ++failures;
      break;
    }
    const uint64_t callback_end = now_ns();

    callback_ns[i] = callback_end - callback_start;
    run_ns[i]      = host.last_run_ns;
    switch_ns[i]   = prepare_ns[i] + commit_ns[i];
    if (callback_ns[i] > deadline_ns) {
      ++xruns;
    }

    host_reap(&host);
  }

  const uint64_t total_switch_ns = total_prepare_ns + total_commit_ns;
  const uint32_t completed = failures ? 0U : SWITCH_COUNT;

  if (completed) {
    qsort(prepare_ns, SWITCH_COUNT, sizeof(uint64_t), compare_u64);
    qsort(commit_ns, SWITCH_COUNT, sizeof(uint64_t), compare_u64);
    qsort(switch_ns, SWITCH_COUNT, sizeof(uint64_t), compare_u64);
    qsort(callback_ns, SWITCH_COUNT, sizeof(uint64_t), compare_u64);
    qsort(run_ns, SWITCH_COUNT, sizeof(uint64_t), compare_u64);
  }

  if (host.events_received != EVENT_COUNT ||
      host.event_cursor != EVENT_COUNT) {
    fprintf(stderr, "event loss: received %llu of %u\n",
            (unsigned long long)host.events_received, EVENT_COUNT);
    ++failures;
  }

  if (host.max_error > 1e-9) {
    fprintf(stderr, "sample continuity error: %.12g\n", host.max_error);
    ++failures;
  }

  printf("hotswap_benchmark switches=%u blocks=%u host_frames=%llu "
         "output_frames=%llu events=%llu\n",
         completed,
         completed,
         (unsigned long long)host.host_frame,
         (unsigned long long)host.total_frames,
         (unsigned long long)host.events_received);
  if (completed) {
    printf("prepare_ns min=%llu p50=%llu p99=%llu max=%llu\n",
           (unsigned long long)prepare_ns[0],
           (unsigned long long)prepare_ns[SWITCH_COUNT / 2U],
           (unsigned long long)prepare_ns[(SWITCH_COUNT * 99U) / 100U],
           (unsigned long long)prepare_ns[SWITCH_COUNT - 1U]);
    printf("commit_ns min=%llu p50=%llu p99=%llu max=%llu\n",
           (unsigned long long)commit_ns[0],
           (unsigned long long)commit_ns[SWITCH_COUNT / 2U],
           (unsigned long long)commit_ns[(SWITCH_COUNT * 99U) / 100U],
           (unsigned long long)commit_ns[SWITCH_COUNT - 1U]);
    printf("run_ns min=%llu p50=%llu p99=%llu max=%llu\n",
           (unsigned long long)run_ns[0],
           (unsigned long long)run_ns[SWITCH_COUNT / 2U],
           (unsigned long long)run_ns[(SWITCH_COUNT * 99U) / 100U],
           (unsigned long long)run_ns[SWITCH_COUNT - 1U]);
    printf("switch_ns min=%llu p50=%llu p99=%llu max=%llu total=%llu\n",
           (unsigned long long)switch_ns[0],
           (unsigned long long)switch_ns[SWITCH_COUNT / 2U],
           (unsigned long long)switch_ns[(SWITCH_COUNT * 99U) / 100U],
           (unsigned long long)switch_ns[SWITCH_COUNT - 1U],
           (unsigned long long)total_switch_ns);
    printf("callback_ns p99=%llu max=%llu deadline_ns=%llu xruns=%u\n",
           (unsigned long long)callback_ns[(SWITCH_COUNT * 99U) / 100U],
           (unsigned long long)callback_ns[SWITCH_COUNT - 1U],
           (unsigned long long)deadline_ns,
           xruns);
  }
  printf("continuity max_abs_error=%.12g max_jump=%.12g "
         "max_boundary_jump=%.12g\n",
         host.max_error,
         host.max_jump,
         host.max_boundary_jump);

  host_destroy(&host);

  printf("allocations=%llu frees=%llu live_bytes=%llu\n",
         (unsigned long long)n_allocations,
         (unsigned long long)n_frees,
         (unsigned long long)live_bytes);

  if (xruns != 0U) {
    ++failures;
  }
  return failures;
}

int
main(void)
{
  int failures = 0;
  failures += test_scale_frame();
  failures += test_rejections();
  failures += test_switches_1000();

  if (n_allocations != n_frees || live_bytes != 0U) {
    fprintf(stderr,
            "memory leak: allocations=%llu frees=%llu live_bytes=%llu\n",
            (unsigned long long)n_allocations,
            (unsigned long long)n_frees,
            (unsigned long long)live_bytes);
    ++failures;
  }

  return failures ? 1 : 0;
}
