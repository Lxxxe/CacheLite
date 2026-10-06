#include "../TestSupport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint16_t kPort = 6379;

class ChildProcess final {
public:
    explicit ChildProcess(const std::string& executable) {
        pid_ = ::fork();
        if (pid_ < 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        if (pid_ == 0) {
            const int nullFd = ::open("/dev/null", O_WRONLY);
            if (nullFd >= 0) {
                ::dup2(nullFd, STDOUT_FILENO);
                ::dup2(nullFd, STDERR_FILENO);
                ::close(nullFd);
            }
            ::execl(executable.c_str(), executable.c_str(), nullptr);
            ::_exit(127);
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ~ChildProcess() {
        stop();
    }

    void stop() noexcept {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

int connectToServer() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::strerror(errno));
    }

    timeval timeout{};
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout)
    );

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    const int converted = ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (converted != 1) {
        ::close(fd);
        throw std::runtime_error("inet_pton failed");
    }

    if (::connect(
            fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        const int error = errno;
        ::close(fd);
        errno = error;
        throw std::runtime_error(std::strerror(error));
    }
    return fd;
}

bool canConnect() {
    try {
        const int fd = connectToServer();
        ::close(fd);
        return true;
    } catch (...) {
        return false;
    }
}

void sendAll(int fd, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t count = ::send(
            fd,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL
        );
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("send failed");
    }
}

std::string receiveUntilClose(int fd) {
    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            response.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            return response;
        }
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::strerror(errno));
    }
}

std::string requestFor(std::size_t index) {
    constexpr std::size_t pingCount = 64;
    const std::string key = "concurrent-key-" + std::to_string(index);
    const std::string value = "value-" + std::to_string(index);
    std::string request =
        "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) +
        "\r\n" + key + "\r\n$" + std::to_string(value.size()) +
        "\r\n" + value + "\r\n" +
        "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) +
        "\r\n" + key + "\r\n";
    for (std::size_t ping = 0; ping < pingCount; ++ping) {
        request += "*1\r\n$4\r\nPING\r\n";
    }
    return request;
}

void servesManyConcurrentClients(const std::string& serverPath) {
    const std::filesystem::path originalDirectory =
        std::filesystem::current_path();
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path workDirectory =
        std::filesystem::temp_directory_path() /
        ("cachelite-server-test-" + std::to_string(now));
    std::filesystem::create_directories(workDirectory / "data");
    std::filesystem::current_path(workDirectory);

    ChildProcess server(serverPath);
    bool ready = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (canConnect()) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    CACHELITE_CHECK(ready);

    constexpr std::size_t clientCount = 64;
    std::mutex failureMutex;
    std::vector<std::string> failures;
    std::vector<std::thread> clients;
    clients.reserve(clientCount);

    for (std::size_t index = 0; index < clientCount; ++index) {
        clients.emplace_back([&, index] {
            try {
                const std::string request = requestFor(index);
                const int fd = connectToServer();
                sendAll(fd, request);
                ::shutdown(fd, SHUT_WR);
                const std::string response = receiveUntilClose(fd);
                ::close(fd);

                const std::string key = "concurrent-key-" + std::to_string(index);
                const std::string value = "value-" + std::to_string(index);
                const std::string pingResponse = "+PONG\r\n";
                std::string expectedResponse =
                    "+OK\r\n$" + std::to_string(value.size()) +
                    "\r\n" + value + "\r\n";
                for (std::size_t ping = 0; ping < 64; ++ping) {
                    expectedResponse += pingResponse;
                }
                if (response != expectedResponse) {
                    std::lock_guard lock(failureMutex);
                    failures.push_back(
                        "unexpected response for " + key + ": " + response
                    );
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(failureMutex);
                failures.push_back(error.what());
            }
        });
    }

    for (std::thread& client : clients) {
        client.join();
    }

    server.stop();
    std::filesystem::current_path(originalDirectory);
    std::error_code cleanupError;
    std::filesystem::remove_all(workDirectory, cleanupError);
    CACHELITE_CHECK(failures.empty());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    return cachelite::test::runSuite(
        "RESP server integration",
        std::vector<std::pair<std::string, std::function<void()>>>{
            {"serves many concurrent clients", [argv] {
                servesManyConcurrentClients(argv[1]);
            }},
        }
    );
}

