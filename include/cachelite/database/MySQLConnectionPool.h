#pragma once

#include "cachelite/database/MySQLRepository.h"

#include <cstddef>
#include <memory>

namespace cachelite::database {

class MySQLConnectionPool {
public:
    class Connection {
    public:
        Connection() noexcept = default;
        ~Connection();

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;

        [[nodiscard]] void* nativeHandle() const noexcept;

        // 数据库连接发生致命错误时，放弃该连接而不是放回连接池。
        void invalidate() noexcept;

    private:
        friend class MySQLConnectionPool;

        Connection(
            void* nativeHandle,
            MySQLConnectionPool* owner
        ) noexcept;

        void reset() noexcept;

        void* nativeHandle_{nullptr};
        MySQLConnectionPool* owner_{nullptr};
    };

    explicit MySQLConnectionPool(
        MySQLConfig config,
        std::size_t maxConnections = 4
    );
    ~MySQLConnectionPool();

    MySQLConnectionPool(const MySQLConnectionPool&) = delete;
    MySQLConnectionPool& operator=(const MySQLConnectionPool&) = delete;
    MySQLConnectionPool(MySQLConnectionPool&&) = delete;
    MySQLConnectionPool& operator=(MySQLConnectionPool&&) = delete;

    [[nodiscard]] Connection acquire();
    [[nodiscard]] std::size_t maxConnections() const noexcept;

private:
    struct Impl;

    void release(void* nativeHandle) noexcept;
    void discard(void* nativeHandle) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace cachelite::database
