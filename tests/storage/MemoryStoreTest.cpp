#include "cachelite/storage/MemoryStore.h"

#include "../TestSupport.h"

#include <chrono>
#include <optional>
#include <thread>

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
        }
    );
}

