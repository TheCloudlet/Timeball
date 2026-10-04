# Timeball

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C++20](https://img.shields.io/badge/c%2B%2B-20-blue.svg)
![CI/CD](https://github.com/TheCloudlet/Timeball/workflows/CI%2FCD%20Pipeline/badge.svg)
![Format](https://github.com/TheCloudlet/Timeball/workflows/Format%20Check/badge.svg)

**Timeball is a timing engine that you link into your own simulator.** You
describe a chip as a graph of connected units, and Timeball computes when each
access completes. Your simulator runs the program.

## The name

A [time ball](https://en.wikipedia.org/wiki/Time_ball) is a ball on a mast,
dropped at an agreed hour so every ship in the harbor can set its chronometer
by the same instant. The ball does not sail; it only keeps the time the others
read. Timeball plays that part for a simulator. Your simulator decides what
runs. Timeball keeps the one timeline that says when, and how long each access
waited.

## The problem

A functional simulator runs the program and checks the results. A timing model
says how long each step takes: memory latency, cache behavior, contention.

Large companies keep these as two models owned by two teams, because one model
cannot be both fast enough to run software and detailed enough to answer a
cycle-level question.

A small team has one simulator. gem5 or SystemC transaction-level modeling
(TLM) would model the whole system and replace the simulator you already have,
when all you need is access latency. So the timing goes inline, as a number
added per access. That number mixes with the execution logic, can only be
tested by running the whole simulator, and is written again for the next
project.

It is also wrong once anything is shared. Forty cycles per access holds while
accesses run one at a time. When two agents use the same resource, one waits,
and a fixed number cannot show the wait. Both agents need one timeline.

Timeball keeps that timeline in its own engine, linked into the simulator you
have. You test the engine by itself, compare it across workloads, and reuse it
on the next project.

## How it works

A host simulator keeps its architectural state and its correctness, and tells
Timeball what work happens: memory accesses, and any other operation with a cost
it already knows. Timeball puts all of it on one timeline and answers when each
piece completes.

If the memory graph looks like this:

```
+------------------------------------------------------------------------------+
|                         [0x00000000, 0x80000000)                             |
|  +----------------+          +---------------------------+                   |
|  | Core           |          | L1                        |                   |
|  | in order       +--------->| 64 sets, 8 ways, 64-byte  |                   |
|  +--+------+------+          | LRU, hit 4                |                   |
|     |      |                 +-------------+-------------+                   |
|     |      |                               |                                 |
|     |      | MMIO 0x40000000               | miss                            |
|     |      | size 0x100                    v                                 |
|     |      |                 +---------------------------+                   |
|     |      |                 | DRAM                      |                   |
|     |      |                 | latency 100               |                   |
|     |      |                 +---------------------------+                   |
|     |      v                                                                 |
|     |  +-------------------------------+                                     |
|     |  | MAC                           |                                     |
|     |  | params  0x00   0x08   0x10    |                                     |
|     |  | START   0x18   STATUS 0x20    |                                     |
|     |  | cost    M * N * K / 64        |                                     |
|     |  +-------------------------------+                                     |
|     |                                                                        |
|     | [0x80000000, 0x80010000)                                               |
|     v                                                                        |
|  +---------------------------+                                               |
|  | ScratchPad                |                                               |
|  | SPM, latency 5            |                                               |
|  | no tags, no misses        |                                               |
|  +---------------------------+                                               |
+------------------------------------------------------------------------------+
```

An address in `[0x00000000, 0x80000000)` goes through L1, and a miss continues
to DRAM. An address in `[0x80000000, 0x80010000)` stops at the scratchpad. The
MAC is a memory-mapped device at `0x40000000`, size `0x100`. A write to START,
at offset `0x18`, launches work costing `M * N * K / 64` cycles. A read of
STATUS, at offset `0x20`, waits until that work finishes. The core port takes
this window before the address map, so the store does not enter L1.

You build that graph as follows. The address map names the two memory
regions. The MAC is not a region on that map. `Attach` registers its window
on the core port, and a store to START launches the work.

```cpp
#include "timeball/core_port.hpp"
#include "timeball/timeball.hpp"
using namespace timeball;

// Nodes are constructed separately and wired together at run time.
Memory<"DRAM"> dram(100);
Memory<"SPM"> spm(5);
Cache<"L1", 64, 8, 64, LRUPolicy, 4> l1(&dram);

// Memory only. An address here is a load or a store, never a device register.
AddressMap map;
map.Map(0x00000000, 0x80000000, &l1);
map.Map(0x80000000, 0x80010000, &spm);

// Parameter registers, then START at 0x18 and STATUS at 0x20.
CommandDevice mac("mac", 3, 0x18, 0x20, [](const std::vector<uint64_t>& p) {
  return p[0] * p[1] * p[2] / 64;
});

EventEngine engine;
CorePort core_port(engine, map);
core_port.Attach(0x40000000, 0x100, mac);  // checked before the address map

core_port.OnLoad(0x1000);                  // L1, and DRAM on a miss
core_port.OnStore(0x40000000 + 0x00, 64);  // M
core_port.OnStore(0x40000000 + 0x08, 64);  // N
core_port.OnStore(0x40000000 + 0x10, 64);  // K
core_port.OnStore(0x40000000 + 0x18, 1);   // START launches M*N*K/64
core_port.OnLoad(0x40000000 + 0x20);       // STATUS waits for that work
core_port.Sync();
Cycle done = core_port.Now();
```

Adding a memory region changes no call site. Adding a node type changes nothing
in the engine.

Nothing is timed until the engine runs, because an answer can depend on work
not yet submitted: another agent's access reaching the same memory first. A
long-running host submits in windows: everything that may begin before a
cycle, then `RunUntil(cycle)`, a promise that nothing later begins earlier.
The engine serves the window and forgets what finished in it, so it holds
only what is in flight, and the result is exactly what submitting everything
at once would give.

### Time is absolute

Every piece of work — a compute step, a transfer, one hop of a memory access —
is an operation on a resource, and every completion is an absolute cycle.
Contention then has a direct expression:

```text
completion = max(dependencies complete, resource free) + cost
```

The cost is static, from the host's table, or dynamic, computed by the node an
access reaches: a cache hit costs its lookup, a miss continues to the next node.
An agent arriving while another holds a resource waits for it, in arrival order,
and the waiting is computed rather than estimated. This is the decision that
makes every other one possible — a relative latency has nowhere to put "it
waited".

### Work requiring several resources

An operation can require one unit of several resources at once. List the extra
resources in `additional_resources`; the existing `resource` is the primary
resource reported by `OperationResult::served_by`.

```cpp
ResourceId transfer = engine.AddResource({"transfer", 1});
ResourceId link = engine.AddResource({"link", 1});
ResourceId buffer = engine.AddResource({"buffer", 2});
// `earlier` is an EventId this engine already returned.
engine.Submit({"copy", transfer, 0, 12, {.after = {earlier}}, {link, buffer}});
```

All named resources must exist and be distinct. The operation waits without
holding any capacity until all resources can serve it, then holds one unit of
each for the same interval and completes once. On each resource, an earlier
waiter cannot be overtaken while waiting for another resource; unrelated
resources continue to work. Ties use priority, then submission order, as
single-resource work does.

The engine emits a record for every occupied resource, primary first, with the
same operation ID and service interval. Only the primary record carries
dependencies, so SQLite stores each dependency once. Count completions from
`RunResult::completed`, or group records by operation ID when counting work.

### Structure is compile-time, occupancy is runtime

A node's geometry, capacity and policy are template parameters, so the
per-access work — tag comparison, victim selection — stays fully inlined with no
indirect call. Nodes hold interface references to each other, so topology is
decided at run time and any graph is expressible, including several agents
sharing one memory.

The engine reaches a node through that interface once per hop; the lookup inside
it has compile-time geometry throughout. A cache hit is one hop and never
touches its successor.

This is a preference on the per-access path, not a system-wide guarantee. The
earlier claim that Timeball "eliminates virtual dispatch" described a design
that could not express a shared resource at all, and has been replaced.

### One record, kept by the engine

Nodes record nothing. The engine writes one record each time a resource is
occupied — a compute step, a transfer, one hop of a memory access — with when it
arrived, when it started and when it finished. Every report is a query over that
one stream, so two reports cannot disagree, and each record already separates
waiting (`start - arrival`) from working (`finish - start`).

A run can stream the records into a SQLite file as they happen: `ops` holds a
row per resource occupied, `deps` a row per dependency an operation waited on,
and `tasks` named spans of cycles. Questions nobody anticipated are answered by
querying rather than by rebuilding with new instrumentation:

```sql
-- The slowest operations end to end. An access is a row per hop, so group.
SELECT op, name, MAX(finish) - MIN(arrival) AS span
FROM ops WHERE op != 0 GROUP BY op ORDER BY span DESC LIMIT 10;

-- Where did the time go, per resource: waiting or working?
SELECT resource, SUM(start - arrival) AS waiting, SUM(finish - start) AS working
FROM ops GROUP BY resource ORDER BY waiting + working DESC;

-- What held up the multiply: the dependency that finished last.
SELECT p.name, p.finish FROM deps d
JOIN ops o ON d.op = o.op AND o.name = 'matmul'
JOIN ops p ON p.op = d.depends_on
ORDER BY p.finish DESC LIMIT 1;
```

Python's stdlib `sqlite3` reads the file with no bespoke parser.
`-DTIMEBALL_WITH_SQLITE=OFF` drops the store and the dependency entirely.

To combine timing records with caller-owned tables in one transaction, construct
`EventStore` with a reference to the caller's open `sqlite3` connection. This
replaces the store's reserved `ops`, `deps`, and `tasks` tables, but does not
change transaction or journal settings. The caller keeps ownership of the
connection, starts the transaction before constructing the store, and writes its
own tables keyed by operation ID. Check the store's `Close()` result before
committing the combined recording; it finalizes only the store's statements. The
caller must also check its metadata writes and final commit, and publish only a
complete recording. No caller-specific schema is added to Timeball.

## Node types

Built-in nodes are examples, not requirements. Each plays one of three roles. A
node that can receive a request is an `AccessNode`. An `Initiator` is a node and
is not one: it originates accesses, it does not serve them.

| Node         | Role        | Models                                            |
| ------------ | ----------- | ------------------------------------------------- |
| `Cache`      | Transformer | Tags, associativity, replacement; may forward     |
| `Memory` | Target      | Fixed latency, no tags, no misses. DRAM and a scratchpad are two of these |
| `AddressMap` | Transformer | Routes by address; adds no latency of its own     |
| `Initiator`  | Initiator   | Where accesses originate; in order, one at a time |

A new node type says what serving an access costs it and where the access goes
next (`Serve`), and optionally what changes once the data arrives (`Complete`,
such as a fill). It needs only `timeball/node.hpp` — the vocabulary, with
nothing about any particular kind of node. `Memory` is the proof this
works: it shares almost no implementation with a cache, and a scratchpad region
is the same node given its own latency.

## Quick start

### Prerequisites

- **C++20 compiler** (GCC 11+, Clang 12+)
- **CMake 3.10+**
- **SQLite3** (optional; `-DTIMEBALL_WITH_SQLITE=OFF` to build without)
- **Network access at configure time** (only with `-DTIMEBALL_BUILD_TESTS=ON`,
  which fetches GoogleTest)

### Build and test

```bash
git clone https://github.com/TheCloudlet/Timeball.git && cd Timeball
cmake -B build -DTIMEBALL_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build
```

## Embedding it

A functional simulator attaches at its core's load/store path, through a
`CorePort`. It reports what it already knows — the instructions it retires and the
loads and stores it performs — and nothing else about the host changes.
Accelerators the core programs through memory-mapped registers are attached
to the core port: a register write can launch work costed from what was written, and
a status read waits for that work, which is where the real core would have
spun.

`timeball/machine.hpp`'s `Machine` template is that wiring, written once: a
host supplies the cache's compile-time geometry as template
arguments and its own runtime numbers — DRAM latency, the core port's cost table —
as one `MachineConfig`, and gets a `Memory` + `Cache` + `EventEngine` +
`CorePort`, already connected.

```cpp
#include "timeball/machine.hpp"
#include "timeball/timeball.hpp"

using MyMachine = timeball::Machine<"L1", 64, 8, 64, timeball::LRUPolicy, 4>;

class YourSimulator {
  MyMachine machine_{{.dram_latency = 100}};
  timeball::CorePort& core_port_ = machine_.GetCorePort();
  // Sizes at 0x00..0x10, START at 0x18, STATUS at 0x20; M*N*K / 64 cycles.
  timeball::CommandDevice mac_{"mac", 3, 0x18, 0x20,
                              [](const std::vector<uint64_t>& p) {
                                return p[0] * p[1] * p[2] / 64;
                              }};

 public:
  YourSimulator() { machine_.Attach(0x4000'0000, 0x100, mac_); }

  // Called from where your simulator already retires instructions, loads and
  // stores.
  void OnRetire(uint64_t n) { core_port_.OnInstructions(n); }
  void OnLoad(uint64_t addr) { core_port_.OnLoad(addr); }
  void OnStore(uint64_t addr, uint64_t value) { core_port_.OnStore(addr, value); }

  // Now and then, and at the end: time what was reported, keep nothing else.
  timeball::Cycle Cycles() {
    core_port_.Sync();
    return core_port_.Now();
  }
};
```

A second machine — a different cache, a different DRAM latency, a different
accelerator — is a different file with a different `Machine<...>` and
`MachineConfig`: `timeball/machine.hpp`, `EventEngine` and `CorePort` never change.
One `CorePort` per engine: it promises the engine that nothing will begin before
its own core's time, which holds only while that core is the only source of
work. `examples/mmio_accelerator.cpp` is a complete one: a stand-in functional
simulator driving a vector unit and a MAC array, reporting where the time
went.

Nodes hold non-owning references to their successors, so construct bottom-up and
destroy top-down. A node holds its whole line array inline — a large last-level
cache is megabytes — so heap-allocate them rather than making them stack locals.

## Performance

`bench/throughput.cpp` times 2,000,000 accesses through L1, L2 and DRAM,
submitted and run in windows. One scenario per process, so the peak resident
size is that scenario's own:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/bin/throughput hits     # working set in L1
./build/bin/throughput misses   # 4 GB of addresses, memory not saturated
./build/bin/throughput agents   # three cores sharing L2 and DRAM
```

On one x86-64 Linux machine, best of three: 3.7, 2.3 and 2.7 million accesses
per second, in 3 to 7 MB. Submitting everything before running instead held
about 420 bytes per access: 844 MB for the same 2,000,000.

## Checking the engine

Two independent ways to ask "is the timing actually right", as distinct from
"did it match last time":

- **`timeball/checking_sink.hpp`** is a `RecordSink` that re-derives the run's
  own rule from what it reports, rather than trusting the engine applied it:
  every record holds its resource forward in time, no resource ever serves
  more than its capacity at once, a dependency finished before it was named,
  and a resource is never served out of arrival order. Attach it alongside
  any other sink in a test or a long run:

  ```cpp
  CheckingSink checker;
  engine.RunUntilIdle(&checker);
  assert(checker.Ok());
  ```

  A host running in windows calls `checker.Retire(horizon)` next to
  `engine.RunUntil(horizon, ...)`, so the checker's own memory stays bounded
  by what is in flight, the same way the engine's does.

- **`test/unit/reference_scheduler.hpp`** is a from-scratch, deliberately slow
  re-derivation of the scheduling rule (no heap, no per-resource queue, no
  windowing — it just asks who is waiting and who is free, one cycle at a
  time), checked against the real engine over hundreds of random programs in
  `test/unit/test_reference_scheduler.cpp`. It covers the scheduling layer — resources,
  dependencies, priorities, ties — not a node's own dynamic cost, which the
  hand-computed cache tests and the record-for-record comparison against a
  prior commit already cover.

## Recording and replaying a core port trace

`CorePort::OnInstructions`, `OnLoad` and `OnStore` are three instructions of a
small language: retire n instructions, load an address, store a value at an
address. `timeball/core_port_trace.hpp` reifies them as `CorePortEvent` — a
`std::variant<InstructionsRetired, MemoryLoad, MemoryStore>` — so a
functional simulator's calls can be recorded once and replayed against
different hardware parameters with no simulator run again:

```cpp
std::vector<CorePortEvent> recorded;  // pushed to as OnInstructions/OnLoad/OnStore
                                  // are called — see examples/core_port_replay.cpp
std::ofstream out(path);
WriteCorePortTrace(out, recorded);
```

```cpp
std::ifstream in(path);
if (auto events = ReadCorePortTrace(in)) {
  core_port.Apply(*events);  // or core_port.Apply(one_event) for a single CorePortEvent
} else {
  // malformed file: reported to the caller, rather than asserted away or thrown
}
```

The file is one line per event — `I <count>`, `L <addr>`, `S <addr>
<value>`, addresses and values in hex or decimal, blank lines and `#`
comments ignored — documented in full on `ReadCorePortTrace`'s declaration.
`examples/core_port_replay.cpp` records one run, then replays that one recording
against two different hardware configurations — faster, then slower — and
prints all three totals. `examples/transcript_replay.cpp` goes the other
direction: a transcript does not have to come from a run at all — it reads
one committed transcript (`examples/data/transcript.trace`, hand-written for
now) and replays it, unchanged, against two different chip graphs, to show
that the timing model answers to cache geometry and not just to what a
program did.

`examples/spike` replays one committed transcript on two machines that
share a DRAM latency and a hit latency and differ only in cache geometry:
one line, or the data working set. The transcript is a Spike commit log
converted into that core port language. A second hart in the log is rejected.
The printed cycles omit instruction fetch, and they are not a comparison
against silicon or RTL. Regeneration, which needs Spike, the proxy kernel,
and a RISC-V compiler, is described in `examples/spike/README.md`. The
test runs from the committed transcript and does not need Spike installed.

## Error handling

No exceptions. Configuration and the per-access path do not return errors:
Timeball is embedded in someone else's simulator, and throwing into a host
that does not expect exceptions behaves unpredictably, quite apart from
exceptions being frequently disabled in this domain. One query does return
`tl::expected`: `EventEngine::CompletionOf` is the cycle, or `kPending`, or
`kRetired`. That channel stops at the query. The engine does not
call `value()` on it.

Configuration mistakes are caught before anything runs, in this order:

1. **Compile time** where the value is a template parameter — a set count that
   is not a power of two is a build failure, not something to handle.
2. **Construction time** for everything else — overlapping address ranges, an
   unwired node. Asserts, so a debug build stops at the mistake and a release
   build carries no check.
3. **An explicit validation call** where a release build must also reject bad
   configuration — `AddressMap::Covers` and `EventEngine::Accepts`. It leaves
   the decision to the caller.

SQLite recording also has runtime I/O failures. After a run, call
`EventStore::Close()` to check the final commit; `Error()` gives the path and
SQLite diagnostic when initialization, writing, or committing fails. The
destructor still closes the store but cannot report whether it succeeded.

## Project structure

```
timeball/
├── include/timeball/
│   ├── timeball.hpp         # What a host includes to embed the engine
│   ├── ids.hpp             # Cycle, EventId, ResourceId, InitiatorId
│   ├── node.hpp            # AccessNode, Request, Route, Writeback
│   ├── event_engine.hpp    # The one timeline: resources, operations
│   ├── initiator.hpp       # Where accesses come from
│   ├── core_port.hpp             # A core's load/store path, and MMIO devices
│   ├── core_port_trace.hpp       # CorePortEvent: a core port's calls, recorded and replayed
│   ├── machine.hpp         # One cache, one memory, one core port
│   ├── record_query.hpp    # filter, group, and fold over records
│   ├── names.hpp           # Compile-time node names
│   ├── cache.hpp           # Cache node
│   ├── memory.hpp          # Fixed-latency memory. DRAM and a scratchpad
│   ├── address_map.hpp     # Routes an access by address
│   ├── event_store.hpp     # SQLite sink for the engine's records (opt-in)
│   ├── checking_sink.hpp   # A sink that verifies the engine's own rule
│   └── policies.hpp        # Replacement policies (LRU, FIFO, Random)
├── src/timeball/            # Non-template engine code (libtimeball_engine)
├── examples/               # A functional simulator hooked at its core port
├── bench/                  # Throughput and footprint
└── test/                   # Unit tests
```

Node types are templates and live in headers. Everything that need not be —
the timeline, routing, the store — compiles once into `libtimeball_engine`.

## Scope

**In scope:** the timing of accesses crossing between units. Contention for a
shared resource, occupancy on both sides, per-agent attribution, and a queryable
record of what happened.

**Out of scope, deliberately:**

- **Anything inside a host simulator's execution** — instruction sets,
  pipelines, hazards, architectural state. The host already has these, and
  duplicating them would make the engine large enough to stop being embeddable.
- **Control inversion.** The host submits work and then runs the engine, so
  timing cannot feed back into what the host decides to submit. For a program
  submitted up front this is exact. A host that must choose each
  agent's next step from the time the last one took needs the engine to drive
  and call back.
- **Out-of-order agents.** Agents are in-order with a single outstanding access.
- **Priority arbitration.** A resource serves in arrival order. An initiator's
  priority only breaks ties between accesses arriving at the same cycle; no
  request carries a priority of its own.

## Contributing

Areas of interest: additional replacement policies (PLRU, RRIP), prefetcher
models, priority arbitration, and node types for memories this engine has not
met yet.

Tests are off by default, so a plain build needs no network. Turn them on to
run them:

```bash
cmake -B build -DTIMEBALL_BUILD_TESTS=ON && cmake --build build
clang-format -i include/timeball/*.hpp src/timeball/*.cpp
ctest --test-dir build
```

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
and include tests that assert timing facts through the node interface rather
than inspecting internals.

## License

MIT License — see LICENSE file for details.

## Citation

```bibtex
@software{timeball2025,
  title={Timeball: An Embeddable Timing Engine for Chip Topologies},
  author={TheCloudlet},
  year={2025},
  url={https://github.com/TheCloudlet/Timeball}
}
```
