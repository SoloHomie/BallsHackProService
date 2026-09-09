#pragma once

class HttpServer;
class AuthService;
class DbPool;

// 管理接口路由 (2026-09-09 拆分自 SystemRoutes):
//   订阅管理: subscription(加减/查询) / set-subscription
//   用户管理: ban / unban / kick-user / online-users
//   福利管理: welfare-grant / welfare-grant-by-amount
//   支付管理: refund
//   兑换码:   redeem/generate / redeem/list / redeem/disable
// 全部挂 route::requireAdmin (登录 + 管理员邮箱)。
// SQL 已全部收敛到各 Store (2026-09-09), 本文件只做参数解析+组JSON。
void registerAdminRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
