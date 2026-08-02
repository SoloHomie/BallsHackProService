#pragma once

class HttpServer;
class AuthService;
class DbPool;

void registerPeerRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
