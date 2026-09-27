#include "cachelite/net/InetAddress.h"
#include "cachelite/net/Socket.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <system_error>

//单线程、阻塞式、单客户端 Echo 服务器
int main() {
    using cachelite::net::InetAddress;
    using cachelite::net::Socket;

    try {
        Socket listenSocket;//创建监听 Socket
        listenSocket.setReuseAddress(true);//设置地址复用
        listenSocket.bind(InetAddress(6379));//绑定创建的服务端地址
        listenSocket.listen();//进入监听状态

        std::cout << "listening on 127.0.0.1:6379\n";

        auto [client, peer] = listenSocket.accept();//accept() 返回的是一个新的客户端 Socket和InetAddress。
        std::cout << "client connected: " << peer.toIpPort() << '\n';

        std::array<char, 4096> buffer{};

        //程序进入循环：读取客户端数据
        while (true) {
            //buffer.data()表示数组首地址，也就是接收数据的内存位置,最多读取多少字节
            const ssize_t count = client.read(buffer.data(), buffer.size());

            if (count == 0) {
                std::cout << "client disconnected\n";
                break;
            }

            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "read"
                );
            }

            std::size_t sent = 0;
            const std::size_t received = static_cast<std::size_t>(count);

            //发送 Echo 数据
            while (sent < received) {
                const ssize_t written = client.send(
                    buffer.data() + sent,
                    received - sent
                );

                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }

                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "send"
                    );
                }

                if (written == 0) {
                    throw std::runtime_error("send returned zero");
                }

                sent += static_cast<std::size_t>(written);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "cachelite_blocking_echo: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
