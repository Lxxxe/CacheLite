#pragma once

#include <string>
#include <string_view>

namespace cachelite::database {

struct LookupResult {
    enum class Status {
        Found,
        NotFound,
        Error
    };

    Status status{Status::NotFound};
    std::string value;
    std::string error;

    [[nodiscard]] static LookupResult found(std::string value);
    [[nodiscard]] static LookupResult notFound();
    [[nodiscard]] static LookupResult failure(std::string error);
};

class KeyValueRepository {
public:
    virtual ~KeyValueRepository() = default;

    [[nodiscard]] virtual LookupResult find(
        std::string_view key
    ) = 0;
};

}  // namespace cachelite::database
