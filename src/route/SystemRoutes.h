#pragma once

class HttpServer;
class AuthService;
class DbPool;

/// 注册系统路由：版本更新、公告、健康检查、下载
void registerSystemRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
