#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <modbus/modbus.h>
#include <sqlite3.h>

namespace mb {
using Clock = std::chrono::steady_clock;

class Args {
public:
    Args(int argc, char** argv, std::initializer_list<std::string> allowed);
    std::string text(const std::string& key, const std::string& fallback = "") const;
    int number(const std::string& key, int fallback, int low, int high) const;
private:
    std::map<std::string, std::string> values_;
};

struct Reading {
    std::int64_t id{}, timestamp_ms{};
    double voltage_v{}, current_a{};
    std::int32_t power_w{};
    std::uint32_t energy_wh{};
    std::string json() const;
};
Reading decode(const std::array<std::uint16_t, 6>& words);
void wait(std::atomic_bool& stop, int milliseconds);
void signals();
bool interrupted();

struct ModbusDelete { void operator()(modbus_t* p) const; };
using Modbus = std::unique_ptr<modbus_t, ModbusDelete>;
Modbus context(const std::string& host, int port, const std::string& serial, int baud, int unit);

class Meter {
public:
    Meter(const std::string& host, int port, const std::string& serial, int baud, int unit);
    Reading read();
private:
    Modbus ctx_;
    std::string host_, serial_;
    int port_;
};

// One connection, serialized across poller, REST and outbox workers.
class Store {
public:
    explicit Store(const std::string& path, int max_rows = 10000);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void append(Reading& reading, bool pending);
    std::vector<Reading> recent(int limit);
    std::optional<Reading> pending();
    void acknowledge(std::int64_t id);
    int pending_count();
private:
    sqlite3* db_{};
    std::mutex mutex_;
    int max_rows_;
};
}
