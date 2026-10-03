# Architecture decisions

可视化总览：打开 [index.html](./index.html) 查看当前项目的模块关系、GET 回源链路、线程模型和目录映射。

本目录保存项目的架构决策记录。每个重要决策应说明问题、候选方案、最终选择和取舍，避免只记录结论。

一期已经确定的边界：

- 目标平台为 Linux x86-64，网络层使用非阻塞 TCP 和 epoll。
- 协议实现 RESP2 子集，不承诺完整 Redis 兼容。
- 一期只实现 String 数据类型和常用 Key 命令。
- MySQL 映射命名空间只提供读取回源，不通过 SET 隐式修改数据库。
- 一期不实现集群、主从复制、事务、Lua、Pub/Sub 和 AOF 重写。

建议的决策记录：

```text
ADR-001-reactor-model.md
ADR-002-resp2-scope.md
ADR-003-storage-sharding.md
ADR-004-aof-semantics.md
ADR-005-mysql-read-through.md
```

