#include "store.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void baseline() {
    const auto normal = mb::decode({2300, 500, 0, 1150, 1, 0});
    require(normal.voltage_v == 230 && normal.current_a == 5 && normal.power_w == 1150 && normal.energy_wh == 65536,
            "register decoding");
    require(mb::decode({0, 0, 0x8000, 0, 0xffff, 0xffff}).power_w == std::numeric_limits<std::int32_t>::min(),
            "signed power boundary");
    require(mb::decode({0, 0, 0, 0, 0xffff, 0xffff}).energy_wh == 4294967295U, "unsigned energy boundary");
    mb::Store store(":memory:", 2);
    require(store.recent(10).empty() && !store.pending(), "empty database");
    auto a = normal, b = normal, c = normal;
    store.append(a, true);
    store.append(b, true);
    require(a.id < b.id && store.pending()->id == a.id, "pending FIFO");
    bool rejected = false;
    try { store.append(c, true); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected && store.pending_count() == 2 && store.recent(10).size() == 2, "backpressure rollback");
    store.acknowledge(a.id);
    store.append(c, false);
    require(c.id > b.id && store.pending_count() == 1 && store.recent(10).size() == 2, "acknowledged retention");
    store.acknowledge(b.id);
    require(!store.pending(), "outbox drains");
}
}
int main() {
    try {
        baseline();
        std::cout << "PASS baseline\n";
        std::cout << "SQLite " << sqlite3_libversion() << ": 1 scenario passed\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
