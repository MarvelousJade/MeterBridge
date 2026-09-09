#include "mqtt.hpp"
#include <arpa/inet.h>
#include <iostream>
#include <stdexcept>

namespace mb {
namespace {
void check(int code) {
    if (code != MOSQ_ERR_SUCCESS) throw std::runtime_error(mosquitto_strerror(code));
}
}
Publisher::Library::Library() { check(mosquitto_lib_init()); }
Publisher::Library::~Library() { mosquitto_lib_cleanup(); }
Publisher::Publisher(Store& store, const std::string& host, int port)
    : client_(mosquitto_new(nullptr, true, this), mosquitto_destroy), store_(store), host_(host), port_(port) {
    if (!client_) throw std::runtime_error("cannot create MQTT client");
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) != 1)
        throw std::runtime_error("MQTT host must be numeric IPv4");
    mosquitto_connect_callback_set(client_.get(), [](mosquitto*, void* data, int rc) {
        auto& self = *static_cast<Publisher*>(data);
        self.connected = rc == 0;
        self.cv_.notify_all();
    });
    mosquitto_disconnect_callback_set(client_.get(), [](mosquitto*, void* data, int) {
        auto& self = *static_cast<Publisher*>(data);
        self.connected = false;
        self.cv_.notify_all();
    });
    mosquitto_publish_callback_set(client_.get(), [](mosquitto*, void* data, int packet) {
        auto& self = *static_cast<Publisher*>(data);
        std::lock_guard lock(self.mutex_);
        if (packet == self.packet_) self.acknowledged_ = true;
        self.cv_.notify_all();
    });
    check(mosquitto_reconnect_delay_set(client_.get(), 1, 5, true));
    check(mosquitto_max_inflight_messages_set(client_.get(), 1));
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}
Publisher::~Publisher() {
    worker_.request_stop();
    cv_.notify_all();
    worker_.join();
    if (network_started_) {
        mosquitto_disconnect(client_.get());
        mosquitto_loop_stop(client_.get(), true);
    }
}
void Publisher::run(std::stop_token stop) {
    // connect_async can still fail immediately (e.g. ECONNREFUSED).
    // Retry in this worker so broker availability never gates polling or REST startup.
    while (!network_started_ && !stop.stop_requested()) {
        try {
            check(mosquitto_connect_async(client_.get(), host_.c_str(), port_, 15));
            check(mosquitto_loop_start(client_.get()));
            network_started_ = true;
        } catch (const std::exception& e) {
            ++errors;
            std::cerr << "mqtt connect: " << e.what() << '\n';
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stop.stop_requested(); });
        }
    }
    while (!stop.stop_requested()) {
        try {
            if (connected) {
                if (const auto reading = store_.pending()) {
                    const auto payload = reading->json();
                    // Hold the lock through publish so a fast PUBACK cannot beat packet assignment.
                    std::unique_lock lock(mutex_);
                    acknowledged_ = false;
                    check(mosquitto_publish(client_.get(), &packet_, "meterbridge/readings",
                                           static_cast<int>(payload.size()), payload.c_str(), 1, false));
                    while (!acknowledged_ && !stop.stop_requested())
                        cv_.wait_for(lock, std::chrono::milliseconds(100));
                    if (stop.stop_requested()) break;
                    lock.unlock();
                    store_.acknowledge(reading->id);
                    continue;
                }
            }
        } catch (const std::exception& e) {
            ++errors;
            std::cerr << "mqtt: " << e.what() << '\n';
        }
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
}
}
