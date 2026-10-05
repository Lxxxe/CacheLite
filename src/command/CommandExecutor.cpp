#include "cachelite/command/CommandExecutor.h"

#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using cachelite::command::CommandExecutionMode;
using cachelite::command::CommandResult;
using cachelite::protocol::RespValue;

std::string upper(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(character))
        ));
    }
    return result;
}

std::optional<std::int64_t> parseInteger(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, result] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (result != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> addIntegers(
    std::int64_t left,
    std::int64_t right
) noexcept {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return std::nullopt;
    }
    return left + right;
}

std::optional<std::int64_t> subtractIntegers(
    std::int64_t left,
    std::int64_t right
) noexcept {
    if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
        (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
        return std::nullopt;
    }
    return left - right;
}

//把若干个 RespValue 参数组合成一个 RESP 数组命令
RespValue makeCommand(
    std::initializer_list<RespValue> elements//表示一个由多个 RespValue 组成的临时列表
) {
    return RespValue::array(std::vector<RespValue>(elements));//把 initializer_list 中的所有元素复制到一个 vector 中
    //RespValue::array()1. 创建一个 RespValue,2. 设置类型为 RESP 数组,3. 保存数组元素
}

}  // namespace

namespace cachelite::command {

CommandExecutor::CommandExecutor(cache::CacheService& cache) noexcept
    : cache_(cache) {//保存 CacheService 的引用
}

CommandResult CommandExecutor::execute(
    const protocol::RespValue& request,
    CommandExecutionMode mode
) {
    CommandResult result;

    if (request.type != RespValue::Type::Array || request.elements.empty()) {
        result.response = RespValue::error(
            "ERR command must be a non-empty RESP array"
        );
        return result;
    }

    for (const RespValue& argument : request.elements) {
        if (argument.type != RespValue::Type::BulkString) {
            result.response = RespValue::error(
                "ERR command arguments must be bulk strings"
            );
            return result;
        }
    }

    const std::string command = upper(request.elements.front().text);
    const bool record = mode == CommandExecutionMode::Normal;//Normal返回ture,Replay返回false
    //record=flase后续只会会写入内存，不会生成新的 AOF

    if (command == "PING") {
        if (request.elements.size() == 1) {
            result.response = RespValue::simpleString("PONG");
        } else if (request.elements.size() == 2) {
            result.response = RespValue::bulkString(request.elements[1].text);
        } else {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'ping'"
            );
        }
        return result;
    }

    if (command == "ECHO") {
        if (request.elements.size() != 2) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'echo'"
            );
        } else {
            result.response = RespValue::bulkString(request.elements[1].text);
        }
        return result;
    }

    if (command == "EXISTS") {
        if (request.elements.size() < 2) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'exists'"
            );
            return result;
        }

        // Redis 对重复 key 只统计一次。
        std::unordered_set<std::string> seen;
        std::int64_t existing = 0;
        for (std::size_t index = 1; index < request.elements.size(); ++index) {
            const std::string& key = request.elements[index].text;
            if (seen.insert(key).second && cache_.exists(key)) {
                ++existing;
            }
        }
        result.response = RespValue::integerValue(existing);
        return result;
    }

    if (command == "SET") {
        // 1. 参数数量校验
        if (request.elements.size() != 3) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'set'"
            );
            return result;
        }
        // 2. 调用内存存储，写入 key-value
        const auto setResult = cache_.set(
            request.elements[1].text,
            request.elements[2].text
        );
        //3. 判断是否超过最大内存限制，OOM保护
        if (setResult == cache::CacheService::SetResult::RejectedByMaxMemory) {
            result.response = RespValue::error(
                "OOM command not allowed when used memory > 'maxmemory'"
            );
            return result;
        }   
        // 4. 一切正常，返回 +OK
        result.response = RespValue::simpleString("OK");
        //5.record判断是Aof重写还是客户端请求
        if (record) {
            result.persistenceCommands.push_back(request);
        }
        return result;
    }

    if (command == "GET") {
        if (request.elements.size() != 2) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'get'"
            );
            return result;
        }

        const auto value = cache_.getLocal(request.elements[1].text);
        result.response = value.has_value()
            ? RespValue::bulkString(*value)
            : RespValue::nullBulkString();
        return result;
    }

    if (command == "MGET") {
        if (request.elements.size() < 2) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'mget'"
            );
            return result;
        }

        std::vector<RespValue> values;
        values.reserve(request.elements.size() - 1);
        for (std::size_t index = 1; index < request.elements.size(); ++index) {
            const auto value = cache_.getLocal(request.elements[index].text);
            values.push_back(
                value.has_value()
                    ? RespValue::bulkString(*value)
                    : RespValue::nullBulkString()
            );
        }
        result.response = RespValue::array(std::move(values));
        return result;
    }

    if (command == "INCR" || command == "INCRBY" ||
        command == "DECR" || command == "DECRBY") {
        const bool hasAmount = command == "INCRBY" || command == "DECRBY";
        if ((!hasAmount && request.elements.size() != 2) ||
            (hasAmount && request.elements.size() != 3)) {
            result.response = RespValue::error(
                std::string("ERR wrong number of arguments for '") +
                (command == "INCR" ? "incr" :
                    command == "INCRBY" ? "incrby" :
                    command == "DECR" ? "decr" : "decrby") + "'"
            );
            return result;
        }

        const bool decrement = command == "DECR" || command == "DECRBY";
        std::int64_t amount = 1;
        if (hasAmount) {
            const auto parsedDelta = parseInteger(request.elements[2].text);
            if (!parsedDelta.has_value()) {
                result.response = RespValue::error(
                    "ERR value is not an integer or out of range"
                );
                return result;
            }
            amount = *parsedDelta;
        }

        const auto currentValue = cache_.getLocal(request.elements[1].text);
        std::int64_t current = 0;
        if (currentValue.has_value()) {
            const auto parsedCurrent = parseInteger(*currentValue);
            if (!parsedCurrent.has_value()) {
                result.response = RespValue::error(
                    "ERR value is not an integer or out of range"
                );
                return result;
            }
            current = *parsedCurrent;
        }

        const auto next = decrement
            ? subtractIntegers(current, amount)
            : addIntegers(current, amount);
        if (!next.has_value()) {
            result.response = RespValue::error(
                "ERR increment or decrement would overflow"
            );
            return result;
        }

        // INCR/INCRBY 修改 value 时保留已有 TTL。
        const auto expiration = cache_.expirationMilliseconds(
            request.elements[1].text
        );
        const auto setResult = cache_.set(
            request.elements[1].text,
            std::to_string(*next)
        );
        if (setResult == cache::CacheService::SetResult::RejectedByMaxMemory) {
            result.response = RespValue::error(
                "OOM command not allowed when used memory > 'maxmemory'"
            );
            return result;
        }
        if (expiration.has_value()) {
            static_cast<void>(cache_.expireAtMilliseconds(
                request.elements[1].text,
                *expiration
            ));
        }

        result.response = RespValue::integerValue(*next);
        if (record) {
            result.persistenceCommands.push_back(request);
        }
        return result;
    }

    if (command == "DEL") {
        if (request.elements.size() < 2) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'del'"
            );
            return result;
        }

        std::int64_t removed = 0;
        for (std::size_t index = 1; index < request.elements.size(); ++index) {
            if (cache_.del(request.elements[index].text)) {
                ++removed;
            }
        }
        result.response = RespValue::integerValue(removed);
        if (record) {
            result.persistenceCommands.push_back(request);
        }
        return result;
    }

    if (command == "EXPIRE") {
        if (request.elements.size() != 3) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'expire'"
            );
            return result;
        }

        const auto seconds = parseInteger(request.elements[2].text);
        if (!seconds.has_value()) {
            result.response = RespValue::error("ERR invalid expire time");
            return result;
        }

        const bool updated = cache_.expire(
            request.elements[1].text,
            std::chrono::seconds(*seconds)
        );
        result.response = RespValue::integerValue(updated ? 1 : 0);

        if (record && updated) {
            const auto expiration = cache_.expirationMilliseconds(
                request.elements[1].text
            );
            if (expiration.has_value()) {
                result.persistenceCommands.push_back(makeCommand({
                    RespValue::bulkString("PEXPIREAT"),
                    RespValue::bulkString(request.elements[1].text),
                    RespValue::bulkString(std::to_string(*expiration))
                }));
            } else {
                result.persistenceCommands.push_back(makeCommand({
                    RespValue::bulkString("DEL"),
                    RespValue::bulkString(request.elements[1].text)
                }));
            }
        }
        return result;
    }

    if (command == "TTL") {
        if (request.elements.size() != 2) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'ttl'"
            );
            return result;
        }

        result.response = RespValue::integerValue(
            cache_.ttlSeconds(request.elements[1].text)
        );
        return result;
    }

    if (command == "PEXPIREAT") {
        if (request.elements.size() != 3) {
            result.response = RespValue::error(
                "ERR wrong number of arguments for 'pexpireat'"
            );
            return result;
        }

        const auto timestamp = parseInteger(request.elements[2].text);
        if (!timestamp.has_value()) {
            result.response = RespValue::error(
                "ERR invalid expire timestamp"
            );
            return result;
        }

        const bool updated = cache_.expireAtMilliseconds(
            request.elements[1].text,
            *timestamp
        );
        result.response = RespValue::integerValue(updated ? 1 : 0);
        if (record) {
            result.persistenceCommands.push_back(request);
        }
        return result;
    }

    result.response = RespValue::error("ERR unknown command '" + command + "'");
    return result;
}

}  // namespace cachelite::command
