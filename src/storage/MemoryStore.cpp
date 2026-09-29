#include "cachelite/storage/MemoryStore.h"

#include <utility>

namespace cachelite::storage {

//这里参数本身是值传递的副本，通过移动直接把副本的内存交给哈希表里的键和值，省去了一次深拷贝，显著提升写入性能
void MemoryStore::set(std::string key, std::string value) {
    values_[std::move(key)] = Entry{std::move(value)};
    //`std::move` 会把字符串对象转成右值引用，触发**移动构造**，直接把字符串的底层内存所有权转移给目标对象，而不是重新分配内存、拷贝字节。
}

std::optional<std::string> MemoryStore::get(std::string_view key) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return std::nullopt;
    }

    if (isExpired(it->second, Clock::now())) {
        values_.erase(it);
        return std::nullopt;
    }

    return it->second.value;
}

bool MemoryStore::del(std::string_view key) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return false;
    }

    if (isExpired(it->second, Clock::now())) {
        values_.erase(it);
        return false;
    }

    values_.erase(it);
    return true;
}

bool MemoryStore::expire(
    std::string_view key,
    std::chrono::seconds lifetime
) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return false;
    }

    if (isExpired(it->second, Clock::now())) {
        values_.erase(it);
        return false;
    }

    if (lifetime <= std::chrono::seconds::zero()) {
        values_.erase(it);
        return true;
    }

    it->second.expiresAt = Clock::now() + lifetime;
    return true;
}

std::int64_t MemoryStore::ttlSeconds(std::string_view key) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return -2;
    }

    const Clock::time_point now = Clock::now();
    if (isExpired(it->second, now)) {
        values_.erase(it);
        return -2;
    }

    if (it->second.expiresAt == Clock::time_point::max()) {
        return -1;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        it->second.expiresAt - now
    ).count();
    return remaining > 0 ? remaining : 0;
}

bool MemoryStore::isExpired(
    const Entry& entry,
    Clock::time_point now
) noexcept {
    //expiresAt不等于最大值(设置了具体的过期时间),且早于当前时间
    return entry.expiresAt != Clock::time_point::max() &&
        entry.expiresAt <= now;
}

std::size_t MemoryStore::size() const noexcept {
    return values_.size();
}

}  // namespace cachelite::storage
