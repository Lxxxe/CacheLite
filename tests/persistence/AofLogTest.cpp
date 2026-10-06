#include "cachelite/cache/CacheService.h"
#include "cachelite/command/CommandExecutor.h"
#include "cachelite/database/KeyValueRepository.h"
#include "cachelite/persistence/AofLog.h"
#include "cachelite/protocol/RespValue.h"

#include "../TestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cachelite::cache::CacheOptions;
using cachelite::cache::CacheService;
using cachelite::command::CommandExecutor;
using cachelite::database::KeyValueRepository;
using cachelite::database::LookupResult;
using cachelite::persistence::AofLog;
using cachelite::persistence::AofFsyncPolicy;
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

std::filesystem::path testPath() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("cachelite-aof-test-" + std::to_string(now) + ".aof");
}

void appendResult(AofLog& log, const cachelite::command::CommandResult& result) {
    for (const RespValue& persistenceCommand : result.persistenceCommands) {
        log.append(persistenceCommand);
    }
}

void appendsAndReplaysCommands() {
    const std::filesystem::path path = testPath();
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);

    {
        Context source;
        AofLog log(path.string());
        appendResult(
            log,
            source.executor.execute(command({"SET", "persistent", "yes"}))
        );
        appendResult(
            log,
            source.executor.execute(command({"EXPIRE", "persistent", "60"}))
        );
    }

    {
        Context restored;
        AofLog log(path.string());
        log.replay(restored.executor);
        CACHELITE_CHECK(restored.cache.getLocal("persistent") ==
                        std::optional<std::string>{"yes"});
        CACHELITE_CHECK(restored.cache.ttlSeconds("persistent") > 0);
    }

    CACHELITE_CHECK(std::filesystem::file_size(path) > 0);
    std::filesystem::remove(path, cleanupError);
}

void truncatesIncompleteTailAfterCrash() {
    const std::filesystem::path path = testPath();
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
    std::uintmax_t validSize = 0;

    {
        Context source;
        AofLog log(path.string());
        appendResult(
            log,
            source.executor.execute(command({"SET", "safe", "value"}))
        );
        log.flush();
        validSize = std::filesystem::file_size(path);
    }

    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output << "*2\r\n$3\r\nGET\r\n";
    }
    CACHELITE_CHECK(std::filesystem::file_size(path) > validSize);

    {
        Context restored;
        AofLog log(path.string());
        log.replay(restored.executor);
        CACHELITE_CHECK(restored.cache.getLocal("safe") ==
                        std::optional<std::string>{"value"});
    }

    CACHELITE_CHECK(std::filesystem::file_size(path) == validSize);
    std::filesystem::remove(path, cleanupError);
}

void replayModeDoesNotCreateAdditionalCommands() {
    Context context;
    const auto result = context.executor.execute(
        command({"SET", "key", "value"}),
        cachelite::command::CommandExecutionMode::Replay
    );
    CACHELITE_CHECK(result.response.type == RespValue::Type::SimpleString);
    CACHELITE_CHECK(result.persistenceCommands.empty());
}

void supportsAllFsyncPolicies() {
    const std::vector<AofFsyncPolicy> policies{
        AofFsyncPolicy::Always,
        AofFsyncPolicy::EverySecond,
        AofFsyncPolicy::No
    };

    for (const AofFsyncPolicy policy : policies) {
        const std::filesystem::path path =
            testPath().concat("-").concat(AofLog::policyName(policy));
        std::error_code cleanupError;
        std::filesystem::remove(path, cleanupError);

        {
            Context source;
            AofLog log(path.string(), policy);
            appendResult(
                log,
                source.executor.execute(command({
                    "SET", "policy-key", AofLog::policyName(policy)
                }))
            );
            log.flush();
        }

        Context restored;
        AofLog log(path.string(), policy);
        log.replay(restored.executor);
        CACHELITE_CHECK(
            restored.cache.getLocal("policy-key") ==
            std::optional<std::string>{AofLog::policyName(policy)}
        );

        std::filesystem::remove(path, cleanupError);
    }
}

}  // namespace

int main() {
    return cachelite::test::runSuite(
        "AOF log",
        std::vector<std::pair<std::string, void (*)()>>{
            {"appends and replays commands", appendsAndReplaysCommands},
            {"truncates incomplete tail after crash", truncatesIncompleteTailAfterCrash},
            {"replay mode does not create additional commands", replayModeDoesNotCreateAdditionalCommands},
            {"supports all fsync policies", supportsAllFsyncPolicies},
        }
    );
}

