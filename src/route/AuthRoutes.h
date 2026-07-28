#pragma once

class HttpServer;
class AuthService;

class DbPool;
void registerAuthRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
