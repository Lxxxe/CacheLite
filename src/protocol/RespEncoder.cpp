#include "cachelite/protocol/RespEncoder.h"

#include <stdexcept>

namespace cachelite::protocol {

std::string RespEncoder::encode(const RespValue& value) {
    switch (value.type) {
    case RespValue::Type::SimpleString:
        return "+" + value.text + "\r\n";

    case RespValue::Type::Error:
        return "-" + value.text + "\r\n";

    case RespValue::Type::Integer:
        return ":" + std::to_string(value.integer) + "\r\n";

    case RespValue::Type::BulkString:
        return "$" + std::to_string(value.text.size()) +
            "\r\n" + value.text + "\r\n";

    case RespValue::Type::Array: {
        std::string result = "*" +
            std::to_string(value.elements.size()) + "\r\n";

        for (const RespValue& element : value.elements) {
            result += encode(element);
        }

        return result;
    }

    case RespValue::Type::NullBulkString:
        return "$-1\r\n";

    case RespValue::Type::NullArray:
        return "*-1\r\n";
    }

    throw std::logic_error("unknown RESP value type");
}

}  // namespace cachelite::protocol
