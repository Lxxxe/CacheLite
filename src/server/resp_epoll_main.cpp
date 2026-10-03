#include "cachelite/cache/CacheService.h"
#include "cachelite/command/CommandExecutor.h"
#include "cachelite/database/MySQLRepository.h"
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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

using cachelite::cache::CacheService;
using cachelite::command::CommandExecutor;
using cachelite::command::CommandResult;
using cachelite::database::LookupResult;
using cachelite::database::MySQLConfig;
using cachelite::database::MySQLRepository;
using cachelite::net::Buffer;
using cachelite::net::EpollPoller;
using cachelite::net::Socket;
using cachelite::persistence::AofLog;
using cachelite::protocol::RespEncoder;
using cachelite::protocol::RespParseStatus;
using cachelite::protocol::RespParser;
using cachelite::protocol::RespValue;
using cachelite::storage::MemoryStore;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept
        : fd_(fd) {
    }

    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

private:
    int fd_;
};

struct ClientConnection {
    Socket socket;
    Buffer input;
    std::string output;
    bool peerClosed{false};
    bool closeAfterWrite{false};
    std::optional<std::uint64_t> pendingRequestId;
    std::uint64_t nextRequestId{0};
};

struct CacheCompletion {
    int clientFd;
    std::uint64_t requestId;
    std::string key;
    LookupResult result;
};

struct CompletionQueue {
    std::mutex mutex;
    std::deque<CacheCompletion> ready;
    int eventFd{-1};
};

bool isGetRequest(
    const RespValue& request,
    std::string& key
) {
    if (request.type != RespValue::Type::Array ||
        request.elements.size() != 2 ||
        request.elements[0].type != RespValue::Type::BulkString ||
        request.elements[1].type != RespValue::Type::BulkString) {
        return false;
    }

    const std::string& command = request.elements[0].text;
    if (command.size() != 3 ||
        std::toupper(static_cast<unsigned char>(command[0])) != 'G' ||
        std::toupper(static_cast<unsigned char>(command[1])) != 'E' ||
        std::toupper(static_cast<unsigned char>(command[2])) != 'T') {
        return false;
    }

    key = request.elements[1].text;
    return true;
}
//线程安全地往完成队列 `completions` 里插入一个完成项completion
void notifyCompletion(
    CompletionQueue& completions,
    CacheCompletion completion
) {
    {
        std::lock_guard lock(completions.mutex);
        completions.ready.push_back(std::move(completion));
    }
    //写入 eventfd
    const std::uint64_t notification = 1;
    while (::write(
               completions.eventFd,
               &notification,
               sizeof(notification)
           ) < 0 && errno == EINTR) {
    }
}

void appendCommandResult(
    ClientConnection& client,
    const CommandResult& result,
    AofLog& aof
) {
    for (const RespValue& persistenceCommand :
         result.persistenceCommands) {
        aof.append(persistenceCommand);
    }
    client.output += RespEncoder::encode(result.response);
}

void processRequests(
    ClientConnection& client,
    CommandExecutor& executor,
    CacheService& cache,
    AofLog& aof,
    CompletionQueue& completions
) {
    RespParser parser;

    while (!client.closeAfterWrite &&
           !client.pendingRequestId.has_value()) {
        RespValue request;
        std::string error;
        const RespParseStatus status = parser.parse(
            client.input,
            request,//解析结果存放
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

        std::string getKey;
        //判断是不是 GET 请求,getkey接收命令中的key
        if (isGetRequest(request, getKey)) {
            //收到 GET key 后首先查询缓存
            const auto cached = cache.getLocal(getKey);//内部调用MemoryStore::get
            if (cached.has_value()) {
                client.output += RespEncoder::encode(
                    RespValue::bulkString(*cached)
                );
                continue;
            }
            //缓存未命中后，生成请求编号
            const std::uint64_t requestId = client.nextRequestId++;
            client.pendingRequestId = requestId;//然后记录当前客户端正在等待回源
            const int clientFd = client.socket.fd();
            try {
                //把任务放入 CacheService 的任务队列
                cache.lookupAsync(
                    getKey,
                    [clientFd, requestId, getKey, &completions](
                        LookupResult result
                    ) mutable {//在 lambda 体内默认是 `const` 只读的,加 `mutable` 解除 const 限制，让按值捕获的变量可以被移动
                        //调用 `notifyCompletion`，线程安全地往完成队列 `completions` 里插入一个完成项
                        notifyCompletion(
                            completions,
                            //完成项是 `CacheCompletion` 结构体，里面打包了
                            CacheCompletion{
                                clientFd,//哪个客户端
                                requestId,//哪条请求
                                std::move(getKey),//查的哪个 key
                                std::move(result)//查找结果
                            }
                        );
                    }
                );
            } catch (const std::exception& lookupError) {
                client.pendingRequestId.reset();
                client.output += RespEncoder::encode(
                    RespValue::error(
                        "ERR cache lookup: " +
                        std::string(lookupError.what())
                    )
                );
            }
            return;
        }

        const CommandResult result = executor.execute(request);
        appendCommandResult(client, result, aof);
    }
}

void processCompletions(
    CompletionQueue& completions,
    EpollPoller& poller,
    std::unordered_map<int, ClientConnection>& clients,
    CommandExecutor& executor,
    CacheService& cache,
    AofLog& aof
) {
    std::uint64_t notifications = 0;
    while (::read(
               completions.eventFd,
               &notifications,
               sizeof(notifications)
           ) > 0) {
    }

    std::deque<CacheCompletion> ready;
    {
        std::lock_guard lock(completions.mutex);
        ready.swap(completions.ready);//从完成队列取出所有结果
    }

    for (CacheCompletion& completion : ready) {
        //通过客户端 fd 查找客户端
        const auto clientIt = clients.find(completion.clientFd);
        if (clientIt == clients.end()) {
            continue;
        }
        //并检查请求编号
        ClientConnection& client = clientIt->second;
        if (!client.pendingRequestId.has_value() ||
            *client.pendingRequestId != completion.requestId) {
            continue;
        }
        client.pendingRequestId.reset();

        if (completion.result.status == LookupResult::Status::Found) {
            // 回源成功后回填内存；即使 value 太大无法缓存，也仍把结果返回给客户端。
            static_cast<void>(cache.set(
                completion.key,
                completion.result.value
            ));//将数据写入内存；更新当前内存使用量；更新 LRU 链表；必要时淘汰旧数据；处理过期时间。
            //将数据编码为 RESP 并写入客户端输出缓冲区
            client.output += RespEncoder::encode(
                RespValue::bulkString(completion.result.value)
            );
        } else if (completion.result.status == LookupResult::Status::NotFound) {
            client.output += RespEncoder::encode(
                RespValue::nullBulkString()
            );
        } else {
            client.output += RespEncoder::encode(
                RespValue::error(
                    "ERR cache backend: " + completion.result.error
                )
            );
        }

        // 一个客户端同一时间只挂起一个回源请求，保证 RESP 响应顺序。
        processRequests(
            client,
            executor,
            cache,
            aof,
            completions
        );

        if ((client.peerClosed || client.closeAfterWrite) &&
            client.output.empty() &&
            !client.pendingRequestId.has_value()) {
            poller.remove(completion.clientFd);
            clients.erase(clientIt);
            std::cout << "client disconnected\n";
            continue;
        }

        std::uint32_t interest = EPOLLIN | EPOLLRDHUP | EPOLLERR;
        if (!client.output.empty()) {
            interest |= EPOLLOUT;
        }
        //注册 EPOLLOUT
        poller.modify(completion.clientFd, interest);
    }
}

void readAvailable(
    ClientConnection& client,
    CommandExecutor& executor,
    CacheService& cache,
    AofLog& aof,
    CompletionQueue& completions
) {
    std::array<char, 4096> buffer{};

    while (!client.closeAfterWrite &&
           !client.pendingRequestId.has_value()) {
        const ssize_t count = client.socket.read(
            buffer.data(),
            buffer.size()
        );

        if (count > 0) {
            client.input.append(
                buffer.data(),
                static_cast<std::size_t>(count)
            );
            processRequests(
                client,
                executor,
                cache,
                aof,
                completions
            );
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
    using cachelite::net::InetAddress;

    try {
        constexpr std::size_t kCacheWorkerCount = 4;
        MemoryStore store;
        MySQLRepository repository(
            MySQLConfig::fromEnvironment(),
            kCacheWorkerCount
        );
        CompletionQueue completions;
        FileDescriptor completionFd(
            ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
        );
        if (completionFd.get() < 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "eventfd"
            );
        }
        completions.eventFd = completionFd.get();

        CacheService cache(
            store,
            repository,
            kCacheWorkerCount
        );
        CommandExecutor executor(cache);
        AofLog aof("data/appendonly.aof");
        aof.replay(executor);

        Socket listenSocket;
        listenSocket.setReuseAddress(true);
        listenSocket.setNonBlocking();
        listenSocket.bind(InetAddress(6379));
        listenSocket.listen();

        EpollPoller poller;
        poller.add(listenSocket.fd(), EPOLLIN);
        poller.add(completionFd.get(), EPOLLIN);
        std::unordered_map<int, ClientConnection> clients;

        std::cout << "RESP epoll server listening on 127.0.0.1:6379\n";

        while (true) {
            for (const EpollPoller::Event event : poller.wait()) {
                //查询完成
                if (event.fd == completionFd.get()) {
                    processCompletions(
                        completions,
                        poller,
                        clients,
                        executor,
                        cache,
                        aof
                    );
                    continue;
                }

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

                // 等待回源时暂停读取，避免响应顺序被后续请求打乱。
                if ((event.events & EPOLLIN) != 0 &&
                    !client.closeAfterWrite &&
                    !client.pendingRequestId.has_value()) {
                    readAvailable(
                        client,
                        executor,
                        cache,
                        aof,
                        completions
                    );
                }

                if ((event.events & EPOLLOUT) != 0 ||
                    !client.output.empty()) {
                    writeAvailable(client);
                }

                if ((client.peerClosed || client.closeAfterWrite) &&
                    client.output.empty() &&
                    !client.pendingRequestId.has_value()) {
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
