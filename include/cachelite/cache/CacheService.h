#pragma once

#include "cachelite/database/KeyValueRepository.h"
#include "cachelite/storage/MemoryStore.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cachelite::cache {

class CacheService {
public:
    using SetResult = storage::MemoryStore::SetResult;
    using LookupCallback = std::function<void(database::LookupResult)>;

    CacheService(
        storage::MemoryStore& memory,
        database::KeyValueRepository& repository,
        std::size_t workerCount = 2
    );
    ~CacheService();

    CacheService(const CacheService&) = delete;
    CacheService& operator=(const CacheService&) = delete;
    CacheService(CacheService&&) = delete;
    CacheService& operator=(CacheService&&) = delete;

    [[nodiscard]] std::optional<std::string> getLocal(
        std::string_view key
    );

    [[nodiscard]] SetResult set(
        std::string key,
        std::string value
    );

    [[nodiscard]] bool del(std::string_view key);
    [[nodiscard]] bool expire(
        std::string_view key,
        std::chrono::seconds lifetime
    );
    [[nodiscard]] bool expireAtMilliseconds(
        std::string_view key,
        std::int64_t timestampMilliseconds
    );
    [[nodiscard]] std::optional<std::int64_t> expirationMilliseconds(
        std::string_view key
    );
    [[nodiscard]] std::int64_t ttlSeconds(std::string_view key);

    // 缓存未命中后，将数据库查询提交到后台线程。
    void lookupAsync(
        std::string key,
        LookupCallback callback
    );

private:
    struct Task {
        std::string key;
        LookupCallback callback;
    };

    void workerLoop();

    storage::MemoryStore& memory_;
    database::KeyValueRepository& repository_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Task> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

}  // namespace cachelite::cache
