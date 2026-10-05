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

    static constexpr std::size_t kMaxLineLength = 1024 * 1024;              //单行 RESP 头的最大长度，防止恶意超长输入。
    static constexpr std::size_t kMaxBulkStringLength = 64 * 1024 * 1024;   //Bulk String 最大长度，当前为 64 MiB
    static constexpr std::size_t kMaxArrayElements = 1024;                  //数组元素数量上限
    static constexpr std::size_t kMaxNestingDepth = 32;                     //嵌套数组的深度上限

    //递归解析首字节、长度、bulk 内容和数组
    [[nodiscard]] ParseResult parseValue(
        std::string_view input,
        std::size_t& offset,
        std::size_t depth,
        RespValue& value,
        std::string& error
    ) const;
    // 读取结尾的协议头，并移动 offset。
    [[nodiscard]] static ParseResult readLine(
        std::string_view input,
        std::size_t& offset,
        std::string_view& line,
        std::string& error
    );
    //把协议头中的十进制数字转换为 int64_t
    [[nodiscard]] static ParseResult parseIntegerLine(
        std::string_view line,
        std::int64_t& value,
        std::string& error
    );
};

}  // namespace cachelite::protocol
