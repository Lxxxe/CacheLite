#pragma once

#include "cachelite/net/InetAddress.h"

#include <cstddef>
#include <sys/types.h>

#include <utility>

namespace cachelite::net {

class Socket {
public:
    Socket();
    explicit Socket(int fd) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    void setReuseAddress(bool enabled);
    void bind(const InetAddress& address);
    void listen(int backlog = 128);

    [[nodiscard]] std::pair<Socket, InetAddress> accept();

    [[nodiscard]] ssize_t read(void* buffer, std::size_t size);
    [[nodiscard]] ssize_t send(const void* data, std::size_t size);

    [[nodiscard]] int fd() const noexcept;

private:
    int fd_{-1};
};

}  // namespace cachelite::net
