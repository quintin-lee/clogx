# formatter.c 拆文件设计（8 分区 → 4 文件）

- 日期：2026-10-11
- 状态：设计已确认（§1/§2/§3 逐节 approved），待写实现计划
- 前置：alloc-zero（socket/KV/journald）、log.c 分区整理均已合入 master（`cdbe479`）

## §1 目标 / 范围 / 拆分单元

**目标**：把 `core/formatter.c`（1129 行）按主题拆成 4 个文件，零行为变化。每个单元职责单一、通过内部头通信。

**4 个单元**（按实测调用关系划分）：

| 文件 | 内容 | 行号依据（2026-10-11 实测） |
|---|---|---|
| `core/formatter.c`（保留） | 公开 API + 调度（`format`/`format_otlp`/`init`/`reset`/`get_format`/`init_for`）+ 共享基础（`append_buf_*` 家族、`format_sec_cached`、`level_to_string`、time/format 全局） | 1057–1129、60–155、306–359 |
| `core/formatter_fmt.c`（新） | fmt 编译解释：`token_match`、`fmt_compile`、`format_impl`（~188 行解释器）、`append_token`、`fmt_op_t`、`g_fmt_cache` | 155–184、756–1025 |
| `core/formatter_json.c`（新） | `format_json_ex` + `append_json_kv`（`append_json_escaped_string`/`json_close_object` 因 otel 共用而留守，见 §2） | 360–522 |
| `core/formatter_otel.c`（新） | `format_otel_json` + `parse_traceparent`/`hex_*` + `trace/span_hex` + `is_zero_id` + `g_hex_lut` | 282–304、400–421、529–755 |

**Non-goals**：不动 `include/log_formatter.h` 公开头；不改任何函数签名与行为；不动测试；`fuzz_formatter` 照常能编过。

**新增连接件**：内部头 `core/formatter_internal.h`（跨单元函数声明，被搬移的 static 去掉 `static`）；新 `.c` 同步加入 Makefile `CORE_SRCS` + CMakeLists（双构建硬要求）。

## §2 符号归属 / 内部头 / 构建同步

**共享符号归属**：
- 留守 `formatter.c` 并去 `static`：`append_buf_*`（6 个）、`format_sec_cached`、`level_to_string`、`json_close_object`（json 与 otel 共用）、`append_json_escaped_string`（json 与 otel 共用）。全局 `g_default_format`/`g_time_format_buf` 留守（公开 API 用）；`g_time_cache` 留守（紧贴 `format_sec_cached`）；`g_fmt_cache` 跟 fmt 走；`g_hex_lut` 跟 otel 走。
- 跟单元走：`append_token`→fmt，`append_json_kv`→json，`is_zero_id`/`trace_id_hex`/`span_id_hex`/`hex_decode`→otel。`parse_traceparent` 若只被 otel 内自用则保持 `static` 不动（搬运时顺手确认，不进内部头）。

**`core/formatter_internal.h`**：只放跨单元声明（约 10 个去 static 函数声明 + 防重入 guard），参照 `core/log_internal.h` 现有风格。`format_impl` 签名只用 `log_record_t`，无需 `fmt_op_t` 前向声明。

**构建同步**（缺一即断）：Makefile 第 116 行 `CORE_SRCS` 加 3 个新文件；CMakeLists 第 106 行附近源文件列表加同样 3 个。`fuzz_formatter` 链接整库，自动覆盖。

## §3 小修 allowlist / 验证 / 回滚

**小修 allowlist**（超出即停）：
1. 旧式 `/* ---- */` 分隔统一为 `═══`（新文件沿用 log.c 风格）；
2. 搬运时错位/过时注释就地修正（如写着旧行号的注释）；
3. 仅限单函数内局部变量命名整理，不动 static 函数名、不动逻辑。

**验证矩阵**：
- `make build/libclogx.a` 零 warning（`-Wconversion` 全开，跨文件后最易爆 `size_t`↔`int`，有 warning 即修）；
- `make test` 全绿（重点 `test_json_formatter` + `make fuzz-build` 能编过）；
- `nm -D build/libclogx.so` 公开符号数与基线一致（内部头函数全 hidden 可见性，天然满足，对照走形式）；
- `make check` format+tidy 干净（mermaid-jsdom 环境问题除外，master 同样失败）。

**回滚**：功能分支单次 FF-merge，`git revert` 搬运 commit 即回（CHANGELOG 另 commit，不影响回滚）。
