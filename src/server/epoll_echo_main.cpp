#include "cachelite/net/EpollPoller.h"
#include "cachelite/net/InetAddress.h"
#include "cachelite/net/Socket.h"

#include <array>
#include <cerrno>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>

namespace {

struct ClientConnection {
    cachelite::net::Socket socket;//客户端socket
    std::string output;//输出缓冲区
    bool peerClosed{false};//标记**对端（客户端）是否已经关闭连接**，默认初始值`false`
};

}  // namespace

int main() {
    using cachelite::net::EpollPoller;
    using cachelite::net::InetAddress;
    using cachelite::net::Socket;

    try {
        Socket listenSocket;
        listenSocket.setReuseAddress(true);//设置地址复用
        listenSocket.setNonBlocking();//开启非阻塞
        listenSocket.bind(InetAddress(6379));
        listenSocket.listen();

        EpollPoller poller;
        poller.add(listenSocket.fd(), EPOLLIN);//添加listenSocket到epoll监听
        //客户端映射表，无序哈希表<键，值>:<fd,ClientConnection>
        std::unordered_map<int, ClientConnection> clients;//与用vector存储对比？

        std::cout << "epoll echo server listening on 127.0.0.1:6379\n";

        while (true) {
            //遍历容器里的每一个就绪事件，拷贝一份到局部变量 event 结构体包含fd和事件类型
            for (const EpollPoller::Event event : poller.wait()) {
                //新连接
                if (event.fd == listenSocket.fd()) {
                    //循环调用 acceptNonBlocking(),把当前已经排队的连接全部取出来
                    while (true) {
                        auto accepted = listenSocket.acceptNonBlocking();//非阻塞版accept
                        if (!accepted.has_value()) {
                            break;
                        }
                        //保存新的客户端连接
                        auto [client, peer] = std::move(*accepted);//accepted 的类型是std::optional<std::pair<Socket, InetAddress>>
                        const int clientFd = client.fd();
                        //把客户端文件描述符加入 epoll
                        poller.add(
                            clientFd,
                            EPOLLIN | EPOLLRDHUP | EPOLLERR
                        );
                        //保存到客户端映射表
                        clients.emplace(
                            clientFd,
                            ClientConnection{
                                std::move(client),
                                {},
                                false
                            }
                        );

                        std::cout << "client connected: "
                                  << peer.toIpPort() << '\n';
                    }

                    continue;
                }
                //根据事件中的文件描述符查找客户端。
                const auto clientIt = clients.find(event.fd);
                //未找到，这个文件描述符已经被删除或不再管理，直接跳过
                if (clientIt == clients.end()) {
                    continue;
                }

                ClientConnection& client = clientIt->second;//拿到unordered_map容器中的ClientConnection\
                
                //- 如果`client.peerClosed`**已经是 true**（之前就已经标记关闭），结果保持 true；
                //- 如果原来 false，但本次 epoll 捕获到挂断 / 错误事件 → 变成 true；
                //- 只有原来没关闭，且本次也没有挂断 / 错误事件 → 保持 false。
                client.peerClosed = client.peerClosed ||
                    (event.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;
                //只要 epoll 上报了这三类事件之一，就标记这个客户端连接**对端已经关闭或者连接出错**。

                //当前客户端有数据可以读取
                if ((event.events & EPOLLIN) != 0) {
                    std::array<char, 4096> buffer{};

                    while (true) {
                        const ssize_t count = client.socket.read(
                            buffer.data(),// 接收数据的内存地址
                            buffer.size()//最多读取 4096 字节
                        );

                        //读取成功
                        if (count > 0) {
                            //加入到输出缓冲区（收到什么就发还什么）
                            client.output.append(
                                buffer.data(),
                                static_cast<std::size_t>(count)
                            );
                            continue;//非阻塞 Socket 一次可能还有更多数据没有读取
                        }

                        if (count == 0) {
                            client.peerClosed = true;//客户端已经正常关闭连接
                            //这里不能马上删除客户端,因为client.output中可能还有尚未发送的数据。
                            break;
                        }
                        //表示系统调用被信号中断，并不一定是网络错误。重新读取即可。
                        if (errno == EINTR) {
                            continue;
                        }

                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }

                        throw std::system_error(
                            errno,
                            std::generic_category(),
                            "read"
                        );
                    }
                }

                //输出缓冲区中有数据
                if (!client.output.empty()) {
                    const ssize_t count = client.socket.send(
                        client.output.data(),
                        client.output.size()
                    );

                    if (count > 0) {
                        //从`output`字符串**头部删掉已经发出去的 count 个字节**。
                        client.output.erase(
                            0,
                            static_cast<std::size_t>(count)
                        );
                    } else if (count == 0) {
                        client.peerClosed = true;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        throw std::system_error(
                            errno,
                            std::generic_category(),
                            "send"
                        );
                    }
                }

                //删除已经关闭的客户端,需同时满足客户端已经关闭，并且输出数据已经全部发送完
                if (client.peerClosed && client.output.empty()) {
                    poller.remove(event.fd);//不再让 epoll 监听这个客户端
                    clients.erase(clientIt);//从客户端映射表中删除连接对象
                    std::cout << "client disconnected\n";
                    continue;
                }

                std::uint32_t interest = EPOLLIN | EPOLLRDHUP | EPOLLERR;
                //如果输出缓冲区还有未发送完的数据增加EPOLLOUT
                if (!client.output.empty()) {
                    interest |= EPOLLOUT;//interest = interest | EPOLLOUT;
                }

                //更新当前客户端下一轮关心的事件
                poller.modify(event.fd, interest);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "cachelite_epoll_echo: " << error.what() << '\n';
        return 1;
    }
}
