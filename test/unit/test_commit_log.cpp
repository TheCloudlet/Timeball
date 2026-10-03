// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "commit_log.hpp"
#include "gtest/gtest.h"
#include "timeball/core_port_trace.hpp"

using namespace timeball;

namespace {

std::string Line(const char* body) {
  return std::string(body) + "\n";
}

}  // namespace

TEST(CommitLog, ANonMemoryInstructionBecomesOneRetiredInstruction) {
  std::stringstream in;
  in << Line(
      "core   0: 0 0x0000000080001000 (0x00000013) x10 0x0000000000000001");
  const CommitLogConversion converted = ConvertCommitLog(in);
  ASSERT_TRUE(converted.ok());
  const std::vector<CorePortEvent> want = {InstructionsRetired{1}};
  EXPECT_EQ(converted.events, want);
}

TEST(CommitLog, ALoadBecomesALoadAtThePrintedAddress) {
  // The register dump is the loaded value, not a second core port event. The
  // access width is not in the commit line's mem record.
  std::stringstream in;
  in << Line(
      "core   0: 3 0x0000000080001004 (0x00054503) x10 0x00000000000000ab "
      "mem 0x0000000080002000");
  const CommitLogConversion converted = ConvertCommitLog(in);
  ASSERT_TRUE(converted.ok());
  const std::vector<CorePortEvent> want = {MemoryLoad{0x0000000080002000}};
  EXPECT_EQ(converted.events, want);
}

TEST(CommitLog, AStoreBecomesAStoreAndDropsTheAccessWidth) {
  // A byte store prints a short hex value. The core port event keeps the value
  // and not the width.
  std::stringstream in;
  in << Line(
      "core   0: 0 0x0000000080001008 (0x00a50023) mem 0x0000000080002004 "
      "0xab");
  const CommitLogConversion converted = ConvertCommitLog(in);
  ASSERT_TRUE(converted.ok());
  const std::vector<CorePortEvent> want = {
      MemoryStore{0x0000000080002004, 0xab}};
  EXPECT_EQ(converted.events, want);
}

TEST(CommitLog, ACompressedInstructionAndABlankLineStillConvert) {
  std::stringstream in;
  in << "\n";
  in << Line("core   0: 0 0x0000000080001000 (0x0001)");
  in << "\n";
  in << Line("core   0: 0 0x0000000080001002 (0x00000013)");
  const CommitLogConversion converted = ConvertCommitLog(in);
  ASSERT_TRUE(converted.ok());
  EXPECT_EQ(converted.events.size(), 2u);
  EXPECT_EQ(converted.events[0], CorePortEvent{InstructionsRetired{1}});
  EXPECT_EQ(converted.events[1], CorePortEvent{InstructionsRetired{1}});
}

TEST(CommitLog, TwoMemoryRecordsOnOneInstructionStayTwoEvents) {
  std::stringstream in;
  in << Line(
      "core   0: 0 0x0000000080001000 (0x00000003) mem 0x0000000080002000 "
      "mem 0x0000000080002001");
  const CommitLogConversion converted = ConvertCommitLog(in);
  ASSERT_TRUE(converted.ok());
  const std::vector<CorePortEvent> want = {MemoryLoad{0x0000000080002000},
                                           MemoryLoad{0x0000000080002001}};
  EXPECT_EQ(converted.events, want);
}

TEST(CommitLog, ASecondHartIsRejectedAndNotMerged) {
  std::stringstream in;
  in << Line("core   0: 0 0x0000000080001000 (0x00000013)");
  in << Line("core   1: 0 0x0000000080001004 (0x00000013)");
  const CommitLogConversion converted = ConvertCommitLog(in);
  EXPECT_FALSE(converted.ok());
  EXPECT_TRUE(converted.events.empty());
}

TEST(CommitLog, ADisassemblyLineIsRejected) {
  // spike -l, not --log-commits: no privilege digit before the pc.
  std::stringstream in;
  in << Line("core   0: 0x0000000080000000 (0x00000013) addi a0, zero, 0");
  const CommitLogConversion converted = ConvertCommitLog(in);
  EXPECT_FALSE(converted.ok());
  EXPECT_TRUE(converted.events.empty());
}

TEST(CommitLog, TheCheckedInTranscriptIsThatConversion) {
  std::ifstream log(TIMEBALL_SPIKE_LOG);
  ASSERT_TRUE(log.good());
  const CommitLogConversion converted = ConvertCommitLog(log);
  ASSERT_TRUE(converted.ok()) << converted.error;

  std::ifstream trace(TIMEBALL_SPIKE_TRACE);
  ASSERT_TRUE(trace.good());
  const auto transcript = ReadCorePortTrace(trace);
  ASSERT_TRUE(transcript.has_value());
  EXPECT_EQ(converted.events, *transcript);
}
