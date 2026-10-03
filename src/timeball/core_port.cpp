// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/core_port.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace timeball {

// Declared with its reasoning in core_port.hpp.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
CommandDevice::CommandDevice(std::string_view name, std::size_t params,
                             uint64_t start_offset, uint64_t status_offset,
                             Cost cost)
    : name_(name),
      params_(params, 0),
      start_(start_offset),
      status_(status_offset),
      cost_(std::move(cost)) {}

Cycle CommandDevice::OnWrite(uint64_t offset, uint64_t value) {
  if (offset == start_) {
    return cost_(params_);
  }
  if (offset % 8 == 0 && offset / 8 < params_.size()) {
    params_[offset / 8] = value;
  }
  return 0;
}

CorePort::CorePort(EventEngine& engine, AccessNode& memory,
                   CorePortConfig config)
    : engine_(&engine),
      memory_(&memory),
      config_(config),
      core_(engine.AddResource({"core", 1})) {}

void CorePort::Attach(uint64_t base, uint64_t size, MmioDevice& device) {
  assert(size > 0 && "Device window is empty");
  assert(base + size > base && "Device window wraps the address space");
  for (const Window& w : windows_) {
    assert((base + size <= w.base || w.end <= base) &&
           "Device windows overlap");
    (void)w;
  }
  windows_.push_back({.base = base,
                      .end = base + size,
                      .device = &device,
                      .resource = engine_->AddResource(
                          {std::string(device.DeviceName()), 1})});
}

CorePort::Window* CorePort::WindowOf(uint64_t addr) {
  for (Window& w : windows_) {
    if (addr >= w.base && addr < w.end) {
      return &w;
    }
  }
  return nullptr;
}

EventId CorePort::CoreOp(std::string_view name, Cycle cost,
                         std::vector<EventId> also_after) {
  if (last_ != 0) {
    also_after.push_back(last_);
  }
  last_ = engine_->Submit(
      {name, core_, config_.core, cost, {0, std::move(also_after)}});
  return last_;
}

void CorePort::MemoryAccess(uint64_t addr, AccessType type) {
  When when;
  if (last_ != 0) {
    when.after.push_back(last_);
  }
  last_ = engine_->SubmitAccess(
      *memory_, {.addr = addr, .type = type, .initiator_id = config_.core},
      std::move(when));
}

void CorePort::OnInstructions(uint64_t count) {
  if (count > 0) {
    const uint64_t cpi = config_.cpi.value();
    const Cycle cost = cpi != 0 && count > Cycle::Max().value() / cpi
                           ? Cycle::Max()
                           : Cycle(count * cpi);
    CoreOp("compute", cost);
  }
}

void CorePort::OnLoad(uint64_t addr) {
  if (Window* w = WindowOf(addr)) {
    std::vector<EventId> after;
    if (w->last_work != 0 && w->device->WaitsForWork(addr - w->base)) {
      after.push_back(w->last_work);
    }
    CoreOp("mmio_read", config_.mmio_cycles, std::move(after));
    return;
  }
  MemoryAccess(addr, AccessType::kLoad);
}

void CorePort::OnStore(uint64_t addr, uint64_t value) {
  if (Window* w = WindowOf(addr)) {
    const EventId write = CoreOp("mmio_write", config_.mmio_cycles);
    const Cycle work = w->device->OnWrite(addr - w->base, value);
    if (work > 0) {
      // Launched by the write, and served by the device in the order its
      // commands arrive; the core does not wait for it.
      w->last_work = engine_->Submit(
          {"work", w->resource, config_.core, work, {0, {write}}});
    }
    return;
  }
  MemoryAccess(addr, AccessType::kStore);
}

void CorePort::Apply(const CorePortEvent& event) {
  std::visit(
      [this](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, InstructionsRetired>) {
          OnInstructions(e.count);
        } else if constexpr (std::is_same_v<T, MemoryLoad>) {
          OnLoad(e.addr);
        } else {
          static_assert(std::is_same_v<T, MemoryStore>);
          OnStore(e.addr, e.value);
        }
      },
      event);
}

void CorePort::Apply(std::span<const CorePortEvent> events) {
  for (const CorePortEvent& event : events) {
    Apply(event);
  }
}

void CorePort::Sync(RecordSink* sink) {
  engine_->RunUntilIdle(sink);
  // Read the answers before the engine may forget them. After an idle run
  // the only id without a cycle is one a previous window already retired.
  if (last_ != 0) {
    const auto completion = engine_->CompletionOf(last_);
    if (completion) {
      now_ = *completion;
    } else if (completion.error() == Absent::kPending) {
      assert(false &&
             "core work is still pending after the engine ran to idle");
      now_ = Cycle::Max();
    }
  }
  for (Window& w : windows_) {
    if (w.last_work == 0) {
      continue;
    }
    const auto completion = engine_->CompletionOf(w.last_work);
    if (completion) {
      w.busy_until = *completion;
    } else if (completion.error() == Absent::kPending) {
      assert(false &&
             "device work is still pending after the engine ran to idle");
      w.busy_until = Cycle::Max();
    }
  }
  // Everything the core submits from here depends on its last work, so
  // nothing can begin before it completed: a safe horizon, and the engine may
  // forget what finished before it.
  engine_->RunUntil(now_, sink);
}

Cycle CorePort::DeviceBusyUntil(const MmioDevice& device) const {
  for (const Window& w : windows_) {
    if (w.device == &device) {
      return w.busy_until;
    }
  }
  assert(false && "Device is not attached to this CorePort");
  return Cycle::Max();
}

}  // namespace timeball
