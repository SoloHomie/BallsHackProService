#pragma once

class HttpServer;
class AuthService;
class DbPool;

/// 注册支付路由：创建订单、ZPay 回调、订单查询
class VPayService;
VPayService *paymentService();

void registerPaymentRoutes(HttpServer &server, AuthService &auth, DbPool &pool);

/// 对账任务：扫描近 2 小时未支付订单, 逐个向平台查单补账
/// (不依赖客户端轮询 — 回调丢失 + 客户端关闭时兜底, 保证付了钱必发货)
void paymentReconcileSweep();
