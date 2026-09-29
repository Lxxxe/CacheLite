#pragma once

#include "cachelite/net/Buffer.h"
#include "cachelite/protocol/RespValue.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cachelite::protocol {

enum class RespParseStatus {
    Complete,
    Incomplete,
    Error
};

class RespParser {
public:
    [[nodiscard]] RespParseStatus parse(
        net::Buffer& input,
        RespValue& value,
        std::string& error
    ) const;

private:
    enum class ParseResult {
        Complete,
        Incomplete,
        Error
    };

    static constexpr std::size_t kMaxLineLength = 1024 * 1024;
    static constexpr std::size_t kMaxBulkStringLength = 64 * 1024 * 1024;
    static constexpr std::size_t kMaxArrayElements = 1024;
    static constexpr std::size_t kMaxNestingDepth = 32;

    [[nodiscard]] ParseResult parseValue(
        std::string_view input,
        std::size_t& offset,
        std::size_t depth,
        RespValue& value,
        std::string& error
    ) const;

    [[nodiscard]] static ParseResult readLine(
        std::string_view input,
        std::size_t& offset,
        std::string_view& line,
        std::string& error
    );

    [[nodiscard]] static ParseResult parseIntegerLine(
        std::string_view line,
        std::int64_t& value,
        std::string& error
    );
};

}  // namespace cachelite::protocol
