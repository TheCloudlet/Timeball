<p align="center">
  <img src="docs/img/cover.png" alt="Timeball: a timing engine for your simulator" width="720">
</p>

# Timeball

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C++20](https://img.shields.io/badge/c%2B%2B-20-blue.svg)
![CI/CD](https://github.com/TheCloudlet/Timeball/workflows/CI%2FCD%20Pipeline/badge.svg)
![Format](https://github.com/TheCloudlet/Timeball/workflows/Format%20Check/badge.svg)

**Timeball is a timing engine that you link into your own simulator.** You
describe a chip as a graph of connected units, and Timeball computes when each
access completes. Your simulator runs the program.

![One run of examples/mmio_accelerator in Perfetto: per-resource work lanes for
the core, L1, DRAM, a vector unit and a MAC array](docs/img/mmio-perfetto.png)

*`examples/mmio_accelerator` recorded to SQLite and opened in
[Perfetto](https://ui.perfetto.dev) with `tools/sqlite_to_perfetto.py`.*

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

If the address graph looks like this:

```
Core / other initiator -> AddressMap
                           | [0x00000000, 0x80000000) -> L1 -> DRAM
                           | [0x80000000, 0x80010000) -> ScratchPad
                           | [0x90000000, 0x90000100) -> MAC registers
                           |                              START -> MAC work
                           |                              STATUS waits for work
```

An address in `[0x00000000, 0x80000000)` goes through L1, and a miss continues
to DRAM. An address in `[0x80000000, 0x80010000)` stops at the scratchpad. The
MAC is a target at `0x90000000`, size `0x100`. A write to START, at offset
`0x18`, launches work costing
`M * N * K / 64` cycles; the value written is ignored. A read of STATUS, at
offset `0x20`, waits until that work finishes. Every access goes through the
address map, so a register access never enters L1.
`MapDevice` forwards an offset, so device-hop records show register offsets;
the address-map hop retains the full address. `Map` leaves addresses unchanged
for caches and memory.

You build that graph as follows. The address map names all three regions.

```cpp
#include "timeball/core_port.hpp"
#include "timeball/timeball.hpp"
using namespace timeball;

// Nodes are constructed separately and wired together at run time.
Memory<"DRAM"> dram(100);
Memory<"SPM"> spm(5);
Cache<"L1", 64, 8, 64, LRUPolicy, 4> l1(&dram);

constexpr uint64_t kMac = 0x90000000;
constexpr uint64_t kStart = 0x18;
constexpr uint64_t kStatus = 0x20;

// Three parameter registers, then START and STATUS.
CommandDevice mac("mac", InitiatorId{1}, 1, 3, kStart, kStatus,
                  [](const std::vector<uint64_t>& p) {
                    return p[0] * p[1] * p[2] / 64;
                  });

AddressMap map;
map.Map(0x00000000, 0x80000000, &l1);
map.Map(0x80000000, 0x80010000, &spm);
map.MapDevice(kMac, kMac + 0x100, &mac);

EventEngine engine;
CorePort core_port(engine, map);

core_port.OnLoad(0x1000);             // L1, and DRAM on a miss
core_port.OnStore(kMac + 0x00, 64);   // M
core_port.OnStore(kMac + 0x08, 64);   // N
core_port.OnStore(kMac + 0x10, 64);   // K
core_port.OnStore(kMac + kStart, 0);  // any value; launches M*N*K/64 cycles
core_port.OnLoad(kMac + kStatus);     // waits for that work
core_port.Sync();
Cycle done = core_port.Now();       // 4205
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

### Dependencies with a delay

`When::after` starts work once every listed event has completed.
`When::after_delay` lists events that must complete and then a further number
of cycles before the work may arrive, such as a signal that takes a fixed time
to cross a link. Both can be mixed, and the work arrives after the latest of
them.

```cpp
// Arrives 5 cycles after `earlier` completes.
engine.Submit({"notify", core, 0, 1, {.after_delay = {{earlier, 5}}}});
```

### Binding a node to a host resource

By default an access node makes its own resource, named for the node. To make
host operations and accesses contend for one existing resource, create it with
`AddResource` and call `engine.BindAccessNode(node, resource)`. The node and the
resource must have the same name, the resource must exist, and a node can be
bound once. Records for both then name the same `resource_id`.

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
and `tasks` named spans of cycles. Device-work rows carry a `work` id and the
`parent` operation id of the access that launched them; ordinary rows have
zeros in both columns. Questions nobody anticipated are answered by querying
rather than by rebuilding with new instrumentation:

```sql
-- The slowest operations end to end. An access is a row per hop, so group.
SELECT op, name, MAX(finish) - MIN(arrival) AS span
FROM ops WHERE op != 0 GROUP BY op ORDER BY span DESC LIMIT 10;

-- Where did the time go, per resource: waiting or working?
SELECT resource, SUM(start - arrival) AS waiting, SUM(finish - start) AS working
FROM ops GROUP BY resource ORDER BY waiting + working DESC;

-- Per-device-work latency through its final work event, excluding later writebacks.
SELECT work, parent,
       MAX(CASE WHEN name = 'work' THEN finish END) - MIN(arrival) AS span
FROM ops WHERE work != 0 GROUP BY work, parent ORDER BY work;

-- What held up the multiply: the dependency that finished last.
SELECT p.name, p.finish FROM deps d
JOIN ops o ON d.op = o.op AND o.name = 'matmul'
JOIN ops p ON p.op = d.depends_on
ORDER BY p.finish DESC LIMIT 1;
```

Every record also carries `resource_id`, the engine's id for the resource it
describes. Names are labels and two resources may share one, so a checker or
analysis that must tell resources apart should key on the id, not the name. The
SQLite store keeps the name.

A caller that owns its own tables may add a nullable `metadata` column to `ops`
holding one JSON object per row. Timeball never writes it; the Perfetto
converter shows each key as a named argument on that row's slice, and rejects a
row whose metadata is not valid JSON, naming the row.

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

### Viewing a recording in Perfetto

`tools/sqlite_to_perfetto.py` turns a recording into a trace that
[ui.perfetto.dev](https://ui.perfetto.dev) opens. It reads only `ops`, `deps`
and `tasks`. It lives in its own [uv](https://docs.astral.sh/uv/) project, so
the C++ build never needs Python:

```bash
cd tools
uv run sqlite_to_perfetto.py run.sqlite -o run.pftrace
uv run sqlite_to_perfetto.py run.sqlite --task steady --resource DRAM --flows
```

One cycle is drawn as one nanosecond, so the timestamps in the UI are cycles.
The tracks are:

- `tasks`: one slice per `tasks` row.
- One group per resource, with:
  - `work`: one slice per row, from `start` to `finish`;
  - `wait`: from `arrival` to `start`, as long as no more than two rows wait
    there at once;
  - `queue`: otherwise, a counter of the rows waiting there.

Overlapping slices are split into more lanes, named `work 2`, `wait 2`, and so
on.

Clicking a slice shows its row: `op`, `name`, `resource`, `initiator`, `addr`,
`depth`, `forwarded`, `arrival`, `start`, `finish`, `wait_cycles`,
`work_cycles`, `work`, `parent`.
`row` is its rowid in `ops`. It also shows `depends_on`, each dependency with
its name and finish cycle, and `released_by`, the dependency that finished
last.

Clicking a `queue` sample shows how many rows were waiting. It lists the
oldest 16, each with its wait so far (`waited`) and in total (`wait_cycles`);
`--queue-detail` changes how many.

`--flows` draws an arrow per dependency, and `--wait-slices` draws every wait
as slices instead of a counter. `--from`/`--to`, `--task` and `--resource`
keep only what overlaps the given cycles or resources. Whether a resource
gets a counter is decided over the waits that are drawn, so a narrow range can
show as slices a resource that is crowded elsewhere.

Measured on a recording of 1.5 M rows:

- converting takes 2 min 13 s, with a peak of 44 MB, which does not grow with
  the run;
- the trace is 735 MB;
- trace processor loads it in 31 s using about 3.5 GB.

That is near the browser's limit, so narrow a large run with the filters, or
serve it with `trace_processor --httpd run.pftrace` and open ui.perfetto.dev,
which connects to it.

## Node types

Initiator and target are roles on each access, not fixed kinds of hardware. A
core load goes CPU -> L1 -> DRAM: L1 receives one hop and sends the next, but
`Request::initiator_id` stays the CPU because both hops serve its load. A core
write to DMA registers serves the CPU; the DMA's later read and write start new
accesses with the DMA as their origin. `Initiator` is a helper for issuing
in-order accesses; an `AccessNode` receives them.

| Node            | Models                                                                 |
|-----------------|------------------------------------------------------------------------|
| `Cache`         | Tags, associativity, replacement, and forwarding on a miss             |
| `Memory`        | Fixed latency without tags or misses, including DRAM or scratchpad     |
| `AddressMap`    | Routing by address with no added latency                               |
| `Initiator`     | In-order access issuance                                               |
| `CommandDevice` | Register accesses and fixed-cost work                                  |
| `DmaDevice`     | Register accesses and a one-unit read then write through its own entry |

An access has a type. `kLoad` and `kStore` read and write the addressed line.
`kHint` is a load whose hit leaves replacement order alone, so a probe does not
disturb what the cache would evict. `kFence` stops at the first cache or memory
and costs that node's hit latency. A cache built with the `InstructionCache`
template argument (the seventh, after `HitLatency`) also drops all its lines on
a fence; a data cache keeps its dirty data. Fixed-latency memory ignores the
type.

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
- **uv** (optional; only for the Perfetto converter in `tools/`, whose tests
  run with `cd tools && uv run pytest`)

### Build and test

```bash
git clone https://github.com/TheCloudlet/Timeball.git && cd Timeball
cmake -B build -DTIMEBALL_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build
```

### Using it from another CMake project

```cmake
add_subdirectory(third_party/timeball)
target_link_libraries(my_host PRIVATE Timeball::engine)
```

The target carries its include paths and requires C++20. When Timeball is not
the top-level project it leaves the host's compiler settings alone and builds
no examples or benchmark; `-DTIMEBALL_BUILD_EXAMPLES=ON` turns them on.

## Embedding it

A functional simulator attaches at its core's load/store path, through a
`CorePort`. It reports what it already knows — the instructions it retires and the
loads and stores it performs — and nothing else about the host changes.
Accelerators the core programs through memory-mapped registers are targets in
the same address map as memory. A register write can launch work costed from
what was written, and a status read waits for that work.

`DmaDevice` has source, destination, START, and STATUS registers at offsets
`0`, `8`, `0x10`, and `0x18`. Its constructor takes a device origin and an entry
node for its accesses. Pass shared DRAM or a map below the core's private L1;
the core can then program the DMA through its own map while both paths contend
for the shared memory. Map its registers with `MapDevice`; only the address map
knows the device's base. Register latency and per-access issue cost are separate.

`timeball/machine.hpp`'s `Machine` template owns an engine and core port. Its
default constructor also wires a cache and DRAM. For several regions, supply
an address map as its entry node:

```cpp
#include "timeball/machine.hpp"
#include "timeball/timeball.hpp"

using MyMachine = timeball::Machine<"L1", 64, 8, 64, timeball::LRUPolicy, 4>;

class YourSimulator {
  timeball::Memory<"DRAM"> dram_{100};
  timeball::Cache<"L1", 64, 8, 64, timeball::LRUPolicy, 4> l1_{&dram_};
  // Sizes at 0x00..0x10, START at 0x18, STATUS at 0x20; M*N*K / 64 cycles.
  timeball::CommandDevice mac_{"mac", timeball::InitiatorId{1}, 1, 3, 0x18,
                              0x20,
                              [](const std::vector<uint64_t>& p) {
                                return p[0] * p[1] * p[2] / 64;
                              }};
  timeball::AddressMap map_;
  MyMachine machine_{map_};
  timeball::CorePort& core_port_ = machine_.GetCorePort();

 public:
  YourSimulator() {
    map_.Map(0, 0x4000'0000, &l1_);
    map_.MapDevice(0x4000'0000, 0x4000'0100, &mac_);
  }

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

A second machine can use a different cache, DRAM latency, and accelerator
without changing `EventEngine` or `CorePort`.
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
│   ├── ids.hpp             # Cycle, EventId, WorkId, ResourceId, InitiatorId
│   ├── node.hpp            # AccessNode, Request, Route, Writeback
│   ├── event_engine.hpp    # The one timeline: resources, operations
│   ├── initiator.hpp       # Where accesses come from
│   ├── core_port.hpp       # A core's load/store path
│   ├── command_device.hpp  # Register target that launches work
│   ├── dma_device.hpp      # One-unit DMA target and initiator
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
├── tools/                  # SQLite recording to Perfetto trace (uv project)
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
