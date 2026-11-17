# Graph batching benchmark

This benchmark models a host-side graph build and event dispatch workload with
the real LV2 descriptor ABI.  It compares a one-at-a-time baseline with a
batched implementation that:

- validates the graph and event stream before allocating storage;
- instantiates all mock plugins and port arrays from one arena;
- replaces per-instance linear port lookup with direct indexed port access;
- stages events into a reusable per-block batch and calls `run()` once per
  active instance per block.

The mock plugin writes one result per event into a result array.  Both paths
are compared for every event, not just by a final checksum.

Run the benchmark from a configured build directory:

    meson compile graph_bench
    ./bench/graph_bench --repeats 5 --json graph_bench.json \
        --csv graph_bench.csv --events-out graph_bench_events.csv

The default three tiers are:

| Tier   | Instances | Audio ports | Connections | Events   | Batch frames |
|--------|-----------|-------------|-------------|----------|--------------|
| small  | 64        | 8           | 504         | 65,536   | 512          |
| medium | 256       | 16          | 4,080       | 262,144  | 2,048        |
| large  | 1,024     | 16          | 16,368      | 1,048,576| 8,192        |

The benchmark exits nonzero if any event differs.
