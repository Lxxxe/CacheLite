#include "cachelite/database/MySQLConnectionPool.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(CACHELITE_ENABLE_MYSQL)

#include <mysql/mysql.h>

#endif

namespace cachelite::database {

struct MySQLConnectionPool::Impl {
    explicit Impl(MySQLConfig poolConfig, std::size_t limit)
        : config(std::move(poolConfig)),
          maxConnections(limit == 0 ? 1 : limit) {
    }

    ~Impl() {
#if defined(CACHELITE_ENABLE_MYSQL)
        for (void* nativeHandle : idleConnections) {
            mysql_close(static_cast<MYSQL*>(nativeHandle));
        }
#endif
    }

    MySQLConfig config;
    const std::size_t maxConnections;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<void*> idleConnections;
    std::size_t totalConnections{0};
};

MySQLConnectionPool::Connection::Connection(
    void* nativeHandle,
    MySQLConnectionPool* owner
) noexcept
    : nativeHandle_(nativeHandle),
      owner_(owner) {
}

MySQLConnectionPool::Connection::~Connection() {
    reset();
}

MySQLConnectionPool::Connection::Connection(
    Connection&& other
) noexcept
    : nativeHandle_(other.nativeHandle_),
      owner_(other.owner_) {
    other.nativeHandle_ = nullptr;
    other.owner_ = nullptr;
}

MySQLConnectionPool::Connection&
MySQLConnectionPool::Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        reset();
        nativeHandle_ = other.nativeHandle_;
        owner_ = other.owner_;
        other.nativeHandle_ = nullptr;
        other.owner_ = nullptr;
    }
    return *this;
}

void* MySQLConnectionPool::Connection::nativeHandle() const noexcept {
    return nativeHandle_;
}

void MySQLConnectionPool::Connection::invalidate() noexcept {
    if (nativeHandle_ != nullptr && owner_ != nullptr) {
        owner_->discard(nativeHandle_);
    }
    nativeHandle_ = nullptr;
    owner_ = nullptr;
}

void MySQLConnectionPool::Connection::reset() noexcept {
    if (nativeHandle_ != nullptr && owner_ != nullptr) {
        owner_->release(nativeHandle_);
    }
    nativeHandle_ = nullptr;
    owner_ = nullptr;
}

MySQLConnectionPool::MySQLConnectionPool(
    MySQLConfig config,
    std::size_t maxConnections
)
    : impl_(std::make_unique<Impl>(std::move(config), maxConnections)) {
}

MySQLConnectionPool::~MySQLConnectionPool() = default;

MySQLConnectionPool::Connection MySQLConnectionPool::acquire() {
#if !defined(CACHELITE_ENABLE_MYSQL)
    throw std::runtime_error(
        "MySQL connection pool is disabled; configure "
        "CACHELITE_ENABLE_MYSQL=ON"
    );
#else
    void* nativeHandle = nullptr;
    {
        std::unique_lock lock(impl_->mutex);
        impl_->condition.wait(lock, [this] {
            return !impl_->idleConnections.empty() ||
                impl_->totalConnections < impl_->maxConnections;
        });

        if (!impl_->idleConnections.empty()) {
            nativeHandle = impl_->idleConnections.back();
            impl_->idleConnections.pop_back();
            return Connection(nativeHandle, this);
        }

        ++impl_->totalConnections;
    }

    MYSQL* connection = mysql_init(nullptr);
    if (connection == nullptr ||
        mysql_real_connect(
            connection,
            impl_->config.host.c_str(),
            impl_->config.user.c_str(),
            impl_->config.password.c_str(),
            impl_->config.database.c_str(),
            impl_->config.port,
            nullptr,
            0
        ) == nullptr) {
        if (connection != nullptr) {
            mysql_close(connection);
        }
        {
            std::lock_guard lock(impl_->mutex);
            --impl_->totalConnections;
        }
        impl_->condition.notify_one();
        throw std::runtime_error("failed to establish MySQL connection");
    }

    return Connection(connection, this);
#endif
}

std::size_t MySQLConnectionPool::maxConnections() const noexcept {
    return impl_->maxConnections;
}

void MySQLConnectionPool::release(void* nativeHandle) noexcept {
#if defined(CACHELITE_ENABLE_MYSQL)
    {
        std::lock_guard lock(impl_->mutex);
        impl_->idleConnections.push_back(nativeHandle);
    }
    impl_->condition.notify_one();
#else
    static_cast<void>(nativeHandle);
#endif
}

void MySQLConnectionPool::discard(void* nativeHandle) noexcept {
#if defined(CACHELITE_ENABLE_MYSQL)
    mysql_close(static_cast<MYSQL*>(nativeHandle));
    {
        std::lock_guard lock(impl_->mutex);
        --impl_->totalConnections;
    }
    impl_->condition.notify_one();
#else
    static_cast<void>(nativeHandle);
#endif
}

}  // namespace cachelite::database
