#include "cachelite/command/CommandExecutor.h"
#include "cachelite/net/Buffer.h"
#include "cachelite/net/EpollPoller.h"
#include "cachelite/net/InetAddress.h"
#include "cachelite/net/Socket.h"
#include "cachelite/persistence/AofLog.h"
#include "cachelite/protocol/RespEncoder.h"
#include "cachelite/protocol/RespParser.h"
#include "cachelite/protocol/RespValue.h"
#include "cachelite/storage/MemoryStore.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>

namespace {

using cachelite::command::CommandExecutor;
using cachelite::command::CommandResult;
using cachelite::net::Buffer;
using cachelite::net::Socket;
using cachelite::persistence::AofLog;
using cachelite::protocol::RespEncoder;
using cachelite::protocol::RespParseStatus;
using cachelite::protocol::RespParser;
using cachelite::protocol::RespValue;
using cachelite::storage::MemoryStore;

struct ClientConnection {
    Socket socket;
    Buffer input;
    std::string output;
    bool peerClosed{false};
    bool closeAfterWrite{false};
};

void processRequests(
    ClientConnection& client,
    CommandExecutor& executor,
    AofLog& aof
) {
    RespParser parser;

    while (!client.closeAfterWrite) {
        RespValue request;
        std::string error;
        const RespParseStatus status = parser.parse(
            client.input,
            request,
            error
        );

        if (status == RespParseStatus::Incomplete) {
            // 半包：保留 Buffer 中的数据，等待下一次 read 补齐。
            return;
        }

        if (status == RespParseStatus::Error) {
            client.output += RespEncoder::encode(
                RespValue::error("ERR protocol error: " + error)
            );
            client.closeAfterWrite = true;
            return;
        }

        const CommandResult result = executor.execute(request);
        for (const RespValue& persistenceCommand :
             result.persistenceCommands) {
            aof.append(persistenceCommand);
        }
        client.output += RespEncoder::encode(result.response);
    }
}

void readAvailable(
    ClientConnection& client,
    CommandExecutor& executor,
    AofLog& aof
) {
    std::array<char, 4096> buffer{};

    while (!client.closeAfterWrite) {
        const ssize_t count = client.socket.read(
            buffer.data(),
            buffer.size()
        );

        if (count > 0) {
            client.input.append(
                buffer.data(),
                static_cast<std::size_t>(count)
            );
            processRequests(client, executor, aof);
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
        MemoryStore store;
        CommandExecutor executor(store);
        AofLog aof("data/appendonly.aof");
        aof.replay(executor);

        Socket listenSocket;
        listenSocket.setReuseAddress(true);
        listenSocket.setNonBlocking();
        listenSocket.bind(InetAddress(6379));
        listenSocket.listen();

        EpollPoller poller;
        poller.add(listenSocket.fd(), EPOLLIN);
        std::unordered_map<int, ClientConnection> clients;

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

                // EPOLLRDHUP 可能和 EPOLLIN 同时到达，仍要先读完剩余数据。
                if ((event.events & EPOLLIN) != 0 &&
                    !client.closeAfterWrite) {
                    readAvailable(client, executor, aof);
                }

                if ((event.events & EPOLLOUT) != 0 ||
                    !client.output.empty()) {
                    writeAvailable(client);
                }

                if ((client.peerClosed || client.closeAfterWrite) &&
                    client.output.empty()) {
                    poller.remove(event.fd);
                    clients.erase(clientIt);
                    std::cout << "client disconnected\n";
                    continue;
                }

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
