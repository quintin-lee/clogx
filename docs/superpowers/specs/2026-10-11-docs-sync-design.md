# 文档与现状同步设计 (docs-sync)

> 范围：方案 A —— 只修过期事实，不动文档结构，不写新章节。

## §1 目标 / 范围 / 非目标

- 目标：消除因前 5 轮改动（热路径零分配、log.c 分区、journald 零分配、formatter 拆 4 TU、socket 拆 3 TU）及版本推进（0.4.0 已发布）导致的文档事实过期，共 6 处点修。
- 范围：`docs/PLATFORM_SUPPORT.md`、`docs/CONTRIBUTING.md`、`docs/ROADMAP.md`、`README.md` 的纯文本事实行。
- 非目标：历史设计文档 `docs/multi-instance-logger-design.md:208` 的 `src/formatter.c` 为历史原样，保留不改；不写新章节；不译中文手册；不动任何 mermaid 图语法。

## §2 逐条改法

1. `docs/PLATFORM_SUPPORT.md:41`：`core/socket_async.c` 单文件指代改为三文件 —— 非阻塞写线程（`core/socket_writer.c`，环形缓冲 `core/socket_ring.c` + 传输层 `core/socket_transport.c`）是 Winsock-native……（只换主语，其余不动）。
2. `docs/CONTRIBUTING.md:88`：内部头列表追加 `formatter_internal.h`、`socket_async_internal.h`（注明 core/ 内）。
3. `docs/ROADMAP.md` §2-B：删除"journald、"二字（`sinks/journald_sink.c` 已存在），保留 fluent/forward、gRPC。
4. `docs/ROADMAP.md` §1 表格："核心 + sink 约 7,868 行"改"约 9,000 行"（实测 `cat core/*.c sinks/*.c | wc -l` 当前 9,032，取整；沿用该文档 pointer-style，不写快照）。
5. `README.md` Directory Layout sinks 行：改为 `console / file / socket (TLS / async) / syslog / journald / OTLP / custom`（journald 插在 syslog 之后）。
6. `docs/ROADMAP.md` §3/§4：标题"发布就绪 (0.3.0 打磨)"改"发布就绪（0.3.0/0.4.0 已交付）"，"交付 0.3.0"改"交付 0.3.0/0.4.0"，表格已 ✅ 项不动。

## §3 验证与回滚

- 验证：① 改后 grep 六处旧文本零命中（`socket_async.c` 单文件指代、7,868、journald 未来式等）；② `make check` 格式检查通过（只碰 .md，C 门禁不受影响）；③ 本轮不动任何 mermaid 图语法（避开本地 jsdom 缺失坑）。
- 回滚：单 commit，`git revert` 即可。
