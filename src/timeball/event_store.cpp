// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/event_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cassert>
#include <string_view>

namespace timeball {

struct EventStore::Impl {
  std::string path;
  std::string error;
  sqlite3* db = nullptr;
  bool owns_database = true;
  sqlite3_stmt* insert_op = nullptr;
  sqlite3_stmt* insert_dep = nullptr;
  sqlite3_stmt* insert_task = nullptr;
  std::string open_task;
  Cycle open_begin = 0;
  bool task_open = false;
  // The furthest cycle any record reached, so a task left open at the end of
  // a run closes at the end of the run rather than at its own start.
  Cycle last_cycle = 0;

  bool Check(int rc, const char* action) {
    if (rc == SQLITE_OK) {
      return true;
    }
    if (error.empty()) {
      error = path + ": " + action + ": " + sqlite3_errmsg(db);
    }
    return false;
  }

  bool Step(sqlite3_stmt* statement, const char* action) {
    const int rc = sqlite3_step(statement);
    return rc == SQLITE_DONE || Check(rc, action);
  }

  bool Exec(const char* sql, const char* action) {
    char* detail = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &detail);
    if (rc != SQLITE_OK && error.empty()) {
      error = path + ": " + action + ": " +
              (detail != nullptr ? detail : sqlite3_errmsg(db));
    }
    sqlite3_free(detail);
    return rc == SQLITE_OK;
  }

  bool Prepare(const char* sql, sqlite3_stmt** out, const char* action) {
    const int rc = sqlite3_prepare_v2(db, sql, -1, out, nullptr);
    if (rc != SQLITE_OK && error.empty()) {
      error = path + ": " + action + ": " + sqlite3_errmsg(db);
    }
    return rc == SQLITE_OK;
  }
};

EventStore::EventStore(const std::string& path)
    : impl_(new Impl{.path = path}) {
  if (sqlite3_open(path.c_str(), &impl_->db) != SQLITE_OK) {
    // A path that cannot be opened is a runtime condition, not a mistake in
    // the topology, so this is reported through IsOpen() rather than asserted
    // away under NDEBUG. Every later call becomes a no-op.
    impl_->error =
        path + ": opening SQLite database: " +
        (impl_->db != nullptr ? sqlite3_errmsg(impl_->db) : "out of memory");
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
    return;
  }

  // Durability is not the point — a run either finishes and is queried, or is
  // discarded. These two pragmas are what make appending millions of rows
  // affordable.
  if (!impl_->Exec("PRAGMA journal_mode = OFF", "setting journal mode") ||
      !impl_->Exec("PRAGMA synchronous = OFF", "setting synchronous mode")) {
    return;
  }

  Initialize();
  if (IsOpen()) {
    impl_->Exec("BEGIN TRANSACTION", "beginning recording");
  }
}

EventStore::EventStore(sqlite3& database)
    : impl_(new Impl{.path = "borrowed SQLite database",
                     .db = &database,
                     .owns_database = false}) {
  Initialize();
}

void EventStore::Initialize() {
  // Rows carry no task name: identity lives in tasks and is joined by cycle
  // range, so it is stored once per span rather than per row. op is 0 for a
  // node's own traffic, which no one waited on.
  if (!impl_->Exec("DROP TABLE IF EXISTS ops;"
                   "DROP TABLE IF EXISTS deps;"
                   "DROP TABLE IF EXISTS tasks;"
                   "CREATE TABLE ops("
                   "  op INTEGER NOT NULL,"
                   "  name TEXT NOT NULL,"
                   "  resource TEXT NOT NULL,"
                   "  initiator INTEGER NOT NULL,"
                   "  addr INTEGER NOT NULL,"
                   "  depth INTEGER NOT NULL,"
                   "  forwarded INTEGER NOT NULL,"
                   "  arrival INTEGER NOT NULL,"
                   "  start INTEGER NOT NULL,"
                   "  finish INTEGER NOT NULL);"
                   "CREATE TABLE deps("
                   "  op INTEGER NOT NULL,"
                   "  depends_on INTEGER NOT NULL);"
                   "CREATE TABLE tasks("
                   "  name TEXT NOT NULL,"
                   "  begin_cycle INTEGER NOT NULL,"
                   "  end_cycle INTEGER NOT NULL);",
                   "initializing schema")) {
    return;
  }

  if (!impl_->Prepare("INSERT INTO ops VALUES(?,?,?,?,?,?,?,?,?,?)",
                      &impl_->insert_op, "preparing ops insert") ||
      !impl_->Prepare("INSERT INTO deps VALUES(?,?)", &impl_->insert_dep,
                      "preparing deps insert") ||
      !impl_->Prepare("INSERT INTO tasks VALUES(?,?,?)", &impl_->insert_task,
                      "preparing tasks insert")) {
    return;
  }
}

bool EventStore::IsOpen() const {
  return impl_->db != nullptr && impl_->error.empty();
}

std::string_view EventStore::Error() const {
  return impl_->error;
}

EventStore::~EventStore() {
  static_cast<void>(Close());
  delete impl_;
}

bool EventStore::Close() {
  if (impl_->db == nullptr) {
    return impl_->error.empty();
  }
  if (IsOpen() && impl_->task_open) {
    // A task the host never closed spans to the last cycle recorded.
    EndTask(std::max(impl_->last_cycle, impl_->open_begin));
  }
  if (impl_->insert_op != nullptr) {
    impl_->Check(sqlite3_finalize(impl_->insert_op), "finalizing ops insert");
    impl_->insert_op = nullptr;
  }
  if (impl_->insert_dep != nullptr) {
    impl_->Check(sqlite3_finalize(impl_->insert_dep), "finalizing deps insert");
    impl_->insert_dep = nullptr;
  }
  if (impl_->insert_task != nullptr) {
    impl_->Check(sqlite3_finalize(impl_->insert_task),
                 "finalizing tasks insert");
    impl_->insert_task = nullptr;
  }
  if (!impl_->owns_database) {
    impl_->db = nullptr;
    return impl_->error.empty();
  }
  if (impl_->error.empty()) {
    impl_->Exec("COMMIT", "committing recording");
  }
  if (!impl_->error.empty()) {
    // Also harmless when initialization failed before a transaction began.
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  const int rc = sqlite3_close(impl_->db);
  impl_->Check(rc, "closing SQLite database");
  if (rc == SQLITE_OK) {
    impl_->db = nullptr;
  }
  return impl_->error.empty();
}

namespace {

// Views into the engine are not NUL-terminated, so text binds carry a length,
// and SQLite copies the text before the view can go away.
int BindText(sqlite3_stmt* s, int column, std::string_view text) {
  // SQLITE_TRANSIENT is SQLite's own sentinel, defined as a cast of -1.
  return sqlite3_bind_text(
      s, column, text.data(), static_cast<int>(text.size()),
      SQLITE_TRANSIENT);  // NOLINT(performance-no-int-to-ptr)
}

}  // namespace

void EventStore::OnRecord(const Record& record) {
  if (!IsOpen()) {
    return;
  }
  impl_->last_cycle = std::max(impl_->last_cycle, record.finish);
  sqlite3_stmt* s = impl_->insert_op;
  if (!impl_->Check(sqlite3_reset(s), "resetting ops row") ||
      !impl_->Check(sqlite3_bind_int64(
                        s, 1, static_cast<sqlite3_int64>(record.op.value())),
                    "binding ops row") ||
      !impl_->Check(BindText(s, 2, record.name), "binding ops row") ||
      !impl_->Check(BindText(s, 3, record.resource), "binding ops row") ||
      !impl_->Check(sqlite3_bind_int64(s, 4, record.initiator.value()),
                    "binding ops row") ||
      !impl_->Check(
          sqlite3_bind_int64(s, 5, static_cast<sqlite3_int64>(record.addr)),
          "binding ops row") ||
      !impl_->Check(sqlite3_bind_int64(s, 6, record.depth),
                    "binding ops row") ||
      !impl_->Check(sqlite3_bind_int(s, 7, record.forwarded ? 1 : 0),
                    "binding ops row") ||
      !impl_->Check(
          sqlite3_bind_int64(
              s, 8, static_cast<sqlite3_int64>(record.arrival.value())),
          "binding ops row") ||
      !impl_->Check(sqlite3_bind_int64(
                        s, 9, static_cast<sqlite3_int64>(record.start.value())),
                    "binding ops row") ||
      !impl_->Check(
          sqlite3_bind_int64(s, 10,
                             static_cast<sqlite3_int64>(record.finish.value())),
          "binding ops row") ||
      !impl_->Step(s, "writing ops row")) {
    return;
  }

  for (EventId dependency : record.after) {
    sqlite3_stmt* d = impl_->insert_dep;
    if (!impl_->Check(sqlite3_reset(d), "resetting deps row") ||
        !impl_->Check(sqlite3_bind_int64(
                          d, 1, static_cast<sqlite3_int64>(record.op.value())),
                      "binding deps row") ||
        !impl_->Check(sqlite3_bind_int64(
                          d, 2, static_cast<sqlite3_int64>(dependency.value())),
                      "binding deps row") ||
        !impl_->Step(d, "writing deps row")) {
      return;
    }
  }
}

void EventStore::BeginTask(const std::string& name, Cycle begin_cycle) {
  if (!IsOpen()) {
    return;
  }
  // Beginning a task while one is open closes it at the same cycle, so tasks
  // tile the timeline rather than overlapping.
  if (impl_->task_open) {
    EndTask(begin_cycle);
  }
  impl_->open_task = name;
  impl_->open_begin = begin_cycle;
  impl_->task_open = true;
}

void EventStore::EndTask(Cycle end_cycle) {
  if (!IsOpen()) {
    return;
  }
  assert(impl_->task_open && "EndTask without a matching BeginTask");
  sqlite3_stmt* s = impl_->insert_task;
  if (!impl_->Check(sqlite3_reset(s), "resetting tasks row") ||
      !impl_->Check(BindText(s, 1, impl_->open_task), "binding tasks row") ||
      !impl_->Check(
          sqlite3_bind_int64(
              s, 2, static_cast<sqlite3_int64>(impl_->open_begin.value())),
          "binding tasks row") ||
      !impl_->Check(sqlite3_bind_int64(
                        s, 3, static_cast<sqlite3_int64>(end_cycle.value())),
                    "binding tasks row") ||
      !impl_->Step(s, "writing tasks row")) {
    return;
  }
  impl_->task_open = false;
}

}  // namespace timeball
