#include "cachelite/net/Socket.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cachelite::net {

Socket::Socket()
    : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)) {
    if (fd_ < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "socket"
        );
    }
}

Socket::Socket(int fd) noexcept
    : fd_(fd) {
}
//RAII,对象离开作用域时会自动释放资源
Socket::~Socket() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}
//std::exchange(other.fd_, -1)
//1. 把 `other.fd_` 当前的值（socket 文件描述符）**先保存下来**；
//2. 将 `other.fd_` 赋值为 `-1`（-1 代表无效 fd）；
//3. 返回刚才保存的旧 fd。
Socket::Socket(Socket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }

        fd_ = std::exchange(other.fd_, -1);
    }

    return *this;
}

void Socket::setReuseAddress(bool enabled) {
    const int option = enabled ? 1 : 0;

    //::直接从**全局命名空间**开始查找，跳过局部 / 类作用域查找。
    if (::setsockopt(//**设置 socket 的选项属性**，用来修改套接字行为（端口复用、缓冲区大小、超时、保活等）
            fd_,
            SOL_SOCKET,//选项所属协议层
            SO_REUSEADDR,//要设置哪一项选项,`SO_REUSEADDR`：地址复用
            &option,
            sizeof(option)
        ) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "setsockopt SO_REUSEADDR"
        );
    }
}

void Socket::setNonBlocking(bool enabled) {
    const int currentFlags = ::fcntl(fd_, F_GETFL, 0);//获取 fd 当前的文件状态标志F_GETFL
    if (currentFlags < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "fcntl F_GETFL"
        );
    }

    //`O_NONBLOCK` 是一个宏，二进制里只有某一位是 1，其余都是 0.开启还是关闭 `O_NONBLOCK` 标志
    const int newFlags = enabled
        ? currentFlags | O_NONBLOCK//把 O_NONBLOCK 这一位强制置 1:开启非阻塞
        : currentFlags & ~O_NONBLOCK;//把 O_NONBLOCK 这一位强制清 0:关闭非阻塞

        //将新flags设置回去
    if (::fcntl(fd_, F_SETFL, newFlags) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "fcntl F_SETFL"
        );
    }
}

void Socket::bind(const InetAddress& address) {
    const sockaddr_in& native = address.native();//native()返回内部成员 `address_` 的常引用

    if (::bind(
            fd_,
            reinterpret_cast<const sockaddr*>(&native),//`reinterpret_cast` 是底层指针类型转换:sockaddr_in*变成 `const sockaddr*`
            sizeof(native)
        ) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "bind"
        );
    }
}

void Socket::listen(int backlog) {
    if (::listen(fd_, backlog) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "listen"
        );
    }
}

std::pair<Socket, InetAddress> Socket::accept() {
    sockaddr_in peer{};//栈上定义IPv4地址结构体，初始化为0
    socklen_t peerLength = static_cast<socklen_t>(sizeof(peer));//peer结构体大小

    const int clientFd = ::accept(
        fd_,
        reinterpret_cast<sockaddr*>(&peer),//传入peer的地址(输出型参数),内核把客户端的 IP、端口信息拷贝进这块内存
        &peerLength//传入peerLength的地址(输入输出参数),- 调用前：告诉内核这块内存的大小（`sizeof(peer)`），防止内核写越界；- 返回后：内核会修改它，填入实际写入的地址结构体长度。
    );

    if (clientFd < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "accept"
        );
    }

    return {
        Socket(clientFd),//包装客户端fd成Socket对象
        InetAddress(peer)//用已经被内核填充好的peer，构造InetAddress对象
    };
}

std::optional<std::pair<Socket, InetAddress>> Socket::acceptNonBlocking() {
    sockaddr_in peer{};
    socklen_t peerLength = static_cast<socklen_t>(sizeof(peer));

    const int clientFd = ::accept(
        fd_,
        reinterpret_cast<sockaddr*>(&peer),
        &peerLength
    );
    //EAGAIN / EWOULDBLOCK非阻塞 socket，当前没有待接受的新连接,`EINTR`：系统调用被信号中断。也不算致命错误
    if (clientFd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::nullopt;
        }
        //如果 errno 不是上面这几个，代表**真的出错**
        throw std::system_error(
            errno,
            std::generic_category(),
            "accept"
        );
    }

    Socket client(clientFd);
    client.setNonBlocking();

    //有新连接成功：返回 `std::optional` 里面包着 `pair<Socket, InetAddress>`（新客户端 socket + 客户端地址）
    return std::make_pair(
        std::move(client),
        InetAddress(peer)
    );
}
//void* buffer接收数据的内存位置,size最多读取多少字节
ssize_t Socket::read(void* buffer, std::size_t size) {
    return ::read(fd_, buffer, size);//fd_是成员变量、
    //返回值是带符号的整数类型,返回值 > 0：实际读取到的字节数,0：客户端已经断开连接,< 0：读取失败，需要查看 errno
}

ssize_t Socket::send(const void* data, std::size_t size) {
    return ::send(fd_, data, size, MSG_NOSIGNAL);
    //返回值 > 0：实际发送成功的字节数,返回值 == 0：本次没有发送数据,返回值 < 0：发送失败
}

int Socket::fd() const noexcept {
    return fd_;
}

}  // namespace cachelite::net
