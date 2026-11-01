# TODO

- Benchmark throughput: file copy in (`guest-file-write`), file copy out (`guest-file-read`) and
  command output streaming (`guest-exec` with `capture-output`). The only numbers so far are the
  1 MiB round trip's time in `tests/iris/run_tests.py`, which is a test and not a measurement.
