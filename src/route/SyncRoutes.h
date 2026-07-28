#pragma once

class HttpServer;
class AuthService;
class DbPool;
class SyncService;

void registerSyncRoutes(HttpServer &server, AuthService &auth, SyncService &sync);
