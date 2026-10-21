#include "reading.hpp"
#include <bit>
#include <chrono>
#include <iomanip>
#include <locale>
#include <sstream>

namespace mb {
std::string Reading::json() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2)
        << "{\"id\":" << id << ",\"timestamp_ms\":" << timestamp_ms
        << ",\"voltage_v\":" << voltage_v << ",\"current_a\":" << current_a
        << ",\"power_w\":" << power_w << ",\"energy_wh\":" << energy_wh << '}';
    return out.str();
}
Reading decode(const std::array<std::uint16_t, 6>& w) {
    const auto power = (std::uint32_t(w[2]) << 16) | w[3];
    const auto energy = (std::uint32_t(w[4]) << 16) | w[5];
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return {0, std::chrono::duration_cast<std::chrono::milliseconds>(now).count(),
            w[0] / 10.0, w[1] / 100.0, std::bit_cast<std::int32_t>(power), energy};
}
}
