#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cachelite::net {

class EpollPoller {
public:
    struct Event {
        int fd;
        std::uint32_t events;
    };

    EpollPoller();
    ~EpollPoller();

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    EpollPoller(EpollPoller&& other) noexcept;
    EpollPoller& operator=(EpollPoller&& other) noexcept;

    void add(int fd, std::uint32_t events);
    void modify(int fd, std::uint32_t events);
    void remove(int fd);

    [[nodiscard]] std::vector<Event> wait(int timeoutMilliseconds = -1);

private:
    int epollFd_{-1};
};

}  // namespace cachelite::net
