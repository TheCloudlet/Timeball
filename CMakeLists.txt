cmake_minimum_required(VERSION 3.14)
project(Timeball VERSION 1.0)

# As a subdirectory of a host project, timeball must not change the host's
# settings or build its examples and tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(TIMEBALL_IS_TOP_LEVEL ON)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED True)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else()
  set(TIMEBALL_IS_TOP_LEVEL OFF)
endif()

# The engine. Node types are templates and stay in headers; everything that
# need not be — the timeline, routing — compiles once into here, so a host
# embedding timeball does not pay for it in every translation unit.
add_library(timeball_engine
  src/timeball/address_map.cpp
  src/timeball/command_device.cpp
  src/timeball/dma_device.cpp
  src/timeball/core_port.cpp
  src/timeball/core_port_trace.cpp
  src/timeball/checking_sink.cpp
  src/timeball/event_engine.cpp
)
target_compile_features(timeball_engine PUBLIC cxx_std_20)
target_include_directories(timeball_engine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
add_library(Timeball::engine ALIAS timeball_engine)
add_library(timeball::engine ALIAS timeball_engine)

# Recording to a database is opt-out: -DTIMEBALL_WITH_SQLITE=OFF drops the
# event store and the sqlite3 dependency entirely, so a host that wants timing
# and no analysis neither compiles nor links it.
option(TIMEBALL_WITH_SQLITE "Record events to a SQLite file" ON)
if(TIMEBALL_WITH_SQLITE)
  find_package(SQLite3)
  if(SQLite3_FOUND)
    target_sources(timeball_engine PRIVATE src/timeball/event_store.cpp)
    # FindSQLite3 names its target SQLite::SQLite3 before CMake 4.1 and
    # SQLite3::SQLite3 from then on.
    if(TARGET SQLite3::SQLite3)
      target_link_libraries(timeball_engine PUBLIC SQLite3::SQLite3)
    else()
      target_link_libraries(timeball_engine PUBLIC SQLite::SQLite3)
    endif()
    target_compile_definitions(timeball_engine PUBLIC TIMEBALL_WITH_SQLITE=1)
  else()
    message(WARNING "SQLite3 not found; building without the event store.")
  endif()
endif()

option(TIMEBALL_BUILD_EXAMPLES "Build the examples and benchmark"
       ${TIMEBALL_IS_TOP_LEVEL})
if(TIMEBALL_BUILD_EXAMPLES)
  # The example host: a stand-in functional simulator timed by hooking only its
  # load/store path. Needs nothing beyond the engine.
  add_executable(mmio_accelerator examples/mmio_accelerator.cpp)
  target_link_libraries(mmio_accelerator PRIVATE timeball_engine)

  # Record a run's CorePortEvents once, replay against different hardware parameters.
  add_executable(core_port_replay examples/core_port_replay.cpp)
  target_link_libraries(core_port_replay PRIVATE timeball_engine)

  # One committed transcript, replayed against two chip graphs.
  add_executable(transcript_replay examples/transcript_replay.cpp)
  target_link_libraries(transcript_replay PRIVATE timeball_engine)
  target_compile_definitions(transcript_replay PRIVATE
    TIMEBALL_TRANSCRIPT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/data/transcript.trace")

  # One committed transcript, two caches. The converter is the example's:
  # libtimeball_engine does not gain a Spike parser.
  add_executable(commit_log examples/spike/commit_log_main.cpp
    examples/spike/commit_log.cpp)
  target_link_libraries(commit_log PRIVATE timeball_engine)
  target_include_directories(commit_log PRIVATE examples/spike)

  add_executable(two_machines examples/spike/two_machines.cpp)
  target_link_libraries(two_machines PRIVATE timeball_engine)
  target_include_directories(two_machines PRIVATE examples/spike)

  # Throughput and footprint, one scenario per run: bench/throughput.cpp.
  add_executable(throughput bench/throughput.cpp)
  target_link_libraries(throughput PRIVATE timeball_engine)
endif()

# Testing is opt-in: -DTIMEBALL_BUILD_TESTS=ON. Off, building the engine and
# the example needs nothing but a compiler — no network, no GoogleTest.
option(TIMEBALL_BUILD_TESTS "Build the unit tests (fetches GoogleTest)" OFF)
if(TIMEBALL_BUILD_TESTS)
  # GoogleTest, fetched at configure time rather than assumed installed.
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  add_executable(unit_tests test/unit/test_components.cpp test/unit/test_core_port.cpp
    test/unit/test_core_port_trace.cpp test/unit/test_checking_sink.cpp
    test/unit/test_commit_log.cpp test/unit/test_machine.cpp
    test/unit/test_record_query.cpp test/unit/test_reference_scheduler.cpp
    test/unit/test_strong_ids.cpp test/unit/test_two_machines.cpp
    examples/spike/commit_log.cpp)
  target_include_directories(unit_tests PRIVATE examples/spike)
  target_compile_definitions(unit_tests PRIVATE
    "TIMEBALL_SPIKE_LOG=\"${CMAKE_CURRENT_SOURCE_DIR}/examples/spike/commit.log\""
    "TIMEBALL_SPIKE_TRACE=\"${CMAKE_CURRENT_SOURCE_DIR}/examples/spike/program.trace\"")
  target_link_libraries(unit_tests PRIVATE timeball_engine GTest::gtest_main)
  gtest_discover_tests(unit_tests)

  add_executable(event_engine_tests test/unit/test_event_engine.cpp)
  target_link_libraries(event_engine_tests PRIVATE timeball_engine GTest::gtest_main)
  gtest_discover_tests(event_engine_tests)

  if(TIMEBALL_BUILD_EXAMPLES)
    # The example must keep running, and keep its answer, as the engine
    # changes: 8 tiles of a 4096-cycle multiply, overlapped with the core.
    add_test(NAME mmio_accelerator_example COMMAND mmio_accelerator)
    set_tests_properties(mmio_accelerator_example PROPERTIES
      PASS_REGULAR_EXPRESSION "total: 33800 cycles")

    # core_port_replay.cpp itself asserts faster hardware finishes the recorded
    # program sooner and slower hardware finishes it later; the three exact
    # cycle counts here pin the record/replay pipeline's own arithmetic
    # without also pinning the printed labels, so relabelling the scenarios
    # does not break this test.
    add_test(NAME core_port_replay_example COMMAND core_port_replay)
    set_tests_properties(core_port_replay_example PROPERTIES
      PASS_REGULAR_EXPRESSION "5106 cycles.*3053 cycles.*9212 cycles")

    # transcript_replay.cpp checks the ordering itself and exits non-zero if
    # it does not hold, so this test locks that ordering only — never the
    # absolute cycle counts, which change if the transcript does (issue #34).
    add_test(NAME transcript_replay_example COMMAND transcript_replay)

    # The example prints both completion cycles and exits non-zero when the
    # one-line cache does not finish later. The pattern checks that both
    # numbers were printed. It does not pin them; test_two_machines.cpp locks
    # the order.
    add_test(NAME two_machines_example COMMAND two_machines
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/spike/program.trace)
    set_tests_properties(two_machines_example PROPERTIES
      PASS_REGULAR_EXPRESSION
      "one line: +[0-9]+ cycles.*working set: +[0-9]+ cycles")
  endif()
endif()
