// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/event_engine.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "tl/expected.hpp"

namespace timeball {

namespace {

// A completion past the top of the range saturates rather than wrapping: a
// wrapped cycle would read as work finishing almost at once.
Cycle SaturatingAdd(Cycle start, Cycle cost) {
  return cost > Cycle::Max() - start ? Cycle::Max() : start + cost;
}

// The classic visitor-from-overload-set: gives std::visit an exhaustive,
// compile-checked case per alternative, so a branch for a fourth event kind
// added later and left unhandled here is a compile error, not a silent gap.
template <typename... Fns>
struct Overload : Fns... {
  using Fns::operator()...;
};
template <typename... Fns>
Overload(Fns...) -> Overload<Fns...>;

// std::get<T> re-checks the active alternative at runtime and throws if it
// disagrees — a check every call site here already knows the answer to, by
// construction, and pays for anyway. A disagreement here is an assert, not a
// throw the optimizer cannot see through. This is that assert, at every one of
// those call sites, in the one place.
template <typename T, typename V>
auto& Unwrap(V& v) {
  auto* p = std::get_if<T>(&v);
  assert(p != nullptr && "variant did not hold the expected alternative");
  return *p;
}

enum class Phase : std::uint8_t { kFinish, kWake, kStart };

Phase PhaseOf(const detail::Entry& entry) {
  return std::visit(
      Overload{
          [](const detail::FinishEvent&) { return Phase::kFinish; },
          [](const detail::WakeEvent&) { return Phase::kWake; },
          [](const detail::StartEvent&) { return Phase::kStart; },
      },
      entry.event);
}

template <typename T>
bool LaterOnTie(const T& a, const T& b) {
  if (a.priority != b.priority) {
    return a.priority < b.priority;
  }
  if (a.sequence != b.sequence) {
    return a.sequence > b.sequence;
  }
  return a.sub > b.sub;
}

}  // namespace

// Earliest arrival first; a tie to higher priority, then submission order.
bool detail::EarlierArrival::operator()(const StartEvent& a,
                                        const StartEvent& b) const {
  if (a.arrival != b.arrival) {
    return a.arrival < b.arrival;
  }
  return LaterOnTie(b, a);
}

// The queue's order is the timeline's order. Earlier first; at one cycle,
// finishing work before arriving work, so whatever a completion releases
// competes with everything else arriving then. Among arrivals, first come first
// served, then higher priority, then submission order. The phase rank is
// explicit, so variant declaration order does not affect it.
bool detail::Later::operator()(const Entry& a, const Entry& b) const {
  if (a.time != b.time) {
    return a.time > b.time;
  }
  const Phase a_phase = PhaseOf(a);
  const Phase b_phase = PhaseOf(b);
  if (a_phase != b_phase) {
    return a_phase > b_phase;
  }
  return std::visit(
      Overload{
          [&](const FinishEvent& av) {
            return LaterOnTie(av, Unwrap<FinishEvent>(b.event));
          },
          [&](const WakeEvent& av) {
            // The resource; wakes carry no other order.
            return av.resource > Unwrap<WakeEvent>(b.event).resource;
          },
          [&](const StartEvent& av) {
            return EarlierArrival{}(Unwrap<StartEvent>(b.event), av);
          },
      },
      a.event);
}

ResourceId EventEngine::AddResource(ResourceSpec spec) {
  assert(!spec.name.empty() && "Resource has no name");
  assert(spec.capacity > 0 && "Resource has no capacity");
  const auto id = static_cast<ResourceId>(resources_.size());
  resources_.push_back(
      {std::move(spec.name), std::vector<Cycle>(spec.capacity, Cycle{0})});
  return id;
}

void EventEngine::BindAccessNode(AccessNode& node, ResourceId resource) {
  assert(resource.value() < resources_.size() &&
         "Access node names an unknown resource");
  assert(node.NodeName() == resources_[resource.value()].name &&
         "Access node and resource names differ");
  assert(!node_resources_.contains(&node) && "Access node is already bound");
  node_resources_.emplace(&node, resource);
}

bool EventEngine::Knows(EventId id) const {
  return id >= 1 && id < next_id_;
}

// The earliest a piece of work with this ready cycle and these dependencies
// could begin, as far as is known now: a dependency still running gives no
// bound yet, so it counts as the end of time.
Cycle EventEngine::Earliest(const When& when) const {
  Cycle ready = when.ready_cycle;
  for (EventId d : when.after) {
    const auto it = live_.find(d);
    if (it == live_.end()) {
      continue;  // retired: completed before the horizon
    }
    ready = std::max(ready, it->second.done ? it->second.cycle : Cycle::Max());
  }
  for (const auto& delayed : when.after_delay) {
    const auto it = live_.find(delayed.event);
    if (it == live_.end()) {
      continue;
    }
    ready = std::max(
        ready, it->second.done ? SaturatingAdd(it->second.cycle, delayed.cycles)
                               : Cycle::Max());
  }
  return ready;
}

EventId EventEngine::NewId() {
  const EventId id = next_id_++;
  live_.emplace(id, Completion{});
  return id;
}

bool EventEngine::Accepts(const Operation& op) const {
  if (op.resource.value() >= resources_.size() || !Accepts(op.when)) {
    return false;
  }
  // ponytail: a few resources per operation; use a set if wide reservations
  // make this quadratic duplicate check show up in a profile.
  for (std::size_t i = 0; i < op.additional_resources.size(); ++i) {
    const ResourceId id = op.additional_resources[i];
    if (id.value() >= resources_.size() || id == op.resource ||
        std::find(op.additional_resources.begin(),
                  op.additional_resources.begin() + i,
                  id) != op.additional_resources.begin() + i) {
      return false;
    }
  }
  return true;
}

bool EventEngine::Accepts(const When& when) const {
  return std::ranges::all_of(when.after,
                             [this](EventId d) { return Knows(d); }) &&
         std::ranges::all_of(
             when.after_delay,
             [this, &when](const auto& d) {
               return Knows(d.event) && live_.contains(d.event) &&
                      std::find(when.after.begin(), when.after.end(),
                                d.event) == when.after.end() &&
                      std::count_if(when.after_delay.begin(),
                                    when.after_delay.end(),
                                    [&](const auto& other) {
                                      return other.event == d.event;
                                    }) == 1;
             }) &&
         Earliest(when) >= horizon_;
}

std::size_t EventEngine::NewJob(JobKind kind) {
  std::size_t index = 0;
  if (free_jobs_.empty()) {
    index = jobs_.size();
    jobs_.emplace_back();
  } else {
    index = free_jobs_.back();
    free_jobs_.pop_back();
    // Reset field by field: a reused slot's vectors keep their capacity, so it
    // allocates nothing. (Assigning Job{} would free them, and the next access
    // to land here would allocate its path again.) after is always there to
    // recover; path only if the slot's previous occupant was itself an access —
    // an operation never allocated one to reuse.
    Job& job = jobs_[index];
    std::vector<EventId> after = std::move(job.after);
    after.clear();
    std::vector<Hop> path;
    if (auto* access = std::get_if<AccessJob>(&job.kind)) {
      path = std::move(access->path);
      path.clear();
    }
    job = Job{};
    job.after = std::move(after);
    if (kind == JobKind::kAccess) {
      job.kind = AccessJob{.path = std::move(path)};
    }
    return index;
  }
  if (kind == JobKind::kAccess) {
    jobs_[index].kind = AccessJob{};
  }
  return index;
}

EventId EventEngine::Submit(const Operation& op) {
  assert(Accepts(op) && "Operation names an unknown resource or dependency");
  return Schedule(op);
}

// RunUntil advances the host horizon before processing earlier events; work
// generated by those events must still be scheduled at their own cycle.
EventId EventEngine::Schedule(const Operation& op) {
  const std::size_t index = NewJob(JobKind::kOperation);
  Job& job = jobs_[index];
  job.id = NewId();
  job.sequence = next_sequence_++;
  job.name = op.name;
  job.initiator = op.initiator;
  job.priority = op.when.priority;
  job.ready = op.when.ready_cycle;
  OperationJob operation{.resources = {op.resource},
                         .duration = op.duration_cycles};
  operation.resources.insert(operation.resources.end(),
                             op.additional_resources.begin(),
                             op.additional_resources.end());
  job.kind = std::move(operation);
  job.after = op.when.after;
  job.after_delay = op.when.after_delay;
  for (const auto& delayed : job.after_delay) {
    job.after.push_back(delayed.event);
  }
  const EventId id = job.id;
  Resolve(index);
  return id;
}

EventId EventEngine::SubmitAccess(AccessNode& entry, const Request& r,
                                  When when) {
  assert(Accepts(when) &&
         "Access depends on an unknown id or begins before the horizon");
  return ScheduleAccess(entry, r, std::move(when));
}

EventId EventEngine::ScheduleAccess(AccessNode& entry, const Request& r,
                                    When when) {
  const std::size_t index = NewJob(JobKind::kAccess);
  Job& job = jobs_[index];
  job.id = NewId();
  job.sequence = next_sequence_++;
  job.name = r.type == AccessType::kStore ? "store" : "load";
  job.initiator = r.initiator_id;
  job.priority = when.priority;
  job.ready = when.ready_cycle;
  job.after = std::move(when.after);
  job.after_delay = std::move(when.after_delay);
  for (const auto& delayed : job.after_delay) {
    job.after.push_back(delayed.event);
  }
  Unwrap<AccessJob>(job.kind).path.push_back({.node = &entry, .request = r});
  const EventId id = job.id;
  Resolve(index);
  return id;
}

void EventEngine::Enqueue(std::size_t causing_job, std::uint32_t priority,
                          std::uint32_t depth, Cycle at, AccessNode& to,
                          const Request& r) {
  const std::size_t index = NewJob(JobKind::kAccess);
  Job& job = jobs_[index];
  job.name = r.type == AccessType::kStore ? "store" : "load";
  // Ordered after the access that caused it, then by how much node traffic came
  // before it: both are the same however the host windows its work.
  job.sequence = jobs_[causing_job].sequence;
  job.sub = ++sends_;
  job.priority = priority;
  job.initiator = r.initiator_id;
  job.ready = at;
  Unwrap<AccessJob>(job.kind).base_depth = depth;
  Unwrap<AccessJob>(job.kind).path.push_back({.node = &to, .request = r});
  Arrive(index, job.ready);
}

void EventEngine::LaunchWork(AccessNode& node, WorkDescription work, Cycle at) {
  auto [target, inserted] = target_work_.try_emplace(&node);
  if (inserted) {
    target->second.resource =
        AddResource({std::string(node.NodeName()) + ".work", 1});
  }

  EventId previous = target->second.last;
  if (work.fixed_cycles > 0 || work.accesses.empty()) {
    When when{.ready_cycle = at};
    if (previous != 0) {
      when.after.push_back(previous);
    }
    previous = Schedule({"work", target->second.resource, work.origin,
                         work.fixed_cycles, std::move(when)});
  }
  if (work.accesses.empty()) {
    target->second.last = previous;
    return;
  }

  assert(work.entry != nullptr && "Access work has no entry node");
  for (const WorkAccess& access : work.accesses) {
    When issue_when{.ready_cycle = at};
    if (previous != 0) {
      issue_when.after.push_back(previous);
    }
    const EventId issue =
        Schedule({"issue", target->second.resource, work.origin,
                  work.issue_cycles, std::move(issue_when)});
    const Request request{.addr = access.addr,
                          .type = access.type,
                          .initiator_id = work.origin,
                          .value = access.value};
    previous = ScheduleAccess(*work.entry, request,
                              {.ready_cycle = at, .after = {issue}});
  }
  target->second.last = Schedule({"work",
                                  target->second.resource,
                                  work.origin,
                                  0,
                                  {.ready_cycle = at, .after = {previous}}});
}

// Folds in dependencies already complete; waits on the rest.
void EventEngine::Resolve(std::size_t index) {
  Job& job = jobs_[index];
  for (EventId d : job.after) {
    const auto delayed =
        std::find_if(job.after_delay.begin(), job.after_delay.end(),
                     [d](const auto& entry) { return entry.event == d; });
    const Cycle lag =
        delayed == job.after_delay.end() ? Cycle{0} : delayed->cycles;
    const auto it = live_.find(d);
    if (it == live_.end()) {
      continue;  // retired, so already satisfied
    }
    const Completion& c = it->second;
    if (c.done) {
      job.ready = std::max(job.ready, SaturatingAdd(c.cycle, lag));
    } else {
      ++job.unmet;
      waiting_[d].push_back(index);
    }
  }
  if (job.unmet == 0) {
    Arrive(index, job.ready);
  }
}

// The job reaches its resource at `at` — for an access, the node at the end of
// its path.
void EventEngine::Arrive(std::size_t index, Cycle at) {
  Job& job = jobs_[index];
  job.start_pending = true;
  if (auto* access = std::get_if<AccessJob>(&job.kind)) {
    access->path.back().arrival = at;
  } else {
    job.ready = at;
  }
  queue_.push({.time = at,
               .event = StartEvent{.arrival = at,
                                   .priority = job.priority,
                                   .sequence = job.sequence,
                                   .sub = job.sub,
                                   .job = index}});
}

std::span<const ResourceId> EventEngine::ResourcesOf(Job& job) {
  auto* access = std::get_if<AccessJob>(&job.kind);
  if (access == nullptr) {
    return Unwrap<OperationJob>(job.kind).resources;
  }
  const AccessNode* node = access->path.back().node;
  const auto it = node_resources_.find(node);
  if (it != node_resources_.end()) {
    access->resource = it->second;
  } else {
    access->resource = AddResource({std::string(node->NodeName()), 1});
    node_resources_.emplace(node, access->resource);
  }
  return {&access->resource, 1};
}

void EventEngine::Start(const Entry& e) {
  const StartEvent& start = Unwrap<StartEvent>(e.event);
  Job& job = jobs_[start.job];
  job.start_pending = false;
  const auto held = ResourcesOf(job);
  bool can_start = true;
  for (ResourceId id : held) {
    Resource& resource = resources_[id.value()];
    resource.waiting.insert(start);
    const Cycle free = *std::min_element(resource.free_cycles.begin(),
                                         resource.free_cycles.end());
    if (free > e.time) {
      ScheduleWake(id, e.time);
      can_start = false;
    }
    if (resource.waiting.begin()->job != start.job) {
      can_start = false;
    }
  }
  if (!can_start) {
    return;
  }

  Cycle cost;
  if (auto* access = std::get_if<AccessJob>(&job.kind)) {
    Hop& hop = access->path.back();
    if (!access->deferred) {
      access->route = hop.node->Serve(hop.request);
    }
    if (access->route.wait_for_work && !access->deferred) {
      const auto target = target_work_.find(hop.node);
      if (target != target_work_.end()) {
        const EventId last = target->second.last;
        const auto completion = live_.find(last);
        if (completion != live_.end() &&
            (!completion->second.done || completion->second.cycle > e.time)) {
          Resource& resource = resources_[held.front().value()];
          resource.waiting.erase(start);
          if (!resource.waiting.empty()) {
            ScheduleWake(held.front(), e.time);
          }
          access->deferred = true;
          job.after.push_back(last);
          // The hop reaches the register resource only after its dependency.
          if (completion->second.done) {
            Arrive(start.job, completion->second.cycle);
          } else {
            job.unmet = 1;
            waiting_[last].push_back(start.job);
          }
          return;
        }
      }
    }
    access->deferred = false;
    cost = access->route.cost;
  } else {
    cost = Unwrap<OperationJob>(job.kind).duration;
  }
  job.start = e.time;
  if (!job.started) {
    job.started = true;
    job.first_start = e.time;
  }
  const Cycle done = SaturatingAdd(e.time, cost);
  for (ResourceId id : held) {
    Resource& resource = resources_[id.value()];
    resource.waiting.erase(start);
    *std::min_element(resource.free_cycles.begin(),
                      resource.free_cycles.end()) = done;
    if (!resource.waiting.empty()) {
      ScheduleWake(id, e.time);
    }
  }
  queue_.push({.time = done,
               .event = FinishEvent{.priority = job.priority,
                                    .sequence = job.sequence,
                                    .sub = job.sub,
                                    .job = start.job}});
}

// Retry the first waiter. It stays queued at every required resource until all
// are available; several resource wakes enqueue only one start.
void EventEngine::Admit(Resource& resource, Cycle at) {
  if (resource.waiting.empty()) {
    return;
  }
  const StartEvent next = *resource.waiting.begin();
  Job& job = jobs_[next.job];
  if (job.start_pending) {
    return;
  }
  job.start_pending = true;
  queue_.push({.time = at, .event = next});
}

// The resource's next wake, at the cycle its first unit frees. At most one is
// pending per resource.
void EventEngine::ScheduleWake(ResourceId id, Cycle at) {
  Resource& resource = resources_[id.value()];
  if (resource.wake_pending) {
    return;
  }
  resource.wake_pending = true;
  at = std::max(at, *std::min_element(resource.free_cycles.begin(),
                                      resource.free_cycles.end()));
  queue_.push({.time = at, .event = WakeEvent{.resource = id}});
}

void EventEngine::Finish(const Entry& e, RunResult& run, RecordSink* sink) {
  const FinishEvent& finish = Unwrap<FinishEvent>(e.event);
  Job& job = jobs_[finish.job];
  Emit(job, e.time, sink);
  AccessJob* access = std::get_if<AccessJob>(&job.kind);
  if (access != nullptr && access->route.work) {
    LaunchWork(*access->path.back().node, std::move(*access->route.work),
               e.time);
  }
  if (access != nullptr && access->route.next != nullptr) {
    // Forwarded: the next node receives it as it leaves this one.
    access->path.push_back({access->route.next, access->route.forward});
    Arrive(finish.job, e.time);
    return;
  }
  if (access != nullptr) {
    // Served. Every node it visited learns so now, innermost first; any traffic
    // it sends back inherits this access's priority and sits one level below
    // the node that sent it.
    const std::uint32_t priority = job.priority;
    for (std::size_t i = access->path.size(); i-- > 0;) {
      const Hop& hop = access->path[i];
      const auto depth = access->base_depth + static_cast<uint32_t>(i) + 1;
      for (const Writeback& s :
           hop.node->Complete(hop.request, i + 1 != access->path.size())) {
        Enqueue(finish.job, priority, depth, e.time, *s.to, s.request);
      }
    }
  }
  Complete(finish.job, e.time, run, sink);
}

// index and at differ in meaning (a job slot and a cycle); one call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void EventEngine::Complete(std::size_t index, Cycle at, RunResult& run,
                           RecordSink* sink) {
  Job& job = jobs_[index];
  end_cycle_ = std::max(end_cycle_, at);
  const EventId id = job.id;
  if (id != 0) {
    live_[id] = {.cycle = at, .done = true};
    completed_.emplace(at, id);
    const auto* access = std::get_if<AccessJob>(&job.kind);
    const OperationResult result{
        .event_id = id,
        .name = job.name,
        .served_by =
            access != nullptr
                ? access->path.back().node->NodeName()
                : std::string_view(resources_[Unwrap<OperationJob>(job.kind)
                                                  .resources.front()
                                                  .value()]
                                       .name),
        .initiator = job.initiator,
        .ready_cycle =
            access != nullptr ? access->path.front().arrival : job.ready,
        .service_cycle = job.first_start,
        .completion_cycle = at,
    };
    run.completed.push_back(result);
  }
  if (auto* access = std::get_if<AccessJob>(&job.kind)) {
    access->path.clear();
  }
  free_jobs_.push_back(index);

  if (id == 0) {
    return;
  }
  const auto waiting = waiting_.find(id);
  if (waiting == waiting_.end()) {
    return;
  }
  const std::vector<std::size_t> released = std::move(waiting->second);
  waiting_.erase(waiting);
  for (std::size_t w : released) {
    Job& dependent = jobs_[w];
    const auto delayed =
        std::find_if(dependent.after_delay.begin(), dependent.after_delay.end(),
                     [id](const auto& entry) { return entry.event == id; });
    const Cycle lag =
        delayed == dependent.after_delay.end() ? Cycle{0} : delayed->cycles;
    dependent.ready = std::max(dependent.ready, SaturatingAdd(at, lag));
    if (--dependent.unmet == 0) {
      Arrive(w, dependent.ready);
    }
  }
}

// The record of the step that just finished: a static operation, or the
// access's current hop.
void EventEngine::Emit(const Job& job, Cycle finish, RecordSink* sink) {
  if (sink == nullptr) {
    return;
  }
  Record record{
      .op = job.id,
      .name = job.name,
      .initiator = job.initiator,
      .start = job.start,
      .finish = finish,
  };
  bool first = true;
  if (const auto* access = std::get_if<AccessJob>(&job.kind)) {
    const Hop& hop = access->path.back();
    record.resource = hop.node->NodeName();
    record.addr = hop.request.addr;
    record.depth =
        access->base_depth + static_cast<uint32_t>(access->path.size() - 1);
    record.forwarded = access->route.next != nullptr;
    record.arrival = hop.arrival;
    first = access->path.size() == 1;
  } else {
    record.arrival = job.ready;
    record.after = job.after;
    for (ResourceId id : Unwrap<OperationJob>(job.kind).resources) {
      record.resource = resources_[id.value()].name;
      sink->OnRecord(record);
      record.after = {};  // dependencies belong to the operation, not each row
    }
    return;
  }
  if (first) {
    record.after = job.after;
  }
  sink->OnRecord(record);
}

void EventEngine::Process(const Entry& e, RunResult& run, RecordSink* sink) {
  std::visit(Overload{
                 [&](const StartEvent&) { Start(e); },
                 [&](const FinishEvent&) { Finish(e, run, sink); },
                 [&](const WakeEvent& wake) {
                   Resource& resource = resources_[wake.resource.value()];
                   resource.wake_pending = false;
                   Admit(resource, e.time);
                 },
             },
             e.event);
}

RunResult EventEngine::RunUntil(Cycle horizon, RecordSink* sink) {
  RunResult run;
  horizon_ = std::max(horizon_, horizon);
  // Strictly before: work arriving exactly at the horizon may still be
  // submitted, and may win a tie there.
  while (!queue_.empty() && queue_.top().time < horizon_) {
    const Entry e = queue_.top();
    queue_.pop();
    Process(e, run, sink);
  }
  while (!completed_.empty() && completed_.top().first < horizon_) {
    live_.erase(completed_.top().second);
    completed_.pop();
  }
  run.end_cycle = end_cycle_;
  return run;
}

RunResult EventEngine::RunUntilIdle(RecordSink* sink) {
  RunResult run;
  while (!queue_.empty()) {
    const Entry e = queue_.top();
    queue_.pop();
    Process(e, run, sink);
  }
  run.end_cycle = end_cycle_;
  return run;
}

std::string_view EventEngine::ResourceName(ResourceId id) const {
  assert(id.value() < resources_.size() && "Name asked of an unknown resource");
  return resources_[id.value()].name;
}

tl::expected<Cycle, Absent> EventEngine::CompletionOf(EventId id) const {
  assert(Knows(id) && "Completion asked of an id this engine did not issue");
  // A release build strips the assert. Returning retired for an id that was
  // never issued would let the caller treat it as already finished and start
  // the next access early. Saturate instead, as a missing cycle used to.
  if (!Knows(id)) {
    return Cycle::Max();
  }
  const auto it = live_.find(id);
  if (it == live_.end()) {
    return tl::unexpected(Absent::kRetired);
  }
  if (!it->second.done) {
    return tl::unexpected(Absent::kPending);
  }
  return it->second.cycle;
}

std::size_t EventEngine::InFlight() const {
  return (jobs_.size() - free_jobs_.size()) + live_.size();
}

}  // namespace timeball
