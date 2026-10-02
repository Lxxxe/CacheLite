#include "cachelite/persistence/AofLog.h"

#include "cachelite/net/Buffer.h"
#include "cachelite/protocol/RespEncoder.h"
#include "cachelite/protocol/RespParser.h"

#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using cachelite::command::CommandExecutionMode;
using cachelite::net::Buffer;
using cachelite::protocol::RespParseStatus;
using cachelite::protocol::RespParser;
using cachelite::protocol::RespValue;

}  // namespace

namespace cachelite::persistence {

AofLog::AofLog(std::string path)
    : path_(std::move(path)) {
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
}

void AofLog::reopenForAppend() {
    output_.open(path_, std::ios::binary | std::ios::app);
    if (!output_.is_open()) {
        throw std::runtime_error("cannot open AOF file: " + path_);
    }
}

void AofLog::append(const protocol::RespValue& command) {
    const std::string encoded = protocol::RespEncoder::encode(command);
    output_.write(
        encoded.data(),
        static_cast<std::streamsize>(encoded.size())
    );
    if (!output_) {
        throw std::runtime_error("cannot append AOF file: " + path_);
    }

    flush();
}

void AofLog::flush() {
    output_.flush();
    if (!output_) {
        throw std::runtime_error("cannot flush AOF file: " + path_);
    }
}

void AofLog::replay(command::CommandExecutor& executor) {
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
