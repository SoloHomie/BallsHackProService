#pragma once

class HttpServer;
class AuthService;
class DbPool;
void registerAssetRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
