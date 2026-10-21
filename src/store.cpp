#include "store.hpp"
#include <stdexcept>

namespace mb {
namespace {
void sql(sqlite3* db, const char* text) {
    if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
struct Statement {
    sqlite3_stmt* ptr{};
    sqlite3* db;
    Statement(sqlite3* connection, const char* text) : db(connection) {
        if (sqlite3_prepare_v2(db, text, -1, &ptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(ptr); }
    int step() {
        const int rc = sqlite3_step(ptr);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        return rc;
    }
};
Reading row(sqlite3_stmt* s) {
    return {sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1),
            sqlite3_column_double(s, 2), sqlite3_column_double(s, 3),
            sqlite3_column_int(s, 4), static_cast<std::uint32_t>(sqlite3_column_int64(s, 5))};
}
}

Store::Store(const std::string& path, int max_rows) : max_rows_(max_rows) {
    try {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
        sqlite3_busy_timeout(db_, 500);
        sql(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
                 "CREATE TABLE IF NOT EXISTS readings("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp_ms INTEGER NOT NULL,"
                 "voltage_v REAL NOT NULL, current_a REAL NOT NULL, power_w INTEGER NOT NULL,"
                 "energy_wh INTEGER NOT NULL, sent INTEGER NOT NULL CHECK(sent IN (0,1)));"
                 "CREATE INDEX IF NOT EXISTS outbox ON readings(sent,id);");
    } catch (...) { if (db_) sqlite3_close(db_); throw; }
}
Store::~Store() { sqlite3_close(db_); }
void Store::append(Reading& r, bool pending) {
    std::lock_guard lock(mutex_);
    sql(db_, "BEGIN IMMEDIATE");
    try {
        Statement count(db_, "SELECT COUNT(*) FROM readings");
        count.step();
        const int excess = sqlite3_column_int(count.ptr, 0) - max_rows_ + 1;
        if (excess > 0) {
            Statement prune(db_, "DELETE FROM readings WHERE id IN (SELECT id FROM readings WHERE sent=1 ORDER BY id LIMIT ?)");
            sqlite3_bind_int(prune.ptr, 1, excess);
            prune.step();
            if (sqlite3_changes(db_) < excess) throw std::runtime_error("outbox full; sample rejected");
        }
        Statement insert(db_, "INSERT INTO readings(timestamp_ms,voltage_v,current_a,power_w,energy_wh,sent) VALUES(?,?,?,?,?,?)");
        sqlite3_bind_int64(insert.ptr, 1, r.timestamp_ms);
        sqlite3_bind_double(insert.ptr, 2, r.voltage_v);
        sqlite3_bind_double(insert.ptr, 3, r.current_a);
        sqlite3_bind_int(insert.ptr, 4, r.power_w);
        sqlite3_bind_int64(insert.ptr, 5, r.energy_wh);
        sqlite3_bind_int(insert.ptr, 6, pending ? 0 : 1);
        insert.step();
        r.id = sqlite3_last_insert_rowid(db_);
        sql(db_, "COMMIT");
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}
std::vector<Reading> Store::recent(int limit) {
    std::lock_guard lock(mutex_);
    Statement query(db_, "SELECT id,timestamp_ms,voltage_v,current_a,power_w,energy_wh FROM readings ORDER BY id DESC LIMIT ?");
    sqlite3_bind_int(query.ptr, 1, limit);
    std::vector<Reading> result;
    while (query.step() == SQLITE_ROW) result.push_back(row(query.ptr));
    return result;
}
std::optional<Reading> Store::pending() {
    std::lock_guard lock(mutex_);
    Statement query(db_, "SELECT id,timestamp_ms,voltage_v,current_a,power_w,energy_wh FROM readings WHERE sent=0 ORDER BY id LIMIT 1");
    if (query.step() == SQLITE_ROW) return row(query.ptr);
    return std::nullopt;
}
void Store::acknowledge(std::int64_t id) {
    std::lock_guard lock(mutex_);
    Statement update(db_, "UPDATE readings SET sent=1 WHERE id=?");
    sqlite3_bind_int64(update.ptr, 1, id);
    update.step();
}
int Store::pending_count() {
    std::lock_guard lock(mutex_);
    Statement query(db_, "SELECT COUNT(*) FROM readings WHERE sent=0");
    query.step();
    return sqlite3_column_int(query.ptr, 0);
}
}
