// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_IDS_HPP
#define TIMEBALL_IDS_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>

// Distinct integer types. A Cycle cannot be passed where an EventId is
// expected. Construction from the integer is implicit. Reading it back
// (.value()) is explicit.

namespace timeball {

// An opaque identifier: EventId, WorkId, ResourceId or InitiatorId. Equality
// and a total order (so it can be a map key, sorted, or the tiebreak field of a
// pair) are all it supports — no arithmetic, so two ids of the same kind cannot
// be added as if they were quantities.
template <typename Tag, typename T>
class Id {
 public:
  constexpr Id() = default;
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr Id(T value) : value_(value) {}

  constexpr explicit operator T() const { return value_; }
  [[nodiscard]] constexpr T value() const { return value_; }

  friend constexpr bool operator==(Id, Id) = default;
  friend constexpr std::strong_ordering operator<=>(Id, Id) = default;

 private:
  T value_{};
};

namespace ids_detail {
struct EventTag {};
struct WorkTag {};
struct ResourceTag {};
struct InitiatorTag {};
}  // namespace ids_detail

using EventId = Id<ids_detail::EventTag, std::uint64_t>;
using WorkId = Id<ids_detail::WorkTag, std::uint64_t>;
using ResourceId = Id<ids_detail::ResourceTag, std::uint32_t>;
using InitiatorId = Id<ids_detail::InitiatorTag, std::uint32_t>;

// A point on the timeline. Unlike the id types, Cycle is a quantity: it
// supports the arithmetic the engine and its hosts need (advancing by a
// duration, taking a modulus to stride through a window) — but still only with
// itself, never silently with a ResourceId or a plain integer meant as
// something else.
class Cycle {
 public:
  constexpr Cycle() = default;
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr Cycle(std::uint64_t value) : value_(value) {}

  constexpr explicit operator std::uint64_t() const { return value_; }
  [[nodiscard]] constexpr std::uint64_t value() const { return value_; }

  friend constexpr bool operator==(Cycle, Cycle) = default;
  friend constexpr std::strong_ordering operator<=>(Cycle, Cycle) = default;

  constexpr Cycle& operator+=(Cycle rhs) {
    value_ += rhs.value_;
    return *this;
  }
  constexpr Cycle& operator-=(Cycle rhs) {
    value_ -= rhs.value_;
    return *this;
  }
  friend constexpr Cycle operator+(Cycle a, Cycle b) { return a += b; }
  friend constexpr Cycle operator-(Cycle a, Cycle b) { return a -= b; }
  friend constexpr Cycle operator%(Cycle a, Cycle b) {
    return Cycle(a.value_ % b.value_);
  }

  // End of time. A missing completion must not look early when asserts are
  // compiled out.
  static constexpr Cycle Max() {
    return Cycle(std::numeric_limits<std::uint64_t>::max());
  }

 private:
  std::uint64_t value_ = 0;
};

}  // namespace timeball

// Cycle has none of these: nothing in this codebase keys a map or set by a
// cycle value, only by an id — add one if that changes rather than ahead of it.
template <typename Tag, typename T>
struct std::hash<timeball::Id<Tag, T>> {
  std::size_t operator()(const timeball::Id<Tag, T>& id) const noexcept {
    return std::hash<T>{}(id.value());
  }
};

#endif  // TIMEBALL_IDS_HPP
