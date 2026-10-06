# CacheLite AOF 策略与性能测试结果

## 测试环境与方法

- Linux/WSL2 环境
- CacheLite Release 构建
- 服务端监听 `127.0.0.1:6379`
- Python 标准库压测客户端：`scripts/resp_benchmark.py`
- `clients` 表示同时保持的 TCP 客户端数
- `pipeline` 表示一次发送的业务操作数
- `pipeline=16` 时，RT 表示一批请求的批次往返时间

压测命令示例：

```bash
python3 scripts/resp_benchmark.py \
  --command set \
  --clients 100 \
  --requests 100000 \
  --pipeline 16
```

## 自动化测试

普通 Debug 测试和 ASan/UBSan 测试均为 8/8 通过，覆盖：

- RESP2 数组、半包、粘包和错误协议
- SET、GET、DEL、MGET、MSET、TTL 和计数命令
- 最大内存、LRU 和过期淘汰
- AOF 追加、回放和不完整尾部截断
- MySQL 连接池和错误路径
- 64 个调用线程提交 10,240 个同 Key 请求时只触发一次后端回源
- 6,400 个不同 Key 的并发回源
- 64 个 TCP 客户端的网络集成测试

## PING 基准

`pipeline=1`、100,000 个请求：

| 客户端数 | QPS | 平均 RT | P99 RT |
| ---: | ---: | ---: | ---: |
| 1 | 11,512 | 0.085 ms | 0.175 ms |
| 10 | 6,456 | 1.543 ms | 3.394 ms |
| 100 | 6,379 | 15.275 ms | 34.674 ms |
| 500 | 6,763 | 40.489 ms | 169.246 ms |

`pipeline=16`、100 个客户端、1,000,000 个请求：

```text
QPS：89,779.66
批次平均 RT：17.131 ms
批次 P99：40.239 ms
错误数：0
```

Pipeline 减少了网络往返次数，因此吞吐明显提高；这个数字不能直接等同于单请求 QPS。

## SET、GET 与混合负载

在 100 个客户端、`pipeline=16` 条件下：

| 操作 | 请求数 | QPS/TPS | 批次平均 RT | 批次 P99 |
| --- | ---: | ---: | ---: | ---: |
| GET | 100,000 | 79,348 QPS | 12.704 ms | 37.993 ms |
| 旧版 SET（每条 flush） | 100,000 | 2,317 QPS | 681.066 ms | 801.370 ms |
| SET+GET | 1,000,000 组 | 2,294 TPS / 4,588 命令 QPS | 696.323 ms | 888.020 ms |

SET+GET 中一次业务操作定义为一条 SET 加一条 GET，因此 TPS 是业务操作数，命令 QPS 是两条 RESP 命令的总吞吐量。

## 三种 AOF 策略对比

测试条件：100 个客户端、`pipeline=16`、100,000 条 SET。

| 策略 | QPS | 批次平均 RT | 批次 P99 | 特点 |
| --- | ---: | ---: | ---: | --- |
| `no` | 84,183 | 12.947 ms | 35.448 ms | 不主动刷盘，性能最高，异常终止可能丢失最近数据 |
| `everysec` | 84,888 | 12.064 ms | 33.843 ms | 后台每秒 flush + fsync，性能和可靠性平衡 |
| `always` | 449 | 3504.957 ms | 5897.277 ms | 每条命令 flush + fsync，持久性最强，写性能最低 |

`everysec` 和 `no` 的性能接近，是因为刷盘工作不在 epoll 主线程中执行。`always` 每条 SET 都等待一次 `fsync()`，因此吞吐显著下降。

## 运行方式

通过环境变量选择策略：

```bash
CACHELITE_AOF_POLICY=always ./build-release-wsl/cachelite_resp_epoll
CACHELITE_AOF_POLICY=everysec ./build-release-wsl/cachelite_resp_epoll
CACHELITE_AOF_POLICY=no ./build-release-wsl/cachelite_resp_epoll
```

未设置环境变量时默认使用 `everysec`。服务器启动时会打印当前策略。

## 结论

当前 CacheLite 的热点读取和流水线吞吐已经达到万级 QPS；单 epoll 线程在非流水线场景下大约在 6,000～7,000 QPS 附近饱和。AOF 策略对写性能影响显著，`everysec` 是当前最适合默认使用的策略。

后续优化方向包括 AOF 批量写入或专用写线程、减少 RESP 和字符串临时对象分配，以及将客户端拆分到多个 Reactor/Worker 事件循环中。
