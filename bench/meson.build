graph_bench = executable(
  'graph_bench',
  files('graph_bench.c'),
  dependencies: [lv2_dep],
  implicit_include_directories: false,
)

test(
  'graph_batching',
  graph_bench,
  args: ['--tier', 'small', '--repeats', '1'],
  suite: 'unit',
  timeout: 120,
)

benchmark(
  'graph_batching_bench',
  graph_bench,
  args: ['--repeats', '5'],
  suite: 'performance',
  timeout: 600,
)
