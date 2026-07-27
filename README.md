# clog

轻量级 C99 日志库：配置驱动、多 Sink 输出、可选异步队列、文件按大小轮转。

## 特性

- 宏 API：`LOG_INFO` / `LOG_DEBUG` / `LOG_WARN` / `LOG_ERROR` / `LOG_FATAL` / `TRACE`
- 多 Sink：控制台（可选 ANSI 着色）、文件（自动建目录 + 轮转）、TCP Socket
- Token 格式化：`%time` `%level` `%msg` `%file` `%line` `%func` 等
- 同步 / 异步可切换；异步路径深拷贝记录，避免栈指针悬空
- 热更新：`log_reload()` 重读配置并重建 Sink / 异步 worker
- 构建：Makefile 与 CMake（含 CTest、`find_package(clog)`）

## 目录结构

```
include/     公共头文件
core/        配置、格式化、分发、队列、异步、轮转
sinks/       console / file / socket
example/     示例程序
tests/       回归测试
cmake/       CMake 包配置模板
```

## 快速开始

```c
#include "log.h"

int main(void) {
    if (log_init("./config.yaml") != 0) {
        return 1;
    }

    LOG_INFO("Server started");
    LOG_WARN("Disk space low: %d%%", 85);
    LOG_ERROR("Failed to connect to database");

    log_flush();
    log_destroy();
    return 0;
}
```

链接时需要 pthread：

```bash
gcc -Iinclude app.c -Lbuild -lclog -lpthread -o app
```

## 构建

### Makefile

```bash
make          # 生成 build/libclog.a 与 build/example
make example
make test     # 编译并运行全部回归测试
make clean
```

### CMake

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

常用选项：

| 选项 | 默认 | 说明 |
|------|------|------|
| `CLOG_BUILD_EXAMPLES` | ON | 构建 example |
| `CLOG_BUILD_TESTS` | ON | 构建并注册 CTest |
| `CLOG_BUILD_SHARED` | OFF | ON 时构建动态库 |

下游项目：

```cmake
find_package(clog REQUIRED)
target_link_libraries(app PRIVATE clog::clog)
```

## 配置

配置文件是简易 `key: value` 文本（非完整 YAML）。`log_init(path)` 传入路径；空路径时默认读取 `./config.yaml`。

示例：

```yaml
level: INFO
async: false
queue_size: 8192
color: true
format: [%time] [%level] %msg
console_enable: true
file_enable: true
file_path: logs/server.log
max_size: 100MB
backups: 10
socket_enable: false
host: 127.0.0.1
port: 5140
```

| 键 | 含义 |
|----|------|
| `level` | 最低输出级别：`TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` |
| `async` | `true` 时启用后台消费线程 |
| `queue_size` | 异步队列容量 |
| `color` | 控制台 ANSI 着色（不影响文件 / socket） |
| `format` | 格式串 |
| `console_enable` | 启用 stdout Sink |
| `file_enable` / `file_path` | 文件 Sink；也可用键 `path` |
| `max_size` | 轮转阈值，支持 `100MB` 或字节数 |
| `backups` | 保留备份个数（`.1` … `.N`） |
| `socket_enable` / `host` / `port` | TCP Socket Sink |

运行时可调：

```c
log_set_level(LOG_LEVEL_DEBUG);
log_get_level();
log_reload();   // 重读 init 时的配置路径
```

## 格式 Token

| Token | 内容 |
|-------|------|
| `%time` | 本地时间 `YYYY-MM-DD HH:MM:SS.uuuuuu` |
| `%level` | 级别名 |
| `%msg` | 消息正文 |
| `%thread` | 线程 ID |
| `%pid` | 进程 ID |
| `%file` / `%line` / `%func` | 源位置 |
| `%module` / `%tag` | 模块与标签（当前写日志入口里 module 固定为 `"main"`） |
| `%newline` | 换行 |

示例：`[%time] [%level] %file:%line %msg`

## 公共 API

```c
int  log_init(const char *yaml_path);  // 0 成功，-1 失败
void log_destroy(void);
void log_flush(void);                  // 异步模式下先排空队列
int  log_reload(void);

LOG_INFO("...");
LOG_DEBUG("...");
LOG_WARN("...");
LOG_ERROR("...");
LOG_FATAL("...");
TRACE("...");
```

更底层接口见 `include/log_config.h`、`log_async.h`、`log_sink.h`、`dispatcher.h`。

## 异步模式

`async: true` 时：

1. 调用线程格式化消息并入队（字符串字段深拷贝）
2. 后台 worker 出队后交给 dispatcher
3. `log_flush()` / `log_destroy()` / `log_reload()` 会正确排空或停掉 worker

队列满时 `put` 会阻塞等待；关闭或 OOM 时回退为同步写出，尽量不丢日志。

## 架构

```
LOG_* ──► log_writevprintf
              ├─ level 过滤
              ├─ 组装 log_record_t
              └─ async? ──► 队列 ──► worker ──► dispatcher
                           └─ sync ─────────────► dispatcher
                                                    ├─ formatter
                                                    └─ console / file / socket
```

## 测试

```bash
make test
# 或
ctest --test-dir build --output-on-failure
```

覆盖异步生命周期、reload 启停 worker、dispatcher 复用、文件轮转、嵌套目录创建、配置热更新等。

## API 文档（Doxygen）

头文件与实现均含 Doxygen 注释。生成 HTML：

```bash
make docs   # 需要安装 doxygen
# 输出：docs/api/html/index.html
```

## 许可证

以仓库内声明为准；若尚未添加许可证文件，使用前请自行补充。
