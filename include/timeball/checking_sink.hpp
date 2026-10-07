// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_CHECKING_SINK_HPP
#define TIMEBALL_CHECKING_SINK_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "timeball/event_engine.hpp"

// Checks the engine's timing and device-work links against every record.
// Collects violations; it does not assert.
// Occupancy and arrival order are tracked per Record::resource_id, so two
// resources sharing a name are checked separately. Capacities are still given
// by name, so same-named resources share the one capacity listed for it.
// Keep one sink across calls that depend on earlier ids.

namespace timeball {

// One place the run's own rule did not hold.
struct Violation {
  std::string what;
};

class CheckingSink final : public RecordSink {
 public:
  // Capacity by Record::resource name, applied to every resource with that
  // name. Unlisted resources default to 1.
  explicit CheckingSink(
      std::unordered_map<std::string, std::uint32_t> capacities = {});

  void OnRecord(const Record& record) override;

  // Forgets work that finished before `before`. Pass the same cycle as
  // RunUntil.
  void Retire(Cycle before);

  [[nodiscard]] bool Ok() const { return violations_.empty(); }
  [[nodiscard]] const std::vector<Violation>& Violations() const {
    return violations_;
  }

 private:
  // One record still holding its resource, kept only until every earlier
  // arrival at that resource has been retired past it — bounded by the
  // resource's own capacity, not by how long the run has been.
  struct Active {
    EventId op = 0;
    Cycle arrival = 0;
    Cycle start = 0;
    Cycle finish = 0;
  };

  [[nodiscard]] std::uint32_t CapacityOf(std::string_view resource) const;

  std::unordered_map<std::string, std::uint32_t> capacities_;
  // Live resources only: an active list is added the first time its resource is
  // seen. Bounded by the number of distinct resources times their capacity:
  // retired as soon as a record's own start proves it can no longer overlap
  // what is kept, which is right for capacity but wrong for ordering — an
  // already-finished record can still have been served ahead of one that
  // arrived first, long after they stop overlapping. So arrival order is
  // checked against a second, separately kept history.
  std::unordered_map<ResourceId, std::vector<Active>> active_by_resource_;
  // Every (arrival, start) pair seen for a resource, checked against every new
  // one. Not retired by time — only Retire() prunes it, by arrival — so it
  // grows with records on that resource since the last Retire.
  std::unordered_map<ResourceId, std::vector<std::pair<Cycle, Cycle>>>
      order_history_;
  // A dependency's completion, so a later record's `after` can be checked
  // against it. Grows with distinct operations submitted since the last Retire;
  // a host that windows its own run keeps this bounded by windowing the checker
  // the same way.
  std::unordered_map<EventId, Cycle> op_finish_;
  // Once anything has been forgotten, a dependency this checker no longer holds
  // might be a real bug or might be one Retire() discarded — the two are
  // indistinguishable from here, so the checker stops accusing (a false "never
  // completed" would be worse than a missed one) and leaves that case to the
  // reference-scheduler property test, which never forgets.
  bool ever_retired_ = false;
  std::vector<Violation> violations_;
};

}  // namespace timeball

#endif  // TIMEBALL_CHECKING_SINK_HPP
