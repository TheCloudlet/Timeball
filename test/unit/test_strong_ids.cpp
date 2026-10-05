// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// Nothing here runs: every check is a static_assert, so a violation is a build
// failure rather than a test failure. This is the proof that Cycle, EventId,
// WorkId, ResourceId and InitiatorId cannot be substituted for one another,
// deliberately or by mistake — the property issue #27 asks for.

#include <type_traits>

#include "timeball/event_engine.hpp"

using namespace timeball;

namespace {

template <typename A, typename B>
constexpr bool kNeitherConverts =
    !std::is_convertible_v<A, B> && !std::is_convertible_v<B, A>;

static_assert(kNeitherConverts<Cycle, EventId>,
              "a cycle must not stand in for an event id, or the reverse");
static_assert(kNeitherConverts<WorkId, EventId>,
              "a device work id must not stand in for an event id");
static_assert(kNeitherConverts<Cycle, ResourceId>,
              "a cycle must not stand in for a resource id, or the reverse");
static_assert(kNeitherConverts<Cycle, InitiatorId>,
              "a cycle must not stand in for an initiator id, or the reverse");
static_assert(
    kNeitherConverts<EventId, ResourceId>,
    "an event id must not stand in for a resource id, or the reverse — "
    "this is the Entry::job confusion issue #27 was written against");
static_assert(
    kNeitherConverts<EventId, InitiatorId>,
    "an event id must not stand in for an initiator id, or the reverse");
static_assert(
    kNeitherConverts<ResourceId, InitiatorId>,
    "a resource id must not stand in for an initiator id, or the reverse");

// Each is still, deliberately, implicitly constructible from its own underlying
// integer — that direction is what keeps `Cycle t = 0` and `.initiator_id =
// agent` working — and only explicitly convertible back out to it.
static_assert(std::is_convertible_v<std::uint64_t, Cycle>);
static_assert(std::is_convertible_v<std::uint64_t, EventId>);
static_assert(std::is_convertible_v<std::uint64_t, WorkId>);
static_assert(std::is_convertible_v<std::uint32_t, ResourceId>);
static_assert(std::is_convertible_v<std::uint32_t, InitiatorId>);
static_assert(!std::is_convertible_v<Cycle, std::uint64_t>);
static_assert(!std::is_convertible_v<EventId, std::uint64_t>);
static_assert(!std::is_convertible_v<WorkId, std::uint64_t>);
static_assert(!std::is_convertible_v<ResourceId, std::uint32_t>);
static_assert(!std::is_convertible_v<InitiatorId, std::uint32_t>);

}  // namespace
