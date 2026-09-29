#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cachelite::storage {

// The current epoll event loop owns this store, so this first version is
// intentionally single-threaded. Thread-safe sharding can be added later.
class MemoryStore {
public:
    void set(std::string key, std::string value);

    [[nodiscard]] std::optional<std::string> get(
        std::string_view key
    ) const;

    [[nodiscard]] bool del(std::string_view key);

    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, std::string> values_;
};

}  // namespace cachelite::storage
