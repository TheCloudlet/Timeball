// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/core_port_trace.hpp"

#include <charconv>
#include <initializer_list>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace timeball {

namespace {

// Hex (0x-prefixed) or decimal, the whole token and nothing but it.
// std::from_chars does not skip a "0x" prefix itself, so it is stripped first
// when present.
bool ParseNumber(std::string_view token, uint64_t& out) {
  int base = 10;
  if (token.size() > 2 && token[0] == '0' &&
      (token[1] == 'x' || token[1] == 'X')) {
    token.remove_prefix(2);
    base = 16;
  }
  if (token.empty()) {
    return false;
  }
  const auto* begin = token.data();
  const auto* end = token.data() + token.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out, base);
  return ec == std::errc{} && ptr == end;
}

// Reads exactly as many more whitespace-separated numeric fields as values has
// slots for, in order — false if a field is missing, is not a number, or a
// field remains after the last one asked for. One shape shared by all three
// event kinds, which otherwise differ only in field count.
bool ReadFields(std::istringstream& tokens,
                std::initializer_list<uint64_t*> values) {
  for (uint64_t* value : values) {
    std::string field;
    if (!(tokens >> field) || !ParseNumber(field, *value)) {
      return false;
    }
  }
  tokens >> std::ws;
  return tokens.eof();
}

}  // namespace

void WriteCorePortTrace(std::ostream& out,
                        const std::vector<CorePortEvent>& events) {
  for (const CorePortEvent& event : events) {
    if (const auto* i = std::get_if<InstructionsRetired>(&event)) {
      out << "I " << i->count << '\n';
    } else if (const auto* l = std::get_if<MemoryLoad>(&event)) {
      out << "L 0x" << std::hex << l->addr << std::dec << '\n';
    } else {
      const auto& s = std::get<MemoryStore>(event);
      out << "S 0x" << std::hex << s.addr << std::dec << ' ' << s.value << '\n';
    }
  }
}

std::optional<std::vector<CorePortEvent>> ReadCorePortTrace(std::istream& in) {
  std::vector<CorePortEvent> events;
  std::string line;
  while (std::getline(in, line)) {
    std::string_view view(line);
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
      view.remove_prefix(1);
    }
    if (view.empty() || view.front() == '#') {
      continue;
    }

    std::istringstream tokens{std::string(view)};
    std::string kind;
    tokens >> kind;
    if (kind == "I") {
      uint64_t count = 0;
      if (!ReadFields(tokens, {&count})) {
        return std::nullopt;
      }
      events.push_back(InstructionsRetired{count});
    } else if (kind == "L") {
      uint64_t addr = 0;
      if (!ReadFields(tokens, {&addr})) {
        return std::nullopt;
      }
      events.push_back(MemoryLoad{addr});
    } else if (kind == "S") {
      uint64_t addr = 0;
      uint64_t value = 0;
      if (!ReadFields(tokens, {&addr, &value})) {
        return std::nullopt;
      }
      events.push_back(MemoryStore{addr, value});
    } else {
      return std::nullopt;
    }
  }
  return events;
}

}  // namespace timeball
