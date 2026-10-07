#include "cachelite/cache/CacheService.h"
#include "cachelite/database/MySQLConnectionPool.h"
#include "cachelite/database/MySQLRepository.h"

#include "../TestSupport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cachelite::database::LookupResult;
using cachelite::database::MySQLConfig;
using cachelite::database::MySQLConnectionPool;
using cachelite::database::MySQLRepository;
using cachelite::cache::CacheOptions;
using cachelite::cache::CacheService;
using cachelite::storage::MemoryStore;

void readsExistingAndMissingKeys() {
    MySQLRepository repository(MySQLConfig::fromEnvironment(), 2);
    const char* configuredKey = std::getenv("CACHELITE_TEST_MYSQL_KEY");
    const std::string key = configuredKey != nullptr
        ? configuredKey : "demo:key";
    const LookupResult found = repository.find(key);
    CACHELITE_CHECK(found.status == LookupResult::Status::Found);

    const std::string missingKey = "cachelite:test:absent:" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count());
    const LookupResult missing = repository.find(missingKey);
    CACHELITE_CHECK(missing.status == LookupResult::Status::NotFound);
}

void waitsWhenConnectionPoolIsFull() {
    MySQLConnectionPool pool(MySQLConfig::fromEnvironment(), 2);
    auto first = pool.acquire();
    auto second = pool.acquire();
    CACHELITE_CHECK(first.nativeHandle() != nullptr);
    CACHELITE_CHECK(second.nativeHandle() != nullptr);

    std::atomic<bool> started{false};
    std::atomic<bool> acquired{false};
    std::exception_ptr workerError;
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        try {
            auto third = pool.acquire();
            acquired.store(third.nativeHandle() != nullptr,
                           std::memory_order_release);
        } catch (...) {
            workerError = std::current_exception();
        }
    });

    for (int attempt = 0;
         attempt < 100 && !started.load(std::memory_order_acquire);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    const bool blocked = !acquired.load(std::memory_order_acquire);
    first = MySQLConnectionPool::Connection{};
    worker.join();

    if (workerError) {
        std::rethrow_exception(workerError);
    }
    CACHELITE_CHECK(blocked);
    CACHELITE_CHECK(acquired.load(std::memory_order_acquire));
}

void servesConcurrentRealQueriesThroughPool() {
    const char* configuredKey = std::getenv("CACHELITE_TEST_MYSQL_KEY");
    const std::string key = configuredKey != nullptr
        ? configuredKey : "demo:key";
    MySQLRepository repository(MySQLConfig::fromEnvironment(), 2);
    constexpr std::size_t requestCount = 32;
    std::vector<LookupResult> results(requestCount);
    std::vector<std::exception_ptr> errors(requestCount);
    std::vector<std::thread> callers;
    callers.reserve(requestCount);
    for (std::size_t index = 0; index < requestCount; ++index) {
        callers.emplace_back([&, index] {
            try {
                results[index] = repository.find(key);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    for (std::size_t index = 0; index < requestCount; ++index) {
        if (errors[index]) {
            std::rethrow_exception(errors[index]);
        }
        CACHELITE_CHECK(results[index].status == LookupResult::Status::Found);
    }
}

void fillsCacheFromRealMySQLOnMiss() {
    const char* configuredKey = std::getenv("CACHELITE_TEST_MYSQL_KEY");
    const std::string key = configuredKey != nullptr
        ? configuredKey : "demo:key";
    MySQLRepository repository(MySQLConfig::fromEnvironment(), 2);
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 2;
    options.backendCacheJitter = std::chrono::seconds{0};
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<LookupResult> completed;
    CacheService cache(memory, repository, options);

    CACHELITE_CHECK(!cache.getLocal(key).has_value());
    cache.lookupAsync(key, [&](LookupResult result) {
        {
            std::lock_guard lock(mutex);
            completed = std::move(result);
        }
        condition.notify_one();
    });
    {
        std::unique_lock lock(mutex);
        CACHELITE_CHECK(condition.wait_for(
            lock, std::chrono::seconds{5}, [&] { return completed.has_value(); }
        ));
    }
    CACHELITE_CHECK(completed->status == LookupResult::Status::Found);
    CACHELITE_CHECK(cache.setFromBackend(key, completed->value) !=
        MemoryStore::SetResult::RejectedByMaxMemory);
    CACHELITE_CHECK(cache.getLocal(key) ==
        std::optional<std::string>{completed->value});
}

}  // namespace

int main() {
    const char* enabled = std::getenv("CACHELITE_TEST_MYSQL");
    if (enabled == nullptr || std::string(enabled) != "1") {
        return 77;
    }

    return cachelite::test::runSuite(
        "live MySQL integration",
        std::vector<std::pair<std::string, void (*)()>>{
            {"reads existing and missing keys", readsExistingAndMissingKeys},
            {"waits when connection pool is full", waitsWhenConnectionPoolIsFull},
            {"serves 32 concurrent real queries through two connections", servesConcurrentRealQueriesThroughPool},
            {"fills cache from real MySQL on miss", fillsCacheFromRealMySQLOnMiss},
        }
    );
}
