#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cachelite::net {

// A growable byte buffer used for nonblocking socket input and output.
class Buffer {
public:
    explicit Buffer(std::size_t initialCapacity = 4096);

    [[nodiscard]] std::size_t readableBytes() const noexcept;
    [[nodiscard]] std::size_t writableBytes() const noexcept;
    [[nodiscard]] const char* peek() const noexcept;
    [[nodiscard]] char* beginWrite() noexcept;
    [[nodiscard]] std::string_view readableView() const noexcept;

    void ensureWritableBytes(std::size_t length);
    void append(const void* data, std::size_t length);
    void append(std::string_view data);

    void retrieve(std::size_t length);
    void retrieveAll() noexcept;
    [[nodiscard]] std::string retrieveAsString(std::size_t length);

private:
    void makeSpace(std::size_t length);

    std::vector<char> storage_;
    std::size_t readIndex_{0};
    std::size_t writeIndex_{0};
};

}  // namespace cachelite::net
