# CacheLite 测试说明

测试需要在 Linux/WSL 中运行。项目使用 CTest 管理测试，不依赖第三方测试框架。

## 普通 Debug 测试

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCACHELITE_BUILD_TESTS=ON \
  -DCACHELITE_ENABLE_MYSQL=OFF

cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

## Sanitizer 测试

```bash
cmake -S . -B build-tests-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCACHELITE_BUILD_TESTS=ON \
  -DCACHELITE_ENABLE_MYSQL=OFF \
  -DCACHELITE_ENABLE_SANITIZERS=ON

cmake --build build-tests-asan -j$(nproc)
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-tests-asan --output-on-failure --timeout 30
```

## 测试目标

| CTest 名称 | 覆盖内容 |
| --- | --- |
| `cachelite.base.build_info` | 项目名称和版本 |
| `cachelite.protocol` | RESP 数组、半包、粘包、错误输入和编码 |
| `cachelite.storage.memory_store` | SET/GET/DEL、TTL、过期、LRU 和最大内存 |
| `cachelite.database` | LookupResult、连接池上限和未启用 MySQL 时的错误路径 |
| `cachelite.command_executor` | 字符串、多 key、计数、过期、Replay 和错误命令 |
| `cachelite.persistence.aof` | AOF 追加、重放和崩溃尾部截断 |
| `cachelite.cache.concurrency` | 64 个调用线程提交约 1 万个同 key 请求、约 6400 个不同 key 并发回源、负缓存、熔断和限流 |
| `cachelite.server.concurrent_clients` | 64 个 TCP 客户端并发，每个连接流水线执行 SET/GET 和 64 次 PING |
| `cachelite.server.benchmark_smoke` | 独立服务进程中预填充 128 个 Key，验证不同 Key GET 和 SET+GET 压测脚本的响应校验 |
| `cachelite.database.mysql_live` | 启用 MySQL 编译时存在；需要显式设置环境变量，否则标记为 Skipped |

## 本轮场景覆盖

| 场景 | 自动化断言 | 数据源 |
| --- | --- | --- |
| 不同 Key GET | 10,000 个不同 Key 的内存读取；128 个 Key 的 RESP 网络压测烟测 | 内存与独立服务进程 |
| MySQL 缓存未命中 | 查询结果回填内存、随后本地命中且后端只查询一次 | 假仓储；真实查询另见下文 |
| 并发回源 | 6,400 个不同 Key 请求，并断言同时活跃的后端调用不超过 8 个 worker | 假仓储 |
| 缓存击穿 | 10,240 个同 Key 请求合并为一次后端查询 | 假仓储 |
| 缓存穿透 | 1,000 次重复查询不存在的 Key 只访问一次后端，负缓存过期后再访问一次 | 假仓储 |
| TTL 大规模过期 | 2,000 个 Key 到期后全部不可读，内存统计归零 | 内存 |
| LRU 淘汰 | 1,000 个 Key 满容量后访问热点 Key，再插入新 Key，确认旧 Key 被淘汰 | 内存 |
| AOF 重启恢复 | 独立进程在 `always` 和 `everysec` 下写入，停止并重启后 GET 恢复 | 独立服务进程 |
| 连接池上限 | 连接数为 2 时第三次 `acquire()` 等待，释放后成功；32 个并发真实查询 | 真实 MySQL，可选 |

普通测试使用独立的 `KeyValueRepository` 假实现，能稳定验证并发控制和回源逻辑，不依赖数据库。这不等同于真实 MySQL 的网络、认证和连接池测试。

## 真实 MySQL 测试（需在你自己的 WSL 终端运行）

先保证 `cachelite.cache_entries` 中有 `demo:key`，并使用启用 MySQL 的构建：

```bash
cmake -S . -B build-mysql \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCACHELITE_ENABLE_MYSQL=ON \
  -DCACHELITE_BUILD_TESTS=ON
cmake --build build-mysql -j"$(nproc)"
```

设置数据库连接信息（密码只在当前终端输入，不写入项目文件）：

```bash
export CACHELITE_MYSQL_HOST=172.17.240.1  # 按当前 WSL 网关地址调整
export CACHELITE_MYSQL_PORT=3306
export CACHELITE_MYSQL_USER=cachelite
export CACHELITE_MYSQL_DATABASE=cachelite
export CACHELITE_TEST_MYSQL_KEY=demo:key
read -rsp 'MySQL password: ' CACHELITE_MYSQL_PASSWORD; echo
export CACHELITE_MYSQL_PASSWORD
export CACHELITE_TEST_MYSQL=1

ctest --test-dir build-mysql -R cachelite.database.mysql_live \
  --output-on-failure
```

未设置 `CACHELITE_TEST_MYSQL=1` 时该项会显示 `Skipped`，不会假装真实数据库测试已通过。

## 本次实测记录（2026-10-07）

- WSL Debug：9/9 测试通过。
- WSL ASan/UBSan：9/9 测试通过。
- 真实 MySQL：用户在已配置数据库凭据的 WSL 终端运行 `ctest --test-dir build-mysql -R cachelite.database.mysql_live --output-on-failure`，`1/1 ... Passed`，测试用时 5.36 秒，总用时 5.41 秒。未设置 `CACHELITE_TEST_MYSQL=1` 的其他环境仍会显示 `Skipped`。
- 独立 Release 服务进程，AOF=`no`，预填充 10,000 个不同 Key 后，100 客户端、Pipeline=16、100,000 次 GET：78,503.78 QPS，错误 0，批次平均 RT 14.153 ms、P99 41.324 ms。

以上 GET 压测在临时目录和动态端口运行，不使用项目日常 AOF；预填充不计入 GET 的 QPS 和 RT。

