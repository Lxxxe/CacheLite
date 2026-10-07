#include "cachelite/cache/CacheService.h"
#include "cachelite/database/KeyValueRepository.h"

#include "../TestSupport.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cachelite::cache::CacheOptions;
using cachelite::cache::CacheService;
using cachelite::database::KeyValueRepository;
using cachelite::database::LookupResult;
using cachelite::storage::MemoryStore;

class FakeRepository final : public KeyValueRepository {
public:
    using Handler = std::function<LookupResult(std::string_view)>;

    FakeRepository(Handler handler, std::chrono::milliseconds delay = {})
        : handler_(std::move(handler)),
          delay_(delay) {
    }

    LookupResult find(std::string_view key) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        const std::size_t active = active_.fetch_add(
            1, std::memory_order_relaxed
        ) + 1;
        std::size_t observed = peakActive_.load(std::memory_order_relaxed);
        while (active > observed && !peakActive_.compare_exchange_weak(
                   observed, active, std::memory_order_relaxed
               )) {
        }
        if (delay_ > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(delay_);
        }
        LookupResult result = handler_(key);
        active_.fetch_sub(1, std::memory_order_relaxed);
        return result;
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t peakActive() const noexcept {
        return peakActive_.load(std::memory_order_relaxed);
    }

private:
    Handler handler_;
    std::chrono::milliseconds delay_;
    std::atomic<std::size_t> calls_{0};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> peakActive_{0};
};

class BlockingRepository final : public KeyValueRepository {
public:
    explicit BlockingRepository(LookupResult result)
        : result_(std::move(result)) {
    }

    LookupResult find(std::string_view) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            started_ = true;
        }
        startedCondition_.notify_all();

        std::unique_lock lock(mutex_);
        releaseCondition_.wait(lock, [this] {
            return released_;
        });
        return result_;
    }

    void waitUntilStarted() {
        std::unique_lock lock(mutex_);
        const bool started = startedCondition_.wait_for(
            lock,
            std::chrono::seconds{5},
            [this] { return started_; }
        );
        CACHELITE_CHECK(started);
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        releaseCondition_.notify_all();
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    LookupResult result_;
    mutable std::mutex mutex_;
    std::condition_variable startedCondition_;
    std::condition_variable releaseCondition_;
    bool started_{false};
    bool released_{false};
    std::atomic<std::size_t> calls_{0};
};

class ResultCollector {
public:
    void add(LookupResult result) {
        {
            std::lock_guard lock(mutex_);
            results_.push_back(std::move(result));
        }
        condition_.notify_all();
    }

    void waitFor(std::size_t count) {
        std::unique_lock lock(mutex_);
        const bool complete = condition_.wait_for(
            lock,
            std::chrono::seconds{10},
            [this, count] { return results_.size() >= count; }
        );
        CACHELITE_CHECK(complete);
    }

    [[nodiscard]] std::vector<LookupResult> results() const {
        std::lock_guard lock(mutex_);
        return results_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<LookupResult> results_;
};

void mergesConcurrentLookupsForOneKey() {
    BlockingRepository repository(LookupResult::found("backend-value"));
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 4;
    options.maxPendingLookups = 1;
    options.maxWaitersPerKey = 12000;
    CacheService cache(memory, repository, options);

    constexpr std::size_t callerCount = 64;
    constexpr std::size_t requestsPerCaller = 160;
    constexpr std::size_t requestCount = callerCount * requestsPerCaller;
    ResultCollector collector;
    std::barrier start(static_cast<std::ptrdiff_t>(callerCount + 1));
    std::vector<std::thread> callers;
    callers.reserve(callerCount);

    for (std::size_t index = 0; index < callerCount; ++index) {
        callers.emplace_back([&] {
            start.arrive_and_wait();
            for (std::size_t request = 0;
                 request < requestsPerCaller;
                 ++request) {
                cache.lookupAsync(
                    "hot-key",
                    [&collector](LookupResult result) {
                        collector.add(std::move(result));
                    }
                );
            }
        });
    }

    start.arrive_and_wait();
    repository.waitUntilStarted();
    for (std::thread& caller : callers) {
        caller.join();
    }
    repository.release();
    collector.waitFor(requestCount);

    CACHELITE_CHECK(repository.calls() == std::size_t{1});
    for (const LookupResult& result : collector.results()) {
        CACHELITE_CHECK(result.status == LookupResult::Status::Found);
        CACHELITE_CHECK(result.value == "backend-value");
    }
}

void handlesManyDifferentKeysConcurrently() {
    constexpr std::size_t callerCount = 64;
    constexpr std::size_t requestsPerCaller = 100;
    constexpr std::size_t requestCount = callerCount * requestsPerCaller;
    FakeRepository repository(
        [](std::string_view key) {
            return LookupResult::found(std::string("value-") + std::string(key));
        },
        std::chrono::milliseconds{2}
    );
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 8;
    options.maxPendingLookups = requestCount;
    options.maxWaitersPerKey = 8;
    CacheService cache(memory, repository, options);

    ResultCollector collector;
    std::vector<std::thread> callers;
    callers.reserve(callerCount);
    for (std::size_t index = 0; index < callerCount; ++index) {
        callers.emplace_back([&, index] {
            for (std::size_t request = 0;
                 request < requestsPerCaller;
                 ++request) {
                const std::string key =
                    "key-" + std::to_string(index * requestsPerCaller + request);
                cache.lookupAsync(
                    key,
                    [&collector](LookupResult result) {
                        collector.add(std::move(result));
                    }
                );
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    collector.waitFor(requestCount);

    CACHELITE_CHECK(repository.calls() == requestCount);
    CACHELITE_CHECK(repository.peakActive() > std::size_t{1});
    CACHELITE_CHECK(repository.peakActive() <= options.workerCount);
    for (const LookupResult& result : collector.results()) {
        CACHELITE_CHECK(result.status == LookupResult::Status::Found);
    }
}

void cachesNegativeResults() {
    FakeRepository repository(
        [](std::string_view) {
            return LookupResult::notFound();
        }
    );
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 1;
    options.negativeCacheTtl = std::chrono::seconds{5};
    CacheService cache(memory, repository, options);
    ResultCollector collector;

    cache.lookupAsync("missing", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(1);
    cache.lookupAsync("missing", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(2);

    CACHELITE_CHECK(repository.calls() == std::size_t{1});
    for (const LookupResult& result : collector.results()) {
        CACHELITE_CHECK(result.status == LookupResult::Status::NotFound);
    }
}

void fillsCacheAfterBackendMiss() {
    FakeRepository repository([](std::string_view key) {
        return LookupResult::found("database:" + std::string(key));
    });
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 2;
    options.backendCacheTtl = std::chrono::seconds{60};
    options.backendCacheJitter = std::chrono::seconds{0};
    CacheService cache(memory, repository, options);
    ResultCollector collector;

    CACHELITE_CHECK(!cache.getLocal("cold-key").has_value());
    cache.lookupAsync("cold-key", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(1);
    const LookupResult result = collector.results().front();
    CACHELITE_CHECK(result.status == LookupResult::Status::Found);
    CACHELITE_CHECK(result.value == "database:cold-key");
    CACHELITE_CHECK(cache.setFromBackend("cold-key", result.value) ==
        MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(cache.getLocal("cold-key") ==
        std::optional<std::string>{"database:cold-key"});
    CACHELITE_CHECK(cache.ttlSeconds("cold-key") > 0);
    CACHELITE_CHECK(repository.calls() == std::size_t{1});
}

void preventsRepeatedPenetrationUntilNegativeTtlExpires() {
    FakeRepository repository([](std::string_view) {
        return LookupResult::notFound();
    });
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 1;
    options.negativeCacheTtl = std::chrono::seconds{1};
    CacheService cache(memory, repository, options);
    ResultCollector collector;

    cache.lookupAsync("absent-key", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(1);
    constexpr std::size_t repeatCount = 1000;
    for (std::size_t index = 0; index < repeatCount; ++index) {
        cache.lookupAsync("absent-key", [&collector](LookupResult result) {
            collector.add(std::move(result));
        });
    }
    collector.waitFor(repeatCount + 1);
    CACHELITE_CHECK(repository.calls() == std::size_t{1});
    for (const LookupResult& result : collector.results()) {
        CACHELITE_CHECK(result.status == LookupResult::Status::NotFound);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    cache.lookupAsync("absent-key", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(repeatCount + 2);
    CACHELITE_CHECK(repository.calls() == std::size_t{2});
}

void opensCircuitAfterBackendFailures() {
    FakeRepository repository(
        [](std::string_view) {
            return LookupResult::failure("backend down");
        }
    );
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 2;
    options.backendFailureThreshold = 2;
    options.circuitCooldown = std::chrono::seconds{60};
    CacheService cache(memory, repository, options);
    ResultCollector collector;

    cache.lookupAsync("failure-1", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    cache.lookupAsync("failure-2", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(2);

    cache.lookupAsync("failure-3", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    collector.waitFor(3);

    CACHELITE_CHECK(repository.calls() == std::size_t{2});
    for (const LookupResult& result : collector.results()) {
        CACHELITE_CHECK(result.status == LookupResult::Status::Error);
    }
}

void rejectsExcessWaitingRequests() {
    BlockingRepository repository(LookupResult::found("value"));
    MemoryStore memory;
    CacheOptions options;
    options.workerCount = 1;
    options.maxPendingLookups = 1;
    options.maxWaitersPerKey = 2;
    CacheService cache(memory, repository, options);
    ResultCollector collector;

    cache.lookupAsync("first", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    repository.waitUntilStarted();

    cache.lookupAsync("second", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });
    cache.lookupAsync("third", [&collector](LookupResult result) {
        collector.add(std::move(result));
    });

    const auto immediateResults = collector.results();
    CACHELITE_CHECK(immediateResults.size() == std::size_t{1});
    CACHELITE_CHECK(immediateResults.front().status == LookupResult::Status::Error);

    repository.release();
    collector.waitFor(3);
    CACHELITE_CHECK(repository.calls() == std::size_t{2});
}

}  // namespace

int main() {
    return cachelite::test::runSuite(
        "cache service concurrency",
        std::vector<std::pair<std::string, void (*)()>>{
            {"merges concurrent lookups for one key", mergesConcurrentLookupsForOneKey},
            {"handles many different keys concurrently", handlesManyDifferentKeysConcurrently},
            {"caches negative results", cachesNegativeResults},
            {"fills cache after backend miss", fillsCacheAfterBackendMiss},
            {"prevents repeated penetration until negative TTL expires", preventsRepeatedPenetrationUntilNegativeTtlExpires},
            {"opens circuit after backend failures", opensCircuitAfterBackendFailures},
            {"rejects excess waiting requests", rejectsExcessWaitingRequests},
        }
    );
}

