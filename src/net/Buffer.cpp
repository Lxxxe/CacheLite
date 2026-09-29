#include "cachelite/net/Buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cachelite::net {

Buffer::Buffer(std::size_t initialCapacity)
    : storage_(initialCapacity) {
}

std::size_t Buffer::readableBytes() const noexcept {
    return writeIndex_ - readIndex_;
}

std::size_t Buffer::writableBytes() const noexcept {
    return storage_.size() - writeIndex_;
}

const char* Buffer::peek() const noexcept {
    return storage_.data() + readIndex_;
}

char* Buffer::beginWrite() noexcept {
    return storage_.data() + writeIndex_;
}

std::string_view Buffer::readableView() const noexcept {
    return {peek(), readableBytes()};
}

void Buffer::ensureWritableBytes(std::size_t length) {
    if (writableBytes() < length) {
        makeSpace(length);
    }
}

void Buffer::append(const void* data, std::size_t length) {
    if (length == 0) {
        return;
    }

    ensureWritableBytes(length);
    std::memcpy(beginWrite(), data, length);
    writeIndex_ += length;
}

void Buffer::append(std::string_view data) {
    append(data.data(), data.size());
}

void Buffer::retrieve(std::size_t length) {
    if (length > readableBytes()) {
        throw std::out_of_range("buffer retrieve exceeds readable bytes");
    }

    if (length == readableBytes()) {
        retrieveAll();
        return;
    }

    readIndex_ += length;
}

void Buffer::retrieveAll() noexcept {
    readIndex_ = 0;
    writeIndex_ = 0;
}

std::string Buffer::retrieveAsString(std::size_t length) {
    if (length > readableBytes()) {
        throw std::out_of_range("buffer retrieve exceeds readable bytes");
    }

    std::string result(peek(), length);
    retrieve(length);
    return result;
}

void Buffer::makeSpace(std::size_t length) {
    if (writableBytes() + readIndex_ >= length) {
        const std::size_t readable = readableBytes();
        std::move(
            storage_.begin() + static_cast<std::ptrdiff_t>(readIndex_),
            storage_.begin() + static_cast<std::ptrdiff_t>(writeIndex_),
            storage_.begin()
        );
        readIndex_ = 0;
        writeIndex_ = readable;
        return;
    }

    storage_.resize(writeIndex_ + length);
}

}  // namespace cachelite::net
