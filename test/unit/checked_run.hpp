// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// Runs an engine (or a CorePort) with timeball/checking_sink.hpp attached
// alongside whatever sink the test already wanted, and fails the current test
// if the run violated its own rule. Every test in this suite goes through this
// rather than calling RunUntilIdle/RunUntil/Sync directly, so the checker is
// exercised by the whole suite (ticket #23), not only its own tests.
//
// The checker here is a fresh one per call, so a test that submits and runs
// in windows should keep its own CheckingSink across calls instead of using
// this — see the windowed helpers in test_event_engine.cpp and
// test_core_port.cpp for that shape.
//
// Test-only. Not part of the library Timeball ships.

#ifndef TIMEBALL_TEST_CHECKED_RUN_HPP
#define TIMEBALL_TEST_CHECKED_RUN_HPP

#include "gtest/gtest.h"
#include "timeball/checking_sink.hpp"
#include "timeball/core_port.hpp"
#include "timeball/event_engine.hpp"

namespace timeball::test {

inline void ReportViolations(const CheckingSink& checker) {
  for (const auto& v : checker.Violations()) {
    ADD_FAILURE() << v.what;
  }
}

inline RunResult RunChecked(EventEngine& engine, RecordSink* sink = nullptr) {
  CheckingSink checker;
  BroadcastSink both(sink, &checker);
  RunResult result = engine.RunUntilIdle(&both);
  ReportViolations(checker);
  return result;
}

inline RunResult RunChecked(EventEngine& engine, Cycle horizon,
                            RecordSink* sink = nullptr) {
  CheckingSink checker;
  BroadcastSink both(sink, &checker);
  RunResult result = engine.RunUntil(horizon, &both);
  ReportViolations(checker);
  return result;
}

// For a test that calls this more than once on the same engine — a later
// call's work may depend on an earlier call's op, which only the same
// checker instance can still know completed. Report the checker's
// violations once, after every call is done.
inline RunResult RunChecked(EventEngine& engine, CheckingSink& checker,
                            RecordSink* sink = nullptr) {
  BroadcastSink both(sink, &checker);
  return engine.RunUntilIdle(&both);
}

inline RunResult RunChecked(EventEngine& engine, Cycle horizon,
                            CheckingSink& checker, RecordSink* sink = nullptr) {
  BroadcastSink both(sink, &checker);
  return engine.RunUntil(horizon, &both);
}

inline void SyncChecked(CorePort& core_port, RecordSink* sink = nullptr) {
  CheckingSink checker;
  BroadcastSink both(sink, &checker);
  core_port.Sync(&both);
  ReportViolations(checker);
}

// For a test that syncs the same CorePort more than once — see RunChecked's
// persistent-checker overload above for why.
inline void SyncChecked(CorePort& core_port, CheckingSink& checker,
                        RecordSink* sink = nullptr) {
  BroadcastSink both(sink, &checker);
  core_port.Sync(&both);
}

}  // namespace timeball::test

#endif  // TIMEBALL_TEST_CHECKED_RUN_HPP
