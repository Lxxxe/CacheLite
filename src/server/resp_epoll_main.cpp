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
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

using cachelite::cache::CacheOptions;
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

std::uint16_t listenPortFromEnvironment() {
    const char* value = std::getenv("CACHELITE_PORT");
    if (value == nullptr) {
        return 6379;
    }

    unsigned int port = 0;
    const char* end = value + std::char_traits<char>::length(value);
    const auto [parsedEnd, error] = std::from_chars(value, end, port);
    if (error != std::errc{} || parsedEnd != end ||
        port == 0 || port > 65535) {
        throw std::invalid_argument("CACHELITE_PORT must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

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
    Socket socket;                                //客户端的 TCP socket，负责保存 fd，并进行 read()、send() 等操作。Socket 是 RAII 对象，连接销毁时会自动关闭 fd。
    Buffer input;                                 //客户端输入缓冲区
    std::string output;                           //待发送给客户端的响应数据
    bool peerClosed{false};                       //表示客户端是否已经关闭连接
    bool closeAfterWrite{false};                  //表示是否在输出数据发送完后关闭连接。
    std::optional<std::uint64_t> pendingRequestId;//当前正在等待异步 MySQL 回源的请求编号
    std::uint64_t nextRequestId{0};               //为当前客户端生成请求编号的计数器，每次异步回源请求使用后递增。
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
        aof.append(persistenceCommand);//把persistenceCommand编码成 RESP 格式并追加到 AOF 文件
    }
    client.output += RespEncoder::encode(result.response);//把响应编码到客户端输出缓冲区
}

void processRequests(
    ClientConnection& client,
    CommandExecutor& executor,
    CacheService& cache,
    AofLog& aof,
    CompletionQueue& completions
) {
    RespParser parser;
    //循环处理client.input中的命令
    while (!client.closeAfterWrite &&
           !client.pendingRequestId.has_value()) {
        RespValue request;//用于接收parse解析完的结果
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

        std::string getKey;//getkey用于接收isGetRequest传回的命令中的key
        //判断是不是 GET 请求,
        if (isGetRequest(request, getKey)) {
            //收到 GET key 后首先查询缓存
            const auto cached = cache.getLocal(getKey);//内部调用MemoryStore::get
            if (cached.has_value()) {
                client.output += RespEncoder::encode(   //封装回应内容
                    RespValue::bulkString(*cached)      //创建批量字符串
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
                        //调用 `notifyCompletion`，线程安全地往完成队列 `completions` 里插入一个完成项，并写入 eventfd
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

        const CommandResult result = executor.execute(request);//执行命令
        appendCommandResult(client, result, aof);//把修改命令写入 AOF,把响应编码到客户端输出缓冲区
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
    std::uint64_t notifications = 0;//清空 eventfd 通知计数
    //一次性读取 eventfd 中的所有通知，避免重复触发 EPOLLIN 事件
    while (::read(
               completions.eventFd,
               &notifications,
               sizeof(notifications)
           ) > 0) {
    }

    std::deque<CacheCompletion> ready;
    {
        std::lock_guard lock(completions.mutex);
        ready.swap(completions.ready);//调用 `swap` 以 O (1) 代价把整个完成队列交换到局部变量 `ready` 中；
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
        client.pendingRequestId.reset();//清除挂起请求编号，表示回源完成

        if (completion.result.status == LookupResult::Status::Found) {
            // 回源成功后回填内存；即使 value 太大无法缓存，也仍把结果返回给客户端。
            static_cast<void>(cache.setFromBackend(
                completion.key,
                completion.result.value
            ));//将数据写入内存；更新当前内存使用量；更新 LRU 链表；必要时淘汰旧数据；处理过期时间。
            //将数据编码为 RESP 并写入客户端输出缓冲区
            client.output += RespEncoder::encode(
                RespValue::bulkString(completion.result.value)
            );
        } else if (completion.result.status == LookupResult::Status::NotFound) {
            //回源未命中，将空值编码为 RESP 并写入客户端输出缓冲区
            client.output += RespEncoder::encode(
                RespValue::nullBulkString()
            );
        } else {
            //回源失败，将错误信息编码为 RESP 并写入客户端输出缓冲区
            client.output += RespEncoder::encode(
                RespValue::error(
                    "ERR cache backend: " + completion.result.error
                )
            );
        }

        // 一个客户端同一时间只挂起一个回源请求，保证 RESP 响应顺序。
        //用 processRequests 继续处理客户端输入缓冲区里排队的下一条命令
        processRequests(
            client,
            executor,
            cache,
            aof,
            completions
        );

        if ((client.peerClosed || client.closeAfterWrite) &&//对端已经关闭连接，或者客户端执行了 QUIT 命令要求写完就关闭
            client.output.empty() &&                        //输出缓冲区已经全部发完
            !client.pendingRequestId.has_value()) {         //没有挂起的异步请求
            poller.remove(completion.clientFd);             //从 epoll 中移除这个客户端，并从 clients 中删除它。
            clients.erase(clientIt);
            std::cout << "client disconnected\n";
            continue;
        }
        //如果客户端输出缓冲区不为空，那么就注册 EPOLLOUT 事件，以便在 socket 可写时发送数据
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
        //实际发送的字节数
        if (count > 0) {
            client.output.erase(0, static_cast<std::size_t>(count));
            continue;
        }
        //本次没有发送数据
        if (count == 0) {
            client.peerClosed = true;
            return;
        }
        //发送失败
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

        CacheOptions cacheOptions;
        cacheOptions.workerCount = kCacheWorkerCount;
        cacheOptions.maxPendingLookups = 1024;
        cacheOptions.maxWaitersPerKey = 1024;
        cacheOptions.negativeCacheTtl = std::chrono::seconds(10);
        cacheOptions.backendCacheTtl = std::chrono::seconds(60);
        cacheOptions.backendCacheJitter = std::chrono::seconds(10);
        cacheOptions.backendFailureThreshold = 5;
        cacheOptions.circuitCooldown = std::chrono::seconds(5);
        CacheService cache(store, repository, cacheOptions);
        CommandExecutor executor(cache);
        const auto aofPolicy = AofLog::policyFromEnvironment();
        AofLog aof("data/appendonly.aof", aofPolicy);
        aof.replay(executor);

        Socket listenSocket;
        listenSocket.setReuseAddress(true);
        listenSocket.setNonBlocking();
        const std::uint16_t listenPort = listenPortFromEnvironment();
        listenSocket.bind(InetAddress(listenPort));
        listenSocket.listen();

        EpollPoller poller;
        poller.add(listenSocket.fd(), EPOLLIN);
        poller.add(completionFd.get(), EPOLLIN);
        std::unordered_map<int, ClientConnection> clients;

        std::cout << "RESP epoll server listening on 127.0.0.1:"
                  << listenPort << '\n'
                  << "AOF fsync policy: "
                  << AofLog::policyName(aofPolicy) << '\n';

        while (true) {
            const auto events = poller.wait();

            // 优先处理回源完成事件，在读取新的 GET 前先完成缓存回填。
            // 这样同一个 key 的新请求可以直接命中刚回填的 MemoryStore。
            for (const EpollPoller::Event event : events) {
                if (event.fd == completionFd.get()) {
                    //把后台 MySQL 线程完成的结果重新交给客户端
                    processCompletions(
                        completions,//回源完成队列
                        poller,     //epoll 事件管理器
                        clients,    //所有已连接客户端
                        executor,   //命令执行器
                        cache,      //缓存服务
                        aof         //AOF 持久化日志
                    );
                    break;
                }
            }

            for (const EpollPoller::Event event : events) {
                //跳过已经处理过的回源完成事件，继续处理本批次中的其他 socket 事件。
                if (event.fd == completionFd.get()) {
                    continue;
                }
                //处理新连接事件
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
                //处理客户端 socket 事件
                const auto clientIt = clients.find(event.fd);
                if (clientIt == clients.end()) {
                    continue;
                }

                ClientConnection& client = clientIt->second;
                client.peerClosed = client.peerClosed ||
                    (event.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;

                // 有数据可读，但等待回源时暂停读取，避免响应顺序被后续请求打乱。
                if ((event.events & EPOLLIN) != 0 &&        //有数据可读
                    !client.closeAfterWrite &&              //客户端没有关闭连接
                    !client.pendingRequestId.has_value()) { //当前客户端没有挂起的回源请求
                    readAvailable(
                        client,
                        executor,
                        cache,
                        aof,
                        completions
                    );
                }

                //有数据可写，或者输出缓冲区不为空，就尝试写入客户端
                if ((event.events & EPOLLOUT) != 0 ||
                    !client.output.empty()) {
                    writeAvailable(client);
                }
                //如果客户端已经关闭连接，或者输出缓冲区为空且没有挂起的回源请求，就移除客户端
                if ((client.peerClosed || client.closeAfterWrite) &&
                    client.output.empty() &&
                    !client.pendingRequestId.has_value()) {
                    poller.remove(event.fd);
                    clients.erase(clientIt);
                    std::cout << "client disconnected\n";
                    continue;
                }
                //更新客户端 socket 的事件兴趣集，继续监听 EPOLLIN、EPOLLRDHUP、EPOLLERR，如果输出缓冲区不为空就监听 EPOLLOUT
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
