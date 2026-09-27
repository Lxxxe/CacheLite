# CacheLite

CacheLite 是一个使用 C++20 编写的单机高并发内存 KV 服务器。项目实现 RESP2 子集、String KV、TTL、近似 LRU 和 AOF，并在此基础上增加受控的 MySQL Read Through 回源能力。

项目目标是完整实现并验证网络收发、协议解析、并发存储、持久化和缓存治理之间的工程关系。CacheLite 不用于替代 Redis，也不承诺完整 Redis 兼容。

## 当前里程碑

当前处于 M0 工程初始化阶段：

- [x] 建立独立工程目录
- [x] 配置 C++20 和 CMake
- [x] 建立基础库、服务端和测试目标
- [x] 提供配置模板和编译预设
- [ ] 在 Linux 或 WSL2 完成首次构建
- [ ] 实现第一个 TCP Echo Server
- [ ] 实现 RESP2 解析和 `PING -> PONG`

## 一期范围

- Linux 非阻塞 TCP 和 epoll Reactor
- RESP2 增量解析
- `PING`、`ECHO`、`GET`、`SET`、`DEL`、`EXISTS`、`EXPIRE`、`TTL`、`INCR`、`MGET`、`MSET`
- String 数据类型
- 惰性过期和定期采样
- `noeviction` 和近似 LRU
- AOF 追加、刷盘和启动恢复
- MySQL Key 映射、连接池、SingleFlight、空值缓存、TTL 抖动和回源限流
- 单元测试、集成测试、故障测试和性能测试

## 一期非目标

- Redis Cluster、Sentinel 和主从复制
- 事务、Lua、Pub/Sub、Streams
- Hash、List、Set 和 Sorted Set
- AOF 后台重写
- 完整 Redis 命令兼容
- 生产级认证、TLS 和多租户隔离

## 环境要求

- Linux x86-64 或 Windows WSL2
- GCC 11+ 或 Clang 14+
- CMake 3.20+
- Make、Ninja 或平台默认的 CMake 生成器

网络层将使用 epoll，因此正式开发和测试应在 Linux 或 WSL2 中进行。当前 M0 骨架不依赖 Linux 接口，可用于验证本机 C++ 工具链。

## 构建

Debug 构建并运行测试：

```bash
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
./build/debug/cachelite_server
```

使用 Sanitizer：

```bash
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset asan
```

也可以使用脚本：

```bash
chmod +x scripts/build.sh
./scripts/build.sh debug
```

预期输出：

```text
CacheLite 0.1.0
M0 engineering skeleton is ready.
```

## 目录结构

```text
CacheLite/
├─ cmake/                  # 编译警告和 Sanitizer 配置
├─ config/                 # 配置模板，不保存密码
├─ docs/architecture/      # 架构决策记录
├─ include/cachelite/      # 对外头文件
├─ src/
│  ├─ base/                # 基础设施
│  ├─ net/                 # Reactor 网络层
│  ├─ protocol/            # RESP2 编解码
│  ├─ storage/             # KV、TTL 和淘汰
│  ├─ persistence/         # AOF
│  ├─ backing/             # MySQL 回源
│  └─ server/              # 服务组装和生命周期
├─ tests/                  # 单元及集成测试
├─ benchmark/              # 压测程序和结果
└─ scripts/                # 构建及测试脚本
```

## 开发顺序

```text
M0  工程骨架和工具链
M1  单线程 Reactor 与 Echo Server
M2  RESP2 解析与 PING ECHO
M3  String KV 与基础命令
M4  TTL 分片存储和近似 LRU
M5  AOF 追加刷盘和恢复
M6  MySQL Read Through 与缓存保护
M7  故障测试压测和优化
```

每个里程碑必须具备可运行程序、自动化测试和对应文档。性能数字只能来自可复现的真实测试。

