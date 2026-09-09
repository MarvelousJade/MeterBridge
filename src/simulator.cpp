#include "core.hpp"
#include <iostream>
#include <poll.h>
#include <unistd.h>

int main(int argc, char** argv) {
    try {
        mb::signals();
        mb::Args args(argc, argv, {"--port", "--serial", "--baud", "--unit"});
        const auto serial = args.text("--serial");
        auto ctx = mb::context("127.0.0.1", args.number("--port", 1502, 1, 65535), serial,
                               args.number("--baud", 9600, 1200, 115200), args.number("--unit", 1, 1, 247));
        std::unique_ptr<modbus_mapping_t, decltype(&modbus_mapping_free)> map(
            modbus_mapping_new(0, 0, 6, 0), modbus_mapping_free);
        if (!map) throw std::runtime_error("cannot allocate register map");
        int listener = -1;
        if (serial.empty()) listener = modbus_tcp_listen(ctx.get(), 1);
        else if (modbus_connect(ctx.get()) < 0) throw std::runtime_error(modbus_strerror(errno));
        if (serial.empty() && listener < 0) throw std::runtime_error(modbus_strerror(errno));
        const auto started = mb::Clock::now();
        std::cout << "meter simulator ready" << std::endl;
        while (!mb::interrupted()) {
            if (serial.empty() && modbus_get_socket(ctx.get()) < 0) {
                pollfd event{listener, POLLIN, 0};
                if (poll(&event, 1, 100) <= 0) continue;
                if (modbus_tcp_accept(ctx.get(), &listener) < 0) continue;
            }
            pollfd event{modbus_get_socket(ctx.get()), POLLIN, 0};
            if (poll(&event, 1, 100) <= 0) continue;
            const auto seconds = std::chrono::duration<double>(mb::Clock::now() - started).count();
            const auto energy = static_cast<std::uint32_t>(1000 + seconds * 1150.0 / 3600.0);
            const std::array<std::uint16_t, 6> words{2300, 500, 0, 1150,
                static_cast<std::uint16_t>(energy >> 16), static_cast<std::uint16_t>(energy)};
            std::copy(words.begin(), words.end(), map->tab_registers);
            std::array<std::uint8_t, MODBUS_TCP_MAX_ADU_LENGTH> request{};
            const int size = modbus_receive(ctx.get(), request.data());
            if (size > 0) {
                const int offset = modbus_get_header_length(ctx.get());
                // The fixture is read-only; clients cannot mutate the register map.
                if (request[offset] != MODBUS_FC_READ_HOLDING_REGISTERS)
                    modbus_reply_exception(ctx.get(), request.data(), MODBUS_EXCEPTION_ILLEGAL_FUNCTION);
                else modbus_reply(ctx.get(), request.data(), size, map.get());
            } else if (size < 0 && serial.empty()) {
                modbus_close(ctx.get());
                modbus_set_socket(ctx.get(), -1);
            }
        }
        if (listener >= 0) close(listener);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
