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


}
