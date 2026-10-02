#pragma once

#include "cachelite/command/CommandExecutor.h"

#include <fstream>
#include <string>

namespace cachelite::persistence {

// A minimal RESP-formatted append-only log. The current implementation
// flushes after each record so a successful command survives process restart.
class AofLog {
public:
    explicit AofLog(std::string path);

    AofLog(const AofLog&) = delete;
    AofLog& operator=(const AofLog&) = delete;

    void append(const protocol::RespValue& command);
    void replay(command::CommandExecutor& executor);
    void flush();

private:
    void reopenForAppend();

    std::string path_;
    std::ofstream output_;
};

}  // namespace cachelite::persistence
