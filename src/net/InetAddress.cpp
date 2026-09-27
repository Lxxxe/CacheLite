#include "cachelite/net/InetAddress.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace cachelite::net {

InetAddress::InetAddress(
    std::uint16_t port,
    bool loopbackOnly
) noexcept {
    address_.sin_family = AF_INET;//地址族:`AF_INET` 代表使用 **IPv4**
    address_.sin_port = htons(port);//`htons` 把主机字节序的 16 位端口，转换成网络大端序，存入 `sin_port`
    address_.sin_addr.s_addr = htonl(//`sin_addr.s_addr`：IPv4 地址
        loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY
        //`loopbackOnly == true` → 使用`INADDR_LOOPBACK`，也就是 `127.0.0.1` 只监听本机回环地址：只能本机程序访问，外部机器连不上。
        //`loopbackOnly == false` → 使用 `INADDR_ANY`，也就是 `0.0.0.0` 监听本机所有网卡，局域网 / 外网客户端都可以连接服务器。
    );
}

InetAddress::InetAddress(std::string_view ip, std::uint16_t port) {
    address_.sin_family = AF_INET;
    address_.sin_port = htons(port);

    const std::string ipString(ip);
    const int result = ::inet_pton(
        AF_INET,
        ipString.c_str(),
        &address_.sin_addr
    );

    if (result == 0) {
        throw std::invalid_argument("invalid IPv4 address: " + ipString);
    }

    if (result < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "inet_pton"
        );
    }
}

InetAddress::InetAddress(const sockaddr_in& address) noexcept
    : address_(address) {
}

std::string InetAddress::ip() const {
    char buffer[INET_ADDRSTRLEN]{};

    const char* result = ::inet_ntop(
        AF_INET,
        &address_.sin_addr,
        buffer,
        sizeof(buffer)
    );

    if (result == nullptr) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "inet_ntop"
        );
    }

    return result;
}

std::uint16_t InetAddress::port() const noexcept {
    return ntohs(address_.sin_port);
}
//把保存的 sockaddr_in 地址信息，拼接成 `IP:端口` 格式的字符串
std::string InetAddress::toIpPort() const {
    return ip() + ":" + std::to_string(port());
}

const sockaddr_in& InetAddress::native() const noexcept {
    return address_;
}

sockaddr_in& InetAddress::native() noexcept {
    return address_;
}

}  // namespace cachelite::net
