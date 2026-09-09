#include "core.hpp"
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    try {
        mb::signals();
        mb::Args args(argc, argv, {"--meter-host", "--meter-port", "--serial", "--baud", "--unit", "--db", "--interval-ms"});
        mb::Meter meter(args.text("--meter-host", "127.0.0.1"), args.number("--meter-port", 1502, 1, 65535),
                        args.text("--serial"), args.number("--baud", 9600, 1200, 115200), args.number("--unit", 1, 1, 247));
        mb::Store store(args.text("--db", "meterbridge.db"));
        const int interval = args.number("--interval-ms", 1000, 50, 60000);
        while (!mb::interrupted()) {
            try { auto reading = meter.read(); store.append(reading, false); std::cout << reading.json() << std::endl; }
            catch (const std::exception& e) { std::cerr << "poll: " << e.what() << '\n'; }
            for (int elapsed = 0; elapsed < interval && !mb::interrupted(); elapsed += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
