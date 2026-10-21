#include "store.hpp"
#include <iostream>
#include <filesystem>
#include <random>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool reject_commit = false;
int commit_hook(void*) { return reject_commit ? 1 : 0; }
int install_commit_hook(sqlite3* db, char**, const sqlite3_api_routines*) {
    sqlite3_commit_hook(db, commit_hook, nullptr);
    return SQLITE_OK;
}
struct CommitInjection {
    CommitInjection() {
        require(sqlite3_auto_extension(reinterpret_cast<void(*)()>(install_commit_hook)) == SQLITE_OK,
                "register SQLite extension");
    }
    ~CommitInjection() {
        reject_commit = false;
        sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install_commit_hook));
    }
};
void commit_failure() {
    CommitInjection injection;
    mb::Store store(":memory:", 1);
    auto saved = mb::decode({2300, 500, 0, 1150, 0, 42});
    store.append(saved, false);
    auto candidate = saved;
    candidate.id = 777; // Even an existing caller identity must survive failure.
    reject_commit = true;
    bool rejected = false;
    try { store.append(candidate, true); } catch (const std::runtime_error& error) {
        rejected = true;
        std::cout << "commit rejection: " << error.what() << '\n';
    }
    reject_commit = false;
    require(rejected, "commit must fail");
    require(store.recent(10).size() == 1 && store.recent(1)[0].id == saved.id,
            "commit failure restores pruned history");
    require(store.pending_count() == 0, "failed commit has no pending sample");
    std::cout << "caller id after failure: " << candidate.id << " (expected 777)\n";
    require(candidate.id == 777, "failed commit must preserve caller identity");
    store.append(candidate, true);
    require(candidate.id > saved.id && store.pending()->id == candidate.id, "retry after commit failure");
}
struct TemporaryDatabase {
    std::filesystem::path directory;
    TemporaryDatabase() {
        for (int attempt = 0; attempt < 100; ++attempt) {
            directory = std::filesystem::temp_directory_path() /
                ("meterbridge-storage-" + std::to_string(std::random_device{}()));
            if (std::filesystem::create_directory(directory)) return;
        }
        throw std::runtime_error("cannot create test directory");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::string path() const { return (directory / "readings.db").string(); }
};
void partial_prune() {
    TemporaryDatabase database;
    auto a = mb::decode({2300, 500, 0, 1150, 0, 42}), b = a, c = a, candidate = a;
    {
        mb::Store store(database.path(), 3);
        store.append(a, false);
        store.append(b, true);
        store.append(c, true);
    }
    mb::Store store(database.path(), 2);
    bool rejected = false;
    try { store.append(candidate, true); } catch (const std::runtime_error& error) {
        rejected = true;
        std::cout << "append rejection: " << error.what() << '\n';
    }
    require(rejected, "insufficient acknowledged history must reject append");
    const auto rows = store.recent(10);
    mb::Store observer(database.path(), 3);
    const auto persisted = observer.recent(10);
    std::cout << "rows after rejected prune: " << rows.size() << " (expected 3)\n"
              << "rows from independent connection: " << persisted.size() << " (expected 3)\n";
    require(persisted.size() == 3, "failed append must not change durable history");
    require(rows.size() == 3 && rows[2].id == a.id && rows[0].id == c.id,
            "partial prune must restore all history");
    require(candidate.id == 0 && store.pending_count() == 2 && store.pending()->id == b.id,
            "rejection preserves identity and pending FIFO");
    store.acknowledge(b.id);
    store.append(candidate, true);
    require(store.recent(10).size() == 2 && store.pending()->id == c.id,
            "same connection recovers after rejected prune");
}
struct Connection {
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{nullptr, sqlite3_close};
    explicit Connection(const std::string& path) {
        sqlite3* raw = nullptr;
        const int result = sqlite3_open(path.c_str(), &raw);
        db.reset(raw);
        require(result == SQLITE_OK, "open independent SQLite connection");
    }
    void sql(const char* command) {
        if (sqlite3_exec(db.get(), command, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db.get()));
    }
};
void insert_failure() {
    TemporaryDatabase database;
    mb::Store store(database.path(), 2);
    auto a = mb::decode({2300, 500, 0, 1150, 0, 42}), b = a, candidate = a;
    store.append(a, false);
    store.append(b, true);
    Connection control(database.path());
    control.sql("CREATE TRIGGER reject_insert BEFORE INSERT ON readings "
                "BEGIN SELECT RAISE(ABORT, 'test insert rejection'); END");
    bool rejected = false;
    try { store.append(candidate, true); } catch (const std::runtime_error& error) {
        rejected = true;
        std::cout << "insert rejection: " << error.what() << '\n';
    }
    require(rejected && candidate.id == 0, "failed insert preserves caller identity");
    require(store.recent(10).size() == 2 && store.recent(10)[1].id == a.id,
            "insert failure rolls back prior eviction");
    require(store.pending_count() == 1 && store.pending()->id == b.id, "insert failure preserves pending row");
    control.sql("DROP TRIGGER reject_insert");
    store.append(candidate, true);
    require(candidate.id > b.id && store.recent(10).size() == 2, "retry after insert failure");
}
void reopen() {
    TemporaryDatabase database;
    auto a = mb::decode({2300, 500, 0xffff, 0xfe0c, 0xffff, 0xffff}), b = a, c = a;
    {
        mb::Store store(database.path(), 3);
        store.append(a, true);
        store.append(b, true);
        store.append(c, false);
        store.acknowledge(a.id);
        store.acknowledge(a.id); // Repeated acknowledgement is harmless.
    }
    {
        mb::Store store(database.path(), 3);
        require(store.pending_count() == 1 && store.pending()->id == b.id,
                "only unacknowledged rows replay on reopen");
        const auto rows = store.recent(10);
        require(rows.size() == 3 && rows[0].id == c.id && rows[2].id == a.id, "history order survives reopen");
        require(rows[0].power_w == -500 && rows[0].energy_wh == 4294967295U &&
                rows[0].timestamp_ms == c.timestamp_ms && rows[0].voltage_v == c.voltage_v,
                "numeric boundaries survive persistence");
        auto next = c;
        store.append(next, true);
        require(next.id > c.id && store.pending()->id == b.id && store.pending_count() == 2,
                "retention preserves older pending row and monotonic ids");
        store.acknowledge(b.id);
        store.acknowledge(next.id);
    }
    mb::Store store(database.path(), 3);
    require(!store.pending() && store.recent(10).size() == 3, "acknowledgement survives second reopen");
}
void lock_recovery() {
    TemporaryDatabase database;
    mb::Store store(database.path(), 2);
    Connection writer(database.path());
    writer.sql("BEGIN IMMEDIATE");
    auto candidate = mb::decode({2300, 500, 0, 1150, 0, 42});
    bool rejected = false;
    try { store.append(candidate, true); } catch (const std::runtime_error& error) {
        rejected = true;
        std::cout << "writer contention: " << error.what() << '\n';
    }
    require(rejected && candidate.id == 0 && store.recent(10).empty(), "busy begin preserves state");
    writer.sql("ROLLBACK");
    store.append(candidate, true);
    require(candidate.id > 0 && store.pending()->id == candidate.id, "retry after writer releases lock");
}
void invalid_capacity() {
    TemporaryDatabase database;
    for (const int capacity : {0, -1}) {
        bool rejected = false;
        try { mb::Store store(database.path(), capacity); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "nonpositive capacity must fail at construction");
        require(!std::filesystem::exists(database.path()), "invalid capacity must not create a database");
    }
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
int main(int argc, char** argv) {
    try {
        const std::string scenario = argc == 2 ? argv[1] : "baseline";
        if (scenario == "baseline") baseline();
        else if (scenario == "commit_failure") commit_failure();
        else if (scenario == "partial_prune") partial_prune();
        else if (scenario == "invalid_capacity") invalid_capacity();
        else if (scenario == "insert_failure") insert_failure();
        else if (scenario == "reopen") reopen();
        else if (scenario == "lock_recovery") lock_recovery();
        else throw std::runtime_error("unknown scenario: " + scenario);
        std::cout << "PASS " << scenario << " (SQLite " << sqlite3_libversion() << ")\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
