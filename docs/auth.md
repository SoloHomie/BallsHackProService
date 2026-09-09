# auth 模块文档（认证核心）

> 业务地图：账号体系的所有业务规则、数据流、排障入口。代码在 `src/auth/`。
> 接口清单见 [ROUTES.md](ROUTES.md) 认证组。

## 组件职责

| 组件 | 文件 | 职责 | 排障入口 |
|------|------|------|---------|
| AuthService | AuthService.h/.cpp | 注册/登录/验证码/重置/OAuth 编排 | Redis `code:*` 键（存用户提交的原始邮箱——诊断神器） |
| UserStore | UserStore.h/.cpp | **users 表 SQL 唯一入口**（+设备绑定/个人资料/管理操作） | users 表 |
| CodeManager | CodeManager.h/.cpp | 邮箱验证码：`code:` 键 + `cooldown:` 冷却 + `used:` 防重放 | Redis 三键族 |
| SessionManager | SessionManager.h | 会话/refresh token（Redis 主 + MySQL 备份） | `refresh:` 键 + refresh_tokens 表 |
| TokenService | TokenService.h/.cpp | JWT 签发/校验（access 15min / refresh 30 天） | 签名密钥 BH_JWT_SECRET |
| PasswordHash | PasswordHash.h/.cpp | 密码哈希（PBKDF2: 盐+密码+uid） | users.salt/password_hash |
| OAuthService | OAuthService.h/.cpp | GitHub/Discord 登录（state 防 CSRF） | Redis `oauth:state:` |

## 关键业务规则（改码前必读）

1. **邮箱归一化**：所有入口先 `normalizeEmail`（trim+小写）再查库。粘帖带尾部换行曾导致"账号不存在"误报（2026-09-06 事故）。
2. **验证码一次性**：校验成功即删除；`used:` 键 60 秒防同码重复提交；`rl:code:` 60 秒/次、`rl:ip:` 5 次/小时。
3. **单设备登录**：每账号仅 1 个有效 refresh token；新登录 `bindSession` 顶掉旧设备。
4. **封禁单一数据源** = MySQL `users.status`（解封必生效）；Redis `kicked:` 是即时踢人通道（心跳 5s 内收到）。
5. **订阅判定**：`subscription_expires` 非空且未过期（统一走 `TimeUtils::subActive`）；Redis `sub:` 缓存、MySQL 兜底。
6. **鉴权走机制**：路由层挂 `route::requireAuth`（见 `src/route/RouteGuard.h`），handler 里 `route::currentUser()` 取身份。**特例**：heartbeat 自己处理（需区分被踢/被封/顶号）。
7. **SQL 安全**：UserStore 全部参数化（StmtGuard RAII）；SessionManager 的 user_id 已按数字插入（2026-09-09 修复漏转义）。

## 数据流

```
注册: send-code → CodeManager.generate(Redis) → EmailService 异步发信
        → doRegister → UserStore.create(雪花ID+PBKDF2)
登录: login → findAuthByEmail(一次查询) → verifyPassword
        → TokenService.generateAccess(15min) + bindSession(顶号)
        → SessionManager.storeRefresh(Redis+MySQL双写)
刷新: refresh → getRefresh(Redis miss→MySQL→回写) → 封禁检查 → 换新对
心跳: heartbeat → version.json 版本检查 + sub 缓存 + online: 续期(30s) + kicked 检查
```

## 排障速查

| 症状 | 判断 |
|------|------|
| "账号不存在"但用户说有 | Redis `code:*` 看原始邮箱（是否带空格/换行/大写） |
| 验证码收不到 | `code:` 键存在吗 → 存在=SMTP 问题（看 Email 日志）→ 不存在=生成/限流问题（`rl:` 键） |
| 登录后马上被顶 | refresh_tokens 表该账号 token 数 >1（单设备规则生效中） |
| 被封用户还能用 | 心跳是否 5s 内到（kicked 窗口 5 分钟）；users.status 是否真的 =1 |
| refresh 报 INVALID_TOKEN | Redis 键过期 + MySQL 里 expires_at < NOW()（双失） |
