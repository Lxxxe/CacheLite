# 压测脚本

`resp_benchmark.py` 使用 Python 标准库直接发送 RESP2 请求，不依赖
`redis-benchmark` 或 `memtier_benchmark`，适合当前 CacheLite 已实现的命令集合。

先启动服务器：

```bash
./build-wsl/cachelite_resp_epoll
```

另开终端执行：

```bash
python3 scripts/resp_benchmark.py \
  --command ping \
  --clients 100 \
  --requests 100000 \
  --pipeline 1
```

脚本输出 QPS、错误数、平均 RT、P50、P95、P99 和最大 RT。

测试 GET 前先写入数据：

```bash
python3 scripts/resp_benchmark.py --command set --key bench:key --value bench:value
python3 scripts/resp_benchmark.py --command get --key bench:key --clients 100 --requests 100000
```

测试 SET + GET 混合业务操作：

```bash
python3 scripts/resp_benchmark.py \
  --command set-get \
  --key bench:key \
  --value bench:value \
  --clients 100 \
  --requests 100000 \
  --pipeline 1
```

这里一次 `SET + GET` 作为一个业务操作，输出 `TPS`；同时输出两条
RESP 命令的总 `command QPS`。

`--pipeline 16` 可以测试流水线吞吐，但此时 RT 表示一批命令的往返时间，
不能直接当作单条命令的 RT；对 `set-get` 模式来说，一批包含
`pipeline * 2` 条 RESP 命令。需要精确单个业务操作延迟时使用
`--pipeline 1`。
