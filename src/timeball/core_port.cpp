// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/core_port.hpp"

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace timeball {

CorePort::CorePort(EventEngine& engine, AccessNode& entry,
                   CorePortConfig config)
    : engine_(&engine),
      entry_(&entry),
      config_(config),
      core_(engine.AddResource({"core", 1})) {}

EventId CorePort::CoreOp(std::string_view name, Cycle cost) {
  std::vector<EventId> after;
  if (last_ != 0) {
    after.push_back(last_);
  }
  last_ =
      engine_->Submit({name, core_, config_.core, cost, {0, std::move(after)}});
  return last_;
}

void CorePort::AddressAccess(uint64_t addr, AccessType type, uint64_t value) {
  When when;
  if (last_ != 0) {
    when.after.push_back(last_);
  }
  last_ = engine_->SubmitAccess(*entry_,
                                {.addr = addr,
                                 .type = type,
                                 .initiator_id = config_.core,
                                 .value = value},
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
  AddressAccess(addr, AccessType::kLoad);
}

void CorePort::OnStore(uint64_t addr, uint64_t value) {
  AddressAccess(addr, AccessType::kStore, value);
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
  // Read the answer before the engine may forget it.
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
  // Everything the core submits from here depends on its last work, so nothing
  // can begin before it completed: a safe horizon, and the engine may forget
  // what finished before it.
  engine_->RunUntil(now_, sink);
}

}  // namespace timeball
