// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef EXAMPLES_SPIKE_COMMIT_LOG_HPP
#define EXAMPLES_SPIKE_COMMIT_LOG_HPP

#include <iosfwd>
#include <string>
#include <vector>

#include "timeball/core_port_trace.hpp"

// Spike's --log-commits printer, turned into the core port event language.
// Not part of libtimeball_engine: a host that never sees Spike does not
// compile this.

namespace timeball {

struct CommitLogConversion {
  std::vector<CorePortEvent> events;
  // Empty when the log converted. Set when the log names a second hart,
  // or a line is not one committed instruction. ConvertCommitLog leaves
  // events empty in that case: nothing is merged onto the one core port.
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

[[nodiscard]] CommitLogConversion ConvertCommitLog(std::istream& in);

}  // namespace timeball

#endif  // EXAMPLES_SPIKE_COMMIT_LOG_HPP
