#pragma once
#include "core.hpp"
#include <condition_variable>
#include <mosquitto.h>
#include <thread>

namespace mb {
class Publisher {
public:
    Publisher(Store& store, const std::string& host, int port);
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    std::atomic_bool connected{false};
    std::atomic_uint64_t errors{0};
private:
    struct Library { Library(); ~Library(); } library_;
    std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> client_;
    Store& store_;
    std::string host_;
    int port_;
    bool network_started_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    int packet_{};
    bool acknowledged_{};
    std::jthread worker_;
    void run(std::stop_token stop);
};
}
