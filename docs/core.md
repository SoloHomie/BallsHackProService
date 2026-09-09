# core 模块文档（基础设施）

> 业务地图：自研 HTTP 服务器、数据库、缓存、配置。代码在 `src/core/`。
> core 是**单向依赖**：不 include 任何业务模块，业务反向依赖 core。

## 组件职责

| 组件 | 文件 | 职责 | 关键机制 |
|------|------|------|---------|
| HttpServer | core/server/ | 自研 HTTP/TLS 服务器（无框架） | 每连接一线程（上限 256）、5s 读超时、路由+守卫分发 |
| TlsConnection | core/server/ | TLS 握手/读写封装 | OpenSSL；握手 5s 超时 |
| DbPool | core/database/ | MySQL 固定大小连接池 | RAII 守卫、借出前 ping、死连重建、3s 超时 |
| DbMigration | core/database/ | 建表/表结构初始化 | 启动时执行 |
| RedisClient | core/cache/ | Redis 单例（redis++ 连接池） | `SUREITY_NO_REDIS` 编译降级模式 |
| SubCache | core/cache/ | 订阅缓存**唯一写入口** | `sub:<uid>` 键，TTL=距过期秒数（≥60s） |
| Env | core/config/ | 环境变量读取 | `.env` 加载 + 默认值 |

## 请求生命周期（排障的总纲）

```
accept (限流: MAX_CONN=256)
  → 设 SO_RCVTIMEO 5s (2026-09-09)
  → TLS handshake (5s 超时)
  → readRequest (读头+按 Content-Length 读体, 上限 10MB)
  → parseRequest → 路由匹配 (method+path 精确)
  → 鉴权守卫 (guard: requireAuth/requireAdmin/requireSubscribed → 422)
  → handler (异常兜底 500 + INTERNAL_ERROR)
  → 状态判定: REDIRECT: 前缀→302; {"success":false 前缀→422 (2026-09-09 前缀判定)
  → sendResponse (reason phrase 全表 + X-Request-Id 头)
  → 访问日志: Logger::info("Http", "req=<id> METHOD path -> status ms")
```

## 关键机制说明

1. **鉴权守卫**：`route()` 第 4 参 `AuthFn`；通过时把用户写入 `route::g_user`（thread_local，每连接一线程 = 每请求上下文）。**前提**：若未来改线程池模型，此机制需同步改造。
2. **并发上限**：`m_activeConns` 原子计数 + RAII 归还（acceptLoop ++，handleClient 任意退出路径 --）。
3. **优雅停机**：响应后 `shutdown(SD_SEND)` + 2s 限时排空对端数据，再 close。
4. **DbPool 借出即 ping**：死连接当场重建（云盘卡顿场景的恢复路径）。

## 排障速查

| 症状 | 判断 |
|------|------|
| 所有请求卡死 | nginx → 9443 通吗；`m_activeConns` 是否打满（日志会显示连接被拒） |
| 随机 500 | `Http` 日志 req= 前缀看异常信息（2026-09-09 起带路径+异常） |
| 422 但业务其实成功 | 响应体是否误以 `{"success":false` 开头（前缀判定规则） |
| 客户端收不到 X-Request-Id | stream 路由（下载）不经过 sendResponse，无此头（已知边界） |
| Redis 挂了服务还活着 | 降级语义：验证码/在线/会话失效，MySQL 兜底路径生效（如 refresh） |
