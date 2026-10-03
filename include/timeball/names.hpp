// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_NAMES_HPP
#define TIMEBALL_NAMES_HPP

#include <algorithm>
#include <cstddef>

// Compile-time node names, usable as non-type template parameters.

namespace timeball {

// Implicit so Cache<"L1", ...> accepts a string literal.
template <size_t N>
struct FixedString {
  char value[N]{};
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr FixedString(const char (&str)[N]) { std::copy_n(str, N, value); }
};

}  // namespace timeball

#endif  // TIMEBALL_NAMES_HPP
