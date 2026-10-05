// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_NODE_HPP
#define TIMEBALL_NODE_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "timeball/ids.hpp"

// What one node sends another, and what serving an access costs. A node type
// includes this and nothing else about its neighbours.

namespace timeball {

// The kind of access. Fixed-latency memory ignores it. A cache does not.
enum class AccessType : std::uint8_t {
  kLoad,   // Reads the addressed line.
  kStore,  // Writes the addressed line.
  kHint,   // Like a load, but a hit leaves replacement order alone.
  kFence,  // Stops at the first cache or memory. An I-cache drops its lines.
};

// One access. The initiator and target are roles of each hop; initiator_id is
// the origin this chain serves, kept when a node forwards it. Delegated work
// starts a new chain with the device's own origin.
struct Request {
  uint64_t addr = 0;
  AccessType type = AccessType::kLoad;
  InitiatorId initiator_id;  // Origin, not the sender of this hop.
  uint64_t value = 0;        // Store value, if the target needs it.
};

class AccessNode;

struct WorkAccess {
  uint64_t addr = 0;
  AccessType type = AccessType::kLoad;
  uint64_t value = 0;
};

// Delegated work has a new origin. A fixed-cost job has no accesses; a job
// with accesses issues them in order through its own entry node. Each access
// pays issue_cycles on the work resource before entering the graph.
struct WorkDescription {
  InitiatorId origin;
  Cycle fixed_cycles = 0;
  AccessNode* entry = nullptr;
  Cycle issue_cycles = 0;
  std::vector<WorkAccess> accesses;
};

// This is what Serve returns. Forwarding keeps the request's origin; work
// delegates to a new origin. The engine schedules both after this hop.
struct Route {
  Cycle cost;                  // Cycle duration of this hop.
  AccessNode* next = nullptr;  // Null means this hop served the access.
  Request forward{};           // Sent to next. A cache miss sends a load.
  // Only demand accesses can launch work: a writeback has no event id to name
  // as the launching access in the work records.
  std::optional<WorkDescription> work;  // Launched when this hop finishes.
  // Serve decides a waiting read's result before the wait begins.
  bool wait_for_work = false;  // This hop waits for this target's last work.
};

// A dirty line evicted downward. No requester waits on it.
struct Writeback {
  AccessNode* to = nullptr;
  Request request{};
};

// A node that receives a request. An Initiator is not one. Computes cost and
// keeps the state that cost depends on. The engine owns the clock and the
// record. Connections are virtual; lookup inside a node stays inlined.
class AccessNode {
 public:
  virtual ~AccessNode() = default;

  // Resource name on the timeline, and the level a record reports.
  [[nodiscard]] virtual std::string_view NodeName() const = 0;

  // Called in service order, so state read here is as of this access.
  virtual Route Serve(const Request& r) = 0;

  // Called at completion, innermost node first. forwarded is true when this
  // node sent the access on. State that waits for data, such as a fill, belongs
  // here. Returns writebacks for the engine to schedule.
  virtual std::vector<Writeback> Complete(const Request& r, bool forwarded) {
    (void)r;
    (void)forwarded;
    return {};
  }
};

}  // namespace timeball

#endif  // TIMEBALL_NODE_HPP
