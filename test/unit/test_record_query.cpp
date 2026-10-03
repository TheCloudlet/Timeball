// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <string>

#include "gtest/gtest.h"
#include "timeball/record_query.hpp"

using namespace timeball;

namespace {

struct Totals {
  Cycle waiting = 0;
  Cycle working = 0;
};

// A hop whose time is known by hand: waiting is start - arrival, working is
// finish - start. The name is a literal so the view outlives the record.
constexpr Record Hop(const char* resource, Cycle arrival, Cycle start,
                     Cycle finish) {
  return Record{.name = "load",
                .resource = resource,
                .arrival = arrival,
                .start = start,
                .finish = finish};
}

Totals AddTime(Totals t, const Record& r) {
  t.waiting += r.start - r.arrival;
  t.working += r.finish - r.start;
  return t;
}

const Record kHops[] = {
    Hop("L1", 0, 2, 6),        // wait 2, work 4
    Hop("DRAM", 6, 6, 106),    // wait 0, work 100
    Hop("L1", 106, 107, 111),  // wait 1, work 4
};

}  // namespace

TEST(RecordQuery, FoldSumsWorkingTimeAcrossHops) {
  // 4 + 100 + 4. The literal is the sum of the comments above, not a call
  // back into Fold.
  const Cycle working = Fold(kHops, Cycle{0}, [](Cycle acc, const Record& r) {
    return acc + (r.finish - r.start);
  });
  EXPECT_EQ(working, 108u);
}

TEST(RecordQuery, FilterThenFoldKeepsOnlyTheMatchingHops) {
  const Cycle working = Fold(
      Filter(kHops, [](const Record& r) { return r.resource == "DRAM"; }),
      Cycle{0},
      [](Cycle acc, const Record& r) { return acc + (r.finish - r.start); });
  EXPECT_EQ(working, 100u);
}

TEST(RecordQuery, GroupSumsWaitingAndWorkingPerResource) {
  const auto by_resource = Group(
      kHops, [](const Record& r) { return std::string(r.resource); }, Totals{},
      AddTime);
  // Two hops at L1, one at DRAM: the map has a key per resource, not per hop.
  EXPECT_EQ(by_resource.size(), 2u);
  EXPECT_EQ(by_resource.at("L1").waiting, 3u);
  EXPECT_EQ(by_resource.at("L1").working, 8u);
  EXPECT_EQ(by_resource.at("DRAM").waiting, 0u);
  EXPECT_EQ(by_resource.at("DRAM").working, 100u);
}

TEST(RecordQuery, StreamingFoldAndGroupKeepTheAnswerNotTheHops) {
  // The same three hops, delivered one at a time the way a long run does.
  // Two L1 hops share one bucket: the sink's state is the answer, not the
  // hops that produced it.
  auto working = Fold(Cycle{0}, [](Cycle acc, const Record& r) {
    return acc + (r.finish - r.start);
  });
  auto by_resource =
      Group([](const Record& r) { return std::string(r.resource); }, Totals{},
            AddTime);
  for (const Record& hop : kHops) {
    working.OnRecord(hop);
    by_resource.OnRecord(hop);
  }
  EXPECT_EQ(working.value(), 108u);
  EXPECT_EQ(by_resource.value().size(), 2u);
  EXPECT_EQ(by_resource.value().at("L1").waiting, 3u);
  EXPECT_EQ(by_resource.value().at("L1").working, 8u);
  EXPECT_EQ(by_resource.value().at("DRAM").working, 100u);
}

TEST(RecordQuery, FilterSinkDropsHopsBeforeTheyReachTheGroup) {
  auto by_resource =
      Group([](const Record& r) { return std::string(r.resource); }, Totals{},
            AddTime);
  auto dram_only =
      Filter([](const Record& r) { return r.resource == "DRAM"; }, by_resource);
  for (const Record& hop : kHops) {
    dram_only.OnRecord(hop);
  }
  EXPECT_EQ(by_resource.value().size(), 1u);
  EXPECT_EQ(by_resource.value().count("L1"), 0u);
  EXPECT_EQ(by_resource.value().at("DRAM").waiting, 0u);
  EXPECT_EQ(by_resource.value().at("DRAM").working, 100u);
}
