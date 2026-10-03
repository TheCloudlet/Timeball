// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_CORE_PORT_HPP
#define TIMEBALL_CORE_PORT_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "timeball/core_port_trace.hpp"
#include "timeball/event_engine.hpp"
#include "timeball/node.hpp"

// A core's load/store path. Retired instructions and the accesses between
// them become time. Plain addresses enter the memory graph. An address in a
// device window programs that device.

namespace timeball {

// A device the core programs through memory-mapped registers.
class MmioDevice {
 public:
  virtual ~MmioDevice() = default;

  // Names the device's resource on the timeline. Static storage: a literal,
  // or a name that outlives the engine.
  [[nodiscard]] virtual std::string_view DeviceName() const = 0;

  // Cycles of work this write launches, or 0 if it only sets a parameter.
  virtual Cycle OnWrite(uint64_t offset, uint64_t value) = 0;

  // True when a read of offset waits for the device's outstanding work.
  [[nodiscard]] virtual bool WaitsForWork(uint64_t offset) const = 0;
};

// Parameter registers, a start register, and a status register.
// Parameter i is the 8-byte register at offset 8 * i. A write to start
// launches work costed from the parameters. A read of status waits until
// that work has finished.
class CommandDevice final : public MmioDevice {
 public:
  using Cost = std::function<Cycle(const std::vector<uint64_t>& params)>;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  CommandDevice(std::string_view name, std::size_t params,
                uint64_t start_offset, uint64_t status_offset, Cost cost);

  [[nodiscard]] std::string_view DeviceName() const override { return name_; }
  Cycle OnWrite(uint64_t offset, uint64_t value) override;
  [[nodiscard]] bool WaitsForWork(uint64_t offset) const override {
    return offset == status_;
  }

 private:
  std::string_view name_;
  std::vector<uint64_t> params_;
  uint64_t start_;
  uint64_t status_;
  Cost cost_;
};

struct CorePortConfig {
  Cycle cpi = 1;          // core cycles per retired instruction
  Cycle mmio_cycles = 1;  // one register read or write
  InitiatorId core = 0;   // whose accesses these are
};

// One in-order core. Each call waits for that core's previous work.
// One CorePort per engine. Sync runs to this core's time and promises nothing
// begins earlier. It does not interleave several cores.
class CorePort {
 public:
  // The engine and memory must outlive the CorePort.
  CorePort(EventEngine& engine, AccessNode& memory, CorePortConfig config = {});

  // Register window [base, base + size). Overlap asserts. device must
  // outlive the CorePort.
  void Attach(uint64_t base, uint64_t size, MmioDevice& device);

  // The core retired count instructions since the last call.
  void OnInstructions(uint64_t count);
  void OnLoad(uint64_t addr);
  void OnStore(uint64_t addr, uint64_t value);

  // Replay recorded OnInstructions, OnLoad, and OnStore calls. The stream
  // can be run against another CorePort without the functional simulator.
  void Apply(const CorePortEvent& event);
  void Apply(std::span<const CorePortEvent> events);

  // Times what has been submitted, then forgets what finished.
  void Sync(RecordSink* sink = nullptr);

  // The core's time: when its last work completed, as of the last Sync.
  [[nodiscard]] Cycle Now() const { return now_; }
  // When the device's last launched work completes, as of the last Sync.
  [[nodiscard]] Cycle DeviceBusyUntil(const MmioDevice& device) const;

 private:
  struct Window {
    uint64_t base = 0;
    uint64_t end = 0;  // exclusive
    MmioDevice* device = nullptr;
    ResourceId resource = 0;
    EventId last_work = 0;
    Cycle busy_until = 0;  // last_work's completion, read at the last Sync
  };

  Window* WindowOf(uint64_t addr);
  // Work on the core, after its previous work and anything in also_after.
  EventId CoreOp(std::string_view name, Cycle cost,
                 std::vector<EventId> also_after = {});
  // A load or store to the memory graph, after the core's previous work.
  void MemoryAccess(uint64_t addr, AccessType type);

  EventEngine* engine_;
  AccessNode* memory_;
  CorePortConfig config_;
  ResourceId core_;
  std::vector<Window> windows_;
  EventId last_ = 0;
  Cycle now_ = 0;  // last_'s completion, read at the last Sync
};

}  // namespace timeball

#endif  // TIMEBALL_CORE_PORT_HPP
