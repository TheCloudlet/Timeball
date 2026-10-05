// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_INITIATOR_HPP
#define TIMEBALL_INITIATOR_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "timeball/event_engine.hpp"
#include "timeball/node.hpp"

// Where accesses come from. Not part of node.hpp: a node type should not have
// to compile the agent that submits to it.

namespace timeball {

// In-order agent, one access outstanding. Not an AccessNode. Issue only
// submits. The next access starts when the previous one completed. Several
// initiators on one engine are ordered by arrival.
class Initiator {
  EventEngine* engine_;
  InitiatorId id_;
  Cycle ready_at_;
  // Breaks ties when this initiator's access arrives at a node at the same
  // cycle as another's — higher wins, DMA yields to cores. Never overrides an
  // earlier arrival.
  uint32_t priority_ = 0;
  EventId last_ = 0;  // 0: nothing issued yet

 public:
  // The engine must outlive the initiator.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Initiator(EventEngine& engine, InitiatorId id, Cycle ready_at = 0,
            uint32_t priority = 0)
      : engine_(&engine), id_(id), ready_at_(ready_at), priority_(priority) {}

  [[nodiscard]] InitiatorId Id() const { return id_; }
  [[nodiscard]] uint32_t Priority() const { return priority_; }

  // When the next access may start. After the last access is retired, the
  // horizon, because nothing may begin before it.
  [[nodiscard]] Cycle BusyUntil() const {
    if (last_ == 0) {
      return std::max(ready_at_, engine_->Horizon());
    }
    const auto completion = engine_->CompletionOf(last_);
    if (!completion) {
      // Pending in a release build must not answer with the horizon, or the
      // next access starts early. Saturate. Retired uses the horizon.
      assert(completion.error() == Absent::kRetired &&
             "BusyUntil asked before the last access has run");
      if (completion.error() == Absent::kPending) {
        return Cycle::Max();
      }
      return std::max(ready_at_, engine_->Horizon());
    }
    return *completion;
  }

  // Submits an access after the previous one completes.
  EventId Issue(AccessNode& into, uint64_t addr, AccessType type,
                uint64_t value = 0) {
    When when{.ready_cycle = std::max(ready_at_, engine_->Horizon()),
              .priority = priority_};
    if (last_ != 0) {
      const auto completion = engine_->CompletionOf(last_);
      // A retired predecessor is already bounded by the horizon.
      if (completion || completion.error() == Absent::kPending) {
        when.after.push_back(last_);
      }
    }
    last_ = engine_->SubmitAccess(
        into, {.addr = addr, .type = type, .initiator_id = id_, .value = value},
        std::move(when));
    return last_;
  }
};

}  // namespace timeball

#endif  // TIMEBALL_INITIATOR_HPP
