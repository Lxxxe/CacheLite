#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cachelite::storage {


class MemoryStore {
public:
    enum class SetResult {
        Inserted,
        Updated,
        RejectedByMaxMemory
    };

    static constexpr std::size_t kDefaultMaxBytes =
        64U * 1024U * 1024U;

    explicit MemoryStore(
        std::size_t maxBytes = kDefaultMaxBytes
    ) noexcept;

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;
    MemoryStore(MemoryStore&&) = delete;
    MemoryStore& operator=(MemoryStore&&) = delete;

    // 写入或覆盖 key。空间不足且无法淘汰时返回 RejectedByMaxMemory。
    [[nodiscard]] SetResult set(std::string key, std::string value);
    //查询 key：
    [[nodiscard]] std::optional<std::string> get(
        std::string_view key
    );
    //删除 key：
    [[nodiscard]] bool del(std::string_view key);
    //设置 key 的存活时间，相对过期时间
    [[nodiscard]] bool expire(
        std::string_view key,
        std::chrono::seconds lifetime
    );

    // 使用绝对 Unix 毫秒时间戳设置过期时间，供 AOF 重放使用。
    [[nodiscard]] bool expireAtMilliseconds(
        std::string_view key,
        std::int64_t timestampMilliseconds
    );

    // 返回绝对过期时间；不存在、已过期或永不过期时返回 nullopt。
    [[nodiscard]] std::optional<std::int64_t> expirationMilliseconds(
        std::string_view key
    );

    //返回剩余时间
    [[nodiscard]] std::int64_t ttlSeconds(std::string_view key);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t maxBytes() const noexcept;
    [[nodiscard]] std::size_t usedBytes() const noexcept;

private:
    using LruList = std::list<std::string>;//LRU 双向链表，保存所有 key

    //每个 key 对应一个 Entry
    struct Entry {
        std::string value;//实际存储值
        // 0 表示永不过期，否则保存 Unix 时间戳（毫秒）。
        std::int64_t expiresAtMilliseconds{0};
        // key.size() + value.size()，用于近似统计缓存占用。
        std::size_t bytes{0};
        // 指向 LRU 链表中对应 key 的位置。
        LruList::iterator lruIterator;//iterator是list 的迭代器类型「指向链表节点的指针」
    };

    using Store = std::unordered_map<std::string, Entry>;
    using StoreIterator = Store::iterator;

    [[nodiscard]] static std::size_t entryBytes(
        std::string_view key,
        std::string_view value
    ) noexcept;

    // 把 key 移到 LRU 链表头部，表示最近被访问。
    void touch(StoreIterator it) noexcept;

    // 同时从哈希表、LRU 链表和内存统计中删除一个 key。
    void eraseEntry(StoreIterator it) noexcept;

    // 清理已经过期的条目，释放它们占用的统计空间。
    void purgeExpired();

    // 为一次插入或更新准备空间。replacedBytes 是被覆盖旧值的大小。
    [[nodiscard]] bool ensureCapacity(
        std::size_t replacedBytes,
        std::size_t incomingBytes,
        const std::string* protectedKey
    );

    // 从 LRU 尾部淘汰条目，protectedKey 用于防止更新操作淘汰自己。
    [[nodiscard]] bool evictBytes(
        std::size_t bytesToFree,
        const std::string* protectedKey
    ) noexcept;

    //获取当前 Unix 时间戳，单位是毫秒
    [[nodiscard]] static std::int64_t nowMilliseconds() noexcept;
    //判断一个 Entry 是否已经过期
    [[nodiscard]] static bool isExpired(
        const Entry& entry,
        std::int64_t nowMilliseconds
    ) noexcept;

    // 哈希表负责 O(1) 查找，LRU 链表负责 O(1) 更新访问顺序。
    Store values_;
    LruList lru_;
    std::size_t maxBytes_;
    std::size_t usedBytes_{0};
};

}  // namespace cachelite::storage
