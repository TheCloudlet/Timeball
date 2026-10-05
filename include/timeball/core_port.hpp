// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_CORE_PORT_HPP
#define TIMEBALL_CORE_PORT_HPP

#include <cstdint>
#include <span>
#include <string_view>

#include "timeball/core_port_trace.hpp"
#include "timeball/event_engine.hpp"
#include "timeball/node.hpp"

// A core's load/store path. Retired instructions and the accesses between them
// become time. Every address enters the same graph.

namespace timeball {

// This core's own timing and identity. Targets price their own accesses.
struct CorePortConfig {
  Cycle cpi = 1;         // Each retired instruction takes this many cycles.
  InitiatorId core = 0;  // Tags this core's events and memory accesses.
};

// One in-order core. Each call waits for that core's previous work. One
// CorePort per engine. Sync runs to this core's time and promises nothing
// begins earlier. It does not interleave several cores.
class CorePort {
 public:
  // The engine and entry node must outlive the CorePort.
  CorePort(EventEngine& engine, AccessNode& entry, CorePortConfig config = {});

  // The core retired count instructions since the last call.
  void OnInstructions(uint64_t count);
  void OnLoad(uint64_t addr);
  void OnStore(uint64_t addr, uint64_t value);

  // Replay recorded OnInstructions, OnLoad, and OnStore calls. The stream can
  // be run against another CorePort without the functional simulator.
  void Apply(const CorePortEvent& event);
  void Apply(std::span<const CorePortEvent> events);

  // Times what has been submitted, then forgets what finished.
  void Sync(RecordSink* sink = nullptr);

  // The core's time: when its last work completed, as of the last Sync.
  [[nodiscard]] Cycle Now() const { return now_; }

 private:
  EventId CoreOp(std::string_view name, Cycle cost);
  void AddressAccess(uint64_t addr, AccessType type, uint64_t value = 0);

  EventEngine* engine_;
  AccessNode* entry_;
  CorePortConfig config_;
  ResourceId core_;
  EventId last_ = 0;
  Cycle now_ = 0;  // last_'s completion, read at the last Sync
};

}  // namespace timeball

#endif  // TIMEBALL_CORE_PORT_HPP
