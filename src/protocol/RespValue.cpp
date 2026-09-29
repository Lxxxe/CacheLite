#include "cachelite/protocol/RespValue.h"

#include <utility>

namespace cachelite::protocol {

RespValue RespValue::simpleString(std::string value) {
    RespValue result;
    result.type = Type::SimpleString;
    result.text = std::move(value);//move把字符串内容移动到 RespValue，减少不必要的拷贝
    return result;
}

RespValue RespValue::error(std::string value) {
    RespValue result;
    result.type = Type::Error;
    result.text = std::move(value);
    return result;
}

RespValue RespValue::integerValue(std::int64_t value) {
    RespValue result;
    result.type = Type::Integer;
    result.integer = value;
    return result;
}

RespValue RespValue::bulkString(std::string value) {
    RespValue result;
    result.type = Type::BulkString;
    result.text = std::move(value);
    return result;
}

RespValue RespValue::array(std::vector<RespValue> values) {
    RespValue result;
    result.type = Type::Array;
    result.elements = std::move(values);
    return result;
}

RespValue RespValue::nullBulkString() {
    RespValue result;
    result.type = Type::NullBulkString;
    return result;
}

RespValue RespValue::nullArray() {
    RespValue result;
    result.type = Type::NullArray;
    return result;
}

}  // namespace cachelite::protocol
