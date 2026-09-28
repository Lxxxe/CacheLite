#include "cachelite/net/EpollPoller.h"

#include <cerrno>
#include <system_error>
#include <utility>

#include <sys/epoll.h>
#include <unistd.h>

namespace cachelite::net {

EpollPoller::EpollPoller()
    : epollFd_(::epoll_create1(EPOLL_CLOEXEC)) {//调用 Linux 系统函数epoll_create1()创建一个epoll，会返回一个新的文件描述符赋值给epollFd_
    if (epollFd_ < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_create1"
        );
    }
}

EpollPoller::~EpollPoller() {
    if (epollFd_ >= 0) {
        ::close(epollFd_);
    }
}

EpollPoller::EpollPoller(EpollPoller&& other) noexcept
    : epollFd_(std::exchange(other.epollFd_, -1)) {
}

EpollPoller& EpollPoller::operator=(EpollPoller&& other) noexcept {
    if (this != &other) {
        if (epollFd_ >= 0) {
            ::close(epollFd_);
        }

        epollFd_ = std::exchange(other.epollFd_, -1);
    }

    return *this;
}

void EpollPoller::add(int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events;//事件类型
    event.data.fd = fd;
    //添加进epollFd_
    if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_ctl ADD"
        );
    }
}

void EpollPoller::modify(int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    //EPOLL_CTL_MOD修改事件
    if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_ctl MOD"
        );
    }
}

void EpollPoller::remove(int fd) {
    if (::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        if (errno == ENOENT || errno == EBADF) {
            return;
        }

        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_ctl DEL"
        );
    }
}

std::vector<EpollPoller::Event> EpollPoller::wait(int timeoutMilliseconds) {
    constexpr int kMaxEvents = 64;//单次 epoll_wait 最多一次取出 64 个就绪事件
    epoll_event rawEvents[kMaxEvents]{};

    const int count = ::epoll_wait(
        epollFd_,
        rawEvents,
        kMaxEvents,
        timeoutMilliseconds
    );

    if (count < 0) {
        if (errno == EINTR) {
            return {};
        }

        throw std::system_error(
            errno,
            std::generic_category(),
            "epoll_wait"
        );
    }

    std::vector<Event> events;//创建要返回的事件 vector
    events.reserve(static_cast<std::size_t>(count));//`reserve` 提前预分配内存，避免 push_back 时多次扩容
    //遍历内核返回的原生事件数组，把每一条事件里的 fd 和事件类型，构造自定义 `EpollPoller::Event` 对象存入 vector
    for (int index = 0; index < count; ++index) {
        events.push_back({
            rawEvents[index].data.fd,
            rawEvents[index].events
        });
    }

    return events;
}

}  // namespace cachelite::net
