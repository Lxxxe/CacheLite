#include "cachelite/storage/MemoryStore.h"

#include <utility>

namespace cachelite::storage {

void MemoryStore::set(std::string key, std::string value) {
    values_[std::move(key)] = std::move(value);
}

std::optional<std::string> MemoryStore::get(std::string_view key) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return std::nullopt;
    }

    return it->second;
}

bool MemoryStore::del(std::string_view key) {
    return values_.erase(std::string(key)) != 0;
}

std::size_t MemoryStore::size() const noexcept {
    return values_.size();
}

}  // namespace cachelite::storage
