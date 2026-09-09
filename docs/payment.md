# payment 模块文档（支付）

> 业务地图：ikunpay 支付全链路。代码在 `src/payment/`（VPayService）+ `src/route/PaymentRoutes.cpp`（协议层）+ `src/route/AdminRoutes.cpp`（退款）。
> 接口清单见 [ROUTES.md](ROUTES.md) 支付组/管理组。

## 组件职责

| 组件 | 职责 |
|------|------|
| VPayService | 支付业务唯一实现：下单、回调验签、查单补账、退款、对账、套餐读取 |
| PaymentRoutes | 协议层：参数解析 → VPayService → 组 JSON；对账任务 `paymentReconcileSweep()`（main 60s 循环调用） |

## 关键业务规则（改码前必读）

1. **签名**：商户 RSA 私钥（`rsa_private.pem`）签下单请求；回调用平台公钥（`rsa_plat_pub.pem`）验签。签名原文**不要**打印进日志（2026-09-09 已删敏感输出）。
2. **补账幂等**：`applyPayment` 用 `status='pending'` 条件 UPDATE 原子抢占，重复回调/查单不会重复发货（进程内 `s_applyMtx` + DB 条件更新双保险）。
3. **付了钱必发货**：三路兜底——①平台回调 ②客户端查单时主动 `syncOrderStatus` ③main 循环 60s 对账扫描（近 2 小时 pending 单，每单 5 分钟内只查一次，Redis `pay:chk:` 节流）。
4. **退款边界**（AdminRoutes `/api/admin/refund`）：无 paid 订单不动任何数据（防误伤兑换码/福利订阅）；30 分钟时限（`force=true` 走人工流程）；**先平台退款全部成功再标记系统**（失败返回 `PLATFORM_REFUND_FAIL`）。
5. **渠道控制**：`PAY_TYPES` 环境变量驱动客户端支付方式下拉框；douyinpay 服务端硬拦截；`PAY_MAINTENANCE=1` 下单全拦。
6. **HTTP 调用**（2026-09-09 重构）：libcurl 直调 + 证书校验（原 `popen("curl -sk")` 已移除）。若容器缺 CA 根证书报 `SSL certificate problem`，加 `CURLOPT_CAINFO` 指系统 CA 包，**不要**回退跳过校验。

## 数据流

```
下单: /api/payment/order → VPayService::createOrder(uid, productId, ip, payType)
        → 平台 create → jump 页解析 → 跟随重定向拿最终URL → qrcode 接口 → 支付链接
回调: 平台 GET /api/payment/callback → rsaVerifySha256 验签 → 金额/订单号校验
        → applyPayment(条件UPDATE) → 加订阅时长 + SubCache::write
查单: /api/payment/order/status → queryOrder; 未付 → syncOrderStatus 平台查单补账
退款: admin/refund → refundUserOrders: 查paid单 → 平台逐笔 refund → 标记 refunded → 清订阅
```

## 排障速查

| 症状 | 判断 |
|------|------|
| 支付不到账 | `payment_orders` 状态 + 日志 `[VPay]` 前缀（回调/查单/对账全有日志） |
| 回调验签失败 | 平台公钥与平台当前 RSA 是否匹配（换密钥事故多发点） |
| 下单返回 ORDER_FAILED | 日志 `[VPay] create order FAILED: ...` 看平台回包 code |
| 补账重复发货？ | 不可能：`applyPayment` 条件 UPDATE 原子抢占（设计保证） |
| 退款卡住 | 先看平台退款是否成功（`PLATFORM_REFUND_FAIL: <平台原文>` 会透出） |
| 套餐列表不对 | `pricing.json` 内容（5s 缓存，改完最多 5 秒生效） |
