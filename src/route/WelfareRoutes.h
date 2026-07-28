#pragma once
class HttpServer; class AuthService; class DbPool;
void registerWelfareRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
