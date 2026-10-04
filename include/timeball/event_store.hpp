// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_EVENT_STORE_HPP
#define TIMEBALL_EVENT_STORE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "timeball/event_engine.hpp"

struct sqlite3;

// Streaming the engine's records to a SQLite file, so a run can be questioned
// after it ends: which operation was slow, whether it was waiting or working,
// and what it waited for.
//
// Separate because this drags in sqlite3: a host that wants timing and no
// analysis should not link it. TIMEBALL_WITH_SQLITE selects whether the
// implementation is compiled at all.
//
// Three tables, written as records arrive and kept nowhere in memory:
//   ops   one row per resource occupied — a static operation, or one hop of
//         a memory access — with arrival, start and finish
//   deps  one row per dependency an operation or access waited on
//   tasks named spans of cycles, joined against ops by cycle range, so a
//         kernel's name is stored once per span rather than on every row

#ifndef TIMEBALL_WITH_SQLITE
#error \
    "timeball/event_store.hpp requires TIMEBALL_WITH_SQLITE. The store was compiled out (-DTIMEBALL_WITH_SQLITE=OFF); guard your include, or re-enable it."
#endif

namespace timeball {

class EventStore final : public RecordSink {
 public:
  // Creates or truncates the database at path. A filesystem or SQLite error
  // makes IsOpen() false; subsequent record and task calls do nothing.
  explicit EventStore(const std::string& path);
  // Borrow an open database and replace the ops, deps, and tasks tables. The
  // caller owns its transaction and connection, which must outlive this store.
  // Close only finalizes this store's writes; the caller must check it before
  // committing and publishing the database.
  explicit EventStore(sqlite3& database);
  ~EventStore() override;

  // False after an open or write failure, or after Close(). Record and task
  // calls are then no-ops.
  [[nodiscard]] bool IsOpen() const;
  // Empty on success; otherwise includes the path, failed action, and SQLite
  // diagnostic. The view is valid until this store is destroyed.
  [[nodiscard]] std::string_view Error() const;

  // Finish the transaction and close an owned database, or finalize writes to a
  // borrowed database. The destructor also calls this but cannot report
  // failure. A borrowed store cannot report the caller's later commit result.
  // Repeated calls return the same success or failure.
  [[nodiscard]] bool Close();

  EventStore(const EventStore&) = delete;
  EventStore& operator=(const EventStore&) = delete;

  // One ops row, and a deps row for each dependency it carries.
  void OnRecord(const Record& record) override;

  // A task is a named span of cycles. BeginTask closes nothing; EndTask closes
  // the open one. They are recorded independently of events, and joined by
  // cycle range at query time.
  void BeginTask(const std::string& name, Cycle begin_cycle);
  void EndTask(Cycle end_cycle);

 private:
  void Initialize();
  struct Impl;
  Impl* impl_;  // sqlite3 handle and prepared statements; see event_store.cpp
};

}  // namespace timeball

#endif  // TIMEBALL_EVENT_STORE_HPP
