// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_COMMAND_DEVICE_HPP
#define TIMEBALL_COMMAND_DEVICE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "timeball/node.hpp"

namespace timeball {

// Register target with parameters, START, and STATUS. START launches work;
// STATUS waits for it. The host owns functional data, so reads return timing.
class CommandDevice final : public AccessNode {
 public:
  using Cost = std::function<Cycle(const std::vector<uint64_t>& params)>;

  // name must outlive this device. base is the start of its mapped region.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  CommandDevice(std::string_view name, uint64_t base, Cycle access_cycles,
                std::size_t params, uint64_t start_offset,
                uint64_t status_offset, Cost cost);

  [[nodiscard]] std::string_view NodeName() const override { return name_; }
  Route Serve(const Request& r) override;

 private:
  std::string_view name_;
  uint64_t base_;
  Cycle access_cycles_;
  std::vector<uint64_t> params_;
  uint64_t start_;
  uint64_t status_;
  Cost cost_;
};

}  // namespace timeball

#endif  // TIMEBALL_COMMAND_DEVICE_HPP
