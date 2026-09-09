# sync / peer 模块文档（双端远程控制）

> 业务地图：远程控制房间。**2026-09-10 合并完成**：存储统一到 PeerStore 的 Redis 键族（用户拍板），原 SyncService（MySQL rooms 表 + 内存条件变量）与 RoomCleaner 已删除。

## 统一架构

```
/api/peer/* (PeerRoutes) ──┐
                           ├──▶ PeerStore (Redis 键族) ◀── Redis (TTL 自过期)
/api/sync/* (SyncRoutes) ──┘
```

- **PeerRoutes**：设备 uid 定位（同账号多设备可互连），addr 存 P2P 打洞地址
- **SyncRoutes**：`uid=email` 定位（单账号单房间语义），addr 空串（纯服务器中转），响应格式保持旧 sync 协议

## Redis 键族（PeerStore 集中管理，业务方不得直接拼键）

| 键 | 内容 | TTL |
|----|------|-----|
| `peer:<code>:0/1` | 槽位地址（sync 下空串） | 120s（活跃续期） |
| `peer:<code>:uid0/uid1` | 槽位 uid（sync 下=email） | 同 |
| `peer:<code>:email` | 控制方 email（旧房间清理） | 同 |
| `peer:<code>:name` | 房间名（仅 sync 客户端传入） | 同 |
| `peer:<code>:notify` | 离开通知（一次性消费） | 30s |
| `peer:<code>:lock` | 建码防撞锁 | 5s |
| `peer:user:<email>` | 用户→房间映射 | 120s |
| `relay:<code>:0/1` | 中转队列（上限 100 条） | 300s |
| `relay:<code>:seq` | 全局序号 | 300s |

## 业务规则

1. **槽位定位**：按 uid 匹配 `:uid0`/`:uid1`；同一 uid 禁止自加入。
2. **房间生命周期**：TTL 自过期（120s 无活跃即消失），**无清理循环**（旧 RoomCleaner 已删）。
3. **离开通知一次性**：`peers`/`status` 消费后删除。
4. **push 授权**：数据只能进对端队列（槽位定位保证方向）；对端不在线 → 数据丢弃（瞬态坐标）。
5. **pull 长轮询**：步进自适应——≤2s 用 10ms（低延迟），>2s 用 50ms（控制 Redis 查询量）。
6. **item 格式**：`{seq, op, x, y}` + 协议尾字段（peer: `split`；sync: `ts` + `extra:{delay_ms}`）。

## 合并后的行为差异清单（sync 客户端，上线前必读）

| 项 | 旧（MySQL+内存） | 新（Redis 键族） | 影响 |
|----|------------------|------------------|------|
| 房间过期 | RoomCleaner 60s 扫描：控制器离线→关房；仅受控方离线→清出 | TTL 120s 整房间过期 | 粒度变粗，房间存续时间略长 |
| 重启恢复 | recoverFromDb 从 rooms 表恢复 | 无需恢复（Redis 即状态） | 无（更简单） |
| pull 数据 | 服务端只存**最后一条**命令 | 队列保留最近 100 条（lastSeq 增量补拉） | 客户端应能处理多增量 |
| push 时对端不在 | 成功（存内存，后被覆盖） | 返回成功但数据丢弃 | 客户端无感（响应同为"坐标已同步"） |
| leave 非成员 | NOT_MEMBER 错误 | 幂等返回成功 | 更宽容 |
| join 自动退出旧房间 | 有（UPDATE 旧房） | 无（旧房 TTL 自清） | 旧房残留 ≤120s |
| room/status 的 status 数字 | 0/1/2 精确 | 1人→0、2人→1 近似 | 客户端若按 status 分支需确认 |
| pull 长轮询实现 | 条件变量（零轮询开销） | Redis 轮询（50ms 步进） | 30s 等待≈600 次查询/请求，单机可承受 |

## 排障速查

| 症状 | 判断 |
|------|------|
| 房间不同步 | `redis-cli ttl peer:<code>:0` 看槽位是否过期；`relay:<code>:0/1` 看队列 |
| sync 客户端 join 报错 | errcode 已翻译（SELF_JOIN/ROOM_FULL/ROOM_NOT_FOUND），与旧文案对齐 |
| 房间 120s 自动消失 | 客户端轮询/心跳断了（TTL 未续期），不是 bug |
| peer 与 sync 房码冲突 | 不可能：同一 SETNX lock 机制防撞 |
