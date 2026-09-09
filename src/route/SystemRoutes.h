#pragma once

class HttpServer;
class AuthService;

/// 系统杂项路由: 载荷密钥下发、公告、引导配置、下载页、ping、健康检查
/// (2026-09-09 拆分: 管理接口→AdminRoutes, 版本/更新→UpdateRoutes)
void registerSystemRoutes(HttpServer &server, AuthService &auth);
