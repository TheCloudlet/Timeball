// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <cstdio>
#include <fstream>

#include "commit_log.hpp"
#include "timeball/core_port_trace.hpp"

// Reads a Spike --log-commits file and writes the core port transcript the
// two-machine example replays. A rejected log writes nothing.

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: commit_log <commit.log> <program.trace>\n");
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const timeball::CommitLogConversion converted =
      timeball::ConvertCommitLog(in);
  if (!converted.ok()) {
    std::fprintf(stderr, "%s\n", converted.error.c_str());
    return 1;
  }
  std::ofstream out(argv[2]);
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  timeball::WriteCorePortTrace(out, converted.events);
  return 0;
}
