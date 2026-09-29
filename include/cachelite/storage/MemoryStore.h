#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cachelite::storage {


class MemoryStore {
public:
    using Clock = std::chrono::steady_clock;
    //写入或覆盖 key：
    void set(std::string key, std::string value);
    //查询 key：
    [[nodiscard]] std::optional<std::string> get(
        std::string_view key
    );
    //删除 key：
    [[nodiscard]] bool del(std::string_view key);
    //设置 key 的存活时间
    [[nodiscard]] bool expire(
        std::string_view key,
        std::chrono::seconds lifetime
    );

    //返回剩余时间
    [[nodiscard]] std::int64_t ttlSeconds(std::string_view key);

    [[nodiscard]] std::size_t size() const noexcept;

private:
    //每个 key 对应一个 Entry
    struct Entry {
        std::string value;//实际缓存值
        Clock::time_point expiresAt{Clock::time_point::max()};//过期时间,time_point::max()：表示永不过期
    };

    [[nodiscard]] static bool isExpired(
        const Entry& entry,
        Clock::time_point now
    ) noexcept;

    std::unordered_map<std::string, Entry> values_;
};

}  // namespace cachelite::storage
