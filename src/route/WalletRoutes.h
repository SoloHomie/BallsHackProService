#pragma once

class HttpServer;
class AuthService;
class DbPool;

/// 注册钱包与福利路由：余额、福利领取、交易记录
void registerWalletRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
