# dumpster

A single-header library providing a rudimentary garbage collection framework for C.

## Demo

The provided demo is a simple app meant to somewhat tax the garbage collector by prompting the user and repeatedly allocating memory then "leaking" it before allocating more. Statistics are provided for the memory usage to give more insight into how memory is used. Only one garbage collection pass is performed, just before the program terminates.

Video coming soon...

## Usage

The `dumpster.h` file can be added to a C project's include section and it should work, without needing to specify extra compilation units. All the program's logic is stored in the header.

You will need at least 3 lines in your program:
1. In your include section, add `#include "dumpster.h"`
2. In your program's entry point, call `dumpster_init()`
3. As necessary in functions, call `dumpster_collect_incremental()` or `dumpster_collect()`

Between `dumpster_collect_incremental()` and `dumpster_collect()`, you should use only one of them since they have different ways of marking memory blocks as used. In general, the incremental variant will have better latency (due to time-bounded collection), but worse overall throughput, which makes it more suitable for user interaction code and less so for walk-away computations.
