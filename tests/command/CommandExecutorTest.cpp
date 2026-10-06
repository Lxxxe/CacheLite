#include "cachelite/cache/CacheService.h"
#include "cachelite/command/CommandExecutor.h"
#include "cachelite/database/KeyValueRepository.h"
#include "cachelite/protocol/RespValue.h"

#include "../TestSupport.h"

#include <chrono>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cachelite::cache::CacheOptions;
using cachelite::cache::CacheService;
using cachelite::command::CommandExecutionMode;
using cachelite::command::CommandExecutor;
using cachelite::database::KeyValueRepository;
using cachelite::database::LookupResult;
using cachelite::protocol::RespValue;
using cachelite::storage::MemoryStore;

class EmptyRepository final : public KeyValueRepository {
public:
    LookupResult find(std::string_view) override {
        return LookupResult::notFound();
    }
};

RespValue command(std::initializer_list<std::string_view> arguments) {
    std::vector<RespValue> values;
    values.reserve(arguments.size());
    for (const std::string_view argument : arguments) {
        values.push_back(RespValue::bulkString(std::string(argument)));
    }
    return RespValue::array(std::move(values));
}

struct Context {
    EmptyRepository repository;
    MemoryStore memory;
    CacheOptions options;
    CacheService cache;
    CommandExecutor executor;

    Context()
        : options(makeOptions()),
          cache(memory, repository, options),
          executor(cache) {
    }

private:
    static CacheOptions makeOptions() {
        CacheOptions result;
        result.workerCount = 1;
        return result;
    }
};

void executesBasicStringCommands() {
    Context context;

    const auto ping = context.executor.execute(command({"PING"}));
    CACHELITE_CHECK(ping.response.type == RespValue::Type::SimpleString);
    CACHELITE_CHECK(ping.response.text == "PONG");
    CACHELITE_CHECK(ping.persistenceCommands.empty());

    const auto echo = context.executor.execute(command({"ECHO", "hello"}));
    CACHELITE_CHECK(echo.response.type == RespValue::Type::BulkString);
    CACHELITE_CHECK(echo.response.text == "hello");

    const auto set = context.executor.execute(command({"SET", "key", "value"}));
    CACHELITE_CHECK(set.response.type == RespValue::Type::SimpleString);
    CACHELITE_CHECK(set.response.text == "OK");
    CACHELITE_CHECK(set.persistenceCommands.size() == std::size_t{1});

    const auto get = context.executor.execute(command({"GET", "key"}));
    CACHELITE_CHECK(get.response.type == RespValue::Type::BulkString);
    CACHELITE_CHECK(get.response.text == "value");

    const auto exists = context.executor.execute(command({"EXISTS", "key"}));
    CACHELITE_CHECK(exists.response.type == RespValue::Type::Integer);
    CACHELITE_CHECK(exists.response.integer == 1);

    const auto del = context.executor.execute(command({"DEL", "key"}));
    CACHELITE_CHECK(del.response.type == RespValue::Type::Integer);
    CACHELITE_CHECK(del.response.integer == 1);
    CACHELITE_CHECK(context.executor.execute(command({"GET", "key"})).response.type ==
                    RespValue::Type::NullBulkString);
}

void executesMultiKeyCommands() {
    Context context;
    static_cast<void>(context.executor.execute(command({"SET", "a", "1"})));
    static_cast<void>(context.executor.execute(command({"SET", "b", "2"})));

    const auto mget = context.executor.execute(command({"MGET", "a", "missing", "b"}));
    CACHELITE_CHECK(mget.response.type == RespValue::Type::Array);
    CACHELITE_CHECK(mget.response.elements.size() == std::size_t{3});
    CACHELITE_CHECK(mget.response.elements[0].text == "1");
    CACHELITE_CHECK(mget.response.elements[1].type == RespValue::Type::NullBulkString);
    CACHELITE_CHECK(mget.response.elements[2].text == "2");

    const auto exists = context.executor.execute(command({"EXISTS", "a", "a", "missing"}));
    CACHELITE_CHECK(exists.response.integer == 1);

    const auto del = context.executor.execute(command({"DEL", "a", "missing", "b"}));
    CACHELITE_CHECK(del.response.integer == 2);
}

void executesCountersAndExpiration() {
    Context context;
    static_cast<void>(context.executor.execute(command({"SET", "counter", "10"})));

    const auto incr = context.executor.execute(command({"INCR", "counter"}));
    CACHELITE_CHECK(incr.response.integer == 11);
    const auto incrBy = context.executor.execute(command({"INCRBY", "counter", "4"}));
    CACHELITE_CHECK(incrBy.response.integer == 15);
    const auto decr = context.executor.execute(command({"DECR", "counter"}));
    CACHELITE_CHECK(decr.response.integer == 14);
    const auto decrBy = context.executor.execute(command({"DECRBY", "counter", "3"}));
    CACHELITE_CHECK(decrBy.response.integer == 11);

    const auto expire = context.executor.execute(command({"EXPIRE", "counter", "1"}));
    CACHELITE_CHECK(expire.response.integer == 1);
    CACHELITE_CHECK(expire.persistenceCommands.size() == std::size_t{1});
    const auto ttl = context.executor.execute(command({"TTL", "counter"}));
    CACHELITE_CHECK(ttl.response.integer >= 0);
    CACHELITE_CHECK(ttl.response.integer <= 1);
}

void replayChangesMemoryWithoutCreatingAofCommands() {
    Context context;
    const auto replayed = context.executor.execute(
        command({"SET", "replayed", "yes"}),
        CommandExecutionMode::Replay
    );

    CACHELITE_CHECK(replayed.response.text == "OK");
    CACHELITE_CHECK(replayed.persistenceCommands.empty());
    CACHELITE_CHECK(context.cache.getLocal("replayed") ==
                    std::optional<std::string>{"yes"});
}

void rejectsInvalidCommands() {
    Context context;

    const auto empty = context.executor.execute(RespValue::array({}));
    CACHELITE_CHECK(empty.response.type == RespValue::Type::Error);

    const auto wrongArguments = context.executor.execute(command({"SET", "key"}));
    CACHELITE_CHECK(wrongArguments.response.type == RespValue::Type::Error);

    const auto unknown = context.executor.execute(command({"NOPE"}));
    CACHELITE_CHECK(unknown.response.type == RespValue::Type::Error);

    const auto wrongType = context.executor.execute(RespValue::simpleString("PING"));
    CACHELITE_CHECK(wrongType.response.type == RespValue::Type::Error);
}

}  // namespace

int main() {
    return cachelite::test::runSuite(
        "command executor",
        std::vector<std::pair<std::string, void (*)()>>{
            {"executes basic string commands", executesBasicStringCommands},
            {"executes multi-key commands", executesMultiKeyCommands},
            {"executes counters and expiration", executesCountersAndExpiration},
            {"replay changes memory without creating AOF commands", replayChangesMemoryWithoutCreatingAofCommands},
            {"rejects invalid commands", rejectsInvalidCommands},
        }
    );
}

