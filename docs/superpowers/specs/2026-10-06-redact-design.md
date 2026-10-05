# 记录期脱敏 (Record-Time Redaction) 设计

> 方向: B 能力拓展(脱敏/过滤链) · 方案1 记录期脱敏 · 状态:设计已批准,待写 plan
> 日期: 2026-10-06 · 现状基线: ABI 71 符号,45 个测试全绿,VERSION 0.3.0(发版时才 bump)

**Goal:** 在 record 构建期对敏感子串做掩码替换,所有 sink 生效,零规则时热路径开销可忽略。

**Architecture:** 复用现有写路径的三段式关卡(level 过滤→rate limit→record 构建),
在截断修整之后、record 定稿之前插入脱敏 choke 点;printf 路径原地改栈缓冲,
KV 路径 scan-then-copy。

**Tech Stack:** C99, `clog_mutex_t`(见 `clog_port.h`),既有 errno 约定(`CLOG_OK=0`,
`CLOG_ERR_INVALID_ARG=-1`)。

---

## §1 API 与规则模型

三个新公共 API(声明进 `include/log.h`,放 trace 段之后):

```c
int    clog_redact_add(const char *pattern, const char *mask);
void   clog_redact_clear(void);
size_t clog_redact_count(void);
```

- 规则 = 子串 `pattern` + 定长 `mask`(mask 为 NULL/空串时用默认 `"***"`)。
- 无 regex:保持 C99 零依赖;子串匹配语义单一、无回溯风险。
- 规则表进程全局,`clog_mutex_t` 保护,上限 `CLOG_MAX_REDACT_RULES 16`
  (`include/log_limits.h` 新增 `#ifndef` 可覆盖项,对齐 `CLOG_MAX_KV 16` 风格)。
- 错误语义:`pattern` 为 NULL/空串、表满 16 条 → `CLOG_ERR_INVALID_ARG(-1)`;
  成功 → `CLOG_OK(0)`。`clog_redact_clear` 无失败路径(返回 void)。
- 多规则按添加顺序依次应用;后加规则可匹配前一规则写入的 mask 文本
  (文档中明确此顺序语义,不做防递归特殊处理——mask 建议使用不含字母数字的
  `"***"` 风格,自匹配无害)。

## §2 挂载点(已实测锚点)

printf 路径(`core/log.c:logger_writevprintf_internal`):

1. `message[CLOG_MAX_MESSAGE_SIZE]` 栈缓冲格式化(L359),截断修整(L366–374);
2. **脱敏点:紧随 L374 之后、`record.message = message`(L392)之前**,
   对 `message` 原地替换,之后 record 构建(L381–393)与分发(L422–433)不变。

KV 路径(`core/log.c:logger_write_kv_internal`,L490–509):

1. `record.message = msg`(L501)与 `record.kv[i] = kvs[i]`(L505–507)引用的
   都是调用者拥有的指针,不可原地改;
2. **scan-then-copy**:先只读扫描 `msg` 与全部 `CLOG_KV_TYPE_STR` 型 value
   有无命中;无命中零拷贝直通,有命中才拷入栈 scratch、脱敏后重定向
   `record.message` / `kv[i].val.str` 指向 scratch。
3. rate-limit 汇总 record(`supp_rec`,L406–408 / L522–525)由内部字符串生成,
   无敏感数据,不经过脱敏。

## §3 开销与语义

- 零规则时热路径仅多一次规则计数检查,与现有 level 检查(L350)同量级。
- 有规则时每条规则对每块文本一次 `strstr` 扫描;命中后 memmove 搬移,
  保证 NUL 截断安全(复用 L366–374 的截断惯例,超长即截断不断言)。
- 线程安全:规则表读多写少;`add`/`clear` 持 mutex 写,热路径扫描短持锁读
  (对标 `module_mutex` 在 L377–379 的用法,不跨越 dispatch 持锁)。
- 脱敏发生在 async 入队之前:async 队列与所有 sink 看到的都是脱敏后文本,
  内存中不残留原文(除调用者自己的原始缓冲)。

## §4 ABI / 测试 / 文档

- ABI:`clogx.map` + `clogx.exports` 按字母序追加 3 符号(71→74),
  `scripts/check_abi_exports.sh` 期望数同步;新 API -only addition,向后兼容。
- 版本:MINOR bump 留到发版时走 `./scripts/release.sh`(本次不跑,span 同款处理)。
- 新测试 `tests/test_redact.c`(Makefile `TESTS` + CMakeLists `CLOG_TEST_SOURCES`
  双注册),用例:默认 mask、自定义 mask、多规则顺序应用、无规则直通、
  KV STR value 脱敏、非 STR 值不受影响、空 pattern 拒绝、满 16 条拒绝、
  `clear` 后计数归零。
- 文档:`docs/user_manual.md` 新增脱敏小节 + `CHANGELOG.md` Unreleased Added 条目。
- 门禁:`make check` 全绿(含 format/tidy/ABI/全测试);mermaid-check 已知环境问题
  (缺全局 jsdom)不在本 spec 范围内。

## Out of scope(明确不做)

- regex/通配符 pattern(零依赖原则)。
- 按 sink 独立 masking(方案2,将来可在 formatter 层叠加,本设计不预留钩子)。
- 过滤链谓词(丢弃规则 + stats,方案3,独立 spec)。
- YAML `redact:` 配置段(先 API,配置化待用户提出再议)。
