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

MySQL 回源测试使用独立的 `KeyValueRepository` 假实现，因此普通测试不依赖实际数据库。真实 MySQL 连接测试需要打开 `CACHELITE_ENABLE_MYSQL=ON`，并配置 MySQL 客户端开发库和连接参数。

