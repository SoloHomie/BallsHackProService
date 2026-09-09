# BallsHackProService 编码规范

> 2026-09-09 重构时制定。所有新代码遵守，存量代码按重构路线图逐步收敛。
> 核心原则：**机制代替契约**（能靠编译器拦住的，不靠"大家都记得"）。

## 1. 分层与依赖方向

| 层 | 目录 | 允许做什么 | 禁止做什么 |
|----|------|-----------|-----------|
| 装配 | `main.cpp` | 创建对象、注册路由、启停循环 | 业务逻辑、SQL、碰 Redis 键 |
| 协议 | `route/` | 解析请求 → 鉴权 → 调 service → 组响应 | 写 SQL、直接摸 `MYSQL*`、直接读业务 Redis 键 |
| 业务 | `auth/` `payment/` `sync/` `welfare/` `email/` | 业务规则、编排 | 不组 HTTP 语义（见 §4） |
| 存储 | 各 `*Store` 类 | **全项目 SQL 唯一居所** | 被业务层之外的代码直接调用 |
| 基建 | `core/` | 基础设施，单向依赖 | include 任何业务模块头 |
| 叶子 | `common/` | 纯工具 | 依赖任何项目模块 |

依赖方向只能从上层指向下层。发现反向依赖（如 service include view）必须上报并修。

## 2. 鉴权

- 需要登录的接口一律走统一鉴权机制（`requireAuth`），禁止在 handler 里手写 `authenticate + jsonErr("UNAUTHORIZED")` 样板。
- 特例（heartbeat 区分被踢/被封、admin 验邮箱、订阅风控）在 handler 内做**额外**判断，基础 JWT 校验仍然走机制。

## 3. SQL

- 一律 prepared statement（`MYSQL_STMT`）或经统一 `esc()` 包装；**禁止把任何变量裸拼进 SQL 字符串**。
- 结果集/语句句柄用 RAII 守卫（`StmtGuard` 模式），禁止裸 `mysql_free_result`。
- 事务放在 store 层，禁止在 route handler 里写事务。

## 4. 错误与响应

- 业务失败统一 `HTTP 422 + {"success":false,"errcode":...,"message":...}`。
- `errcode`：机器可读标识（如 `INVALID_CREDENTIALS`），客户端按它分支。
- `message`：面向用户的中文文案，**禁止把平台/系统错误原文透给客户端**（平台错误只进日志）。
- service 层返回结构/枚举，不返回拼好的 JSON 字符串（展示归 route 层）。

## 5. 文档（每个文件必须）

- **文件头**：模块职责一句话 + 关键业务规则 + 排障入口（出问题看哪）。
- **公有接口**：契约注释 = 输入 / 输出 / 错误码 / 副作用。
- **业务关键处**写 WHY 注释（为什么这么判、这么设计），不写复述代码的 HOW 注释。
- 每个业务模块在 `docs/` 有一篇业务文档（职责、数据流、排障清单）。

## 6. 内存与线程

- RAII 贯穿（连接守卫、stmt 守卫）；禁止裸 `new`/`delete`。
- 共享可变状态必须显式加锁，或注释说明"为什么无锁安全"。
- 禁止无限 `detach()` 线程；异步任务必须有上限和退出路径。
- 单例（如 RedisClient）的线程安全语义必须在头文件注明。

## 7. 命名与风格

- 文件名 = 类名（`AuthService.cpp` ↔ `class AuthService`）；路由文件 `XxxRoutes.cpp`。
- include 排序：自身模块 → 项目模块 → 第三方 → 标准库。
- 注释：业务规则/排障用中文，标识符一律英文。
- 禁止调试 `printf` 进生产路径；日志走统一 Logger（含时间戳、模块名、request-id）。

## 8. 构建与清理

- 新增/删除 `.cpp`/`.h` 必须同时更新 `CMakeLists.txt` 和 `BallsHackProService.vcxproj`。
- 删除文件前 grep 全仓确认无引用（死代码判定三查：编译清单、引用、git 状态）。
- 重构任何模块后：编译通过 + 行为等价验证，才算完成一个单元。
