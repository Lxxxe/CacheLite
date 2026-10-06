#include "cachelite/persistence/AofLog.h"

#include "cachelite/net/Buffer.h"
#include "cachelite/protocol/RespEncoder.h"
#include "cachelite/protocol/RespParser.h"

#include <filesystem>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using cachelite::command::CommandExecutionMode;
using cachelite::net::Buffer;
using cachelite::protocol::RespParseStatus;
using cachelite::protocol::RespParser;
using cachelite::protocol::RespValue;

}  // namespace

namespace cachelite::persistence {

AofLog::AofLog(std::string path, AofFsyncPolicy policy)
    : path_(std::move(path)),
      policy_(policy) {
    const std::filesystem::path filePath(path_);
    const std::filesystem::path parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::system_error(
                error.value(),
                std::generic_category(),
                "create AOF directory"
            );
        }
    }

    reopenForAppend();

    if (policy_ == AofFsyncPolicy::EverySecond) {
        syncThread_ = std::thread(&AofLog::syncLoop, this);
    }
}

AofLog::~AofLog() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    stopCondition_.notify_one();

    if (syncThread_.joinable()) {
        syncThread_.join();
    }

    std::lock_guard lock(mutex_);
    try {
        flushUnlocked();
        if (policy_ != AofFsyncPolicy::No) {
            syncUnlocked();
        }
    } catch (...) {
        // Destructors cannot report I/O errors. The append path reports
        // synchronous errors, and normal shutdown still attempts a final sync.
    }
    output_.close();
    closeSyncFile();
}

AofFsyncPolicy AofLog::policyFromEnvironment() {
    const char* rawPolicy = std::getenv("CACHELITE_AOF_POLICY");
    if (rawPolicy == nullptr || *rawPolicy == '\0') {
        return AofFsyncPolicy::EverySecond;
    }

    std::string value(rawPolicy);
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );

    if (value == "always") {
        return AofFsyncPolicy::Always;
    }
    if (value == "everysec") {
        return AofFsyncPolicy::EverySecond;
    }
    if (value == "no") {
        return AofFsyncPolicy::No;
    }

    throw std::invalid_argument(
        "CACHELITE_AOF_POLICY must be always, everysec, or no"
    );
}

const char* AofLog::policyName(AofFsyncPolicy policy) noexcept {
    switch (policy) {
    case AofFsyncPolicy::Always:
        return "always";
    case AofFsyncPolicy::EverySecond:
        return "everysec";
    case AofFsyncPolicy::No:
        return "no";
    }

    return "unknown";
}

void AofLog::reopenForAppend() {
    output_.open(path_, std::ios::binary | std::ios::app);
    if (!output_.is_open()) {
        throw std::runtime_error("cannot open AOF file: " + path_);
    }

#if defined(__linux__)
    syncFd_ = ::open(
        path_.c_str(),
        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
        0644
    );
    if (syncFd_ < 0) {
        output_.close();
        throw std::system_error(
            errno,
            std::generic_category(),
            "open AOF sync descriptor"
        );
    }
#endif
}

void AofLog::closeSyncFile() noexcept {
#if defined(__linux__)
    if (syncFd_ >= 0) {
        ::close(syncFd_);
        syncFd_ = -1;
    }
#else
    syncFd_ = -1;
#endif
}

void AofLog::append(const protocol::RespValue& command) {
    const std::string encoded = protocol::RespEncoder::encode(command);

    std::lock_guard lock(mutex_);
    if (syncError_) {
        std::rethrow_exception(syncError_);
    }

    output_.write(
        encoded.data(),
        static_cast<std::streamsize>(encoded.size())
    );
    if (!output_) {
        throw std::runtime_error("cannot append AOF file: " + path_);
    }

    if (policy_ == AofFsyncPolicy::Always) {
        flushUnlocked();
        syncUnlocked();
    }
}

void AofLog::flush() {
    std::lock_guard lock(mutex_);
    if (syncError_) {
        std::rethrow_exception(syncError_);
    }
    flushUnlocked();
}

void AofLog::flushUnlocked() {
    output_.flush();
    if (!output_) {
        throw std::runtime_error("cannot flush AOF file: " + path_);
    }
}

void AofLog::syncUnlocked() {
#if defined(__linux__)
    if (syncFd_ < 0) {
        throw std::runtime_error("AOF sync descriptor is not open");
    }
    if (::fsync(syncFd_) < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "fsync AOF file"
        );
    }
#else
    // The networking server targets Linux. On other hosts flush remains the
    // strongest portable guarantee available from std::ofstream here.
#endif
}

void AofLog::syncLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (stopCondition_.wait_for(
                lock,
                std::chrono::seconds(1),
                [this] { return stopping_; }
            )) {
            break;
        }

        try {
            flushUnlocked();
            syncUnlocked();
        } catch (...) {
            syncError_ = std::current_exception();
            return;
        }
    }
}

void AofLog::replay(command::CommandExecutor& executor) {
    std::lock_guard lock(mutex_);
    if (syncError_) {
        std::rethrow_exception(syncError_);
    }
    flushUnlocked();

    std::ifstream inputFile(path_, std::ios::binary);
    if (!inputFile.is_open()) {
        throw std::runtime_error("cannot read AOF file: " + path_);
    }

    const std::string bytes{
        std::istreambuf_iterator<char>(inputFile),
        std::istreambuf_iterator<char>{}
    };

    Buffer input;
    input.append(bytes);
    RespParser parser;
    std::size_t validBytes = 0;

    while (input.readableBytes() > 0) {
        const std::size_t before = input.readableBytes();
        RespValue commandValue;
        std::string error;
        const RespParseStatus status = parser.parse(
            input,
            commandValue,
            error
        );

        if (status == RespParseStatus::Incomplete) {
            // 崩溃可能导致最后一条 AOF 记录只有半条，丢弃这部分尾巴。
            break;
        }

        if (status == RespParseStatus::Error) {
            throw std::runtime_error("AOF parse error: " + error);
        }

        const command::CommandResult result = executor.execute(
            commandValue,
            CommandExecutionMode::Replay
        );
        if (result.response.type == protocol::RespValue::Type::Error) {
            throw std::runtime_error(
                "AOF command execution error: " + result.response.text
            );
        }
        validBytes += before - input.readableBytes();
    }

    if (validBytes < bytes.size()) {
        output_.close();
        closeSyncFile();
        std::error_code error;
        std::filesystem::resize_file(path_, validBytes, error);
        if (error) {
            throw std::system_error(
                error.value(),
                std::generic_category(),
                "truncate incomplete AOF"
            );
        }
        reopenForAppend();
    }
}

}  // namespace cachelite::persistence
