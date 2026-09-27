#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <string_view>

namespace cachelite::net {

class InetAddress {
public:
    // loopbackOnly=true  -> 127.0.0.1:port
    // loopbackOnly=false -> 0.0.0.0:port
    explicit InetAddress(
        std::uint16_t port,
        bool loopbackOnly = true//默认参数
    ) noexcept;

    // Construct an address from a dotted-decimal IPv4 string.
    InetAddress(std::string_view ip, std::uint16_t port);

    // Used when accept() returns the peer address.
    explicit InetAddress(const sockaddr_in& address) noexcept;

    [[nodiscard]] std::string ip() const;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string toIpPort() const;

    [[nodiscard]] const sockaddr_in& native() const noexcept;
    [[nodiscard]] sockaddr_in& native() noexcept;

private:
    sockaddr_in address_{};//IPv4 socket 地址结构体类型
};

}  // namespace cachelite::net
