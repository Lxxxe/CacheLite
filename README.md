# CacheLite

CacheLite 是一个 C++20 编写的单机内存 KV 服务器：使用 Linux 非阻塞 Socket 与 epoll 处理多客户端连接，支持 RESP2 子集、String KV、TTL、精确 LRU 和 AOF；`GET` 内存未命中时，可通过后台工作线程从 MySQL 查询并回填缓存。

这是用于学习和验证网络、存储、持久化及缓存治理的项目，**不是完整 Redis 实现，也不面向生产环境**。

## 已实现的能力

- **网络与协议**：单 Reactor 线程、非阻塞读写、RESP2 增量解析，处理半包、粘包和流水线请求；MySQL 回源完成后通过 `eventfd` 通知 Reactor。
- **命令与存储**：`PING`、`ECHO`、`GET`、`SET`、`DEL`、`EXISTS`、`EXPIRE`、`TTL`、`MGET`、`INCR`、`INCRBY`、`DECR`、`DECRBY`。内存存储使用哈希表和双向链表维护精确 LRU，默认按 Key 与 Value 字节数限制为 64 MiB；过期 Key 在访问和写入准备空间时清理。
- **持久化**：以 RESP 命令追加 AOF，启动时重放；支持 `always`、`everysec`、`no` 三种刷盘策略，重放时保留绝对过期时间。
- **MySQL 只读回源**：可选启用 MySQL C 客户端，使用连接池、后台工作线程和任务队列；同 Key 请求合并、空值缓存、回源队列限流、TTL 抖动及失败熔断。`SET` 等本地写命令不会同步写入 MySQL。
- **验证**：CTest 单元与进程集成测试、ASan/UBSan、可选真实 MySQL 集成测试，以及独立 RESP 压测脚本。

请求路径概览：

```text
客户端 → epoll Reactor → RESP 解析 → 命令执行 → 内存/AOF → 响应
                               └─ GET 未命中 → 回源任务队列 → MySQL worker
                                                  → eventfd 通知 Reactor → 回填/响应
```

## 构建与运行

网络服务需要 Linux 或 WSL2、支持 C++20 的编译器及 CMake 3.20+。默认构建**不启用 MySQL**；此时本地未命中的 `GET` 返回空值。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCACHELITE_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/cachelite_resp_epoll
```

服务默认监听 `127.0.0.1:6379`。另开终端可用 `nc 127.0.0.1 6379` 发送 RESP 请求；也可通过 `CACHELITE_PORT` 指定端口。AOF 默认写入**当前工作目录**下的 `data/appendonly.aof`，启动时从该文件恢复，因此重启时应保持相同工作目录。

```bash
# 三选一；不设置时默认 everysec
CACHELITE_AOF_POLICY=always ./build/cachelite_resp_epoll
CACHELITE_AOF_POLICY=everysec ./build/cachelite_resp_epoll
CACHELITE_AOF_POLICY=no ./build/cachelite_resp_epoll
```

`always` 每次追加后刷盘，`everysec` 由后台线程约每秒刷盘，`no` 不主动调用 `fsync()`；三者的写入性能与异常断电数据风险不同。详细压测条件见 [AOF 性能记录](docs/performance/aof-benchmark.md)。

需要真实 MySQL 回源时，先安装 MySQL/MariaDB 客户端开发库，用 `-DCACHELITE_ENABLE_MYSQL=ON` 重新配置构建，再建立 [示例表](docs/mysql_schema.sql)。连接由 `CACHELITE_MYSQL_HOST`、`CACHELITE_MYSQL_PORT`、`CACHELITE_MYSQL_USER`、`CACHELITE_MYSQL_PASSWORD`、`CACHELITE_MYSQL_DATABASE` 环境变量配置；不要把密码提交到仓库。完整构建和验证步骤见 [测试说明](tests/README.md)。

`config/cachelite.example.toml` 目前只是规划模板，**服务端尚未读取该文件**；运行配置以代码默认值和上述环境变量为准。

## 测试与性能记录

- 普通 WSL Debug 测试：9/9 通过；ASan/UBSan：9/9 通过。
- 真实 MySQL 集成测试：用户在 WSL 的 `build-mysql` 中运行，`cachelite.database.mysql_live` 显示 **Passed（5.36 秒）**。它覆盖真实查询、连接池满时等待、32 个并发查询以及缓存回填；更多场景与假仓储测试的边界见 [测试说明](tests/README.md)。
- 不同 Key 的内存 `GET`：独立 Release 服务、AOF=`no`、预填充 10,000 个 Key、100 客户端、Pipeline=16、100,000 次请求，测得 **78,503.78 QPS，错误 0**。此时 RT 是一批流水线命令的往返时间，不是单条命令延迟；预填充不计入吞吐统计。

压测方法和其他模式见 [压测脚本说明](scripts/README.md)。这些数字只适用于所列测试环境与配置，不能代表 MySQL 回源性能或生产环境性能。

## 当前边界

- 单机、单 Reactor 线程；MySQL 查询由独立 worker 执行。没有集群、复制或 Redis 完整命令兼容。
- 当前只支持 String 值；没有 `MSET`、Hash/List/Set 等数据结构，也没有 AOF 重写。
- 精确 LRU 的容量估算按 Key/Value 字节数计算，不等于进程实际内存占用；过期清理由访问和写入触发，没有独立的定期采样线程。
- 尚未提供生产级认证、TLS、每连接缓冲区上限和完整监控指标。

## 代码结构

```text
include/cachelite/     公共接口与数据结构
src/net/               Socket、Buffer、epoll 封装
src/protocol/          RESP2 编码与增量解析
src/storage/           内存 KV、TTL、LRU
src/persistence/       AOF 追加、刷盘和重放
src/database/          MySQL 仓储与连接池
src/cache/             异步回源和缓存保护
src/command/           命令执行与 AOF 重放共用逻辑
src/server/            RESP 服务及分阶段 Echo 示例
tests/                 单元、并发和进程集成测试
scripts/               构建与压测脚本
docs/                  架构、数据库表和性能记录
```
