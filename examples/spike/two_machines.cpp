// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// Replays one committed transcript on two machines and prints both completion
// cycles. The transcript is examples/spike/program.trace, the core port event
// form of a Spike commit log. Spike is not run here.
//
// These cycles omit instruction fetch: a commit log has no fetch, and none is
// invented. They are Timeball's timing of the committed accesses, not a
// comparison against silicon or RTL.

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <vector>

#include "machines.hpp"
#include "timeball/core_port_trace.hpp"

namespace {

template <typename MachineT>
timeball::Cycle CompletionOf(
    const std::vector<timeball::CorePortEvent>& events) {
  MachineT machine(timeball::kReplayConfig);
  timeball::CorePort& core_port = machine.GetCorePort();
  core_port.Apply(events);
  core_port.Sync();
  return core_port.Now();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: two_machines <program.trace>\n");
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const auto events = timeball::ReadCorePortTrace(in);
  if (!events.has_value()) {
    std::fprintf(stderr, "%s is not a CorePort transcript\n", argv[1]);
    return 1;
  }

  const timeball::Cycle one_line =
      CompletionOf<timeball::OneLineMachine>(*events);
  const timeball::Cycle working_set =
      CompletionOf<timeball::WorkingSetMachine>(*events);

  std::printf("one line:    %" PRIu64 " cycles\n", one_line.value());
  std::printf("working set: %" PRIu64 " cycles\n", working_set.value());

  if (one_line <= working_set) {
    std::fprintf(stderr,
                 "the one-line cache should finish later than the cache "
                 "that holds the working set\n");
    return 1;
  }
  return 0;
}
