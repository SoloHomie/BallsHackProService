#pragma once

class HttpServer;
class AuthService;
class DbPool;

/// 注册支付路由：创建订单、ZPay 回调、订单查询
void registerPaymentRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
