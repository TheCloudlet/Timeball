// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "commit_log.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spike_example {
namespace {

using namespace timeball;

struct Scan {
  std::string_view rest;
  bool ok = true;

  void SkipWs() {
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
      rest.remove_prefix(1);
    }
  }

  bool Consume(std::string_view literal) {
    SkipWs();
    if (!rest.starts_with(literal)) {
      ok = false;
      return false;
    }
    rest.remove_prefix(literal.size());
    return true;
  }

  bool Decimal(uint64_t& out) {
    SkipWs();
    if (rest.empty() ||
        !std::isdigit(static_cast<unsigned char>(rest.front()))) {
      ok = false;
      return false;
    }
    out = 0;
    while (!rest.empty() &&
           std::isdigit(static_cast<unsigned char>(rest.front()))) {
      const uint64_t digit = static_cast<uint64_t>(rest.front() - '0');
      if (out > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
        ok = false;
        return false;
      }
      out = out * 10 + digit;
      rest.remove_prefix(1);
    }
    return true;
  }

  bool Hex(uint64_t& out) {
    SkipWs();
    if (rest.size() < 3 || rest[0] != '0' ||
        (rest[1] != 'x' && rest[1] != 'X')) {
      ok = false;
      return false;
    }
    rest.remove_prefix(2);
    if (rest.empty() ||
        !std::isxdigit(static_cast<unsigned char>(rest.front()))) {
      ok = false;
      return false;
    }
    out = 0;
    int digits = 0;
    while (!rest.empty() &&
           std::isxdigit(static_cast<unsigned char>(rest.front()))) {
      if (++digits > 16) {
        ok = false;
        return false;
      }
      const unsigned char c = static_cast<unsigned char>(rest.front());
      const uint64_t digit = std::isdigit(c) ? c - '0'
                             : c >= 'a'      ? c - 'a' + 10
                                             : c - 'A' + 10;
      out = (out << 4) | digit;
      rest.remove_prefix(1);
    }
    return true;
  }
};

bool ParseHexToken(std::string_view token, uint64_t& out) {
  Scan scan{token};
  if (!scan.Hex(out) || !scan.rest.empty()) {
    return false;
  }
  return scan.ok;
}

// One Spike commit line:
//   core   0: <priv> 0x<pc> (0x<insn>) [reg writes...] [mem 0x<addr>
//   [0x<value>]]...
// A line with no mem record is one retired instruction. Each mem record is a
// load, or a store when a value follows the address. Width is not kept. The
// reg-write dump between the insn and the first mem is ignored: the core port
// language has no place for it, and a load's value is not its address.
bool ParseCommitLine(std::string_view line, uint64_t& hart,
                     std::vector<CorePortEvent>& events, std::string& error) {
  Scan scan{line};
  uint64_t priv = 0;
  uint64_t pc = 0;
  uint64_t insn = 0;
  if (!scan.Consume("core") || !scan.Decimal(hart) || !scan.Consume(":") ||
      !scan.Decimal(priv) || !scan.Hex(pc) || !scan.Consume("(") ||
      !scan.Hex(insn) || !scan.Consume(")")) {
    error = "not a commit line";
    return false;
  }
  (void)priv;
  (void)pc;
  (void)insn;

  std::vector<std::string> tokens;
  {
    std::istringstream remainder{std::string(scan.rest)};
    std::string token;
    while (remainder >> token) {
      tokens.push_back(std::move(token));
    }
  }

  bool seen_mem = false;
  for (std::size_t i = 0; i < tokens.size();) {
    if (tokens[i] != "mem") {
      if (seen_mem) {
        error = "trailing junk after a memory record";
        return false;
      }
      ++i;
      continue;
    }
    seen_mem = true;
    if (i + 1 >= tokens.size()) {
      error = "memory record is missing its address";
      return false;
    }
    uint64_t addr = 0;
    if (!ParseHexToken(tokens[i + 1], addr)) {
      error = "memory address is not hex";
      return false;
    }
    // A store prints the value after the address. A load does not, so the next
    // token is another "mem" or the end of the line.
    if (i + 2 < tokens.size() && tokens[i + 2] != "mem") {
      uint64_t value = 0;
      if (!ParseHexToken(tokens[i + 2], value)) {
        error = "stored value is not hex";
        return false;
      }
      events.push_back(MemoryStore{addr, value});
      i += 3;
    } else {
      events.push_back(MemoryLoad{addr});
      i += 2;
    }
  }
  if (!seen_mem) {
    events.push_back(InstructionsRetired{1});
  }
  return true;
}

}  // namespace

CommitLogConversion ConvertCommitLog(std::istream& in) {
  std::vector<CorePortEvent> events;
  std::optional<uint64_t> hart;
  std::string line;
  std::size_t number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.find_first_not_of(" \t") == std::string::npos) {
      continue;
    }
    uint64_t line_hart = 0;
    std::vector<CorePortEvent> line_events;
    std::string error;
    if (!ParseCommitLine(line, line_hart, line_events, error)) {
      return {{}, "line " + std::to_string(number) + ": " + error};
    }
    // One core port is one core. A second hart is another core's traffic, and
    // merging it onto this core port would time two programs as one.
    if (hart.has_value() && *hart != line_hart) {
      return {{},
              "line " + std::to_string(number) + " names hart " +
                  std::to_string(line_hart) +
                  "; this transcript already has hart " +
                  std::to_string(*hart)};
    }
    hart = line_hart;
    events.insert(events.end(), line_events.begin(), line_events.end());
  }
  return {std::move(events), {}};
}

}  // namespace spike_example
