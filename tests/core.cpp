#include "core.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        const auto normal = mb::decode({2300, 500, 0, 1150, 1, 0});
        require(normal.voltage_v == 230 && normal.current_a == 5 && normal.power_w == 1150 && normal.energy_wh == 65536,
                "scaling and high-word-first decoding");
        require(mb::decode({0, 0, 0xffff, 0xfe0c, 0xffff, 0xffff}).power_w == -500, "signed export power");
        require(mb::decode({0, 0, 0x8000, 0, 0, 0}).power_w == std::numeric_limits<std::int32_t>::min(), "signed boundary");
        require(mb::decode({0, 0, 0, 0, 0xffff, 0xffff}).energy_wh == 4294967295U, "unsigned energy boundary");
        mb::Store store(":memory:", 2);
        auto a = normal, b = normal, c = normal;
        store.append(a, true);
        store.append(b, true);
        require(a.id < b.id && store.pending()->id == a.id, "outbox order");
        bool full = false;
        try { store.append(c, true); } catch (const std::runtime_error&) { full = true; }
        require(full && store.pending_count() == 2 && store.recent(10).size() == 2, "full outbox rolls back");
        store.acknowledge(a.id);
        store.append(c, false);
        require(c.id > b.id && store.pending_count() == 1 && store.recent(10).size() == 2, "evict acknowledged history only");
        store.acknowledge(b.id);
        require(!store.pending(), "acknowledged outbox drains");
        std::cout << "core checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
