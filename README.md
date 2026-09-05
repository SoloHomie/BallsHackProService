# BallsHackProService

BallsHackPro 后端服务 — C++20 跨平台 HTTP 服务器（自研 HTTP/TLS，MySQL + Redis）。

本 README 不止说明本仓库：它覆盖 **BallsHackPro 整个项目** 的架构、各模块职责，以及「出问题去哪找」的排查索引。项目由多轮 AI 辅助开发，本文档的目标是让接手者能快速定位任意功能对应的代码位置。

---

## 一、系统全景

整个产品由三部分组成：**客户端**（独立仓库 `SoloHomie/BallsHackPro`）、**服务端**（本仓库）、**Android 载荷**（随客户端分发）。

```
┌──────────────────┐   HTTPS (443)      ┌────────────────┐
│  客户端            │ ─────────────────▶│ nginx 反代       │
│  BallsHackPro     │                   │ (限流/静态文件)   │
│  Windows Qt6/QML  │                   └───────┬────────┘
└────────┬─────────┘                           │ 127.0.0.1:9443
         │ adb (MuMu 模拟器)                    ▼
         │                            ┌──────────────────────┐
         ▼                            │ SuretyService (本仓库) │
┌──────────────────┐  TCP 27890/27891 │  C++20 HTTP/TLS 服务   │
│ inject 注入器      │ ───────────────▶│         │             │
│ (dlopen 载荷)     │                 └─────────┼─────────────┘
└────────┬─────────┘                           │
         ▼                                     ├──▶ MySQL 8.0 (业务数据)
┌──────────────────┐                           └──▶ Redis 7   (验证码/在线/缓存)
│ libentity.so     │  ← 实体流/锁球/改名 数据桥
│ libview.so       │  ← 视野调整
│ 弹窗三件套         │  ← MXLoader + libmx.s + libNetHTProtect (游戏内弹窗处理)
└──────────────────┘
```

**数据流要点**：

1. **业务链路**：客户端所有 HTTP 请求走 `https://api.solohomie.top` → nginx → 本服务（监听 127.0.0.1:9443）。业务失败统一返回 `HTTP 422 + {"success":false,"errcode","message"}`。
2. **游戏链路**：客户端通过 adb 把加密载荷（`.enc`）推送进模拟器，`inject` 注入器用 `dlopen` 拉起 `libentity.so`，之后客户端与 libentity 之间走 **TCP 27890/27891**（实体流、锁球指令、去皮/排名命令）。
3. **载荷加密**：所有 `.enc` 为 AES-256-GCM 密文，密钥不编译进二进制，由 `/api/payload/key` 在登录+订阅校验后下发（详见 §五）。

---

## 二、本仓库目录地图

```
BallsHackProService/
├── src/
│   ├── main.cpp               # 入口：加载 .env、路由注册、清理循环
│   ├── core/
│   │   ├── server/            # 自研 HTTP/TLS 服务器 (HttpServer/TlsConnection)
│   │   ├── database/          # MySQL 连接池 (DbPool) + 表初始化
│   │   ├── cache/             # Redis 客户端封装 (RedisClient)
│   │   └── config/            # 环境变量读取 (Env) / 服务端常量 (ServerConfig)
│   ├── auth/                  # ★ 认证核心
│   │   ├── AuthService        # 注册/登录/验证码登录/重置密码/OAuth/封禁
│   │   ├── UserStore          # users 表全部读写 (exists/findByEmail/create/封禁)
│   │   ├── CodeManager        # 邮箱验证码 (Redis code:/cooldown:/used: 键)
│   │   ├── SessionManager     # 会话/单设备登录/refresh token
│   │   ├── TokenService       # JWT 生成与校验
│   │   ├── PasswordHash       # 密码哈希 (salt + hash)
│   │   └── OAuthService       # GitHub/Discord 第三方登录
│   ├── route/                 # ★ API 路由（一个文件一组接口，见 §三）
│   ├── payment/               # ikunpay 支付 (VPayService: 下单/回调验签/订单表)
│   ├── sync/                  # 房间/双端远程控制 (控制方-受控方坐标流转)
│   ├── email/                 # SMTP 验证码邮件 (resend)
│   ├── view/                  # OAuth 回调页 / 邮件 HTML 模板
│   ├── data/                  # (预留) 数据文件
│   └── common/                # JSON 解析、Snowflake ID、字符串工具
├── bot/                       # Discord 管理机器人 (python, main.py)
├── tools/                     # gen_beautify.ps1 (美化资源清单生成)
├── html/                      # 静态模板 (OAuth 回调页等)
├── assets/                    # 客户端可下载资源（含美化包, 不进 git）
├── pricing.json               # 套餐定义 (VPayService.plans 读取)
├── guide.json                 # 客户端引导弹窗配置
├── announcement.json          # 系统公告
├── version.json               # ★ 版本检查源 (heartbeat 读取: latest_version/download_url)
├── CMakeLists.txt / Dockerfile / docker-compose.yml
└── BallsHackProService.vcxproj  # Windows VS 本地调试工程 (Linux 生产用 CMake)
```

---

## 三、API 总览

| 分组 | 路径前缀 | 文件 | 职责 |
|------|---------|------|------|
| 认证 | `/api/auth/*` | route/AuthRoutes.cpp | 验证码、注册、登录、重置密码、OAuth、刷新、登出、**心跳/版本检查** |
| 支付 | `/api/payment/*` | route/PaymentRoutes.cpp | 下单、回调、订单查询 |
| 福利 | `/api/welfare/*` | route/WelfareRoutes.cpp | 福利活动列表、领取 |
| 钱包 | `/api/wallet/*` | route/WalletRoutes.cpp | 余额查询 |
| 同步/远程 | `/api/sync/*`、`/api/peer/*` | route/SyncRoutes.cpp / PeerRoutes.cpp | 房间生命周期、双端远程控制坐标流转 |
| 资产 | `/api/assets/*` | route/AssetRoutes.cpp | 美化资源清单、下载 |
| 载荷 | `/api/payload/key` | route/SystemRoutes.cpp | 载荷解密密钥下发（订阅+设备风控门控） |
| 系统 | `/api/announcement` 等 | route/SystemRoutes.cpp | 公告、版本检查（heartbeat 合并）、设备绑定 |
| 兑换码 | `/api/redeem/*` | route/RedeemRoutes.cpp | 兑换码生成/使用 |
| 用户 | `/api/user/*` | route/AuthRoutes.cpp 尾部 | 个人信息、昵称修改 |

认证组完整清单：`send-code` `register` `login` `verify-login` `reset-password` `oauth/github|discord|callback|links|unlink` `refresh` `logout` `heartbeat`。

---

## 四、关键机制（理解逻辑的入口）

### 1. 邮箱验证码
- 生成：`CodeManager::generate` → Redis 键 `code:<email>|<purpose>`（TTL 同验证码有效期）+ `cooldown:` 冷却键 + `rl:code:`/`rl:ip:` 限流键（route 层）。
- 校验成功后一次性删除（防重放），并写 `used:<code>` 60 秒防重复提交。
- **邮箱先归一化**（trim + 小写）再查库——修复过「复制粘贴带尾部换行 → 误报账号不存在」的事故，注意保持。

### 2. 会话与单设备
- 登录签发 access(15min) + refresh(30天)；`bindSession` 使旧设备 token 失效；封禁用户 refresh 时直接作废 token。
- 封禁唯一数据源 = MySQL `users.status`（解封必生效），心跳用 Redis `kicked:` 键做即时踢人。

### 3. 订阅判定与载荷密钥
- `subscription_expires` 为准，Redis `sub:<uid>` 缓存，MySQL 兜底。
- `/api/payload/key`：登录 + 订阅 + 设备指纹（7 天 ≤8 台）三重门控后下发密钥。

### 4. 版本检查与更新
- heartbeat 读 `version.json` 下发 `latest_version/download_url/force_update/silent_update`；发布产物（zip/安装器/完整文件集）由 nginx 静态托管；更新包不应包含 Android 载荷（载荷单独随客户端目录分发）。

### 5. 错误约定
- 业务失败一律 `HTTP 422` + `{"success":false,"errcode":...,"message":...}`；客户端按 `errcode` 区分处理，`message` 直接展示给用户（文案要面向用户写）。

---

## 五、出问题去哪找（排查索引）

| 症状 | 先去哪 | 排查入口 |
|------|--------|---------|
| 注册/登录/重置密码报错 | src/auth/ | AuthService.cpp；**Redis `code:*`/`cooldown:*` 键里存着用户提交的原始邮箱**（粘尾换行/拼错一眼可见）；`users` 表查账号状态 |
| 验证码收不到 | src/email/ + AuthRoutes | SMTP 异步线程；检查 `BH_SMTP_*` 环境变量、resend 平台 |
| 心跳断开/被踢/在线人数不对 | AuthRoutes heartbeat | Redis `online:<uid>`(TTL30s)、`kicked:<uid>`；MySQL `users.status` |
| 支付不到账/回调失败 | src/payment/ | VPayService.cpp（curl 调 ikunpay）；`payment_orders` 表；日志 `[VPay]` |
| 福利领取异常 | WelfareRoutes.cpp | `welfare_events`/`welfare_claims` 表（关闭活动用 UPDATE 别 DELETE） |
| 版本不更新/死循环 | SystemRoutes + version.json | 客户端 UpdateService 比对 manifest；服务端 version.json 内容 |
| 载荷解密失败 | SystemRoutes /api/payload/key | 订阅过期？设备超 8 台？密钥与打包密钥不一致？ |
| 房间/远程控制不同步 | src/sync/ + PeerRoutes | `rooms` 表 + Redis 房间键 |
| 兑换码无效 | RedeemRoutes.cpp | `redeem_codes`/`redeem_records` 表 |
| 客户端连不上 | nginx 层 | 443 → 9443 反代配置、限流(fail2ban/nginx)、CF 回源 |

**服务器端诊断三件套**（以实际部署环境为准）：

```bash
# 服务是否在监听 (nginx 反代目标)
ss -tlnp | grep 9443
# 验证码键 (含用户提交的原始邮箱 — 诊断神器)
redis-cli keys 'code:*' 'cooldown:*'
# 账号查询
mysql -e "SELECT id,email,username,status,subscription_expires FROM users WHERE email='...'"
```

---

## 六、构建与部署

### Linux（生产，Docker）
```bash
docker compose up -d --build        # 服务容器 (bhpro-app) + MySQL + Redis
```
- 编译用 CMake（`-j1`，机器内存小）；nginx 配置在宿主机，反代 443 → 127.0.0.1:9443。
- 发布产物放 docker 卷，由 nginx 静态托管。

### Windows（本地调试，VS2022）
- 打开 `BallsHackProService.sln`，Debug x64。依赖 vcpkg（mysql/hiredis/openssl/curl）。
- **已知 Windows 编译坑（都修过，改码时保持）**：
  - 无 `unistd.h` → 用 `#ifndef _WIN32` 守卫，`usleep` 换 `Sleep`
  - 无 `popen/pclose` → MSVC 用 `_popen/_pclose`（见 VPayService.cpp 顶部映射）
  - `std::max/min` 与 windows.h 宏冲突 → 写 `(std::max)(...)`
  - 新增 .cpp 记得加进 `BallsHackProService.vcxproj`（Linux CMake 是自动收集的，不会漏）

### 环境变量（全部必填，见 .env 模板；**勿提交真实值**）
`BH_JWT_SECRET` `BH_DB_HOST/USER/PASSWORD` `BH_SMTP_*` `BH_OAUTH_GITHUB_*` `BH_OAUTH_DISCORD_*` `BH_PAYLOAD_KEY` `PAY_API_URL/APP_ID/APP_KEY/NOTIFY_URL/RETURN_URL` `PAY_TYPES` `PAY_MAINTENANCE`

---

## 七、数据库表速览

| 表 | 用途 |
|----|------|
| users | 账号（email 唯一、salt/password_hash、subscription_expires、status=封禁、balance） |
| refresh_tokens | 登录态（每账号 1 个有效 token = 单设备） |
| device_bind | 设备指纹绑定（载荷密钥风控：7 天 ≤8 台） |
| oauth_links | 第三方账号绑定 |
| payment_orders | 支付订单（ikunpay 回调更新） |
| welfare_events / welfare_claims | 福利活动 / 领取记录 |
| redeem_codes / redeem_records | 兑换码 / 使用记录 |
| rooms | 远程控制房间（控制方/受控方） |

---

## 八、开发约定

- 服务端源码统一 UTF-8；客户端有 GBK 容错解析（历史问题，别删）。
- 客户端仓库规则：业务逻辑一律 C++ service 类 + 信号，QML 只做展示。
- 载荷密文打包必须与服务器 `BH_PAYLOAD_KEY` 同源；发版前核对三件套/载荷 md5。
- 发版流程、服务器运维手册见团队内部文档（本 README 只作代码索引）。
