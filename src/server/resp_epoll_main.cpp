#include "cachelite/net/Buffer.h"
#include "cachelite/net/EpollPoller.h"
#include "cachelite/net/InetAddress.h"
#include "cachelite/net/Socket.h"
#include "cachelite/protocol/RespEncoder.h"
#include "cachelite/protocol/RespParser.h"
#include "cachelite/protocol/RespValue.h"
#include "cachelite/storage/MemoryStore.h"

#include <array>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>

namespace {

using cachelite::net::Buffer;
using cachelite::net::Socket;
using cachelite::protocol::RespEncoder;
using cachelite::protocol::RespParseStatus;
using cachelite::protocol::RespParser;
using cachelite::protocol::RespValue;
using cachelite::storage::MemoryStore;

struct ClientConnection {
    Socket socket;
    Buffer input;//接收缓冲区
    std::string output;//待发送响应
    bool peerClosed{false};//客户端是否关闭连接
    bool closeAfterWrite{false};//协议出错后，发送错误响应再关闭
};

//字符串大写转换工具,接收一个只读的字符串视图，返回一个全新的、所有字母都转为大写的 `std::string`
std::string upper(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    //遍历每个字符，调用 `std::toupper` 转大写。
    for (const char character : text) {//提前分配和输入字符串等长的内存,避免多次扩容拷贝
        result.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(character))
            //先转 `unsigned char`：`std::toupper` 的入参要求必须是 `EOF` 或 `unsigned char` 取值范围；如果 `char` 是有符号类型，负值的扩展字符会触发未定义行为。
            //再转回 `char`：把转换后的整数转回字符类型存入结果
        ));
    }
    return result;
}

std::optional<std::int64_t> parseInteger(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, result] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (result != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

RespValue dispatchCommand(MemoryStore& store, const RespValue& request) {
    //Redis 命令的标准传输格式必须是非空的 RESP 数组（`*` 开头）
    if (request.type != RespValue::Type::Array || request.elements.empty()) {
        return RespValue::error("ERR command must be a non-empty RESP array");
    }
    //数组内的每一个命令参数，都必须是批量字符串（`$` 开头）。如果出现整数、嵌套数组等其他类型，属于非法格式，返回错误。
    for (const RespValue& argument : request.elements) {
        if (argument.type != RespValue::Type::BulkString) {
            return RespValue::error("ERR command arguments must be bulk strings");
        }
    }
    //取数组第一个元素作为命令名，调用 `upper` 统一转为大写
    const std::string command = upper(request.elements.front().text);

    if (command == "PING") {
        //无参数（仅命令名，数组长度 1）返回简单字符串 `+PONG\r\n`，是 Redis 最经典的心跳回复。
        if (request.elements.size() == 1) {
            return RespValue::simpleString("PONG");
        }
        //带 1 个参数（数组长度 2）：返回批量字符串，内容就是传入的参数，相当于回显内容，是 PING 命令的扩展用法。
        if (request.elements.size() == 2) {
            return RespValue::bulkString(request.elements[1].text);
        }
        //参数数量不符：返回参数数量错误。
        return RespValue::error("ERR wrong number of arguments for 'ping'");
    }

    if (command == "ECHO") {
        //必须且只能带 1 个参数（数组长度 2），否则返回参数错误。
        if (request.elements.size() != 2) {
            return RespValue::error("ERR wrong number of arguments for 'echo'");
        }
        //正常执行时返回批量字符串，内容就是输入的参数，原样回显。
        return RespValue::bulkString(request.elements[1].text);
    }

    if (command == "SET") {
        if (request.elements.size() != 3) {
            return RespValue::error("ERR wrong number of arguments for 'set'");
        }

        store.set(
            request.elements[1].text,
            request.elements[2].text
        );
        return RespValue::simpleString("OK");
    }

    if (command == "GET") {
        if (request.elements.size() != 2) {
            return RespValue::error("ERR wrong number of arguments for 'get'");
        }

        const auto value = store.get(request.elements[1].text);
        if (!value.has_value()) {
            return RespValue::nullBulkString();
        }

        return RespValue::bulkString(*value);
    }

    if (command == "DEL") {
        if (request.elements.size() < 2) {
            return RespValue::error("ERR wrong number of arguments for 'del'");
        }

        std::int64_t removed = 0;
        for (std::size_t index = 1; index < request.elements.size(); ++index) {
            if (store.del(request.elements[index].text)) {
                ++removed;
            }
        }
        return RespValue::integerValue(removed);
    }

    if (command == "EXPIRE") {
        if (request.elements.size() != 3) {
            return RespValue::error(
                "ERR wrong number of arguments for 'expire'"
            );
        }

        const auto seconds = parseInteger(request.elements[2].text);
        if (!seconds.has_value()) {
            return RespValue::error("ERR invalid expire time");
        }

        return RespValue::integerValue(
            store.expire(
                request.elements[1].text,
                std::chrono::seconds(*seconds)
            ) ? 1 : 0
        );
    }

    if (command == "TTL") {
        if (request.elements.size() != 2) {
            return RespValue::error("ERR wrong number of arguments for 'ttl'");
        }

        return RespValue::integerValue(
            store.ttlSeconds(request.elements[1].text)
        );
    }

    return RespValue::error("ERR unknown command '" + command + "'");
}

void processRequests(ClientConnection& client, MemoryStore& store) {
    RespParser parser;

    while (!client.closeAfterWrite) {
        RespValue request;
        std::string error;
        //RESP 协议解析函数,从input析出一条完整 RESP 命令之后，会填充到request
        const RespParseStatus status = parser.parse(
            client.input,
            request,
            error
        );

        if (status == RespParseStatus::Incomplete) {
            // 半包：数据还不够组成完整 RESP 帧，等待下一次 read。
            return;
        }
        //第一个字节不是合法 RESP 类型
        if (status == RespParseStatus::Error) {
            client.output += RespEncoder::encode(
                RespValue::error("ERR protocol error: " + error)
            );
            client.closeAfterWrite = true;
            return;
        }
        // 命令分发，并把响应编码后追加到 output。
        client.output += RespEncoder::encode(
            dispatchCommand(store, request)
        );
    }
}

void readAvailable(ClientConnection& client, MemoryStore& store) {
    std::array<char, 4096> buffer{};

    while (!client.closeAfterWrite) {
        const ssize_t count = client.socket.read(
            buffer.data(),
            buffer.size()
        );

        if (count > 0) {
            //每次读取的数据都会追加到input因为 TCP 是字节流，可能出现：半包：一次没有收到完整请求,粘包：一次收到多个请求
            client.input.append(
                buffer.data(),
                static_cast<std::size_t>(count)
            );
            processRequests(client, store);//解析请求
            continue;
        }

        if (count == 0) {
            client.peerClosed = true;
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        throw std::system_error(errno, std::generic_category(), "read");
    }
}

void writeAvailable(ClientConnection& client) {
    while (!client.output.empty()) {
        const ssize_t count = client.socket.send(
            client.output.data(),
            client.output.size()
        );

        if (count > 0) {
            client.output.erase(0, static_cast<std::size_t>(count));
            continue;
        }

        if (count == 0) {
            client.peerClosed = true;
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        throw std::system_error(errno, std::generic_category(), "send");
    }
}

}  // namespace

int main() {
    using cachelite::net::EpollPoller;
    using cachelite::net::InetAddress;

    try {
        Socket listenSocket;
        listenSocket.setReuseAddress(true);
        listenSocket.setNonBlocking();
        listenSocket.bind(InetAddress(6379));
        listenSocket.listen();

        EpollPoller poller;
        poller.add(listenSocket.fd(), EPOLLIN);
        std::unordered_map<int, ClientConnection> clients;
        MemoryStore store;//服务器启动时创建一个全局内存存储对象：

        std::cout << "RESP epoll server listening on 127.0.0.1:6379\n";

        while (true) {
            for (const EpollPoller::Event event : poller.wait()) {
                if (event.fd == listenSocket.fd()) {
                    while (true) {
                        auto accepted = listenSocket.acceptNonBlocking();
                        if (!accepted.has_value()) {
                            break;
                        }

                        auto [client, peer] = std::move(*accepted);
                        const int clientFd = client.fd();
                        poller.add(
                            clientFd,
                            EPOLLIN | EPOLLRDHUP | EPOLLERR
                        );
                        clients.emplace(
                            clientFd,
                            ClientConnection{
                                std::move(client),
                                Buffer{},
                                {},
                                false,
                                false
                            }
                        );

                        std::cout << "client connected: "
                                  << peer.toIpPort() << '\n';
                    }
                    continue;
                }

                const auto clientIt = clients.find(event.fd);
                if (clientIt == clients.end()) {
                    continue;
                }

                ClientConnection& client = clientIt->second;
                client.peerClosed = client.peerClosed ||
                    (event.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;

                //只有收到可读事件，并且当前没有准备关闭连接，才去执行读回调 readAvailable
                if ((event.events & EPOLLIN) != 0 &&
                    !client.closeAfterWrite) {//closeAfterWrite发完剩下的数据之后，就关闭这个连接
                    readAvailable(client, store);
                }
                //内核发送缓冲区有空位或用户层 output 缓冲区还有残留数据没发完
                if ((event.events & EPOLLOUT) != 0 ||
                    !client.output.empty()) {
                    writeAvailable(client);
                }
                //对端已经关闭连接或者标记把剩余数据发完再关，并且`client.output.empty()` 用户发送缓冲区已经清空
                if ((client.peerClosed || client.closeAfterWrite) &&
                    client.output.empty()) {
                    poller.remove(event.fd);
                    clients.erase(clientIt);
                    std::cout << "client disconnected\n";
                    continue;
                }
                //当`output`不为空（还有数据要发）：**把 EPOLLOUT 加入监听**。
                std::uint32_t interest = EPOLLIN | EPOLLRDHUP | EPOLLERR;
                if (!client.output.empty()) {
                    interest |= EPOLLOUT;
                }
                poller.modify(event.fd, interest);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "cachelite_resp_epoll: " << error.what() << '\n';
        return 1;
    }
}
