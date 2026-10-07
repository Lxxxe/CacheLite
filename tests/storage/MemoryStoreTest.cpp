#include "cachelite/storage/MemoryStore.h"

#include "../TestSupport.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using cachelite::storage::MemoryStore;

void supportsSetGetUpdateDelete() {
    MemoryStore store;
    CACHELITE_CHECK(store.set("key", "value") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.get("key") == std::optional<std::string>{"value"});
    CACHELITE_CHECK(store.set("key", "new") == MemoryStore::SetResult::Updated);
    CACHELITE_CHECK(store.get("key") == std::optional<std::string>{"new"});
    CACHELITE_CHECK(store.del("key"));
    CACHELITE_CHECK(!store.exists("key"));
    CACHELITE_CHECK(store.size() == std::size_t{0});
    CACHELITE_CHECK(store.usedBytes() == std::size_t{0});
}

void handlesTtlAndExpiration() {
    MemoryStore store;
    CACHELITE_CHECK(store.set("short", "value") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.expire("short", std::chrono::seconds{1}));
    const std::int64_t ttl = store.ttlSeconds("short");
    CACHELITE_CHECK(ttl >= 0 && ttl <= 1);
    CACHELITE_CHECK(store.expirationMilliseconds("short").has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds{1150});
    CACHELITE_CHECK(store.get("short") == std::nullopt);
    CACHELITE_CHECK(store.ttlSeconds("short") == -2);
}

void expiresImmediatelyForNonPositiveLifetime() {
    MemoryStore store;
    CACHELITE_CHECK(store.set("key", "value") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.expire("key", std::chrono::seconds{0}));
    CACHELITE_CHECK(!store.exists("key"));
}

void evictsLeastRecentlyUsedEntry() {
    MemoryStore store(6);
    CACHELITE_CHECK(store.set("a", "1") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.set("b", "22") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.get("a").has_value());

    CACHELITE_CHECK(store.set("c", "33") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.exists("a"));
    CACHELITE_CHECK(!store.exists("b"));
    CACHELITE_CHECK(store.exists("c"));
    CACHELITE_CHECK(store.usedBytes() == std::size_t{5});
}

void rejectsEntryLargerThanCapacity() {
    MemoryStore store(3);
    CACHELITE_CHECK(
        store.set("key", "value") == MemoryStore::SetResult::RejectedByMaxMemory
    );
    CACHELITE_CHECK(store.size() == std::size_t{0});
}

void updatingEntryResetsItsTtl() {
    MemoryStore store;
    CACHELITE_CHECK(store.set("key", "value") == MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.expire("key", std::chrono::seconds{1}));
    CACHELITE_CHECK(store.set("key", "new-value") == MemoryStore::SetResult::Updated);
    CACHELITE_CHECK(!store.expirationMilliseconds("key").has_value());
    CACHELITE_CHECK(store.get("key") == std::optional<std::string>{"new-value"});
}

void readsManyDifferentKeys() {
    MemoryStore store;
    constexpr std::size_t keyCount = 10000;
    for (std::size_t index = 0; index < keyCount; ++index) {
        CACHELITE_CHECK(store.set(
            "key-" + std::to_string(index),
            "value-" + std::to_string(index)
        ) == MemoryStore::SetResult::Inserted);
    }
    for (std::size_t index = keyCount; index > 0; --index) {
        const std::size_t id = index - 1;
        CACHELITE_CHECK(store.get("key-" + std::to_string(id)) ==
            std::optional<std::string>{"value-" + std::to_string(id)});
    }
    CACHELITE_CHECK(store.size() == keyCount);
}

void expiresLargeKeySetAndReclaimsCapacity() {
    constexpr std::size_t keyCount = 2000;
    MemoryStore store;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    for (std::size_t index = 0; index < keyCount; ++index) {
        const std::string key = "ttl-" + std::to_string(index);
        CACHELITE_CHECK(store.set(key, "value") ==
            MemoryStore::SetResult::Inserted);
        CACHELITE_CHECK(store.expireAtMilliseconds(key, now + 1000));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    for (std::size_t index = 0; index < keyCount; ++index) {
        CACHELITE_CHECK(!store.get("ttl-" + std::to_string(index)).has_value());
    }
    CACHELITE_CHECK(store.size() == std::size_t{0});
    CACHELITE_CHECK(store.usedBytes() == std::size_t{0});
}

void evictsOldKeysUnderLargeLruWorkload() {
    constexpr std::size_t keyCount = 1000;
    std::size_t capacity = 0;
    for (std::size_t index = 0; index < keyCount; ++index) {
        capacity += ("key-" + std::to_string(index)).size() + 1;
    }
    MemoryStore store(capacity);
    for (std::size_t index = 0; index < keyCount; ++index) {
        CACHELITE_CHECK(store.set("key-" + std::to_string(index), "x") ==
            MemoryStore::SetResult::Inserted);
    }
    CACHELITE_CHECK(store.get("key-0") == std::optional<std::string>{"x"});
    CACHELITE_CHECK(store.set("new-key", "x") ==
        MemoryStore::SetResult::Inserted);
    CACHELITE_CHECK(store.get("key-0") == std::optional<std::string>{"x"});
    CACHELITE_CHECK(!store.get("key-1").has_value());
    CACHELITE_CHECK(store.get("new-key") == std::optional<std::string>{"x"});
    CACHELITE_CHECK(store.usedBytes() <= capacity);
}

}  // namespace

int main() {
    return cachelite::test::runSuite(
        "memory store",
        std::vector<std::pair<std::string, void (*)()>>{
            {"supports set/get/update/delete", supportsSetGetUpdateDelete},
            {"handles TTL and expiration", handlesTtlAndExpiration},
            {"expires immediately for non-positive lifetime", expiresImmediatelyForNonPositiveLifetime},
            {"evicts least recently used entry", evictsLeastRecentlyUsedEntry},
            {"rejects entry larger than capacity", rejectsEntryLargerThanCapacity},
            {"updating entry resets TTL", updatingEntryResetsItsTtl},
            {"reads 10000 different keys", readsManyDifferentKeys},
            {"expires 2000 keys", expiresLargeKeySetAndReclaimsCapacity},
            {"evicts old keys under large LRU workload", evictsOldKeysUnderLargeLruWorkload},
        }
    );
}

