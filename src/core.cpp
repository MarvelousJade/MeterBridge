#include "core.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <bit>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace mb {
namespace {
volatile std::sig_atomic_t stopped = 0;
void on_signal(int) { stopped = 1; }
std::runtime_error modbus_error() { return std::runtime_error(modbus_strerror(errno)); }
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
// Numeric IPv4 and a bounded connect avoid blocking shutdown on DNS or TCP retries.
int connect_tcp(const std::string& host, int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("meter host must be numeric IPv4");
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) throw modbus_error();
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        pollfd event{fd, POLLOUT, 0};
        int error = 0;
        socklen_t size = sizeof(error);
        if (errno != EINPROGRESS || poll(&event, 1, 500) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0) {
            close(fd);
            throw std::runtime_error("meter connection failed or timed out");
        }
    }
    if (fcntl(fd, F_SETFL, 0) < 0) { close(fd); throw modbus_error(); }
    return fd;
}
}

Args::Args(int argc, char** argv, std::initializer_list<std::string> allowed) {
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc || std::find(allowed.begin(), allowed.end(), argv[i]) == allowed.end())
            throw std::runtime_error("unknown option or missing value: " + std::string(argv[i]));
        if (!values_.emplace(argv[i], argv[i + 1]).second)
            throw std::runtime_error("duplicate option: " + std::string(argv[i]));
    }
}
std::string Args::text(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}
int Args::number(const std::string& key, int fallback, int low, int high) const {
    const auto value = text(key, std::to_string(fallback));
    int result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < low || result > high)
        throw std::runtime_error("invalid value for " + key);
    return result;
}
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
void wait(std::atomic_bool& stop, int milliseconds) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stop && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
void signals() { std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal); std::signal(SIGPIPE, SIG_IGN); }
bool interrupted() { return stopped != 0; }
void ModbusDelete::operator()(modbus_t* p) const {
    if (modbus_get_socket(p) >= 0) modbus_close(p);
    modbus_free(p);
}
Modbus context(const std::string& host, int port, const std::string& serial, int baud, int unit) {
    Modbus ctx(serial.empty() ? modbus_new_tcp(host.c_str(), port)
                             : modbus_new_rtu(serial.c_str(), baud, 'N', 8, 1));
    if (!ctx) throw modbus_error();
    if (modbus_set_slave(ctx.get(), unit) < 0 ||
        modbus_set_response_timeout(ctx.get(), 0, 500000) < 0 ||
        modbus_set_byte_timeout(ctx.get(), 0, 100000) < 0)
        throw modbus_error();
    return ctx;
}
Meter::Meter(const std::string& host, int port, const std::string& serial, int baud, int unit)
    : ctx_(context(host, port, serial, baud, unit)), host_(host), serial_(serial), port_(port) {}
Reading Meter::read() {
    if (modbus_get_socket(ctx_.get()) < 0) {
        if (serial_.empty()) modbus_set_socket(ctx_.get(), connect_tcp(host_, port_));
        else if (modbus_connect(ctx_.get()) < 0) throw modbus_error();
    }
    std::array<std::uint16_t, 6> words{};
    if (modbus_read_registers(ctx_.get(), 0, static_cast<int>(words.size()), words.data()) != 6) {
        auto error = modbus_error();
        modbus_close(ctx_.get());
        modbus_set_socket(ctx_.get(), -1);
        throw error;
    }
    return decode(words);
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
