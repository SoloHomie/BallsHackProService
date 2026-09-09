#pragma once

class HttpServer;
class AuthService;

/// /api/sync/* 路由 (2026-09-10 合并: 存储统一到 PeerStore Redis 键族,
/// 原 SyncService 已删除; 响应格式保持旧 sync 协议, 差异见 docs/sync-peer.md)
void registerSyncRoutes(HttpServer &server, AuthService &auth);
