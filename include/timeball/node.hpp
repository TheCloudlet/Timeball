// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_NODE_HPP
#define TIMEBALL_NODE_HPP

#include <cstdint>
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

// What the sender knows. Timing (start, depth, waiters) is the engine's.
struct Request {
  uint64_t addr = 0;
  AccessType type = AccessType::kLoad;
  InitiatorId initiator_id;  // Identifies who issued the access.
  uint64_t value = 0;        // Store value, if the target needs it.
};

class AccessNode;

// This is what Serve returns. The engine forwards a non-null next later.
struct Route {
  Cycle cost;                  // Cycle duration of this hop.
  AccessNode* next = nullptr;  // Null means this hop served the access.
  Request forward{};           // Sent to next. A cache miss sends a load.
  Cycle work = 0;  // Background work launched when this hop finishes.
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
