# 路由层与接口总表（协议层）

> 2026-09-09 统一鉴权重构后生成。本文件 = 全部 HTTP 接口的**业务地图**：每个接口做什么、要不要登录、代码在哪、出问题去哪看。
> 配合 `src/route/RouteGuard.h` 阅读（统一鉴权机制）。

## 1. 统一鉴权机制（RouteGuard）

**旧世界**：每个 handler 手写 `authenticate + jsonErr("UNAUTHORIZED")` 样板（42 处）。
**新世界**：注册路由时挂守卫，`HttpServer` 在 handler 执行前统一拦截：

```cpp
server.route("GET", "/api/x", handler, route::requireAuth(auth));        // 登录即可
server.route("POST", "/api/admin/x", h, route::requireAdmin(auth));      // 管理员
server.route("GET", "/api/assets/elf", h, route::requireSubscribed(auth)); // 登录+订阅
```

- 守卫通过后，当前用户存进线程局部上下文，handler 内用 `route::currentUser()` 取（`.sub`=用户id, `.email`=邮箱）。
- **前提**：HttpServer 每连接一线程，thread_local 即每请求上下文。若未来改线程池模型，此机制需同步改造（注释已标注在 RouteGuard.h）。
- **特例**：`/api/auth/heartbeat` 不挂守卫——它需要区分"被踢/被封/顶号/正常"四种状态，自己处理 `authenticate`/`peekToken`。
- 订阅判定唯一实现 = `route::isSubscribed(auth, subExpires)`（规则：`subscription_expires` 非空且未过期，判定走 `TimeUtils::subActive`）。

## 2. 接口总表

### 认证组 `/api/auth/*`（AuthRoutes.cpp）

| 接口 | 鉴权 | 业务 | 排障入口 |
|------|------|------|---------|
| POST `/api/auth/send-code` | 公开+限流 | 发邮箱验证码。同邮箱 60s/次、同 IP 5 次/小时 | Redis `rl:code:` / `rl:ip:` 键 |
| POST `/api/auth/login` | 公开 | 密码登录。**先查封禁再登录**（防尾部空白绕过） | users.status |
| POST `/api/auth/verify-login` | 公开 | 验证码登录，同上封禁前置 | Redis `code:` 键 |
| POST `/api/auth/register` | 公开 | 注册（带验证码） | users 表 + `code:` |
| POST `/api/auth/reset-password` | 公开 | 重置密码（带验证码） | `code:` 键 |
| GET `/api/auth/oauth/github` / `discord` | 公开 | 拿 OAuth 跳转 URL（state 防 CSRF） | Redis `oauth:state:` |
| GET `/api/auth/oauth/callback` | 公开 | OAuth 回调（浏览器目标），渲染 HTML | `oauth:state:` 键 |
| GET `/api/auth/oauth/links` | requireAuth | 已绑定的第三方平台列表 | oauth_links 表 |
| POST `/api/auth/oauth/unlink` | requireAuth | 解绑某平台 | oauth_links 表 |
| POST `/api/auth/refresh` | refresh_token | 换新 access+refresh。封禁则作废并拒绝 | refresh_tokens 表 |
| POST `/api/auth/logout` | 公开 | 作废 refresh token | refresh_tokens |
| POST `/api/auth/heartbeat` | **特例** | 心跳+版本检查+订阅状态+在线续期(30s)+踢人检查 | Redis `online:`/`kicked:`；version.json |

### 用户组 `/api/user/*`（AuthRoutes.cpp 尾部）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| GET `/api/user/profile` | requireAuth | 个人信息（含余额/订阅到期时间戳） |
| POST `/api/user/update-profile` | requireAuth | 改昵称（2-20 字符，a-z0-9_-，参数化 SQL） |

### 支付组 `/api/payment/*`（PaymentRoutes.cpp）

| 接口 | 鉴权 | 业务 | 排障 |
|------|------|------|------|
| GET `/api/payment/plans` | 公开 | 套餐列表 + 可用支付方式（PAY_TYPES 驱动） | pricing.json |
| POST `/api/payment/order` | requireAuth | 下单。douyinpay 服务端硬拦截；PAY_MAINTENANCE=1 全拦 | VPayService |
| GET `/api/payment/callback` | 公开 | ikunpay 回调（RSA 验签） | payment_orders 表 |
| GET `/api/payment/return` | 公开 | 支付完成跳转页 | — |
| GET `/api/payment/order/status` | requireAuth | 查订单状态；未付则平台主动查单补账 | Redis `pay:order:` |

### 福利组 `/api/welfare/*`（WelfareRoutes.cpp）

| 接口 | 鉴权 | 业务 | 排障 |
|------|------|------|------|
| GET `/api/welfare/available` | requireAuth | 可领取活动列表 | welfare_events |
| POST `/api/welfare/claim` | requireAuth | 领取（加 Pro 天数，写订阅缓存） | welfare_claims；关闭活动用 UPDATE 别 DELETE |
| POST `/api/welfare/redeem` | requireAuth | **兑换码**（事务+防重放+限流 10次/10分） | redeem_codes/redeem_records；`rl:redeem:` |

### 钱包组（WalletRoutes.cpp）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| GET `/api/wallet/balance` | requireAuth | 余额（走 UserStore::balanceOf，不直连 DB） |

### 远程控制 A 套 `/api/sync/*`（SyncRoutes.cpp → SyncService）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| POST `/api/sync/create` | requireAuth | 建房间（创建者=控制方） |
| POST `/api/sync/join` | requireAuth | 加入房间 |
| POST `/api/sync/leave` | requireAuth | 退出 |
| GET `/api/sync/room/status` | **公开** | 查房间状态（业务如此设计） |
| POST `/api/sync/push` | requireAuth | 控制方推坐标（仅控制方可推） |
| GET `/api/sync/pull` | requireAuth | 受控方拉坐标（长轮询，最长 30s） |

### 远程控制 B 套 `/api/peer/*`（PeerRoutes.cpp → Redis 直接）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| POST `/api/peer/create` | requireAuth | 建房间（slot0=控制方，TTL 120s） |
| POST `/api/peer/join` | requireAuth | 加入（slot1，同一设备禁自加入） |
| GET `/api/peer/status` | **公开** | 房间状态（含离开通知） |
| GET `/api/peer/peers` | requireAuth | 取对方地址 + 续期 |
| POST `/api/peer/push` | requireAuth | 推数据进对端 relay 队列（最近 100 条） |
| GET `/api/peer/pull` | requireAuth | 长轮询拉数据（最长 1s） |
| POST `/api/peer/unregister` | requireAuth | 退出/关房（控制器退=关房） |
| POST `/api/peer/heartbeat` | requireAuth | 续期（兼容保留） |

> ⚠ sync 与 peer 是**两套平行实现**（功能重叠），客户端两套都在用。合并方案待拍板（重构路线图阶段 7）。

### 资产组 `/api/assets/*`（AssetRoutes.cpp）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| GET `/api/assets/signatures` | requireAuth | 特征码（非订阅只发全局组） |
| GET `/api/assets/offsets` | requireAuth | 偏移（非订阅只发摇杆XY） |
| GET `/api/assets/elf` | requireSubscribed | 游戏 ELF 载荷（base64） |
| GET `/api/assets/beautify/manifest` | requireAuth | 美化资源清单 |
| GET `/api/assets/beautify/file` | requireSubscribed | 美化文件下载（路径校验+base64） |
| GET `/api/assets/beautify/preview` | 公开 | 美化预览图（仅 png 缩略图） |

### 系统组（SystemRoutes.cpp：payload/key、公告、guide、下载页、ping、health）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| GET `/api/payload/key` | requireSubscribed | 载荷解密密钥下发（+设备风控：7 天 ≤8 台） |
| GET `/api/announcement` | 公开 | 系统公告（announcement.json） |
| GET `/api/guide` | 公开 | 引导弹窗配置（guide.json） |
| GET `/` | 公开 | 下载页 HTML |
| GET `/api/ping` | 公开 | 延迟探测 |
| GET `/api/health` | 公开 | 健康检查（含用户数） |

### 更新组（UpdateRoutes.cpp，2026-09-09 拆分）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| GET `/api/version` | 公开 | 版本检查（semver 比较，force_update 判定） |
| GET `/api/system/version` | 公开 | 旧接口：直接返回 version.json 原文 |
| GET `/api/update/manifest` | 公开 | 增量更新清单（latest_full 遍历+MD5） |
| GET `/api/update/download` | 公开(stream) | 全量 zip 下载 |
| GET `/api/dl?file=` / HEAD | 公开(stream) | releases/ 文件下载 |
| GET `/api/download` | 公开(stream) | 安装包下载（setup-latest.exe，文件名带版本号） |

### 资产组补充（AssetRoutes.cpp，2026-09-09 自 main 迁入）

| 接口 | 鉴权 | 业务 |
|------|------|------|
| GET `/api/mxelf` | 公开 | 游戏 ELF 载荷（BIN 原始返回） |
| GET `/api/installer` | 公开 | 安装包 zip（BIN 原始返回） |

### 管理组 `/api/admin/*`（AdminRoutes.cpp，全部 requireAdmin）

| 接口 | 业务 |
|------|------|
| POST `/api/admin/subscription` | 加减订阅天数（相对） |
| GET `/api/admin/subscription` | 查订阅（含剩余天数） |
| GET `/api/admin/online-users` | 在线用户（Redis `online:*` 扫描） |
| POST `/api/admin/kick-user` | 踢下线（写 `kicked:` 5 分钟窗口） |
| POST `/api/admin/set-subscription` | 设订阅到期时间（绝对值，覆盖式） |
| POST `/api/admin/welfare-grant` | 定向发福利（指定邮箱） |
| POST `/api/admin/welfare-grant-by-amount` | 按累计充值金额批量发福利（幂等） |
| POST `/api/admin/refund` | 退款（30 分钟时限，force=true 走人工；先平台退款再标记） |
| POST `/api/admin/ban` | 封禁（status=1 + 即时踢） |
| POST `/api/admin/unban` | 解封（status=0 + 清踢标记） |
| POST `/api/admin/redeem/generate` | 生成兑换码（批量 ≤500） |
| GET `/api/admin/redeem/list` | 兑换码列表（分页 50/页） |
| POST `/api/admin/redeem/disable` | 停用兑换码（不影响已兑换） |

## 3. 排障速查（按症状）

| 症状 | 判断路径 |
|------|---------|
| "请先登录"但明明登录了 | 先看 Redis `online:` 键在不在 → 不在=token 过期/顶号；在=JWT 验签问题（BH_JWT_SECRET 变了？） |
| 管理员接口 403 | RouteGuard 的 `kAdminEmail` 与账号邮箱不一致 |
| 订阅功能提示"需要Pro订阅" | MySQL `subscription_expires` + Redis `sub:` 缓存是否过期/被清 |
| 验证码收不到 | Redis `code:`/`rl:code:` 键 + SMTP 异步线程日志 |
| 支付不到账 | `payment_orders` 状态 + 60s 对账扫描（`paymentReconcileSweep`）+ 回调日志 |
| 房间不同步 | 先确定客户端走的 sync 还是 peer 套 → 对应 Redis 键/rooms 表 |

## 4. 相关文件

- 机制：`src/route/RouteGuard.h`、`src/core/server/HttpServer.h`（AuthFn 类型）
- 接口实现：`src/route/`（AuthRoutes/SystemRoutes/AdminRoutes/UpdateRoutes/PaymentRoutes/WelfareRoutes/RedeemRoutes/WalletRoutes/SyncRoutes/PeerRoutes/AssetRoutes）
- 存储层：`src/auth/UserStore`、`src/welfare/WelfareStore`、`src/redeem/RedeemStore`、`src/peer/PeerStore`、`src/sync/SyncService+RoomCleaner`、`src/payment/VPayService`
- 模块文档：[auth.md](auth.md)、[payment.md](payment.md)、[sync-peer.md](sync-peer.md)、[core.md](core.md)
