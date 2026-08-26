#pragma once
class HttpServer; class AuthService; class DbPool;
void registerRedeemRoutes(HttpServer &server, AuthService &auth, DbPool &pool);
