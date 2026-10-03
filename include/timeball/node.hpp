// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_NODE_HPP
#define TIMEBALL_NODE_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "timeball/ids.hpp"

// What one node sends another, and what serving an access costs.
// A node type includes this and nothing else about its neighbours.

namespace timeball {

enum class AccessType : std::uint8_t { kLoad, kStore, kHint, kFence };

// What the sender knows: address, load or store, and who issued it.
// Start time, depth, and who waits are the engine's, and are not on here.
struct Request {
  uint64_t addr = 0;
  AccessType type = AccessType::kLoad;
  InitiatorId initiator_id;
};

class AccessNode;

// This node's service cost, and where the access goes next.
// null next: served here. The engine decides when.
struct Route {
  Cycle cost;
  AccessNode* next = nullptr;
  Request forward{};
};

// A dirty line evicted downward. No requester waits on it.
struct Writeback {
  AccessNode* to = nullptr;
  Request request{};
};

// A node that receives a request. An Initiator is not one.
// Computes cost and keeps the state that cost depends on. The engine owns
// the clock and the record. Connections are virtual; lookup inside a node
// stays inlined.
class AccessNode {
 public:
  virtual ~AccessNode() = default;

  // Resource name on the timeline, and the level a record reports.
  [[nodiscard]] virtual std::string_view NodeName() const = 0;

  // Called in service order, so state read here is as of this access.
  virtual Route Serve(const Request& r) = 0;

  // Called at completion, innermost node first. forwarded is true when this
  // node sent the access on. State that waits for data, such as a fill,
  // belongs here. Returns writebacks for the engine to schedule.
  virtual std::vector<Writeback> Complete(const Request& r, bool forwarded) {
    (void)r;
    (void)forwarded;
    return {};
  }
};

}  // namespace timeball

#endif  // TIMEBALL_NODE_HPP
