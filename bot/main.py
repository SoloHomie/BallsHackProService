"""BallsHackPro Admin Bot — Discord subscription management (2026-08-21 完善)
认证: 自签 JWT (HS256, admin@solohomie.top) — DB 无 admin 账号, 密码登录不可行
命令: !checksub !addsub !remsub !setsub !ban !unban !kick !online
安全: 仅 ALLOWED_USER_ID 可用 (其他人在频道里发命令无效)
"""
import os, time, uuid, json, hmac, hashlib, base64
from dotenv import load_dotenv
load_dotenv()

# 代理必须在 import discord 之前设
proxy = os.getenv("HTTPS_PROXY") or os.getenv("HTTP_PROXY")
if proxy:
    os.environ["HTTPS_PROXY"] = proxy
    os.environ["HTTP_PROXY"] = proxy

import requests, discord
from discord.ext import commands
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)   # 自签名证书警告忽略 (2026-08-21)

TOKEN     = os.getenv("DISCORD_BOT_TOKEN")
SECRET    = os.getenv("BH_ADMIN_SECRET")          # 服务器 .env 的 BH_JWT_SECRET
ALLOWED   = {int(x) for x in os.getenv("ALLOWED_USER_ID", "").split(",") if x.strip()}
API_URL   = os.getenv("BH_API_URL", "https://api.solohomie.top:8443")

# ── 受限账号 (2026-08-21): 仅限开天卡 — 每次最多加1天, 每天最多20人, 禁其他命令 ──
LIMITED = {1527674206643556352}
LIMIT_MAX_DAYS = 1
LIMIT_DAILY_USERS = 20
LIMIT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "limit_log.json")

def load_limits():
    try:
        with open(LIMIT_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}

def save_limits(d):
    try:
        with open(LIMIT_FILE, "w", encoding="utf-8") as f:
            json.dump(d, f)
    except Exception:
        pass

def check_limited(ctx, email, days):
    """受限账号检查: 返回 (是否允许, 拒绝原因)"""
    if ctx.author.id not in LIMITED:
        return True, None
    if days > LIMIT_MAX_DAYS:
        return False, f"受限账号每次最多加 {LIMIT_MAX_DAYS} 天"
    today = time.strftime("%Y-%m-%d", time.gmtime())
    d = load_limits()
    day = d.get(today, [])
    if email not in day:
        if len(day) >= LIMIT_DAILY_USERS:
            return False, f"今日已为 {LIMIT_DAILY_USERS} 人开卡, 已达上限"
        day.append(email)
        d[today] = day
        save_limits(d)
    return True, None

def is_limited_user(ctx):
    return ctx.author.id in LIMITED

if not TOKEN:
    raise RuntimeError("DISCORD_BOT_TOKEN not set in .env")
if not SECRET:
    raise RuntimeError("BH_ADMIN_SECRET not set in .env")

intents = discord.Intents.default()
intents.message_content = True   # 前缀命令需要 (2026-08-21)
# aiohttp 不读环境变量代理, 需显式传 proxy (2026-08-21)
bot = commands.Bot(command_prefix="!", intents=intents,
                   proxy=os.getenv("HTTPS_PROXY") or os.getenv("HTTP_PROXY") or None)


# ── 自签 JWT (HS256, admin@solohomie.top, 900s) ──
# 注意: claims 必须无空格 JSON (separators=(',',':')) — 服务端手动解析器
# 不处理 "key": "value" 的空格, 有空格 email 会解析错 → "无管理员权限" (2026-08-21)
def get_token():
    h = base64.urlsafe_b64encode(json.dumps({"alg": "HS256", "typ": "JWT"},
                                            separators=(",", ":")).encode()).rstrip(b"=").decode()
    now = int(time.time())
    p = base64.urlsafe_b64encode(json.dumps({
        "sub": "1", "email": "admin@solohomie.top", "provider": "password",
        "jti": str(uuid.uuid4()), "iat": now, "exp": now + 900},
        separators=(",", ":")).encode()).rstrip(b"=").decode()
    sig = base64.urlsafe_b64encode(
        hmac.new(SECRET.encode(), f"{h}.{p}".encode(), hashlib.sha256).digest()).rstrip(b"=").decode()
    return f"{h}.{p}.{sig}"


def api(method, path, **kwargs):
    try:
        r = requests.request(method, f"{API_URL}{path}",
            headers={"Authorization": f"Bearer {get_token()}"},
            verify=False, timeout=15, **kwargs)
        return r.json(), None
    except Exception as e:
        return None, str(e)


def allowed(ctx):
    return ctx.author.id in ALLOWED


async def reply(ctx, data, err):
    if err:
        await ctx.reply(f"失败: {err}")
    elif data.get("success"):
        await ctx.reply(f"[OK] {data.get('message', 'OK')}")
    else:
        await ctx.reply(f"[X] {data.get('message', '操作失败')}")


# ── Commands ──

@bot.command(name="checksub")
@commands.check(allowed)
async def checksub(ctx, email: str):
    """查询订阅: !checksub 邮箱"""
    data, err = api("GET", f"/api/admin/subscription?email={email}")
    if err:
        await ctx.reply(f"失败: {err}"); return
    if not data.get("success"):
        await ctx.reply(f"[X] {data.get('message', '查询失败')}"); return
    exp = data.get("subscription_expires", "未订阅")
    rem = data.get("remaining_days", 0)
    await ctx.reply(
        f"用户: {data.get('username', email)}\n"
        f"状态: {'封禁' if data.get('status') else '正常'}\n"
        f"到期时间: `{exp}`\n"
        f"剩余天数: {rem} 天" +
        (" 有效" if rem > 0 else " 已过期")
    )


@bot.command(name="addsub")
@commands.check(allowed)
async def addsub(ctx, email: str, days: int):
    """加会员: !addsub 邮箱 30"""
    if days <= 0:
        await ctx.reply("天数必须大于0"); return
    if is_limited_user(ctx):
        ok, msg = check_limited(ctx, email, days)
        if not ok:
            await ctx.reply(f"失败: {msg}"); return
    await reply(ctx, *api("POST", "/api/admin/subscription",
                          json={"email": email, "days": days}))


@bot.command(name="remsub")
@commands.check(allowed)
async def remsub(ctx, email: str, days: int):
    """减会员(退款): !remsub 邮箱 30"""
    if is_limited_user(ctx):
        await ctx.reply("失败: 受限账号仅限开天卡(加1天)"); return
    if days <= 0:
        await ctx.reply("天数必须大于0"); return
    await reply(ctx, *api("POST", "/api/admin/subscription",
                          json={"email": email, "days": -days}))


@bot.command(name="setsub")
@commands.check(allowed)
async def setsub(ctx, email: str, expires: str):
    """设到期: !setsub 邮箱 2026-12-31 23:59:59 | clear"""
    if is_limited_user(ctx):
        await ctx.reply("失败: 受限账号仅限开天卡(加1天)"); return
    await reply(ctx, *api("POST", "/api/admin/set-subscription",
                          json={"email": email, "expires": expires}))


@bot.command(name="ban")
@commands.check(allowed)
async def ban(ctx, email: str):
    """封禁: !ban 邮箱"""
    if is_limited_user(ctx):
        await ctx.reply("失败: 受限账号仅限开天卡(加1天)"); return
    await reply(ctx, *api("POST", "/api/admin/ban", json={"email": email}))


@bot.command(name="unban")
@commands.check(allowed)
async def unban(ctx, email: str):
    """解封: !unban 邮箱"""
    if is_limited_user(ctx):
        await ctx.reply("失败: 受限账号仅限开天卡(加1天)"); return
    await reply(ctx, *api("POST", "/api/admin/unban", json={"email": email}))


@bot.command(name="kick")
@commands.check(allowed)
async def kick(ctx, email: str):
    """踢下线: !kick 邮箱"""
    if is_limited_user(ctx):
        await ctx.reply("失败: 受限账号仅限开天卡(加1天)"); return
    await reply(ctx, *api("POST", "/api/admin/kick-user", json={"email": email}))


@bot.command(name="refund")
@commands.check(allowed)
async def refund(ctx, email: str, force: str = ""):
    """全额退款: 最新付费30分钟内可退(自动调平台API) + 标记订单/取消订阅。force=人工已退款仅标记, 不受时限。!refund 邮箱 [force]"""
    if is_limited_user(ctx):
        await ctx.reply("失败: 受限账号仅限开天卡(加1天)"); return
    payload = {"email": email}
    if force == "force":
        payload["force"] = "true"
    await reply(ctx, *api("POST", "/api/admin/refund", json=payload))


@bot.command(name="online")
@commands.check(allowed)
async def online(ctx):
    """在线用户: !online"""
    data, err = api("GET", "/api/admin/online-users")
    if err:
        await ctx.reply(f"失败: {err}"); return
    if data.get("success"):
        users = data.get("users", [])
        lines = "\n".join(f"- {u.get('email', '')}" for u in users[:20])
        await ctx.reply(f"在线 {data.get('count', len(users))} 人:\n{lines}" if lines
                        else "当前无人在线")
    else:
        await ctx.reply(f"[X] {data.get('message', '查询失败')}")


@bot.event
async def on_command_error(ctx, error):
    if isinstance(error, commands.CheckFailure):
        print(f"[Auth] denied user={ctx.author.id} allowed={ALLOWED}")   # 调试 (2026-08-21)
        await ctx.reply("无权限")
    elif isinstance(error, commands.MissingRequiredArgument):
        await ctx.reply(f"参数不完整: {ctx.command.help}")
    else:
        print(f"[Err] {type(error).__name__}: {error}")   # 所有异常打日志 (2026-08-21)
        await ctx.reply(f"错误: {error}")


@bot.event
async def on_ready():
    print(f"[Bot] {bot.user} online")


if __name__ == "__main__":
    bot.run(TOKEN)
