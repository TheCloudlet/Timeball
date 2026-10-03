// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/checking_sink.hpp"

#include <algorithm>
#include <format>

namespace timeball {

CheckingSink::CheckingSink(
    std::unordered_map<std::string, std::uint32_t> capacities)
    : capacities_(std::move(capacities)) {}

std::uint32_t CheckingSink::CapacityOf(std::string_view resource) const {
  const auto it = capacities_.find(std::string(resource));
  return it == capacities_.end() ? 1 : it->second;
}

void CheckingSink::OnRecord(const Record& record) {
  if (record.arrival > record.start || record.start > record.finish) {
    violations_.push_back(
        {std::format("op {} at {}: arrival {} > start {}, or start > finish "
                     "{} — a record must hold its resource forward in time",
                     record.op.value(), record.resource, record.arrival.value(),
                     record.start.value(), record.finish.value())});
  }

  // Only the first record of an operation or access carries what it waited
  // on; each dependency must already have finished no later than this
  // arrival, or the record's own cause-and-effect is broken. A dependency
  // this checker no longer holds is ambiguous once anything has been
  // retired — see ever_retired_ — so it is not accused past that point.
  for (EventId dep : record.after) {
    const auto it = op_finish_.find(dep);
    if (it == op_finish_.end()) {
      if (!ever_retired_) {
        violations_.push_back(
            {std::format("op {} depends on {}, which was never seen complete",
                         record.op.value(), dep.value())});
      }
    } else if (it->second > record.arrival) {
      violations_.push_back(
          {std::format("op {} arrived at {} but its dependency {} did not "
                       "finish until {}",
                       record.op.value(), record.arrival.value(), dep.value(),
                       it->second.value())});
    }
  }

  // Every arrival at this resource, past or present, must agree on order: no
  // later arrival is served ahead of an earlier one. Kept independently of
  // the capacity check below — an entry that has long since finished can
  // still be evidence that it was skipped over.
  std::vector<std::pair<Cycle, Cycle>>& order =
      order_history_[std::string(record.resource)];
  for (const auto& [arrival, start] : order) {
    const bool earlier_started_later =
        arrival < record.arrival && start > record.start;
    const bool later_started_earlier =
        record.arrival < arrival && record.start > start;
    if (earlier_started_later || later_started_earlier) {
      violations_.push_back({std::format(
          "at {}, an operation arriving at {} started at {}, "
          "and one arriving at {} started at {} — served out "
          "of arrival order",
          record.resource, std::min(arrival, record.arrival).value(),
          (arrival < record.arrival ? start : record.start).value(),
          std::max(arrival, record.arrival).value(),
          (arrival < record.arrival ? record.start : start).value())});
    }
  }
  order.emplace_back(record.arrival, record.start);

  // Capacity: retired as soon as a record can no longer overlap what is
  // kept — right here, since overlap is exactly what capacity is about.
  std::vector<Active>& active =
      active_by_resource_[std::string(record.resource)];
  std::erase_if(active,
                [&](const Active& a) { return a.finish <= record.start; });
  const std::uint32_t capacity = CapacityOf(record.resource);
  if (active.size() + 1 > capacity) {
    violations_.push_back(
        {std::format("resource {} served {} operations at once, more than "
                     "its capacity of {}",
                     record.resource, active.size() + 1, capacity)});
  }
  active.push_back({.op = record.op,
                    .arrival = record.arrival,
                    .start = record.start,
                    .finish = record.finish});

  if (record.op != 0) {
    // Overwritten on every hop of an access, so the value left once it stops
    // being called is its last hop's finish — the access's real completion.
    // A hop of one access can only be recorded once every earlier hop of the
    // same access already was (each is the previous one's dependent), so
    // this is never overwritten by an older value arriving late.
    op_finish_[record.op] = record.finish;
  }
}

void CheckingSink::Retire(Cycle before) {
  ever_retired_ = true;
  std::erase_if(op_finish_,
                [before](const auto& entry) { return entry.second < before; });
  for (auto& [resource, order] : order_history_) {
    std::erase_if(order,
                  [before](const auto& entry) { return entry.first < before; });
  }
}

}  // namespace timeball
