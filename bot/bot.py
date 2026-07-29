"""BallsHackPro Admin Bot — Discord subscription management"""
import os, requests, discord
from discord.ext import commands
from dotenv import load_dotenv

load_dotenv()

TOKEN = os.getenv("DISCORD_BOT_TOKEN")
API_URL = os.getenv("BH_API_URL", "http://127.0.0.1:8920")
ADMIN_EMAIL = os.getenv("ADMIN_EMAIL", "admin@solohomie.top")
ADMIN_PASSWORD = os.getenv("ADMIN_PASSWORD")

if not TOKEN:
    raise RuntimeError("DISCORD_BOT_TOKEN not set in .env")
if not ADMIN_PASSWORD:
    raise RuntimeError("ADMIN_PASSWORD not set in .env")

bot = commands.Bot(command_prefix="!", intents=discord.Intents.default())

# ── Auth cache ──
_auth = {"token": None}

def get_token():
    if _auth["token"]:
        return _auth["token"]
    r = requests.post(f"{API_URL}/api/auth/login",
        json={"email": ADMIN_EMAIL, "password": ADMIN_PASSWORD}, timeout=10)
    if r.status_code == 200:
        _auth["token"] = r.json().get("access_token", "")
    return _auth["token"]

def api(method, path, **kwargs):
    token = get_token()
    if not token:
        return None, "管理员认证失败"
    headers = {"Authorization": f"Bearer {token}"}
    url = f"{API_URL}{path}"
    r = requests.request(method, url, headers=headers, timeout=10, **kwargs)
    try:
        data = r.json()
    except:
        return None, f"HTTP {r.status_code}"
    if r.status_code == 401:
        _auth["token"] = None
        return None, "token过期，请重试"
    return data, None

# ── Commands ──

@bot.command(name="checksub")
async def checksub(ctx, email: str):
    """查询某用户的订阅到期时间"""
    data, err = api("GET", f"/api/admin/subscription?email={email}")
    if err:
        await ctx.reply(f"❌ {err}")
        return
    if not data.get("success"):
        await ctx.reply(f"❌ {data.get('message', '查询失败')}")
        return
    exp = data.get("subscription_expires", "未订阅")
    rem = data.get("remaining_days", 0)
    name = data.get("username", email)
    await ctx.reply(
        f"📧 **{name}**\n"
        f"到期时间: `{exp}`\n"
        f"剩余天数: **{rem}** 天" +
        (" ✅" if rem > 0 else " ❌ 已过期")
    )

@bot.command(name="addsub")
async def addsub(ctx, email: str, days: int):
    """给用户添加订阅天数"""
    if days <= 0:
        await ctx.reply("❌ 天数必须大于0")
        return
    data, err = api("POST", "/api/admin/subscription",
        json={"email": email, "days": days})
    if err:
        await ctx.reply(f"❌ {err}")
        return
    if data.get("success"):
        await ctx.reply(f"✅ {email}: {data['message']}")
    else:
        await ctx.reply(f"❌ {data.get('message', '操作失败')}")

@bot.command(name="remsub")
async def remsub(ctx, email: str, days: int):
    """减少用户的订阅天数"""
    if days <= 0:
        await ctx.reply("❌ 天数必须大于0")
        return
    data, err = api("POST", "/api/admin/subscription",
        json={"email": email, "days": -days})
    if err:
        await ctx.reply(f"❌ {err}")
        return
    if data.get("success"):
        await ctx.reply(f"✅ {email}: {data['message']}")
    else:
        await ctx.reply(f"❌ {data.get('message', '操作失败')}")

@bot.event
async def on_ready():
    print(f"[Bot] {bot.user} online")

if __name__ == "__main__":
    bot.run(TOKEN)
