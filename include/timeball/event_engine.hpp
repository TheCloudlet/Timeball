// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_EVENT_ENGINE_HPP
#define TIMEBALL_EVENT_ENGINE_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "timeball/ids.hpp"
#include "timeball/node.hpp"
#include "tl/expected.hpp"

// One timeline. Work completes at max(dependencies complete, resource free) +
// cost Cost is the host's, on an Operation, or the node's, for an access.

namespace timeball {

struct ResourceSpec {
  std::string name;
  std::uint32_t capacity = 1;  // how many operations it serves at once
};

// When work may start.
struct When {
  Cycle ready_cycle = 0;
  // Ids this engine already returned. The graph is acyclic by construction.
  std::vector<EventId> after;
  // Same-cycle tie break. Higher first. Never overrides an earlier arrival.
  std::uint32_t priority = 0;
  // An event's completion plus a delay before this work may arrive.
  struct DelayedAfter {
    EventId event;
    Cycle cycles;
  };
  std::vector<DelayedAfter> after_delay;
};

// Host-costed work: a compute step or a DMA transfer. Field order is part of
// Submit({...}). Moving a field still compiles and assigns the wrong member.
// Re-run the suite after changing this shape.
struct Operation {
  // A literal, or storage that outlives the engine.
  std::string_view name;
  ResourceId resource = 0;
  InitiatorId initiator = 0;
  Cycle duration_cycles = 0;
  When when;
  // Known, distinct resources, excluding resource above. Acquire one unit of
  // each together; no capacity is held while waiting for the others.
  std::vector<ResourceId> additional_resources;
};

// One occupation of one resource: a static operation, or one hop.
struct Record {
  // Submitted work this belongs to. 0 is a writeback; nobody waits on it.
  EventId op = 0;
  std::string_view name;  // the operation's name; "load" or "store"
  std::string_view resource;
  InitiatorId initiator = 0;
  uint64_t addr = 0;       // an access's address at this hop
  uint32_t depth = 0;      // hops below the entry; 0 if it entered here
  bool forwarded = false;  // sent on past here
  Cycle arrival = 0;       // reached the resource
  Cycle start = 0;         // start - arrival is waiting
  Cycle finish = 0;        // finish - start is service
  // Dependencies. Only on the first record, and only during OnRecord.
  std::span<const EventId> after;

  [[nodiscard]] bool demand() const { return op != 0; }
};

struct OperationResult {
  EventId event_id = 0;
  // The operation's name; "load" or "store" for an access.
  std::string_view name;
  // The resource that served it; for an access, the node it was served at.
  std::string_view served_by;
  InitiatorId initiator = 0;
  Cycle ready_cycle = 0;    // its own constraint and its dependencies
  Cycle service_cycle = 0;  // when service began
  Cycle completion_cycle = 0;
};

// Receives every Record as the resource it describes finishes, in time order.
class RecordSink {
 public:
  virtual ~RecordSink() = default;
  virtual void OnRecord(const Record& record) = 0;
};

// Keeps the records in memory, for tests and small runs.
struct RecordingSink final : RecordSink {
  std::vector<Record> records;

  void OnRecord(const Record& record) override {
    records.push_back(record);
    records.back().after = {};  // the view dies with the callback
  }
};

// Hands every record to two sinks, so a host's own sink and a diagnostic one —
// timeball/checking_sink.hpp, say — can both watch the same run. Either may be
// null.
class BroadcastSink final : public RecordSink {
 public:
  BroadcastSink(RecordSink* first, RecordSink* second)
      : first_(first), second_(second) {}

  void OnRecord(const Record& record) override {
    if (first_ != nullptr) {
      first_->OnRecord(record);
    }
    if (second_ != nullptr) {
      second_->OnRecord(record);
    }
  }

 private:
  RecordSink* first_;
  RecordSink* second_;
};

struct RunResult {
  Cycle end_cycle = 0;
  // Host-submitted work, in completion order. Writebacks are omitted.
  std::vector<OperationResult> completed;
};

// Bookkeeping. Not a public API. Not nested in EventEngine: a nested class with
// default member initializers makes std::variant's default constructor unusable
// for a member of that class ([class.mem]).
namespace detail {

// One visit: the node, the request it received, and when it arrived.
struct Hop {
  AccessNode* node;
  Request request;
  Cycle arrival = 0;
};

// A static Operation on all the resources it named, primary first.
struct OperationJob {
  std::vector<ResourceId> resources;
  Cycle duration = 0;
};

// Nodes visited so far, and the current Serve result. Cost is not known up
// front.
struct AccessJob {
  std::vector<Hop> path;
  Route route;
  bool deferred = false;    // A waiting hop already called Serve.
  uint32_t base_depth = 0;  // a node's own traffic sits below its sender
  ResourceId resource = 0;  // current hop's resource, registered by the engine
};

// One submitted operation or access, until it completes. An access with no id
// is a node's own traffic. Kind is one alternative.
struct Job {
  EventId id = 0;
  std::string_view name;
  InitiatorId initiator = 0;
  std::uint32_t priority = 0;
  // Host submission order, then order within the access that caused it. Not the
  // order the engine allocated the slot.
  std::uint64_t sequence = 0;
  std::uint64_t sub = 0;
  Cycle ready = 0;        // its own constraint, then its dependencies'
  std::size_t unmet = 0;  // dependencies not yet complete
  Cycle first_start = 0;
  bool started = false;
  bool start_pending = false;  // one retry even if several resources wake it
  Cycle start = 0;             // the current step's
  std::vector<EventId> after;  // recorded with the first step, then dropped
  std::vector<When::DelayedAfter> after_delay;
  std::variant<OperationJob, AccessJob> kind;
};

struct Completion {
  // End of time until done, so a missing cycle does not look early.
  Cycle cycle = Cycle::Max();
  bool done = false;
};

// At one cycle: completions, then a freed resource, then new arrivals. Each
// event carries only its own fields.
struct StartEvent {
  Cycle arrival = 0;  // when it reached this resource; first come, served
  std::uint32_t priority = 0;
  std::uint64_t sequence = 0;
  std::uint64_t sub = 0;
  std::size_t job = 0;
};
struct FinishEvent {
  std::uint32_t priority = 0;
  std::uint64_t sequence = 0;
  std::uint64_t sub = 0;
  std::size_t job = 0;
};
struct WakeEvent {
  ResourceId resource = 0;  // wakes carry no other order
};

// Total order, so equal keys do not fall through to the heap. The comparator,
// not the variant declaration, defines phase precedence.
struct Entry {
  Cycle time = 0;
  std::variant<StartEvent, WakeEvent, FinishEvent> event;
};

struct Later {
  bool operator()(const Entry& a, const Entry& b) const;
};

// Earliest arrival, then higher priority, then submission order. Waiting queues
// hold Start events only.
struct EarlierArrival {
  bool operator()(const StartEvent& a, const StartEvent& b) const;
};

struct Resource {
  std::string name;
  std::vector<Cycle> free_cycles;  // one per unit of capacity
  // A multi-resource operation waits in every required resource's queue.
  // Erasing it from all queues on acquisition prevents partial ownership.
  std::set<StartEvent, EarlierArrival> waiting;
  bool wake_pending = false;
};

struct TargetWork {
  ResourceId resource = 0;
  EventId last = 0;
};

// Initial alternative of Job::kind, so a reused slot can keep its Hop vector.
enum class JobKind : std::uint8_t { kOperation, kAccess };

}  // namespace detail

// Why CompletionOf has no cycle. kPending: not run. kRetired: forgotten after
// the horizon. An id this engine never issued asserts.
enum class Absent { kPending, kRetired };

class EventEngine final {
 public:
  // A resource with no name or no capacity asserts.
  ResourceId AddResource(ResourceSpec spec);
  // An access and host operations may contend for one existing Resource. The
  // node and Resource must have the same name.
  void BindAccessNode(AccessNode& node, ResourceId resource);

  // True when Submit would accept op. Ask in a release build, where Submit's
  // asserts are compiled out.
  [[nodiscard]] bool Accepts(const Operation& op) const;
  // Whether SubmitAccess would take an access with these constraints.
  [[nodiscard]] bool Accepts(const When& when) const;
  [[nodiscard]] bool Knows(EventId id) const;

  EventId Submit(const Operation& op);

  // An access entering the graph at entry. when constrains its start.
  EventId SubmitAccess(AccessNode& entry, const Request& r, When when = {});

  // Runs what happens before horizon, then forgets it. Nothing submitted
  // afterwards may begin earlier. Ties fall to submission order.
  RunResult RunUntil(Cycle horizon, RecordSink* sink = nullptr);

  // Runs until idle. Promises nothing. Submit every agent before the call that
  // should order them together.
  RunResult RunUntilIdle(RecordSink* sink = nullptr);

  // Nothing may begin before this.
  [[nodiscard]] Cycle Horizon() const { return horizon_; }

  [[nodiscard]] std::string_view ResourceName(ResourceId id) const;

  // The cycle if the id has run and is still remembered. kPending while in
  // flight, kRetired after the horizon. An unknown id asserts.
  [[nodiscard]] tl::expected<Cycle, Absent> CompletionOf(EventId id) const;

  // Live jobs and unretired ids.
  [[nodiscard]] std::size_t InFlight() const;

 private:
  using Hop = detail::Hop;
  using OperationJob = detail::OperationJob;
  using AccessJob = detail::AccessJob;
  using Job = detail::Job;
  using Completion = detail::Completion;
  using StartEvent = detail::StartEvent;
  using FinishEvent = detail::FinishEvent;
  using WakeEvent = detail::WakeEvent;
  using Entry = detail::Entry;
  using Later = detail::Later;
  using Resource = detail::Resource;
  using JobKind = detail::JobKind;

  // Schedules a writeback. No id, nobody waiting. priority and depth are the
  // completing access's.
  void Enqueue(std::size_t causing_job, std::uint32_t priority,
               std::uint32_t depth, Cycle at, AccessNode& to, const Request& r);

  std::size_t NewJob(JobKind kind);
  EventId NewId();
  void Resolve(std::size_t job);
  [[nodiscard]] Cycle Earliest(const When& when) const;
  void Process(const Entry& e, RunResult& run, RecordSink* sink);
  void Emit(const Job& job, Cycle finish, RecordSink* sink);
  void Arrive(std::size_t job, Cycle at);
  std::span<const ResourceId> ResourcesOf(Job& job);
  void Start(const Entry& e);
  void Admit(Resource& resource, Cycle at);
  void ScheduleWake(ResourceId id, Cycle at);
  void Finish(const Entry& e, RunResult& run, RecordSink* sink);
  void Complete(std::size_t job, Cycle at, RunResult& run, RecordSink* sink);

  // A deque, so a resource's name stays put as nodes are registered mid-run.
  std::deque<Resource> resources_;
  std::unordered_map<const AccessNode*, ResourceId> node_resources_;
  std::unordered_map<const AccessNode*, detail::TargetWork> target_work_;

  // Live jobs only: a slot is reused once its job completes. A deque, so a job
  // stays put while a node completing it sends traffic of its own.
  std::deque<Job> jobs_;
  std::vector<std::size_t> free_jobs_;
  std::priority_queue<Entry, std::vector<Entry>, Later> queue_;
  std::unordered_map<EventId, std::vector<std::size_t>> waiting_;

  // What outlives a job: the completion of every submitted id not yet retired,
  // so later work can depend on it. Ids from next_id_ down that are missing
  // here have been retired.
  std::unordered_map<EventId, Completion> live_;
  // Completed ids by completion cycle, earliest first, so retiring touches only
  // what is retired rather than everything live.
  std::priority_queue<std::pair<Cycle, EventId>,
                      std::vector<std::pair<Cycle, EventId>>, std::greater<>>
      completed_;
  std::uint64_t next_id_ = 1;  // the next id to hand out; incremented, so
                               // kept as the raw integer EventId wraps
  Cycle horizon_ = 0;

  std::uint64_t next_sequence_ = 0;  // host submissions only
  std::uint64_t sends_ = 0;          // node traffic sent, ever
  Cycle end_cycle_ = 0;
};

}  // namespace timeball

#endif  // TIMEBALL_EVENT_ENGINE_HPP
