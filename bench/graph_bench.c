#define _POSIX_C_SOURCE 200809L

#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOCK_CONTEXT_URI "urn:lv2:bench:context"
#define MAX_REPEATS 32U
#define BAND_COUNT 16U

typedef struct {
  uint32_t id;
  uint32_t frame;
  uint32_t instance;
  uint32_t port;
  int32_t  value;
} BenchEvent;

typedef struct {
  int32_t  value;
  uint32_t sequence;
} EventResult;

typedef struct {
  uint32_t src_instance;
  uint32_t src_port;
  uint32_t dst_instance;
  uint32_t dst_port;
} Connection;

typedef struct {
  uint32_t port;
  void*    data;
} PortBinding;

typedef struct {
  unsigned char* data;
  size_t         size;
  size_t         used;
} Arena;

typedef struct {
  Arena*       arena;
  EventResult* results;
  uint32_t     audio_ports;
} MockContext;

typedef struct {
  uint32_t         id;
  uint32_t         port_count;
  uint32_t         control_port;
  uint32_t         output_port;
  void**           ports;
  const BenchEvent* pending;
  uint32_t         pending_count;
  void*            pending_port;
  uint32_t         sequence;
  EventResult*     results;
  bool             arena_allocated;
} MockInstance;

typedef struct {
  uint32_t      id;
  MockInstance* mock;
  PortBinding*  bindings;
  size_t        binding_count;
  size_t        binding_capacity;
} HostInstance;

typedef struct {
  const char*       name;
  const LV2_Descriptor* descriptor;
  uint32_t          num_instances;
  uint32_t          audio_ports;
  uint32_t          num_connections;
  uint32_t          num_events;
  uint32_t          batch_frames;
  uint32_t          port_count;
  uint32_t          control_port;
  uint32_t          output_port;
  const Connection* connections;
  const BenchEvent* events;
} GraphPlan;

typedef struct {
  const LV2_Descriptor* descriptor;
  uint32_t          num_instances;
  uint32_t          audio_ports;
  uint32_t          port_count;
  uint32_t          control_port;
  uint32_t          output_port;
  uint32_t          built_instances;
  bool              fast;
  HostInstance*     instances;
  HostInstance**    instance_ptrs;
  float*            audio_buffers;
  float*            control_buffers;
  float*            zero_buffers;
  void*             arena_data;
  size_t            arena_size;
  EventResult*      results;
  uint32_t*         counts;
  uint32_t*         offsets;
  uint32_t*         active;
  uint32_t*         cursor;
  BenchEvent*       batch;
  uint32_t          batch_capacity;
} Graph;

typedef struct {
  const char* name;
  uint32_t    instances;
  uint32_t    audio_ports;
  uint32_t    events;
  uint32_t    frames;
  uint32_t    batch_frames;
} Tier;

typedef struct {
  double validate_ms;
  double build_ms;
  double prepare_ms;
  double event_ms;
  double cleanup_ms;
  double total_ms;
} Timing;

typedef struct {
  const Tier* tier;
  uint32_t    connections;
  uint32_t    port_count;
  Timing      slow;
  Timing      fast;
  double      build_reduction;
  double      event_reduction;
  double      total_reduction;
  uint32_t    mismatches;
  uint64_t    slow_digest;
  uint64_t    fast_digest;
  uint64_t    slow_bands[BAND_COUNT];
  uint64_t    fast_bands[BAND_COUNT];
  uint32_t    sample_count;
  BenchEvent  sample_events[7];
  EventResult slow_samples[7];
  EventResult fast_samples[7];
} TierReport;

static const Tier TIERS[] = {
  {"small", 64U, 8U, 65536U, 65536U, 512U},
  {"medium", 256U, 16U, 262144U, 262144U, 2048U},
  {"large", 1024U, 16U, 1048576U, 1048576U, 8192U},
};

static size_t
align_up(size_t value, size_t alignment)
{
  return (value + alignment - 1U) & ~(alignment - 1U);
}

static void*
arena_alloc(Arena* arena, size_t size)
{
  const size_t alignment = 16U;
  const size_t offset    = align_up(arena->used, alignment);
  if (offset > arena->size || size > arena->size - offset) {
    return NULL;
  }

  void* const result = arena->data + offset;
  arena->used        = offset + size;
  return result;
}

static LV2_Handle
mock_instantiate(const LV2_Descriptor*     descriptor,
                 double                    sample_rate,
                 const char*               bundle_path,
                 const LV2_Feature* const* features)
{
  (void)descriptor;
  (void)sample_rate;
  (void)bundle_path;

  MockContext* const context =
    (MockContext*)lv2_features_data(features, MOCK_CONTEXT_URI);
  if (!context) {
    return NULL;
  }

  const uint32_t port_count = 2U * context->audio_ports + 1U;
  MockInstance*  instance   = NULL;
  void**         ports      = NULL;

  if (context->arena) {
    instance = (MockInstance*)arena_alloc(context->arena, sizeof(*instance));
    ports    = (void**)arena_alloc(
      context->arena, (size_t)port_count * sizeof(*ports));
    if (!instance || !ports) {
      return NULL;
    }

    memset(instance, 0, sizeof(*instance));
    memset(ports, 0, (size_t)port_count * sizeof(*ports));
  } else {
    instance = (MockInstance*)calloc(1U, sizeof(*instance));
    ports    = (void**)calloc((size_t)port_count, sizeof(*ports));
    if (!instance || !ports) {
      free(ports);
      free(instance);
      return NULL;
    }
  }

  instance->port_count      = port_count;
  instance->control_port    = 2U * context->audio_ports;
  instance->output_port     = context->audio_ports;
  instance->ports           = ports;
  instance->results         = context->results;
  instance->arena_allocated = context->arena != NULL;

  return instance;
}

static void
mock_connect_port(LV2_Handle instance, uint32_t port, void* data)
{
  MockInstance* const mock = (MockInstance*)instance;
  if (port < mock->port_count) {
    mock->ports[port] = data;
  }
}

static void
mock_activate(LV2_Handle instance)
{
  (void)instance;
}

static void
mock_run(LV2_Handle instance, uint32_t sample_count)
{
  (void)sample_count;

  MockInstance* const mock = (MockInstance*)instance;
  if (!mock->pending_port || !mock->ports[mock->output_port]) {
    return;
  }

  float* const control = (float*)mock->pending_port;
  float* const output  = (float*)mock->ports[mock->output_port];

  for (uint32_t i = 0U; i < mock->pending_count; ++i) {
    const BenchEvent* const event = &mock->pending[i];
    *control                       = (float)event->value;

    const int32_t readback = (int32_t)*control;
    const int32_t result =
      readback + (int32_t)mock->id * 131 + (int32_t)mock->sequence;

    *output                          = (float)result;
    mock->results[event->id].value    = result;
    mock->results[event->id].sequence = mock->sequence;
    ++mock->sequence;
  }

  mock->pending       = NULL;
  mock->pending_count = 0U;
}

static void
mock_deactivate(LV2_Handle instance)
{
  (void)instance;
}

static void
mock_cleanup(LV2_Handle instance)
{
  MockInstance* const mock = (MockInstance*)instance;
  if (!mock->arena_allocated) {
    free(mock->ports);
    free(mock);
  }
}

static const LV2_Descriptor MOCK_DESCRIPTOR = {
  "urn:lv2:bench:mock",
  mock_instantiate,
  mock_connect_port,
  mock_activate,
  mock_run,
  mock_deactivate,
  mock_cleanup,
  NULL,
};

static HostInstance*
graph_instance(Graph* graph, uint32_t index)
{
  return graph->fast ? &graph->instances[index]
                     : graph->instance_ptrs[index];
}

static PortBinding*
find_binding(HostInstance* instance, uint32_t port)
{
  for (size_t i = 0U; i < instance->binding_count; ++i) {
    if (instance->bindings[i].port == port) {
      return &instance->bindings[i];
    }
  }

  return NULL;
}

static bool
append_binding(const LV2_Descriptor* descriptor,
               HostInstance*         instance,
               uint32_t              port,
               void*                 data)
{
  if (instance->binding_count == instance->binding_capacity) {
    const size_t capacity =
      instance->binding_capacity ? instance->binding_capacity * 2U : 8U;
    PortBinding* const bindings =
      (PortBinding*)realloc(instance->bindings, capacity * sizeof(*bindings));
    if (!bindings) {
      return false;
    }

    instance->bindings         = bindings;
    instance->binding_capacity = capacity;
  }

  instance->bindings[instance->binding_count].port = port;
  instance->bindings[instance->binding_count].data = data;
  ++instance->binding_count;
  descriptor->connect_port(instance->mock, port, data);
  return true;
}

static bool
validate_plan(const GraphPlan* plan)
{
  if (!plan->descriptor || !plan->descriptor->instantiate ||
      !plan->descriptor->connect_port || !plan->descriptor->run) {
    fprintf(stderr, "%s: incomplete descriptor\n", plan->name);
    return false;
  }

  if (plan->num_instances == 0U || plan->audio_ports == 0U ||
      plan->batch_frames == 0U ||
      plan->control_port != 2U * plan->audio_ports ||
      plan->port_count != plan->control_port + 1U) {
    fprintf(stderr, "%s: invalid graph dimensions\n", plan->name);
    return false;
  }

  if ((uint64_t)plan->num_instances * plan->audio_ports > (uint64_t)SIZE_MAX) {
    fprintf(stderr, "%s: port storage overflow\n", plan->name);
    return false;
  }

  const uint64_t expected =
    (uint64_t)(plan->num_instances - 1U) * plan->audio_ports;
  if ((uint64_t)plan->num_connections != expected) {
    fprintf(stderr, "%s: wrong connection count\n", plan->name);
    return false;
  }

  uint8_t* const seen = (uint8_t*)calloc(
    (size_t)plan->num_instances * plan->audio_ports, sizeof(*seen));
  if (!seen) {
    fprintf(stderr, "%s: out of memory\n", plan->name);
    return false;
  }

  bool valid = true;
  for (uint32_t i = 0U; i < plan->num_connections && valid; ++i) {
    const Connection* const connection = &plan->connections[i];
    if (connection->src_instance >= plan->num_instances ||
        connection->src_port >= plan->audio_ports ||
        connection->dst_instance == 0U ||
        connection->dst_instance >= plan->num_instances ||
        connection->dst_port >= plan->audio_ports ||
        connection->src_instance == connection->dst_instance) {
      fprintf(stderr, "%s: invalid connection %" PRIu32 "\n", plan->name, i);
      valid = false;
      break;
    }

    const size_t key =
      (size_t)connection->dst_instance * plan->audio_ports +
      connection->dst_port;
    if (seen[key]) {
      fprintf(stderr, "%s: duplicate destination %" PRIu32 "\n", plan->name, i);
      valid = false;
      break;
    }

    seen[key] = 1U;
  }

  for (uint32_t instance = 1U; instance < plan->num_instances && valid;
       ++instance) {
    for (uint32_t port = 0U; port < plan->audio_ports; ++port) {
      const size_t key = (size_t)instance * plan->audio_ports + port;
      if (!seen[key]) {
        fprintf(stderr,
                "%s: unconnected input %" PRIu32 ":%" PRIu32 "\n",
                plan->name,
                instance,
                port);
        valid = false;
        break;
      }
    }
  }

  for (uint32_t port = 0U; port < plan->audio_ports && valid; ++port) {
    if (seen[port]) {
      fprintf(stderr, "%s: source input is connected\n", plan->name);
      valid = false;
    }
  }

  for (uint32_t i = 0U; i < plan->num_events && valid; ++i) {
    const BenchEvent* const event = &plan->events[i];
    if (event->instance >= plan->num_instances ||
        event->port != plan->control_port ||
        (i > 0U && event->frame < plan->events[i - 1U].frame)) {
      fprintf(stderr, "%s: invalid event %" PRIu32 "\n", plan->name, i);
      valid = false;
    }
  }

  free(seen);
  return valid;
}

static bool
build_slow(const GraphPlan* plan, EventResult* results, Graph* graph)
{
  memset(graph, 0, sizeof(*graph));
  graph->descriptor    = plan->descriptor;
  graph->num_instances = plan->num_instances;
  graph->audio_ports   = plan->audio_ports;
  graph->port_count    = plan->port_count;
  graph->control_port  = plan->control_port;
  graph->output_port   = plan->output_port;
  graph->results       = results;
  graph->fast          = false;

  const size_t instance_count = plan->num_instances;
  const size_t audio_count    = instance_count * plan->audio_ports;

  graph->instance_ptrs =
    (HostInstance**)calloc(instance_count, sizeof(*graph->instance_ptrs));
  graph->audio_buffers =
    (float*)calloc(audio_count, sizeof(*graph->audio_buffers));
  graph->control_buffers =
    (float*)calloc(instance_count, sizeof(*graph->control_buffers));
  graph->zero_buffers =
    (float*)calloc(plan->audio_ports, sizeof(*graph->zero_buffers));

  if (!graph->instance_ptrs || !graph->audio_buffers ||
      !graph->control_buffers || !graph->zero_buffers) {
    return false;
  }

  MockContext context = {NULL, results, plan->audio_ports};
  LV2_Feature feature = {MOCK_CONTEXT_URI, &context};
  const LV2_Feature* features[2] = {&feature, NULL};

  for (uint32_t i = 0U; i < plan->num_instances; ++i) {
    HostInstance* const instance = (HostInstance*)calloc(1U, sizeof(*instance));
    if (!instance) {
      return false;
    }

    instance->id                      = i;
    graph->instance_ptrs[i]           = instance;
    graph->built_instances            = i + 1U;
    instance->mock                    = (MockInstance*)mock_instantiate(
      plan->descriptor, 48000.0, "/bench/", features);
    if (!instance->mock) {
      return false;
    }

    instance->mock->id = i;
    for (uint32_t port = 0U; port < plan->audio_ports; ++port) {
      void* const data =
        &graph->audio_buffers[(size_t)i * plan->audio_ports + port];
      if (!append_binding(
            plan->descriptor, instance, plan->audio_ports + port, data)) {
        return false;
      }
    }

    if (!append_binding(plan->descriptor,
                        instance,
                        plan->control_port,
                        &graph->control_buffers[i])) {
      return false;
    }
  }

  HostInstance* const source = graph->instance_ptrs[0];
  for (uint32_t port = 0U; port < plan->audio_ports; ++port) {
    if (!append_binding(plan->descriptor,
                        source,
                        port,
                        &graph->zero_buffers[port])) {
      return false;
    }
  }

  for (uint32_t i = 0U; i < plan->num_connections; ++i) {
    const Connection* const connection = &plan->connections[i];
    if (connection->src_instance >= plan->num_instances ||
        connection->src_port >= plan->audio_ports ||
        connection->dst_instance >= plan->num_instances ||
        connection->dst_port >= plan->audio_ports ||
        connection->dst_instance == 0U ||
        connection->src_instance == connection->dst_instance) {
      return false;
    }

    HostInstance* const destination =
      graph->instance_ptrs[connection->dst_instance];
    if (find_binding(destination, connection->dst_port)) {
      return false;
    }

    void* const data =
      &graph->audio_buffers[(size_t)connection->src_instance *
                              plan->audio_ports +
                            connection->src_port];
    if (!append_binding(
          plan->descriptor, destination, connection->dst_port, data)) {
      return false;
    }
  }

  for (uint32_t instance = 1U; instance < plan->num_instances; ++instance) {
    HostInstance* const host = graph->instance_ptrs[instance];
    for (uint32_t port = 0U; port < plan->audio_ports; ++port) {
      if (!find_binding(host, port)) {
        return false;
      }
    }
  }

  return true;
}

static uint32_t
max_events_per_block(const BenchEvent* events,
                     uint32_t          num_events,
                     uint32_t          batch_frames)
{
  uint32_t maximum = 0U;
  uint32_t start   = 0U;

  while (start < num_events) {
    const uint64_t block = (uint64_t)events[start].frame / batch_frames;
    const uint64_t limit = (block + 1U) * batch_frames;
    uint32_t       end   = start;
    while (end < num_events && events[end].frame < limit) {
      ++end;
    }

    const uint32_t count = end - start;
    if (count > maximum) {
      maximum = count;
    }
    start = end;
  }

  return maximum ? maximum : 1U;
}

static bool
build_fast(const GraphPlan* plan, EventResult* results, Graph* graph)
{
  memset(graph, 0, sizeof(*graph));
  graph->descriptor    = plan->descriptor;
  graph->num_instances = plan->num_instances;
  graph->audio_ports   = plan->audio_ports;
  graph->port_count    = plan->port_count;
  graph->control_port  = plan->control_port;
  graph->output_port   = plan->output_port;
  graph->results       = results;
  graph->fast          = true;

  const size_t instance_count = plan->num_instances;
  const size_t audio_count    = instance_count * plan->audio_ports;
  const size_t instance_bytes = align_up(sizeof(MockInstance), 16U);
  const size_t ports_bytes =
    align_up((size_t)plan->port_count * sizeof(void*), 16U);
  const size_t per_instance = instance_bytes + ports_bytes;

  if (instance_count > ((size_t)-1 - 32U) / per_instance) {
    return false;
  }

  graph->arena_size = instance_count * per_instance + 32U;
  graph->instances =
    (HostInstance*)calloc(instance_count, sizeof(*graph->instances));
  graph->audio_buffers =
    (float*)calloc(audio_count, sizeof(*graph->audio_buffers));
  graph->control_buffers =
    (float*)calloc(instance_count, sizeof(*graph->control_buffers));
  graph->zero_buffers =
    (float*)calloc(plan->audio_ports, sizeof(*graph->zero_buffers));
  graph->arena_data = calloc(1U, graph->arena_size);

  if (!graph->instances || !graph->audio_buffers ||
      !graph->control_buffers || !graph->zero_buffers ||
      !graph->arena_data) {
    return false;
  }

  Arena arena = {graph->arena_data, graph->arena_size, 0U};
  MockContext context = {&arena, results, plan->audio_ports};
  LV2_Feature feature = {MOCK_CONTEXT_URI, &context};
  const LV2_Feature* features[2] = {&feature, NULL};

  for (uint32_t i = 0U; i < plan->num_instances; ++i) {
    HostInstance* const instance = &graph->instances[i];
    instance->id                 = i;
    graph->built_instances       = i + 1U;
    instance->mock               = (MockInstance*)mock_instantiate(
      plan->descriptor, 48000.0, "/bench/", features);
    if (!instance->mock) {
      return false;
    }

    instance->mock->id = i;
    for (uint32_t port = 0U; port < plan->audio_ports; ++port) {
      plan->descriptor->connect_port(
        instance->mock,
        plan->audio_ports + port,
        &graph->audio_buffers[(size_t)i * plan->audio_ports + port]);
    }

    plan->descriptor->connect_port(
      instance->mock, plan->control_port, &graph->control_buffers[i]);
  }

  for (uint32_t port = 0U; port < plan->audio_ports; ++port) {
    plan->descriptor->connect_port(
      graph->instances[0].mock, port, &graph->zero_buffers[port]);
  }

  for (uint32_t i = 0U; i < plan->num_connections; ++i) {
    const Connection* const connection = &plan->connections[i];
    HostInstance* const destination =
      &graph->instances[connection->dst_instance];
    void* const data =
      &graph->audio_buffers[(size_t)connection->src_instance *
                              plan->audio_ports +
                            connection->src_port];
    plan->descriptor->connect_port(
      destination->mock, connection->dst_port, data);
  }

  return true;
}

static bool
prepare_fast_events(Graph* graph, const GraphPlan* plan)
{
  const size_t instance_count = plan->num_instances;
  graph->counts =
    (uint32_t*)calloc(instance_count, sizeof(*graph->counts));
  graph->offsets =
    (uint32_t*)calloc(instance_count, sizeof(*graph->offsets));
  graph->active =
    (uint32_t*)calloc(instance_count, sizeof(*graph->active));
  graph->cursor =
    (uint32_t*)calloc(instance_count, sizeof(*graph->cursor));
  graph->batch_capacity =
    max_events_per_block(plan->events, plan->num_events, plan->batch_frames);
  graph->batch =
    (BenchEvent*)calloc(graph->batch_capacity, sizeof(*graph->batch));

  return graph->counts && graph->offsets && graph->active && graph->cursor &&
         graph->batch;
}

static void
destroy_graph(Graph* graph)
{
  if (graph->descriptor) {
    for (uint32_t i = 0U; i < graph->built_instances; ++i) {
      HostInstance* const instance = graph_instance(graph, i);
      if (instance && instance->mock) {
        graph->descriptor->cleanup(instance->mock);
      }
    }
  }

  if (graph->fast) {
    free(graph->instances);
    free(graph->arena_data);
  } else {
    for (uint32_t i = 0U; i < graph->built_instances; ++i) {
      HostInstance* const instance = graph->instance_ptrs[i];
      if (instance) {
        free(instance->bindings);
        free(instance);
      }
    }
    free(graph->instance_ptrs);
  }

  free(graph->audio_buffers);
  free(graph->control_buffers);
  free(graph->zero_buffers);
  free(graph->counts);
  free(graph->offsets);
  free(graph->active);
  free(graph->cursor);
  free(graph->batch);
  memset(graph, 0, sizeof(*graph));
}

static bool
process_slow(Graph* graph, const GraphPlan* plan)
{
  for (uint32_t i = 0U; i < plan->num_events; ++i) {
    const BenchEvent* const event = &plan->events[i];
    if (event->instance >= graph->num_instances ||
        event->port >= graph->port_count) {
      return false;
    }

    HostInstance* destination = NULL;
    for (uint32_t j = 0U; j < graph->num_instances; ++j) {
      HostInstance* const candidate = graph->instance_ptrs[j];
      if (candidate->id == event->instance) {
        destination = candidate;
        break;
      }
    }

    if (!destination) {
      return false;
    }

    PortBinding* const binding = find_binding(destination, event->port);
    if (!binding) {
      return false;
    }

    BenchEvent* const staged = (BenchEvent*)malloc(sizeof(*staged));
    if (!staged) {
      return false;
    }

    *staged                     = *event;
    destination->mock->pending  = staged;
    destination->mock->pending_count = 1U;
    destination->mock->pending_port  = binding->data;
    graph->descriptor->run(destination->mock, 1U);
    free(staged);
  }

  return true;
}

static bool
process_fast(Graph* graph, const GraphPlan* plan)
{
  if (!graph->counts || !graph->offsets || !graph->active || !graph->cursor ||
      !graph->batch) {
    return false;
  }

  uint32_t block_start = 0U;

  while (block_start < plan->num_events) {
    const uint64_t block =
      (uint64_t)plan->events[block_start].frame / plan->batch_frames;
    const uint64_t limit = (block + 1U) * plan->batch_frames;
    uint32_t       block_end = block_start;
    while (block_end < plan->num_events &&
           plan->events[block_end].frame < limit) {
      ++block_end;
    }

    uint32_t active_count = 0U;
    for (uint32_t i = block_start; i < block_end; ++i) {
      const uint32_t destination = plan->events[i].instance;
      if (graph->counts[destination]++ == 0U) {
        graph->active[active_count++] = destination;
      }
    }

    uint32_t offset = 0U;
    for (uint32_t i = 0U; i < active_count; ++i) {
      const uint32_t destination = graph->active[i];
      graph->offsets[destination] = offset;
      graph->cursor[destination]  = offset;
      offset += graph->counts[destination];
    }

    if (offset != block_end - block_start) {
      return false;
    }

    for (uint32_t i = block_start; i < block_end; ++i) {
      const BenchEvent* const event       = &plan->events[i];
      const uint32_t          destination = event->instance;
      graph->batch[graph->cursor[destination]++] = *event;
    }

    for (uint32_t i = 0U; i < active_count; ++i) {
      const uint32_t destination = graph->active[i];
      HostInstance* const host   = &graph->instances[destination];
      host->mock->pending        = &graph->batch[graph->offsets[destination]];
      host->mock->pending_count  = graph->counts[destination];
      host->mock->pending_port   = host->mock->ports[graph->control_port];
      graph->descriptor->run(host->mock, block_end - block_start);
      graph->counts[destination] = 0U;
    }

    block_start = block_end;
  }

  return true;
}

static double
now_ms(void)
{
  struct timespec timestamp;
  clock_gettime(CLOCK_MONOTONIC, &timestamp);
  return (double)timestamp.tv_sec * 1000.0 +
         (double)timestamp.tv_nsec / 1000000.0;
}

static bool
run_slow_once(GraphPlan* plan, EventResult* results, Timing* timing)
{
  Graph       graph;
  const double start = now_ms();
  if (!build_slow(plan, results, &graph)) {
    destroy_graph(&graph);
    return false;
  }

  const double built = now_ms();
  if (!process_slow(&graph, plan)) {
    destroy_graph(&graph);
    return false;
  }

  const double processed = now_ms();
  destroy_graph(&graph);
  const double cleaned = now_ms();

  timing->validate_ms = 0.0;
  timing->build_ms   = built - start;
  timing->prepare_ms = 0.0;
  timing->event_ms   = processed - built;
  timing->cleanup_ms = cleaned - processed;
  timing->total_ms   = processed - start;
  return true;
}

static bool
run_fast_once(GraphPlan* plan, EventResult* results, Timing* timing)
{
  Graph       graph;
  const double start = now_ms();
  if (!validate_plan(plan)) {
    return false;
  }

  const double validated = now_ms();
  if (!build_fast(plan, results, &graph)) {
    destroy_graph(&graph);
    return false;
  }

  const double built = now_ms();
  if (!prepare_fast_events(&graph, plan)) {
    destroy_graph(&graph);
    return false;
  }

  const double prepared = now_ms();
  if (!process_fast(&graph, plan)) {
    destroy_graph(&graph);
    return false;
  }

  const double processed = now_ms();
  destroy_graph(&graph);
  const double cleaned = now_ms();

  timing->validate_ms = validated - start;
  timing->build_ms   = built - validated;
  timing->prepare_ms = prepared - built;
  timing->event_ms   = processed - prepared;
  timing->cleanup_ms = cleaned - processed;
  timing->total_ms   = processed - start;
  return true;
}

static int
compare_double(const void* lhs, const void* rhs)
{
  const double a = *(const double*)lhs;
  const double b = *(const double*)rhs;
  return (a > b) - (a < b);
}

enum TimingField {
  TIMING_VALIDATE,
  TIMING_BUILD,
  TIMING_PREPARE,
  TIMING_EVENT,
  TIMING_CLEANUP,
  TIMING_TOTAL
};

static double
timing_field(const Timing* timing, enum TimingField field)
{
  switch (field) {
  case TIMING_VALIDATE:
    return timing->validate_ms;
  case TIMING_BUILD:
    return timing->build_ms;
  case TIMING_PREPARE:
    return timing->prepare_ms;
  case TIMING_EVENT:
    return timing->event_ms;
  case TIMING_CLEANUP:
    return timing->cleanup_ms;
  case TIMING_TOTAL:
    return timing->total_ms;
  }

  return 0.0;
}

static double
median_timing(const Timing* timings, uint32_t count, enum TimingField field)
{
  double values[MAX_REPEATS];
  for (uint32_t i = 0U; i < count; ++i) {
    values[i] = timing_field(&timings[i], field);
  }

  qsort(values, count, sizeof(*values), compare_double);
  return count % 2U ? values[count / 2U]
                    : (values[count / 2U - 1U] + values[count / 2U]) / 2.0;
}

static uint64_t
digest_results(const EventResult* results, uint32_t count)
{
  uint64_t digest = 1469598103934665603ULL;

  for (uint32_t i = 0U; i < count; ++i) {
    uint32_t value = (uint32_t)results[i].value;
    for (uint32_t byte = 0U; byte < 4U; ++byte) {
      digest ^= (value >> (byte * 8U)) & 0xFFU;
      digest *= 1099511628211ULL;
    }

    value = results[i].sequence;
    for (uint32_t byte = 0U; byte < 4U; ++byte) {
      digest ^= (value >> (byte * 8U)) & 0xFFU;
      digest *= 1099511628211ULL;
    }
  }

  return digest;
}

static uint64_t
digest_band(const EventResult* results, uint32_t start, uint32_t end)
{
  return digest_results(results + start, end - start);
}

static uint32_t
compare_results(const GraphPlan*   plan,
                const EventResult* slow,
                const EventResult* fast)
{
  uint32_t mismatches = 0U;
  for (uint32_t i = 0U; i < plan->num_events; ++i) {
    if (slow[i].value != fast[i].value ||
        slow[i].sequence != fast[i].sequence) {
      if (mismatches < 8U) {
        fprintf(stderr,
                "%s: event %" PRIu32 " mismatch: slow=(%" PRId32
                ",%" PRIu32 ") fast=(%" PRId32 ",%" PRIu32 ")\n",
                plan->name,
                i,
                slow[i].value,
                slow[i].sequence,
                fast[i].value,
                fast[i].sequence);
      }
      ++mismatches;
    }
  }

  return mismatches;
}

static Connection*
make_connections(uint32_t instances, uint32_t ports, uint32_t* count)
{
  const uint64_t total = (uint64_t)(instances - 1U) * ports;
  if (total == 0U) {
    *count = 0U;
    return (Connection*)calloc(1U, sizeof(Connection));
  }

  Connection* const connections =
    (Connection*)calloc((size_t)total, sizeof(*connections));
  if (!connections) {
    return NULL;
  }

  uint32_t index = 0U;
  for (uint32_t instance = 1U; instance < instances; ++instance) {
    for (uint32_t port = 0U; port < ports; ++port) {
      connections[index].src_instance = instance - 1U;
      connections[index].src_port     = port;
      connections[index].dst_instance = instance;
      connections[index].dst_port     = port;
      ++index;
    }
  }

  *count = index;
  return connections;
}

static BenchEvent*
make_events(uint32_t instances,
            uint32_t events,
            uint32_t frames,
            uint32_t control_port)
{
  BenchEvent* const result =
    (BenchEvent*)calloc(events, sizeof(*result));
  if (!result) {
    return NULL;
  }

  uint32_t state = 0x9E3779B9U;
  for (uint32_t i = 0U; i < events; ++i) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;

    result[i].id       = i;
    result[i].frame    = (uint32_t)(((uint64_t)i * frames) / events);
    result[i].instance = state % instances;
    result[i].port     = control_port;
    result[i].value    = (int32_t)(state % 2001U) - 1000;
  }

  return result;
}

static double
reduction_pct(double before, double after)
{
  return before > 0.0 ? 100.0 * (before - after) / before : 0.0;
}

static bool
run_tier(const Tier* tier,
         uint32_t    repeats,
         TierReport* report,
         FILE*       events_file)
{
  uint32_t   connections_count = 0U;
  Connection* connections = make_connections(
    tier->instances, tier->audio_ports, &connections_count);
  if (!connections) {
    fprintf(stderr, "%s: failed to create connections\n", tier->name);
    return false;
  }

  const uint32_t control_port = 2U * tier->audio_ports;
  BenchEvent* const events = make_events(
    tier->instances, tier->events, tier->frames, control_port);
  if (!events) {
    free(connections);
    fprintf(stderr, "%s: failed to create events\n", tier->name);
    return false;
  }

  EventResult* const slow_results =
    (EventResult*)calloc(tier->events, sizeof(*slow_results));
  EventResult* const fast_results =
    (EventResult*)calloc(tier->events, sizeof(*fast_results));
  if (!slow_results || !fast_results) {
    free(slow_results);
    free(fast_results);
    free(events);
    free(connections);
    fprintf(stderr, "%s: failed to allocate results\n", tier->name);
    return false;
  }

  GraphPlan plan;
  plan.name            = tier->name;
  plan.descriptor      = &MOCK_DESCRIPTOR;
  plan.num_instances   = tier->instances;
  plan.audio_ports     = tier->audio_ports;
  plan.num_connections = connections_count;
  plan.num_events      = tier->events;
  plan.batch_frames    = tier->batch_frames;
  plan.port_count      = control_port + 1U;
  plan.control_port    = control_port;
  plan.output_port     = tier->audio_ports;
  plan.connections     = connections;
  plan.events          = events;

  Timing warmup;
  if (!run_slow_once(&plan, slow_results, &warmup) ||
      !run_fast_once(&plan, fast_results, &warmup)) {
    fprintf(stderr, "%s: warmup failed\n", tier->name);
    free(slow_results);
    free(fast_results);
    free(events);
    free(connections);
    return false;
  }

  Timing slow_timings[MAX_REPEATS];
  Timing fast_timings[MAX_REPEATS];
  for (uint32_t i = 0U; i < repeats; ++i) {
    if (!run_slow_once(&plan, slow_results, &slow_timings[i])) {
      fprintf(stderr, "%s: slow run failed\n", tier->name);
      free(slow_results);
      free(fast_results);
      free(events);
      free(connections);
      return false;
    }

    if (!run_fast_once(&plan, fast_results, &fast_timings[i])) {
      fprintf(stderr, "%s: fast run failed\n", tier->name);
      free(slow_results);
      free(fast_results);
      free(events);
      free(connections);
      return false;
    }
  }

  report->tier        = tier;
  report->connections = connections_count;
  report->port_count  = plan.port_count;
  report->slow.build_ms =
    median_timing(slow_timings, repeats, TIMING_BUILD);
  report->slow.validate_ms = 0.0;
  report->slow.prepare_ms  = 0.0;
  report->slow.event_ms =
    median_timing(slow_timings, repeats, TIMING_EVENT);
  report->slow.cleanup_ms =
    median_timing(slow_timings, repeats, TIMING_CLEANUP);
  report->slow.total_ms =
    median_timing(slow_timings, repeats, TIMING_TOTAL);
  report->fast.build_ms =
    median_timing(fast_timings, repeats, TIMING_BUILD);
  report->fast.prepare_ms =
    median_timing(fast_timings, repeats, TIMING_PREPARE);
  report->fast.validate_ms =
    median_timing(fast_timings, repeats, TIMING_VALIDATE);
  report->fast.event_ms =
    median_timing(fast_timings, repeats, TIMING_EVENT);
  report->fast.cleanup_ms =
    median_timing(fast_timings, repeats, TIMING_CLEANUP);
  report->fast.total_ms =
    median_timing(fast_timings, repeats, TIMING_TOTAL);
  report->build_reduction =
    reduction_pct(report->slow.build_ms, report->fast.build_ms);
  report->event_reduction =
    reduction_pct(report->slow.event_ms, report->fast.event_ms);
  report->total_reduction =
    reduction_pct(report->slow.total_ms, report->fast.total_ms);
  report->mismatches = compare_results(&plan, slow_results, fast_results);
  report->slow_digest = digest_results(slow_results, tier->events);
  report->fast_digest = digest_results(fast_results, tier->events);

  const uint32_t head = tier->events < 5U ? tier->events : 5U;
  const uint32_t tail =
    tier->events - head < 2U ? tier->events - head : 2U;
  report->sample_count = head + tail;
  for (uint32_t i = 0U; i < head; ++i) {
    report->sample_events[i] = events[i];
    report->slow_samples[i]  = slow_results[i];
    report->fast_samples[i]  = fast_results[i];
  }
  for (uint32_t i = 0U; i < tail; ++i) {
    const uint32_t source = tier->events - tail + i;
    const uint32_t target = head + i;
    report->sample_events[target] = events[source];
    report->slow_samples[target]  = slow_results[source];
    report->fast_samples[target]  = fast_results[source];
  }

  for (uint32_t band = 0U; band < BAND_COUNT; ++band) {
    const uint32_t start = (uint32_t)(((uint64_t)tier->events * band) /
                                     BAND_COUNT);
    const uint32_t end   = (uint32_t)(((uint64_t)tier->events * (band + 1U)) /
                                    BAND_COUNT);
    report->slow_bands[band] = digest_band(slow_results, start, end);
    report->fast_bands[band] = digest_band(fast_results, start, end);
  }

  if (events_file) {
    for (uint32_t i = 0U; i < tier->events; ++i) {
      const bool equal =
        slow_results[i].value == fast_results[i].value &&
        slow_results[i].sequence == fast_results[i].sequence;
      fprintf(events_file,
              "%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
              ",%" PRId32 ",%" PRIu32 ",%" PRId32 ",%" PRIu32 ",%d\n",
              tier->name,
              i,
              events[i].frame,
              events[i].instance,
              events[i].port,
              slow_results[i].value,
              slow_results[i].sequence,
              fast_results[i].value,
              fast_results[i].sequence,
              equal ? 1 : 0);
    }
  }

  free(slow_results);
  free(fast_results);
  free(events);
  free(connections);
  return true;
}

static void
print_samples(const TierReport* report)
{
  printf("  sample events (%s):\n", report->tier->name);
  printf("    event  inst  port  frame  slow(value,seq)  fast(value,seq)  "
         "equal\n");

  for (uint32_t i = 0U; i < report->sample_count; ++i) {
    printf("    %5" PRIu32 "  %4" PRIu32 "  %4" PRIu32 "  %5" PRIu32
           "  (%" PRId32 ",%" PRIu32 ")  (%" PRId32 ",%" PRIu32 ")  %s\n",
           report->sample_events[i].id,
           report->sample_events[i].instance,
           report->sample_events[i].port,
           report->sample_events[i].frame,
           report->slow_samples[i].value,
           report->slow_samples[i].sequence,
           report->fast_samples[i].value,
           report->fast_samples[i].sequence,
           report->slow_samples[i].value == report->fast_samples[i].value &&
               report->slow_samples[i].sequence ==
                 report->fast_samples[i].sequence
             ? "yes"
             : "no");
  }
}

static bool
write_json(FILE* file, const TierReport* reports, uint32_t count,
           uint32_t repeats)
{
  fprintf(file, "{\n  \"repeats\": %" PRIu32 ",\n  \"tiers\": [\n", repeats);

  for (uint32_t i = 0U; i < count; ++i) {
    const TierReport* const report = &reports[i];
    fprintf(file,
            "    {\n"
            "      \"name\": \"%s\",\n"
            "      \"instances\": %" PRIu32 ",\n"
            "      \"audio_ports\": %" PRIu32 ",\n"
            "      \"connections\": %" PRIu32 ",\n"
            "      \"events\": %" PRIu32 ",\n"
            "      \"baseline\": {\"validate_ms\": %.6f, \"build_ms\": %.6f, "
            "\"prepare_ms\": %.6f, \"dispatch_ms\": %.6f, "
            "\"cleanup_ms\": %.6f, \"total_ms\": %.6f},\n"
            "      \"batched\": {\"validate_ms\": %.6f, \"build_ms\": %.6f, "
            "\"prepare_ms\": %.6f, \"dispatch_ms\": %.6f, "
            "\"cleanup_ms\": %.6f, \"total_ms\": %.6f},\n"
            "      \"reduction_pct\": {\"build\": %.3f, \"dispatch\": %.3f, "
            "\"total\": %.3f},\n"
            "      \"events_equal\": %s,\n"
            "      \"mismatches\": %" PRIu32 ",\n"
            "      \"slow_digest\": \"%016" PRIx64 "\",\n"
            "      \"fast_digest\": \"%016" PRIx64 "\",\n",
            report->tier->name,
            report->tier->instances,
            report->tier->audio_ports,
            report->connections,
            report->tier->events,
            report->slow.validate_ms,
            report->slow.build_ms,
            report->slow.prepare_ms,
            report->slow.event_ms,
            report->slow.cleanup_ms,
            report->slow.total_ms,
            report->fast.validate_ms,
            report->fast.build_ms,
            report->fast.prepare_ms,
            report->fast.event_ms,
            report->fast.cleanup_ms,
            report->fast.total_ms,
            report->build_reduction,
            report->event_reduction,
            report->total_reduction,
            report->mismatches == 0U ? "true" : "false",
            report->mismatches,
            report->slow_digest,
            report->fast_digest);

    fprintf(file, "      \"bands\": [\n");
    for (uint32_t band = 0U; band < BAND_COUNT; ++band) {
      fprintf(file,
              "        {\"start\": %" PRIu64 ", \"slow\": \"%016" PRIx64
              "\", \"fast\": \"%016" PRIx64 "\", \"equal\": %s}%s\n",
              (uint64_t)report->tier->events * band / BAND_COUNT,
              report->slow_bands[band],
              report->fast_bands[band],
              report->slow_bands[band] == report->fast_bands[band] ? "true"
                                                                   : "false",
              band + 1U < BAND_COUNT ? "," : "");
    }
    fprintf(file, "      ]\n    }%s\n", i + 1U < count ? "," : "");
  }

  fprintf(file, "  ]\n}\n");
  return !ferror(file);
}

static bool
write_summary_csv(FILE* file, const TierReport* reports, uint32_t count)
{
  fprintf(file,
          "tier,instances,audio_ports,connections,events,"
          "baseline_validate_ms,baseline_build_ms,baseline_prepare_ms,"
          "baseline_dispatch_ms,baseline_cleanup_ms,baseline_total_ms,"
          "batched_validate_ms,batched_build_ms,batched_prepare_ms,"
          "batched_dispatch_ms,batched_cleanup_ms,batched_total_ms,"
          "build_reduction_pct,"
          "dispatch_reduction_pct,total_reduction_pct,mismatches,equal,"
          "slow_digest,fast_digest\n");

  for (uint32_t i = 0U; i < count; ++i) {
    const TierReport* const report = &reports[i];
    fprintf(file,
            "%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
            ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.3f,%.3f,%.3f,"
            "%" PRIu32 ",%d,%016" PRIx64 ",%016" PRIx64 "\n",
            report->tier->name,
            report->tier->instances,
            report->tier->audio_ports,
            report->connections,
            report->tier->events,
            report->slow.validate_ms,
            report->slow.build_ms,
            report->slow.prepare_ms,
            report->slow.event_ms,
            report->slow.cleanup_ms,
            report->slow.total_ms,
            report->fast.validate_ms,
            report->fast.build_ms,
            report->fast.prepare_ms,
            report->fast.event_ms,
            report->fast.cleanup_ms,
            report->fast.total_ms,
            report->build_reduction,
            report->event_reduction,
            report->total_reduction,
            report->mismatches,
            report->mismatches == 0U ? 1 : 0,
            report->slow_digest,
            report->fast_digest);
  }

  return !ferror(file);
}

static bool
parse_u32(const char* text, uint32_t* value)
{
  char*          end    = NULL;
  unsigned long parsed = strtoul(text, &end, 10);
  if (!text[0] || !end || *end || parsed > UINT32_MAX) {
    return false;
  }

  *value = (uint32_t)parsed;
  return true;
}

static void
usage(const char* program)
{
  fprintf(stderr,
          "Usage: %s [--repeats N] [--tier all|small|medium|large] "
          "[--json PATH] [--csv PATH] [--events-out PATH]\n",
          program);
}

int
main(int argc, char** argv)
{
  uint32_t    repeats    = 5U;
  const char* tier_name  = "all";
  const char* json_path  = NULL;
  const char* csv_path   = NULL;
  const char* events_path = NULL;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--repeats") && i + 1 < argc) {
      if (!parse_u32(argv[++i], &repeats) || repeats == 0U ||
          repeats > MAX_REPEATS) {
        fprintf(stderr, "invalid repeat count\n");
        return 2;
      }
    } else if (!strcmp(argv[i], "--tier") && i + 1 < argc) {
      tier_name = argv[++i];
    } else if (!strcmp(argv[i], "--json") && i + 1 < argc) {
      json_path = argv[++i];
    } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
      csv_path = argv[++i];
    } else if (!strcmp(argv[i], "--events-out") && i + 1 < argc) {
      events_path = argv[++i];
    } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  bool known_tier = !strcmp(tier_name, "all");
  for (uint32_t i = 0U; i < sizeof(TIERS) / sizeof(TIERS[0]); ++i) {
    if (!strcmp(tier_name, TIERS[i].name)) {
      known_tier = true;
    }
  }

  if (!known_tier) {
    fprintf(stderr, "unknown tier: %s\n", tier_name);
    return 2;
  }

  FILE* events_file = NULL;
  if (events_path) {
    events_file = fopen(events_path, "w");
    if (!events_file) {
      fprintf(stderr, "failed to open %s\n", events_path);
      return 2;
    }

    fprintf(events_file,
            "tier,event,frame,instance,port,baseline_value,"
            "baseline_sequence,batched_value,batched_sequence,equal\n");
  }

  TierReport reports[sizeof(TIERS) / sizeof(TIERS[0])];
  uint32_t   report_count = 0U;

  printf("LV2 host graph benchmark (median of %" PRIu32 " runs)\n", repeats);
  printf("tier      inst  ports  conns  events    build(ms)  dispatch(ms)  "
         "total(ms)  vs-base   equal\n");

  for (uint32_t i = 0U; i < sizeof(TIERS) / sizeof(TIERS[0]); ++i) {
    if (strcmp(tier_name, "all") && strcmp(tier_name, TIERS[i].name)) {
      continue;
    }

    TierReport* const report = &reports[report_count];
    if (!run_tier(&TIERS[i], repeats, report, events_file)) {
      if (events_file) {
        fclose(events_file);
      }
      return 1;
    }

    printf("%-9s %4" PRIu32 "  %5" PRIu32 "  %5" PRIu32 "  %7" PRIu32
           "  %9.3f  %12.3f  %9.3f  %7.1f%%  %s\n",
           report->tier->name,
           report->tier->instances,
           report->tier->audio_ports,
           report->connections,
           report->tier->events,
           report->slow.build_ms,
           report->slow.event_ms,
           report->slow.total_ms,
           report->total_reduction,
           report->mismatches == 0U ? "yes" : "no");
    printf("          batched: validate %.3f ms, build %.3f ms, prepare "
           "%.3f ms, dispatch %.3f ms, total %.3f ms; reduction build "
           "%.1f%%, dispatch %.1f%%\n",
           report->fast.validate_ms,
           report->fast.build_ms,
           report->fast.prepare_ms,
           report->fast.event_ms,
           report->fast.total_ms,
           report->build_reduction,
           report->event_reduction);

    uint32_t equal_bands = 0U;
    for (uint32_t band = 0U; band < BAND_COUNT; ++band) {
      if (report->slow_bands[band] == report->fast_bands[band]) {
        ++equal_bands;
      }
    }

    printf("          events equal: %s (%" PRIu32 "/%" PRIu32
           "), bands equal: %" PRIu32 "/%" PRIu32
           ", digest: %016" PRIx64 " == %016" PRIx64 "\n",
           report->mismatches == 0U ? "yes" : "no",
           report->tier->events - report->mismatches,
           report->tier->events,
           equal_bands,
           BAND_COUNT,
           report->slow_digest,
           report->fast_digest);
    print_samples(report);

    ++report_count;
  }

  if (events_file) {
    const bool wrote_events = !ferror(events_file);
    fclose(events_file);
    if (!wrote_events) {
      fprintf(stderr, "failed to write %s\n", events_path);
      return 2;
    }
  }

  if (csv_path) {
    FILE* const file = fopen(csv_path, "w");
    if (!file || !write_summary_csv(file, reports, report_count)) {
      fprintf(stderr, "failed to write %s\n", csv_path);
      if (file) {
        fclose(file);
      }
      return 2;
    }
    fclose(file);
  }

  if (json_path) {
    FILE* const file = fopen(json_path, "w");
    if (!file || !write_json(file, reports, report_count, repeats)) {
      fprintf(stderr, "failed to write %s\n", json_path);
      if (file) {
        fclose(file);
      }
      return 2;
    }
    fclose(file);
  }

  for (uint32_t i = 0U; i < report_count; ++i) {
    if (reports[i].mismatches != 0U) {
      return 1;
    }
  }

  return 0;
}
