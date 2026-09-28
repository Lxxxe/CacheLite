#pragma once

#include "cachelite/net/InetAddress.h"

#include <cstddef>
#include <optional>
#include <sys/types.h>

#include <utility>

namespace cachelite::net {

class Socket {
public:
    Socket();
    //用已有文件描述符创建 Socket 对象”的构造函数
    explicit Socket(int fd) noexcept;//explicit**禁止隐式类型转换**，只允许显式调用构造函数
    ~Socket();
    //禁用原生拷贝构造和复制构造函数
    //直接拷贝回存在两个相同fd的socket对象，析构的时候会 close 两次，造成严重 bug
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    //自己重新实现，拷贝完删除原来的socket，Socket&&右值引用。只能 b = std::move(a);不能 b=a;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    void setReuseAddress(bool enabled);
    void setNonBlocking(bool enabled = true);
    void bind(const InetAddress& address);
    void listen(int backlog = 128);

    //[[nodiscard]] 是 C++17 引入的属性（attribute），作用：标记这个函数的返回值不应该被丢弃；如果调用方调用了函数却不接收返回值，编译器会报警告。
    [[nodiscard]] std::pair<Socket, InetAddress> accept();
    [[nodiscard]] std::optional<std::pair<Socket, InetAddress>> acceptNonBlocking();

    [[nodiscard]] ssize_t read(void* buffer, std::size_t size);
    [[nodiscard]] ssize_t send(const void* data, std::size_t size);

    [[nodiscard]] int fd() const noexcept;

private:
    int fd_{-1};
};

}  // namespace cachelite::net
