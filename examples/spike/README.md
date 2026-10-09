# One transcript, two caches

`two_machines` reads `program.trace` and replays it on two machines. Both use
the same DRAM latency and the same hit latency. One cache holds a single
64-byte line. The other holds 64 KiB, the data working set of `scan.c`.
It prints both completion cycles.

Those cycles omit instruction fetch. A commit log records retired
instructions and their data accesses, and the converter does not invent
fetches. The numbers are timeball timing that transcript. They are not a
comparison against silicon or RTL.

The test locks only the order: the one-line cache finishes later. It does
not lock the cycle counts, and it does not run Spike.

## What is modeled

```
 Spike (external; one hart, rv64gc, cache models off) running pk + scan.c
   | --log-commits -> commit.log
   v
 commit_log -> program.trace (CorePortEvents: retire, load, store)
   | replayed; Spike is not run again
   v
 timeball Machine
   CorePort (cpi = 1)            data accesses only, no instruction fetch
     v
   L1 data cache, 64 B lines, LRU, hit = 1 cycle
     one line (1 set x 1 way)  or  64 KiB (256 sets x 4 ways)
     v miss
   DRAM, 100 cycles
```

There is no instruction cache, L2, TLB, bus, or second core. Only what the
commit log records is timed.

## Regenerating the transcript

Spike, the proxy kernel (`pk`), and `riscv64-unknown-elf-gcc` are external
tools. They are not in this repo, and the test does not need them installed.

Build Spike with commit logging (`configure --enable-commitlog` on a tree
that still gates it). Leave Spike's cache models off: do not pass `--ic`,
`--dc`, `--l2`, or `--log-cache-miss`. Do not pass `-l`; that mixes a
disassembly trace into the log, and the converter rejects it.

```bash
riscv64-unknown-elf-gcc -O2 -o scan examples/spike/scan.c
spike --isa=rv64gc --log-commits --log=examples/spike/commit.log pk scan
cmake -B build && cmake --build build --target commit_log
./build/bin/commit_log examples/spike/commit.log examples/spike/program.trace
```

`commit.log` here is that capture: `scan.c`, one hart, Spike's cache
models left off. A log that names a second hart is rejected and is not
merged onto the one core port. `program.trace` is `commit_log`'s conversion of
`commit.log`. The test replays `program.trace` and does not run Spike.
Older Spike trees print commits only after `configure --enable-commitlog`.
