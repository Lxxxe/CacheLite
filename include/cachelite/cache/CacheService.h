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
#include <unordered_map>
#include <vector>

namespace cachelite::cache {

struct CacheOptions {
    std::size_t workerCount{2};//后台 MySQL 查询线程数量
    std::size_t maxPendingLookups{1024};//回源任务队列最多允许保存多少个不同 key 的任务
    std::size_t maxWaitersPerKey{1024};//同一个 key 最多允许多少个请求等待回源结果
    std::chrono::seconds negativeCacheTtl{10};//MySQL 查不到 key 后，空值结果缓存多长时间(秒)
    std::chrono::seconds backendCacheTtl{60};//MySQL 查询成功后，回填到内存中的基础 TTL,当前默认是 60 秒
    std::chrono::seconds backendCacheJitter{10};//给 MySQL 回填数据增加的最大随机时间
    std::size_t backendFailureThreshold{5};//MySQL 连续失败多少次后开启熔断
    std::chrono::seconds circuitCooldown{5};//熔断开启后持续多长时间。5 秒后允许新的请求重新尝试 MySQL。
};

class CacheService {
public:
    using SetResult = storage::MemoryStore::SetResult;
    using LookupCallback = std::function<void(database::LookupResult)>;

    CacheService(
        storage::MemoryStore& memory,
        database::KeyValueRepository& repository,
        CacheOptions options = {}
    );
    ~CacheService();

    CacheService(const CacheService&) = delete;
    CacheService& operator=(const CacheService&) = delete;
    CacheService(CacheService&&) = delete;
    CacheService& operator=(CacheService&&) = delete;

    [[nodiscard]] std::optional<std::string> getLocal(
        std::string_view key
    );
    [[nodiscard]] bool exists(std::string_view key);

    [[nodiscard]] SetResult set(
        std::string key,
        std::string value
    );

    // 将数据库回源结果写入缓存，并为其设置带抖动的短 TTL。
    [[nodiscard]] SetResult setFromBackend(
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
    //代表一个真正提交给数据库的查询任务
    struct Task {
        std::string key;
    };
    //代表某个 key 当前有哪些客户端在等待查询结果
    struct PendingLookup {
        std::vector<LookupCallback> waiters;
    };

    void workerLoop();
    void completeLookup(
        const std::string& key,
        database::LookupResult result
    );

    storage::MemoryStore& memory_;//内存缓存的引用
    database::KeyValueRepository& repository_;//数据库仓储接口的引用
    CacheOptions options_;//保存运行时配置的副本,最上面
    std::mutex mutex_;
    std::condition_variable condition_;//用于唤醒 worker 线程
    std::queue<Task> tasks_;//保存待执行的数据库回源任务。
    std::unordered_map<std::string, PendingLookup> inflight_;//当前正在进行数据库回源的 key，以及所有等待这个 key 查询结果的客户端回调。
    std::unordered_map<
        std::string,
        std::chrono::steady_clock::time_point
    > negativeCache_;//保存近期确认不存在的 key，以及空值缓存的过期时间。
    std::vector<std::thread> workers_;//保存所有后台 worker 线程
    std::size_t backendFailureCount_{0};//记录数据库连续失败次数
    bool circuitOpen_{false};//表示数据库熔断是否开启
    std::chrono::steady_clock::time_point circuitOpenedAt_{};//记录熔断开启的时间
    bool stopping_{false};//表示 CacheService 是否正在停止
};

}  // namespace cachelite::cache
