#pragma once

#include "cachelite/cache/CacheService.h"
#include "cachelite/protocol/RespValue.h"

#include <vector>

namespace cachelite::command {

enum class CommandExecutionMode {
    Normal,//客户端正常请求，需要生成 AOF 记录
    Replay//程序启动时重放 AOF，只修改内存，不再次写 AOF
};

struct CommandResult {
    protocol::RespValue response;//返回给客户端的 RESP 响应
    std::vector<protocol::RespValue> persistenceCommands;//需要写入 AOF 的命令列表
};

// Executes protocol commands without knowing anything about sockets or epoll.
// In normal mode it also returns the canonical commands that should be logged.
class CommandExecutor {
public:
    //使用 explicit 防止隐式构造
    explicit CommandExecutor(cache::CacheService& cache) noexcept;

    [[nodiscard]] CommandResult execute(
        const protocol::RespValue& request,
        CommandExecutionMode mode = CommandExecutionMode::Normal
    );

private:
    cache::CacheService& cache_;
};

}  // namespace cachelite::command
