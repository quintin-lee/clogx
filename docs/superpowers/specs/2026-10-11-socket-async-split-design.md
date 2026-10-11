# socket_async.c 拆分设计（3 TU，零行为变化）

- 状态：设计已通过（§1/§2/§3 逐节 approved），待写实现计划。
- 日期：2026-10-11

## §1 目标 / 范围 / 拆分单元

- 目标：`core/socket_async.c`（823 行）按三簇拆成三个 TU，零行为变化。
- 范围：只动 `core/socket_async.c` + 新增内部头 + 双构建同步。不动公共头（`include/socket_async.h`）、不动测试、不动行为。
- 拆分后四件套（ring 结构体已在公共头，无需搬）：
  - `core/socket_ring.c` —— 环缓冲（原 56–339 行：create/put/get_batch/close/signal/destroy/depth），只依赖公共头；
  - `core/socket_transport.c` —— backoff + socket 建连/发送/TLS（原 341–487 行），全 static 保留；
  - `core/socket_writer.c` —— writer 线程 + 启停（原 492–823 行：cleanup/connect/send/thread/start/ring/stop/dropped）；
  - `core/socket_async_internal.h`（新建）—— 暴露 `struct socket_writer_t` 定义 + transport helper 声明，仅内部 include。
- Writer 与启停同文件：启停函数直接碰 `writer->ring/thread` 字段，另拆只会多一层 getter，得不偿失。

## §2 归属 / 头文件 / 构建同步

- 内部头放 `core/`（沿用 `formatter_internal.h` precedent），只被三个新 TU include。内容：
  - `struct socket_writer_t` 完整定义（从原 42–53 行原样搬）；
  - transport helper 前向声明（backoff_next / backoff_sleep / socket_set_nonblocking / socket_connect_nonblocking / socket_send_nonblocking / tls_send_nonblocking，其中 TLS 项带 `#ifdef CLOG_USE_TLS`）。
  - ring 相关零新增（`socket_ring_slot_t` / `socket_ring_buffer_t` 已在公共头 79–111 行）。
- static 去留：transport helper 去 static（跨单元给 writer 用）；ring 函数本来就是非 static 公开 API，不动；writer 内部函数（cleanup/connect/send/thread）保持 static（同单元内）。
- 注释归属：文件头 `@file` docblock（含 Lock-free Design）跟 ring 走（讲的是环缓冲）；`/* ── Internal writer state ── */` + struct 定义进内部头；`/* ── Ring Buffer Implementation ── */` 进 `socket_ring.c`。新文件分隔沿用文件内既有 `/* ── */` 单行风格（不引入 log.c 的 ═══）。
- 构建同步：Makefile `CORE_SRCS` + CMakeLists 对应列表各加两行（原 `socket_async.c` 一项替换成 ring/transport/writer 三项）。测试 `test_socket_async` 链静态库，零改动。

## §3 小修白名单 / 验证 / 回滚

- 小修白名单（顺手、零行为）：三簇交界处的 stale 注释（如提到行号/位置的）顺手修正；单函数内局部命名仅在明显笔误时改。不做：static 改名、逻辑搬移、ring 公共头任何改动。
- 验证矩阵：`make build/libclogx.a` 零 warning → `test_socket_async`（尤其 writer integration）全 PASS → `make test` 全绿 → `make check`（已知 mermaid-check 因缺 jsdom 模块失败属环境问题，要求 master 同样失败）→ `nm -D` ABI 符号数与基线一致（走形式，无头文件改动）。
- 回滚：单 commit（含构建同步），`git revert` 一条命令回滚。
