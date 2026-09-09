#pragma once

class HttpServer;

// 版本检查与更新下载路由 (2026-09-09 拆分自 SystemRoutes):
//   /api/version         版本检查 (semver 比较 + force_update 判定)
//   /api/update/manifest 增量更新清单 (latest_full 遍历 + MD5)
//   /api/update/download 全量 zip 下载
//   /api/dl?file=        更新文件下载 (releases/ 内容寻址)
//   /api/download        安装包下载 (setup-latest.exe)
void registerUpdateRoutes(HttpServer &server);
