# BallsHackProService

BallsHackPro 后端服务 — C++20 构建的跨平台 HTTP 服务器，提供用户认证、支付订阅、房间同步、资产管理等 API。

## 功能

- **用户系统** — 邮箱验证码注册/登录、OAuth 第三方登录（GitHub、Discord）、JWT 鉴权、Token 刷新与会话管理
- **支付订阅** — 易支付集成、套餐管理、订单创建与回调处理
- **房间同步** — 控制方/受控方实时坐标同步（长轮询）、房间生命周期管理
- **资产管理** — 客户端二进制下载、版本检查、公告推送
- **钱包 & 福利** — 余额查询、福利活动领取

## 技术栈

| 组件 | 选型 |
|------|------|
| 语言 | C++20 |
| 构建 | CMake 3.20+ |
| HTTP/TLS | 自研（OpenSSL + 跨平台 socket） |
| 数据库 | MySQL 8.0（连接池） |
| 缓存 | Redis（hiredis + redis++） |
| HTTP 客户端 | libcurl |
| 容器化 | Docker (Ubuntu 24.04) |

## 项目结构

```
├── src/
│   ├── main.cpp              # 入口，路由注册，清理循环
│   ├── core/
│   │   ├── server/           # HTTP/TLS 服务器
│   │   ├── database/         # MySQL 连接池、表迁移
│   │   ├── config/           # 环境变量配置读取
│   │   └── cache/            # Redis 客户端封装
│   ├── auth/                 # 认证（注册/登录/OAuth/JWT/验证码）
│   ├── route/                # API 路由处理器
│   ├── email/                # SMTP 邮件发送
│   ├── payment/              # 易支付对接
│   ├── sync/                 # 房间同步服务
│   ├── view/                 # OAuth 回调 / 邮件 HTML 模板
│   └── common/               # 工具（JSON、日志、Snowflake ID、字符串）
├── html/                     # 静态 HTML 模板
├── assets/                   # 客户端下载文件
├── third_party/redis-plus-plus/  # redis++ 源码
├── CMakeLists.txt
├── Dockerfile
└── docker-compose.yml
```

## API 概览

### 认证 `/api/auth/`

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/auth/send-code` | 发送邮箱验证码 |
| POST | `/api/auth/register` | 注册 |
| POST | `/api/auth/login` | 密码登录 |
| POST | `/api/auth/verify-login` | 验证码登录 |
| POST | `/api/auth/reset-password` | 重置密码 |
| GET | `/api/auth/oauth/github` | 获取 GitHub OAuth URL |
| GET | `/api/auth/oauth/discord` | 获取 Discord OAuth URL |
| GET | `/api/auth/oauth/callback` | OAuth 回调（浏览器重定向） |
| GET | `/api/auth/oauth/links` | 查看已绑定的 OAuth 平台 |
| POST | `/api/auth/oauth/unlink` | 解绑 OAuth 平台 |
| POST | `/api/auth/refresh` | 刷新 Access Token |
| POST | `/api/auth/logout` | 登出 |
| POST | `/api/auth/heartbeat` | 心跳 / 在线状态 / 版本检查 |

### 同步 `/api/sync/`

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/sync/create` | 创建房间（创建者自动成为控制方） |
| POST | `/api/sync/join` | 加入房间 |
| POST | `/api/sync/leave` | 退出房间 |
| GET | `/api/sync/room/status` | 查询房间状态 |
| POST | `/api/sync/push` | 控制方推送坐标 |
| GET | `/api/sync/pull` | 受控方拉取坐标（长轮询） |

### 支付 `/api/payment/`

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/payment/plans` | 套餐列表 |
| POST | `/api/payment/order` | 创建订单 |
| GET | `/api/payment/callback` | 支付回调 |
| GET | `/api/payment/order/status` | 查询订单状态 |

### 其他

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/system/version` | 服务版本信息 |
| POST | `/api/user/update-profile` | 更新用户名 |
| GET | `/api/download` | 下载页 |
| GET | `/api/mxelf` | 下载 mxelf 二进制 |
| GET | `/api/installer` | 下载安装包 |

> 钱包、福利、资产接口详见 `src/route/` 目录下对应路由文件。

## 环境变量

复制 `.env.linux` 为 `.env` 并填入实际值：

| 变量 | 说明 |
|------|------|
| `BH_SMTP_HOST` | SMTP 服务器地址 |
| `BH_SMTP_PORT` | SMTP 端口 |
| `BH_SMTP_USER` | SMTP 用户名 |
| `BH_SMTP_SENDER` | 发件人地址 |
| `BH_SMTP_AUTH_CODE` | SMTP 授权码 |
| `BH_OAUTH_GITHUB_ID` | GitHub OAuth App Client ID |
| `BH_OAUTH_GITHUB_SECRET` | GitHub OAuth App Client Secret |
| `BH_OAUTH_DISCORD_ID` | Discord OAuth App Client ID |
| `BH_OAUTH_DISCORD_SECRET` | Discord OAuth App Client Secret |
| `BH_OAUTH_PROXY` | HTTP 代理（OAuth 请求走代理） |
| `BH_JWT_SECRET` | JWT 签名密钥（64 位 hex） |
| `BH_DB_USER` | MySQL 用户名 |
| `BH_DB_PASSWORD` | MySQL 密码 |
| `BH_TLS_DISABLE` | 设为 `1` 关闭 TLS（开发环境） |
| `PAY_API_URL` | 易支付接口地址 |
| `PAY_APP_ID` | 易支付 App ID |
| `PAY_APP_KEY` | 易支付签名密钥 |
| `PAY_NOTIFY_URL` | 支付回调通知地址 |

数据库连接默认 `127.0.0.1:3306/bhpro`，Redis 默认 `127.0.0.1:6379`。

## 构建

### 依赖

- CMake ≥ 3.20
- GCC ≥ 11 或 MSVC 2022
- OpenSSL (libssl-dev)
- MySQL client (libmysqlclient-dev)
- libcurl (libcurl4-openssl-dev)
- hiredis (libhiredis-dev)
- redis++（自动从 `third_party/` 源码构建）

### Linux

```bash
# 安装依赖
sudo apt install build-essential cmake g++ libssl-dev \
  libmysqlclient-dev libcurl4-openssl-dev libhiredis-dev

# 构建 redis++
cd third_party/redis-plus-plus
mkdir -p build && cd build
cmake .. -DREDIS_PLUS_PLUS_BUILD_TEST=OFF -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) && sudo make install && sudo ldconfig

# 构建项目
cd ../../..
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Windows (MSVC)

```powershell
# 使用 vcpkg 安装依赖
vcpkg install openssl libmysql curl hiredis

cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Docker

```bash
docker compose up -d --build
```

服务默认监听 `0.0.0.0:8920`。

## 数据库

服务启动时自动执行表迁移（`DbMigration::run`），创建以下表：

- `users` — 用户账户
- `oauth_links` — OAuth 绑定记录
- `payment_orders` — 支付订单
- `refresh_tokens` — 刷新令牌
- `rooms` — 同步房间
- `welfare_events` — 福利活动
- `welfare_claims` — 福利领取记录

## 许可

专有软件。未经授权不得分发。
