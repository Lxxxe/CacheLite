#include "cachelite/net/Buffer.h"
#include "cachelite/protocol/RespEncoder.h"
#include "cachelite/protocol/RespParser.h"
#include "cachelite/protocol/RespValue.h"

#include "../TestSupport.h"

#include <string>
#include <vector>

namespace {

using cachelite::net::Buffer;
using cachelite::protocol::RespEncoder;
using cachelite::protocol::RespParseStatus;
using cachelite::protocol::RespParser;
using cachelite::protocol::RespValue;

void parsesArrayOfBulkStrings() {
    Buffer input;
    input.append("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");

    RespValue value;
    std::string error;
    const RespParser parser;
    const RespParseStatus status = parser.parse(input, value, error);

    CACHELITE_CHECK(status == RespParseStatus::Complete);
    CACHELITE_CHECK(error.empty());
    CACHELITE_CHECK(value.type == RespValue::Type::Array);
    CACHELITE_CHECK(value.elements.size() == std::size_t{2});
    CACHELITE_CHECK(value.elements[0].text == "GET");
    CACHELITE_CHECK(value.elements[1].text == "key");
    CACHELITE_CHECK(input.readableBytes() == std::size_t{0});
}

void preservesIncompleteBulkString() {
    Buffer input;
    input.append("$5\r\nhe");

    RespValue value;
    std::string error;
    const RespParser parser;
    const RespParseStatus incomplete = parser.parse(input, value, error);

    CACHELITE_CHECK(incomplete == RespParseStatus::Incomplete);
    CACHELITE_CHECK(input.readableView() == "$5\r\nhe");

    input.append("llo\r\n");
    const RespParseStatus complete = parser.parse(input, value, error);
    CACHELITE_CHECK(complete == RespParseStatus::Complete);
    CACHELITE_CHECK(value.type == RespValue::Type::BulkString);
    CACHELITE_CHECK(value.text == "hello");
    CACHELITE_CHECK(input.readableBytes() == std::size_t{0});
}

void parsesMultipleValuesFromOneRead() {
    Buffer input;
    input.append("+OK\r\n:42\r\n");
    const RespParser parser;
    RespValue value;
    std::string error;

    CACHELITE_CHECK(parser.parse(input, value, error) == RespParseStatus::Complete);
    CACHELITE_CHECK(value.type == RespValue::Type::SimpleString);
    CACHELITE_CHECK(value.text == "OK");
    CACHELITE_CHECK(parser.parse(input, value, error) == RespParseStatus::Complete);
    CACHELITE_CHECK(value.type == RespValue::Type::Integer);
    CACHELITE_CHECK(value.integer == 42);
    CACHELITE_CHECK(input.readableBytes() == std::size_t{0});
}

void rejectsMalformedInput() {
    Buffer input;
    input.append("?bad\r\n");
    RespValue value;
    std::string error;

    CACHELITE_CHECK(
        RespParser{}.parse(input, value, error) == RespParseStatus::Error
    );
    CACHELITE_CHECK(!error.empty());
}

void encodesAllMainValueKinds() {
    CACHELITE_CHECK(
        RespEncoder::encode(RespValue::simpleString("OK")) == "+OK\r\n"
    );
    CACHELITE_CHECK(
        RespEncoder::encode(RespValue::error("ERR bad")) == "-ERR bad\r\n"
    );
    CACHELITE_CHECK(
        RespEncoder::encode(RespValue::integerValue(-7)) == ":-7\r\n"
    );
    CACHELITE_CHECK(
        RespEncoder::encode(RespValue::bulkString("hello")) ==
        "$5\r\nhello\r\n"
    );
    CACHELITE_CHECK(
        RespEncoder::encode(RespValue::nullBulkString()) == "$-1\r\n"
    );
    CACHELITE_CHECK(
        RespEncoder::encode(RespValue::array({
            RespValue::bulkString("PING")
        })) == "*1\r\n$4\r\nPING\r\n"
    );
}

}  // namespace

int main() {
    return cachelite::test::runSuite(
        "protocol",
        std::vector<std::pair<std::string, void (*)()>>{
            {"parses array of bulk strings", parsesArrayOfBulkStrings},
            {"preserves incomplete bulk string", preservesIncompleteBulkString},
            {"parses multiple values from one read", parsesMultipleValuesFromOneRead},
            {"rejects malformed input", rejectsMalformedInput},
            {"encodes main RESP value kinds", encodesAllMainValueKinds},
        }
    );
}

