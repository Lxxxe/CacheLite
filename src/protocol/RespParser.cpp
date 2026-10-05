#include "cachelite/protocol/RespParser.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace cachelite::protocol {

RespParseStatus RespParser::parse(
    net::Buffer& input,
    RespValue& value,//传出解析后并封装好RESP数据类型的结构体
    std::string& error
) const {
    error.clear();

    RespValue parsed;
    std::size_t offset = 0;
    const ParseResult result = parseValue(
        input.readableView(),
        offset,
        0,
        parsed,
        error
    );

    if (result == ParseResult::Complete) {
        input.retrieve(offset);
        value = std::move(parsed);
        return RespParseStatus::Complete;
    }

    if (result == ParseResult::Incomplete) {
        return RespParseStatus::Incomplete;
    }

    return RespParseStatus::Error;
}

RespParser::ParseResult RespParser::parseValue(
    std::string_view input,//string_view（C++17 引入）只读视图，不拥有数据，不会复制字符串,只保存指针和长度,适合解析网络缓冲区中的数据,input 本身不会被修改
    std::size_t& offset,//表示当前解析位置，是一个输入输出参数
    std::size_t depth,//表示当前 RESP 数组的嵌套深度
    RespValue& value,//输出参数，用来保存解析结果
    std::string& error
) const {
    //防止恶意客户端发送过深的嵌套数组，导致递归栈溢出
    if (depth > kMaxNestingDepth) {
        error = "RESP nesting depth exceeds limit";
        return ParseResult::Error;
    }
    //半包
    if (offset >= input.size()) {
        return ParseResult::Incomplete;
    }

    //读取 RESP 类型前缀字节，同时 offset 后移一位
    const char prefix = input[offset++];
    std::string_view line;//用于接收readLine解析出的命令

    switch (prefix) {
    //简单字符串（Simple String）
    case '+': {
        //调用 `readLine` 从 offset 开始读到 `\r\n` 为止，把 `\r\n` 之前的内容提取出来存到 `line` 里,同时 `offset` 跳过 `\r\n`，指向下一个值的起始位置
        const ParseResult result = readLine(input, offset, line, error);
        if (result != ParseResult::Complete) {
            return result;
        }
        //将line封装成简单字符串类型的 `RespValue`
        value = RespValue::simpleString(std::string(line));
        return ParseResult::Complete;
    }
    //错误信息（Error）
    case '-': {
        const ParseResult result = readLine(input, offset, line, error);
        if (result != ParseResult::Complete) {
            return result;
        }
        //将line封装成错误类型的 `RespValue`
        value = RespValue::error(std::string(line));
        return ParseResult::Complete;
    }
    //整数（Integer）
    case ':': {
        const ParseResult result = readLine(input, offset, line, error);
        if (result != ParseResult::Complete) {
            return result;
        }

        std::int64_t integer = 0;
        //把这行字符串解析成 64 位整数
        if (parseIntegerLine(line, integer, error) != ParseResult::Complete) {
            return ParseResult::Error;
        }

        value = RespValue::integerValue(integer);
        return ParseResult::Complete;
    }
    //批量字符串（Bulk String）
    case '$': {
        // 1. 读取长度行
        const ParseResult result = readLine(input, offset, line, error);
        if (result != ParseResult::Complete) {
            return result;
        }
        // 2. 解析长度值
        std::int64_t length = 0;
        if (parseIntegerLine(line, length, error) != ParseResult::Complete) {
            return ParseResult::Error;
        }
        // 3. 特殊：长度为 -1 代表 null 批量字符串
        if (length == -1) {
            value = RespValue::nullBulkString();
            return ParseResult::Complete;
        }
        // 4. 长度合法性校验
        if (length < -1 ||
            static_cast<std::uint64_t>(length) > kMaxBulkStringLength) {
            error = "RESP bulk string length is invalid or too large";
            return ParseResult::Error;
        }
        // 5. 半包检查：缓冲区剩余字节够不够「字符串内容 + 末尾 \r\n」
        const std::size_t stringLength = static_cast<std::size_t>(length);
        if (input.size() - offset < stringLength + 2) {
            return ParseResult::Incomplete;
        }
        // 6. 校验末尾必须是 \r\n
        if (input.substr(offset + stringLength, 2) != "\r\n") {
            error = "RESP bulk string is not terminated by CRLF";
            return ParseResult::Error;
        }
        // 7. 截取字符串，更新 offset
        value = RespValue::bulkString(
            std::string(input.substr(offset, stringLength))
        );
        offset += stringLength + 2;
        return ParseResult::Complete;
    }
    //数组（Array）
    case '*': {
        // 1. 读取元素个数行
        const ParseResult result = readLine(input, offset, line, error);
        if (result != ParseResult::Complete) {
            return result;
        }
        // 2. 解析元素个数
        std::int64_t count = 0;
        if (parseIntegerLine(line, count, error) != ParseResult::Complete) {
            return ParseResult::Error;
        }
        // 3. 特殊：个数为 -1 代表 null 数组
        if (count == -1) {
            value = RespValue::nullArray();
            return ParseResult::Complete;
        }
        // 4. 个数合法性校验
        if (count < -1 ||
            static_cast<std::uint64_t>(count) > kMaxArrayElements) {
            error = "RESP array length is invalid or too large";
            return ParseResult::Error;
        }
        // 5. 循环解析每个元素，递归调用 parseValue
        std::vector<RespValue> elements;
        elements.reserve(static_cast<std::size_t>(count));

        for (std::int64_t index = 0; index < count; ++index) {
            RespValue element;
            const ParseResult elementResult = parseValue(
                input,
                offset,
                depth + 1,
                element,
                error
            );

            if (elementResult != ParseResult::Complete) {
                return elementResult;
            }

            elements.push_back(std::move(element));
        }
        // 6. 组装成数组类型返回
        value = RespValue::array(std::move(elements));
        return ParseResult::Complete;
    }

    default:
        error = "unknown RESP type prefix";
        return ParseResult::Error;
    }
}
//读取 RESP 协议中的“一行协议头”
RespParser::ParseResult RespParser::readLine(
    std::string_view input, //当前 Buffer 中尚未消费的全部输入数据
    std::size_t& offset,    //当前解析位置
    std::string_view& line, //输出读取到的内容
    std::string& error
) {
    //从 `offset` 位置开始，在输入缓冲区中查找第一个 `\r\n` 的起始下标
    //找到：`lineEnd` 就是 `\r` 所在的下标,没找到：返回 `std::string_view::npos` 特殊值
    const std::size_t lineEnd = input.find("\r\n", offset);
    //没找到 `\r\n` 的处理（半包 / 超长错误）
    if (lineEnd == std::string_view::npos) {
        //**当前未完结的半行长度已经超过最大限制 `kMaxLineLength`**：直接判定为协议错误。这是安全防护，防止恶意客户端发送无限长的行耗尽服务器内存。
        if (input.size() - offset > kMaxLineLength) {
            error = "RESP line exceeds limit";
            return ParseResult::Error;
        }
        //**长度未超限但没找到结束符**：返回 `Incomplete`（半包）
        return ParseResult::Incomplete;
    }
    //即使找到了完整的行，也要校验单行总长度是否超限
    if (lineEnd - offset > kMaxLineLength) {
        error = "RESP line exceeds limit";
        return ParseResult::Error;
    }
    //`substr` 截取从offset开始lineEnd - offset的长度赋值给输出参数 `line`
    line = input.substr(offset, lineEnd - offset);
    offset = lineEnd + 2;//`offset` 向后移动 `2` 字节，跳过 `\r\n`，正好指向下一行的起始位置
    return ParseResult::Complete;
}

RespParser::ParseResult RespParser::parseIntegerLine(
    std::string_view line,
    std::int64_t& value,
    std::string& error
) {
    if (line.empty()) {
        error = "RESP integer is empty";
        return ParseResult::Error;
    }
    //`std::from_chars` 是 C++17 引入的**低开销字符串转整数函数**，专门用于高性能数值解析，不分配内存、不抛异常。
    const auto [end, result] = std::from_chars(
        line.data(),//定义解析的字符范围 `[起始指针, 结束指针)`
        line.data() + line.size(),
        value//输出参数，解析成功的整数写入 `value`
    );

    if (result != std::errc{} || end != line.data() + line.size()) {
        error = "RESP integer is invalid";
        return ParseResult::Error;
    }

    return ParseResult::Complete;
}

}  // namespace cachelite::protocol
