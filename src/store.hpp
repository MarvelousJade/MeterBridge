#pragma once

#include "reading.hpp"
#include <mutex>
#include <optional>
#include <vector>
#include <sqlite3.h>

namespace mb {
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
