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
#include "store.hpp"

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


}
