// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_CORE_PORT_TRACE_HPP
#define TIMEBALL_CORE_PORT_TRACE_HPP

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <variant>
#include <vector>

// CorePort::OnInstructions, OnLoad and OnStore are really three instructions of
// a small language: retire n instructions, load an address, store a value at an
// address. CorePortEvent is that language as data, so a functional simulator's
// output can be recorded once and replayed against different hardware
// parameters without running it again — see CorePort::Apply and
// core_port_trace.cpp's text format below.

namespace timeball {

struct InstructionsRetired {
  uint64_t count = 0;
  friend bool operator==(const InstructionsRetired&,
                         const InstructionsRetired&) = default;
};
struct MemoryLoad {
  uint64_t addr = 0;
  friend bool operator==(const MemoryLoad&, const MemoryLoad&) = default;
};
struct MemoryStore {
  uint64_t addr = 0;
  uint64_t value = 0;
  friend bool operator==(const MemoryStore&, const MemoryStore&) = default;
};

// One call a functional simulator makes into a CorePort, reified. Order matches
// CorePort::OnInstructions/OnLoad/OnStore; nothing here depends on that order.
using CorePortEvent =
    std::variant<InstructionsRetired, MemoryLoad, MemoryStore>;

// One line per event:
//   I <count>          OnInstructions(count)
//   L <addr>            OnLoad(addr)
//   S <addr> <value>     OnStore(addr, value)
// addr and value in hex (0x-prefixed) or decimal; count in decimal. Blank
// lines and lines starting with # are ignored. Written by WriteCorePortTrace,
// in the order given.
void WriteCorePortTrace(std::ostream& out,
                        const std::vector<CorePortEvent>& events);

// nullopt if any line is malformed. A bad file is not a programming error,
// so this does not assert. Nothing is returned: a partial trace is not
// replayed.
[[nodiscard]] std::optional<std::vector<CorePortEvent>> ReadCorePortTrace(
    std::istream& in);

}  // namespace timeball

#endif  // TIMEBALL_CORE_PORT_TRACE_HPP
