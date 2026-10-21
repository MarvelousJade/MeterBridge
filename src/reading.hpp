#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace mb {
struct Reading {
    std::int64_t id{}, timestamp_ms{};
    double voltage_v{}, current_a{};
    std::int32_t power_w{};
    std::uint32_t energy_wh{};
    std::string json() const;
};
Reading decode(const std::array<std::uint16_t, 6>& words);
}
