#include "cachelite/database/MySQLRepository.h"
#include "cachelite/database/MySQLConnectionPool.h"

#include <charconv>
#include <cstdlib>
#include <system_error>
#include <utility>

#if defined(CACHELITE_ENABLE_MYSQL)

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include <mysql/mysql.h>

#endif

namespace cachelite::database {

MySQLConfig MySQLConfig::fromEnvironment() {
    MySQLConfig config;

    const auto overrideString = [](
        std::string& target,
        const char* name
    ) {
        if (const char* value = std::getenv(name); value != nullptr) {
            target = value;
        }
    };

    overrideString(config.host, "CACHELITE_MYSQL_HOST");
    overrideString(config.user, "CACHELITE_MYSQL_USER");
    overrideString(config.password, "CACHELITE_MYSQL_PASSWORD");
    overrideString(config.database, "CACHELITE_MYSQL_DATABASE");

    if (const char* portText = std::getenv("CACHELITE_MYSQL_PORT");
        portText != nullptr) {
        unsigned int port = 0;
        const auto [end, result] = std::from_chars(
            portText,
            portText + std::char_traits<char>::length(portText),
            port
        );
        if (result == std::errc{} &&
            *end == '\0' &&
            port <= 65535U) {
            config.port = static_cast<std::uint16_t>(port);
        }
    }

    return config;
}

LookupResult LookupResult::found(std::string value) {
    return LookupResult{
        Status::Found,
        std::move(value),
        {}
    };
}

LookupResult LookupResult::notFound() {
    return LookupResult{Status::NotFound, {}, {}};
}

LookupResult LookupResult::failure(std::string error) {
    return LookupResult{
        Status::Error,
        {},
        std::move(error)
    };
}

MySQLRepository::MySQLRepository(
    MySQLConfig config,
    std::size_t maxConnections
)
    :
      pool_(std::make_unique<MySQLConnectionPool>(
          std::move(config),
          maxConnections
      )) {
}

MySQLRepository::~MySQLRepository() = default;

LookupResult MySQLRepository::find(std::string_view key) {
#if !defined(CACHELITE_ENABLE_MYSQL)
    static_cast<void>(key);
    // 未启用 MySQL 时保持服务器可运行，将其视为空的回源数据源。
    return LookupResult::notFound();
#else
    //MySQL C API 里长度参数类型是`unsigned long`，做边界校验，key 太长直接报错。
    if (key.size() > std::numeric_limits<unsigned long>::max()) {
        return LookupResult::failure("cache key is too large");
    }
    //从MySQL连接池取出一条数据库连接，拿到底层原生`MYSQL*`句柄。连接池避免每次查询新建销毁连接，提升性能。
    MySQLConnectionPool::Connection connection = pool_->acquire();
    MYSQL* nativeConnection = static_cast<MYSQL*>(
        connection.nativeHandle()
    );

    using Statement = std::unique_ptr<
        MYSQL_STMT,
        decltype(&mysql_stmt_close)
    >;
    Statement statement(
        mysql_stmt_init(nativeConnection),
        &mysql_stmt_close
    );
    if (!statement) {
        return LookupResult::failure("mysql_stmt_init failed");
    }

    constexpr char query[] =
        "SELECT cache_value FROM cache_entries "
        "WHERE cache_key = ? LIMIT 1";
    if (mysql_stmt_prepare(
            statement.get(),
            query,
            static_cast<unsigned long>(sizeof(query) - 1)
        ) != 0) {
        return LookupResult::failure(
            "mysql_stmt_prepare: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    MYSQL_BIND parameter{};
    parameter.buffer_type = MYSQL_TYPE_STRING;
    parameter.buffer = const_cast<char*>(key.data());
    parameter.buffer_length = static_cast<unsigned long>(key.size());
    if (mysql_stmt_bind_param(statement.get(), &parameter) != 0) {
        return LookupResult::failure(
            "mysql_stmt_bind_param: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    if (mysql_stmt_execute(statement.get()) != 0) {
        return LookupResult::failure(
            "mysql_stmt_execute: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    bool updateMaxLength = true;
    if (mysql_stmt_attr_set(
            statement.get(),
            STMT_ATTR_UPDATE_MAX_LENGTH,
            &updateMaxLength
        ) != 0) {
        return LookupResult::failure(
            "mysql_stmt_attr_set: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    if (mysql_stmt_store_result(statement.get()) != 0) {
        return LookupResult::failure(
            "mysql_stmt_store_result: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    using Metadata = std::unique_ptr<
        MYSQL_RES,
        decltype(&mysql_free_result)
    >;
    Metadata metadata(
        mysql_stmt_result_metadata(statement.get()),
        &mysql_free_result
    );
    if (!metadata) {
        return LookupResult::failure(
            "mysql_stmt_result_metadata: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    MYSQL_FIELD* fields = mysql_fetch_fields(metadata.get());
    if (fields == nullptr || mysql_num_fields(metadata.get()) != 1) {
        return LookupResult::failure("unexpected MySQL result shape");
    }

    const std::size_t bufferSize = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(fields[0].max_length)
    );
    std::vector<char> valueBuffer(bufferSize);
    unsigned long valueLength = 0;

    MYSQL_BIND result{};
    result.buffer_type = MYSQL_TYPE_STRING;
    result.buffer = valueBuffer.data();
    result.buffer_length = static_cast<unsigned long>(valueBuffer.size());
    result.length = &valueLength;
    if (mysql_stmt_bind_result(statement.get(), &result) != 0) {
        return LookupResult::failure(
            "mysql_stmt_bind_result: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    const int fetchResult = mysql_stmt_fetch(statement.get());
    if (fetchResult == MYSQL_NO_DATA) {
        return LookupResult::notFound();
    }
    if (fetchResult != 0) {
        return LookupResult::failure(
            "mysql_stmt_fetch: " +
            std::string(mysql_stmt_error(statement.get()))
        );
    }

    return LookupResult::found(
        std::string(
            valueBuffer.data(),
            static_cast<std::size_t>(valueLength)
        )
    );
#endif
}

}  // namespace cachelite::database
