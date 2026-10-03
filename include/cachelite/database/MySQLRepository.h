#pragma once

#include "cachelite/database/KeyValueRepository.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cachelite::database {

struct MySQLConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string user{"root"};
    std::string password;
    std::string database{"cachelite"};

    [[nodiscard]] static MySQLConfig fromEnvironment();
};

class MySQLConnectionPool;

class MySQLRepository final : public KeyValueRepository {
public:
    explicit MySQLRepository(
        MySQLConfig config,
        std::size_t maxConnections = 4
    );
    ~MySQLRepository();

    MySQLRepository(const MySQLRepository&) = delete;
    MySQLRepository& operator=(const MySQLRepository&) = delete;
    MySQLRepository(MySQLRepository&&) = delete;
    MySQLRepository& operator=(MySQLRepository&&) = delete;

    [[nodiscard]] LookupResult find(
        std::string_view key
    ) override;

private:
    std::unique_ptr<MySQLConnectionPool> pool_;
};

}  // namespace cachelite::database
