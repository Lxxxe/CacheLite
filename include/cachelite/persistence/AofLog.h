#pragma once

#include "cachelite/command/CommandExecutor.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace cachelite::persistence {

enum class AofFsyncPolicy {
    Always,
    EverySecond,
    No
};

// A RESP-formatted append-only log with configurable durability.
class AofLog {
public:
    explicit AofLog(
        std::string path,
        AofFsyncPolicy policy = AofFsyncPolicy::EverySecond
    );

    ~AofLog() noexcept;

    AofLog(const AofLog&) = delete;
    AofLog& operator=(const AofLog&) = delete;

    [[nodiscard]] static AofFsyncPolicy policyFromEnvironment();
    [[nodiscard]] static const char* policyName(AofFsyncPolicy policy) noexcept;

    void append(const protocol::RespValue& command);
    void replay(command::CommandExecutor& executor);
    void flush();

private:
    void reopenForAppend();
    void closeSyncFile() noexcept;
    void flushUnlocked();
    void syncUnlocked();
    void syncLoop();

    std::string path_;
    AofFsyncPolicy policy_;
    std::ofstream output_;
    int syncFd_{-1};
    std::mutex mutex_;
    std::condition_variable stopCondition_;
    bool stopping_{false};
    std::exception_ptr syncError_;
    std::thread syncThread_;
};

}  // namespace cachelite::persistence
