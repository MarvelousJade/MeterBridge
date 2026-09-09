#include "core.hpp"
#include "mqtt.hpp"
#include <charconv>
#include <httplib.h>
#include <iostream>
#include <thread>

namespace {
std::int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(mb::Clock::now().time_since_epoch()).count();
}
struct Health {
    std::atomic_bool meter{false}, storage{true};
    std::atomic_int64_t last_sample{0}, poll_us{0};
    std::atomic_uint64_t samples{0}, errors{0};
};
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "meterbridge [options]\n"
                "  --meter-host IPv4   127.0.0.1    --meter-port N   1502\n"
                "  --serial PATH       RTU device  --baud N         9600 (8N1)\n"
                "  --unit N            1           --interval-ms N 1000\n"
                "  --db PATH           meterbridge.db\n"
                "  --http-port N       8080        --max-rows N     10000\n"
                "  --mqtt-host IPv4    disabled    --mqtt-port N    1883\n";
            return 0;
        }
        mb::signals();
        mb::Args args(argc, argv, {"--meter-host", "--meter-port", "--serial", "--baud", "--unit", "--db",
                                  "--interval-ms", "--http-port", "--mqtt-host", "--mqtt-port", "--max-rows"});
        const int interval = args.number("--interval-ms", 1000, 50, 60000);
        const int http_port = args.number("--http-port", 8080, 1, 65535);
        const int mqtt_port = args.number("--mqtt-port", 1883, 1, 65535);
        const int max_rows = args.number("--max-rows", 10000, 1, 1000000);
        mb::Meter meter(args.text("--meter-host", "127.0.0.1"), args.number("--meter-port", 1502, 1, 65535),
                        args.text("--serial"), args.number("--baud", 9600, 1200, 115200), args.number("--unit", 1, 1, 247));
        mb::Store store(args.text("--db", "meterbridge.db"), max_rows);
        Health health;
        std::atomic_bool stop{false};
        std::unique_ptr<mb::Publisher> publisher;
        if (!args.text("--mqtt-host").empty())
            publisher = std::make_unique<mb::Publisher>(store, args.text("--mqtt-host"), mqtt_port);

        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(2, 8); };
        server.set_read_timeout(1, 0);
        server.set_write_timeout(1, 0);
        server.set_keep_alive_max_count(4);
        server.set_payload_max_length(1024);
        server.Get("/health", [&](const httplib::Request&, httplib::Response& response) {
            try {
                const auto last = health.last_sample.load();
                const auto age = last == 0 ? -1 : monotonic_ms() - last;
                const bool fresh = age >= 0 && age <= interval * 3LL;
                const bool mqtt_ok = !publisher || publisher->connected.load();
                const bool ok = health.meter && health.storage && fresh && mqtt_ok;
                response.status = ok ? 200 : 503;
                const auto boolean = [](bool value) { return value ? "true" : "false"; };
                std::string body = "{\"status\":\"" + std::string(ok ? "ok" : "degraded") +
                    "\",\"meter_connected\":" + boolean(health.meter) +
                    ",\"storage_ok\":" + boolean(health.storage) +
                    ",\"mqtt_connected\":" + (publisher ? std::string(boolean(publisher->connected)) : "null") +
                    ",\"sample_age_ms\":" + std::to_string(age) +
                    ",\"samples\":" + std::to_string(health.samples.load()) +
                    ",\"poll_errors\":" + std::to_string(health.errors.load()) +
                    ",\"last_poll_us\":" + std::to_string(health.poll_us.load()) +
                    ",\"pending\":" + std::to_string(store.pending_count()) +
                    ",\"mqtt_errors\":" + std::to_string(publisher ? publisher->errors.load() : 0) + '}';
                response.set_content(body, "application/json");
            } catch (const std::exception&) {
                response.status = 503;
                response.set_content("{\"error\":\"storage unavailable\"}", "application/json");
            }
        });
        server.Get("/readings", [&](const httplib::Request& request, httplib::Response& response) {
            int limit = 20;
            if (request.has_param("limit")) {
                const auto value = request.get_param_value("limit");
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
                if (error != std::errc{} || end != value.data() + value.size() || limit < 1 || limit > 100) {
                    response.status = 400;
                    response.set_content("{\"error\":\"limit must be 1..100\"}", "application/json");
                    return;
                }
            }
            try {
                std::string body = "[";
                for (const auto& reading : store.recent(limit)) {
                    if (body.size() > 1) body += ',';
                    body += reading.json();
                }
                response.set_content(body + ']', "application/json");
            } catch (const std::exception&) {
                response.status = 503;
                response.set_content("{\"error\":\"storage unavailable\"}", "application/json");
            }
        });
        if (!server.bind_to_port("127.0.0.1", http_port)) throw std::runtime_error("HTTP port unavailable");
        std::jthread poller([&](std::stop_token token) {
            std::stop_callback on_stop(token, [&] { stop = true; });
            int backoff = 250;
            while (!stop) {
                try {
                    const auto started = mb::Clock::now();
                    mb::Reading reading;
                    try { reading = meter.read(); }
                    catch (...) { health.meter = false; throw; }
                    health.poll_us = std::chrono::duration_cast<std::chrono::microseconds>(mb::Clock::now() - started).count();
                    health.meter = true;
                    try { store.append(reading, bool(publisher)); }
                    catch (...) { health.storage = false; throw; }
                    health.storage = true;
                    health.last_sample = monotonic_ms();
                    ++health.samples;
                    backoff = 250;
                    mb::wait(stop, interval);
                } catch (const std::exception& e) {
                    ++health.errors;
                    std::cerr << "poll: " << e.what() << '\n';
                    mb::wait(stop, backoff);
                    backoff = std::min(backoff * 2, 5000);
                }
            }
        });
        std::atomic_bool http_failed{false};
        std::jthread http([&](std::stop_token token) {
            std::stop_callback on_stop(token, [&] { server.stop(); });
            if (!server.listen_after_bind()) { http_failed = true; stop = true; }
        });
        std::cout << "MeterBridge listening on http://127.0.0.1:" << http_port << std::endl;
        while (!mb::interrupted() && !stop) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop = true;
        server.stop();
        http.join();
        poller.join();
        return http_failed ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
