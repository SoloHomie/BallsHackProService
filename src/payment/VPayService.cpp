#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif
#include "VPayService.h"
#include "../core/database/DbPool.h"
#include "../common/Snowflake.h"
#include "../common/StringUtils.h"
#include "../core/cache/RedisClient.h"
#include "../core/config/Env.h"
#include <mysql/mysql.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <ctime>
#include <algorithm>
#include <unistd.h>
#include <mutex>

// ── base64 编码 ──
static std::string b64enc(const unsigned char *d, int n) {
    static const char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve(((n+2)/3)*4);
    for(int i=0;i<n;i+=3){unsigned int v=((unsigned int)d[i])<<16;v|=(i+1<n)?((unsigned int)d[i+1])<<8:0;v|=(i+2<n)?(unsigned int)d[i+2]:0;
        o+=T[(v>>18)&0x3F];o+=T[(v>>12)&0x3F];o+=(i+1<n)?T[(v>>6)&0x3F]:'=';o+=(i+2<n)?T[v&0x3F]:'=';}
    return o;
}

// ── 查询串全量解析 (k=v 逐项 URL 解码, 用于回调验签) ──
static std::vector<std::pair<std::string,std::string>> splitQuery(const std::string &q){
    std::vector<std::pair<std::string,std::string>> out;
    size_t p=0;
    while(p<q.size()){
        size_t e=q.find('&',p); if(e==std::string::npos)e=q.size();
        std::string kv=q.substr(p,e-p);
        size_t eq=kv.find('=');
        if(eq!=std::string::npos) out.push_back({kv.substr(0,eq), StringUtils::urlDecode(kv.substr(eq+1))});
        p=e+1;
    }
    return out;
}

const std::vector<PricePlan>& VPayService::plans(){
    static std::vector<PricePlan> s;
    static time_t lastLoad = 0;
    time_t now = time(nullptr);
    if (now - lastLoad < 5) return s;  // 5s cache
    lastLoad = now;

    s.clear();
    std::string dir = Env::exeDir();
    std::ifstream f(dir + "/pricing.json");
    if (!f) {
        s = {{"1","体验会员",500,1},{"2","周会员",2500,7},{"3","月会员",8800,30},
             {"4","季会员",19800,90},{"5","年会员",28800,365}};
        return s;
    }
    std::ostringstream ss; ss << f.rdbuf();
    std::string raw = ss.str();

    // Simple manual JSON array parser (avoids Qt dependency)
    auto extractStr = [](const std::string &src, const std::string &key) -> std::string {
        auto pos = src.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = src.find(':', pos + key.size() + 2);
        if (pos == std::string::npos) return "";
        pos = src.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        size_t end = src.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return src.substr(pos + 1, end - pos - 1);
    };
    auto extractInt = [](const std::string &src, const std::string &key) -> int64_t {
        auto pos = src.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        pos = src.find(':', pos + key.size() + 2);
        if (pos == std::string::npos) return 0;
        pos++; // skip ':'
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n')) pos++;
        size_t end = pos;
        while (end < src.size() && (isdigit(src[end]) || src[end] == '-')) end++;
        if (end == pos) return 0;
        return std::stoll(src.substr(pos, end - pos));
    };

    // Find each {...} object in the array
    size_t p = raw.find('[');
    if (p == std::string::npos) { s = {{"1","fallback",500,1}}; return s; }
    while (true) {
        size_t objStart = raw.find('{', p);
        if (objStart == std::string::npos) break;
        size_t objEnd = raw.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = raw.substr(objStart, objEnd - objStart + 1);
        s.push_back({extractStr(obj, "id"),
                     extractStr(obj, "name"),
                     (uint64_t)extractInt(obj, "amount"),
                     (uint32_t)extractInt(obj, "days")});
        p = objEnd + 1;
    }
    if (s.empty()) s = {{"1","fallback",500,1}};
    return s;
}

std::string VPayService::readFile(const std::string &p) const {std::ifstream f(p);if(!f)return"";std::ostringstream ss;ss<<f.rdbuf();return ss.str();}

VPayService::VPayService(DbPool &pool,const std::string &u,const std::string &id,
                         const std::string &mk,const std::string &nu,const std::string &ru)
    :m_pool(pool),m_apiUrl(u),m_appId(id),m_md5Key(mk),m_notifyUrl(nu),m_returnUrl(ru)
{while(!m_apiUrl.empty()&&m_apiUrl.back()=='/')m_apiUrl.pop_back();}

// ── 签名串: 参数名 ASCII 升序, 排除 sign/sign_type/空值, 拼 k=v&k=v ──
std::string VPayService::buildSignStr(const std::vector<std::pair<std::string,std::string>> &p) const {
    std::vector<std::pair<std::string,std::string>> s;
    for(auto &x:p) if(x.first!="sign"&&x.first!="sign_type"&&!x.second.empty()) s.push_back(x);
    std::sort(s.begin(),s.end(),[](auto&a,auto&b){return a.first<b.first;});
    std::ostringstream ss; for(size_t i=0;i<s.size();++i){if(i)ss<<"&";ss<<s[i].first<<"="<<s[i].second;}
    return ss.str();
}

// ── RSA-SHA256 签名 (商户私钥 rsa_private.pem) ──
std::string VPayService::rsaSignSha256(const std::string &data) const {
    std::string pem = readFile("rsa_private.pem");
    if (pem.empty()) { printf("[VPay] rsa_private.pem missing\n"); return ""; }
    BIO *b = BIO_new_mem_buf(pem.data(), (int)pem.size());
    EVP_PKEY *pk = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr);
    BIO_free(b);
    if (!pk) { printf("[VPay] merchant private key parse FAILED\n"); return ""; }
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    std::string sig;
    if (EVP_DigestSignInit(c, nullptr, EVP_sha256(), nullptr, pk) == 1) {
        size_t need = 0;
        EVP_DigestSign(c, nullptr, &need, (const unsigned char*)data.data(), data.size());
        sig.resize(need);
        if (EVP_DigestSign(c, (unsigned char*)sig.data(), &need,
                           (const unsigned char*)data.data(), data.size()) != 1) sig.clear();
    }
    EVP_MD_CTX_free(c); EVP_PKEY_free(pk);
    return sig.empty() ? "" : b64enc((const unsigned char*)sig.data(), (int)sig.size());
}

static std::string httpReq(const std::string &url, const std::string &postData, bool isPost){
    std::string r;
    for (int i = 0; i < 3 && r.empty(); ++i) {
        std::string cmd = "curl -sk --max-time 15 --connect-timeout 6 "
            + (isPost ? std::string("-X POST \"") + url + "\" -d \"" + postData + "\"" : std::string("\"") + url + "\"");
        FILE*fp=popen(cmd.c_str(),"r");
        if(fp){char bf[4096];while(fgets(bf,sizeof(bf),fp))r+=bf;pclose(fp);}
        if (!r.empty()) break;
        printf("[VPay] http empty resp (attempt %d), retrying...\n", i + 1);
        usleep(600 * 1000);
    }
    return r;
}
static std::string httpPost(const std::string &url, const std::string &data){return httpReq(url,data,true);}
static std::string httpGet(const std::string &url){return httpReq(url,"",false);}

// ── 短超时 GET/POST (穿透链路用, 失败快速回退 jump 链接) ──
static std::string httpReqT(const std::string &url, const std::string &data, bool isPost){
    std::string r;
    std::string cmd = "curl -sk --max-time 8 --connect-timeout 6 "
        + (isPost ? std::string("-X POST \"") + url + "\" -d \"" + data + "\"" : std::string("\"") + url + "\"");
    FILE*fp=popen(cmd.c_str(),"r");
    if(fp){char bf[4096];while(fgets(bf,sizeof(bf),fp))r+=bf;pclose(fp);}
    return r;
}
static std::string httpPostJsonT(const std::string &url, const std::string &json){
    std::string r;
    // json 用单引号包裹: 双引号会被 shell 剥掉导致非法 JSON (上游报"订单号不能为空")
    std::string cmd = "curl -sk --max-time 8 --connect-timeout 6 -X POST \"" + url + "\" "
        "-H \"Content-Type: application/json; charset=utf-8\" -H \"X-Requested-With: XMLHttpRequest\" "
        "--data-raw '" + json + "'";
    FILE*fp=popen(cmd.c_str(),"r");
    if(fp){char bf[4096];while(fgets(bf,sizeof(bf),fp))r+=bf;pclose(fp);}
    return r;
}

// ── HTML 实体解码 (jump 页表单值可能含 &amp; 等) ──
static std::string htmlDecode(const std::string &in){
    std::string o; o.reserve(in.size());
    for(size_t i=0;i<in.size();){
        if(in[i]=='&'){
            size_t e=in.find(';',i);
            if(e!=std::string::npos&&e-i<=6){
                std::string ent=in.substr(i+1,e-i-1);
                if(ent=="amp")o+='&';else if(ent=="lt")o+='<';else if(ent=="gt")o+='>';
                else if(ent=="quot")o+='"';else if(ent=="apos"||ent=="#39")o+='\'';
                else if(ent=="nbsp")o+=' ';else{o+=in.substr(i,e-i+1);}
                i=e+1;continue;
            }
        }
        o+=in[i++];
    }
    return o;
}

// ── 解析 jump 页自动提交表单 → (action, 隐藏字段) ──
static bool extractForm(const std::string &html, std::string &action,
                        std::vector<std::pair<std::string,std::string>> &fields){
    size_t fs=html.find("<form"); if(fs==std::string::npos)return false;
    size_t fe=html.find("</form>",fs); if(fe==std::string::npos)fe=html.size();
    std::string fm=html.substr(fs,fe-fs);
    action.clear();
    {
        std::string key="action=\"";
        size_t ap=fm.find(key);
        if(ap!=std::string::npos){
            ap+=key.size();
            size_t b=fm.find('"',ap);
            if(b!=std::string::npos)action=htmlDecode(fm.substr(ap,b-ap));
        }
    }
    if(action.empty())return false;
    size_t p=0;
    while((p=fm.find("<input",p))!=std::string::npos){
        size_t ie=fm.find('>',p); if(ie==std::string::npos)break;
        std::string tag=fm.substr(p,ie-p+1);
        auto getAttr=[&](const std::string &an)->std::string{
            std::string key=an+"=\"";
            size_t a=tag.find(key);
            if(a==std::string::npos)return"";
            a+=key.size();
            size_t b=tag.find('"',a);
            return b==std::string::npos?"":htmlDecode(tag.substr(a,b-a));
        };
        std::string ty=getAttr("type"), name=getAttr("name"), val=getAttr("value");
        p=ie+1;
        if(name.empty())continue;
        if(!ty.empty()&&ty!="hidden")continue;   // 只提交隐藏字段
        fields.push_back({name,val});
    }
    return !fields.empty();
}

// ── 取 JSON 子对象 (StringUtils::jsonVal 不解析对象值) ──
static std::string jsonObj(const std::string &json, const std::string &key){
    std::string pat="\""+key+"\":";
    auto pos=json.find(pat); if(pos==std::string::npos)return"";
    pos+=pat.size();
    while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t'))++pos;
    if(pos>=json.size()||json[pos]!='{')return"";
    int depth=0; size_t i=pos;
    for(;i<json.size();++i){
        if(json[i]=='{')depth++;
        else if(json[i]=='}'&&--depth==0)break;
    }
    return i<json.size()?json.substr(pos,i-pos+1):"";
}

// ── 穿透 jump 页拿真实支付码 ──
// jkou 的 jump 链接指向平台中转页, 中转页再自动提交表单到上游收银台,
// 收银台经 /api/order/qrcode 才给出真实支付码 (wxp://、alipays://…)。
// 客户端直接展示真实码 = 一次扫码直达支付页, 消除"扫码后又是二维码"。
// 任一步失败返回空串 → 调用方回退原 jump 链接 (旧行为)。
static std::string resolveRealPayCode(const std::string &jumpUrl){
    // 1. GET jump 页 → 提取自动提交表单
    std::string html=httpReqT(jumpUrl,"",false);
    std::string action; std::vector<std::pair<std::string,std::string>> fields;
    if(!extractForm(html,action,fields)){
        printf("[VPay] jump page form parse FAILED, html(%.160s)\n",html.c_str());
        return"";
    }
    // action 相对路径 → 相对 jumpUrl 解析
    if(action.rfind("http",0)!=0){
        size_t s=jumpUrl.find("://"); if(s==std::string::npos)return"";
        size_t h=jumpUrl.find('/',s+3);
        std::string base=(h==std::string::npos)?jumpUrl:jumpUrl.substr(0,h);
        if(action.empty()||action[0]!='/')action="/"+action;
        action=base+action;
    }
    // 2. POST 表单 → 302 → 收银台 /pay/<upstream_oid>
    std::ostringstream bd; bool first=true;
    for(auto &kv:fields){ if(!first)bd<<"&"; first=false; bd<<kv.first<<"="<<StringUtils::urlEncode(kv.second); }
    std::string finalUrl;
    {
        std::string cmd="curl -sk --max-time 8 --connect-timeout 6 -L -o /dev/null -w \"%{url_effective}\" "
            "-X POST \""+action+"\" -d \""+bd.str()+"\"";
        FILE*fp=popen(cmd.c_str(),"r");
        if(fp){char bf[1024];while(fgets(bf,sizeof(bf),fp))finalUrl+=bf;pclose(fp);}
        while(!finalUrl.empty()&&(finalUrl.back()=='\n'||finalUrl.back()=='\r'))finalUrl.pop_back();
    }
    if(finalUrl.empty()){printf("[VPay] upstream submit empty\n");return"";}
    printf("[VPay] upstream final url: [%s]\n",finalUrl.c_str());
    size_t ps=finalUrl.rfind("/pay/");
    if(ps==std::string::npos){printf("[VPay] upstream final url unexpected: %s\n",finalUrl.c_str());return"";}
    std::string uoid=finalUrl.substr(ps+5);
    size_t qpos=uoid.find_first_of("/?#");
    if(qpos!=std::string::npos)uoid=uoid.substr(0,qpos);
    if(uoid.empty()){printf("[VPay] upstream order id empty\n");return"";}
    // 3. POST /api/order/qrcode → {code:200,data:{qrcode:"wxp://…",scheme,uri,qrcode_data}}
    std::string apiHost=finalUrl.substr(0,finalUrl.find('/',finalUrl.find("://")+3));
    std::string qresp=httpPostJsonT(apiHost+"/api/order/qrcode","{\"order_id\":\""+uoid+"\"}");
    if(StringUtils::jsonVal(qresp,"code")!="200"){
        printf("[VPay] upstream qrcode api failed: %.200s\n",qresp.c_str());
        return"";
    }
    std::string dq=jsonObj(qresp,"data");
    std::string qc=StringUtils::jsonVal(dq,"qrcode");
    if(qc.empty())qc=StringUtils::jsonVal(dq,"scheme");
    if(qc.empty())qc=StringUtils::jsonVal(dq,"uri");
    // qrcode_data 是 base64 图片, 客户端二维码生成器吃不下 → 交给 jump 兜底
    if(qc.rfind("data:image",0)==0)qc.clear();
    if(qc.empty()){printf("[VPay] upstream qrcode empty (data: %.200s)\n",dq.c_str());return"";}
    printf("[VPay] real pay code: %.12s…(%zu chars)\n",qc.c_str(),qc.size());
    return qc;
}

uint32_t VPayService::daysForProduct(const std::string &pid) const {for(auto &p:plans())if(p.productId==pid)return p.days;return 0;}

// ── 下单: /api/pay/create 返回 JSON {code:0,msg,trade_no,pay_type,pay_info,...} ──
// method=jump → pay_type=jump, pay_info=支付跳转链接 (扫码/打开后拉起支付)
std::pair<bool,std::string> VPayService::createOrder(uint64_t uid,const std::string &pid,
                                                     const std::string &clientIp,
                                                     const std::string &payType){
    uint64_t amt=0; std::string nm; bool found=false;
    for(auto &p:plans()) if(p.productId==pid){amt=p.amount;nm=p.name;found=true;break;}
    if(!found||amt==0) return{false,"套餐不存在"};

    uint64_t oid=Snowflake::generate(); std::string ono=std::to_string(oid);
    uint32_t d=daysForProduct(pid);
    std::string money=std::to_string(amt/100)+"."+std::to_string(amt%100/10)+std::to_string(amt%10);

    auto g=m_pool.acquire(5000);if(!g)return{false,"DB"};
    MYSQL* db=g->get();MYSQL_STMT*s=mysql_stmt_init(db);
    const char*sql="INSERT INTO payment_orders(id,user_id,amount,duration_days,status)VALUES(?,?,?,?,'pending')";
    mysql_stmt_prepare(s,sql,(unsigned long)strlen(sql));MYSQL_BIND b[4]={};
    b[0].buffer_type=MYSQL_TYPE_LONGLONG;b[0].buffer=&oid;b[1].buffer_type=MYSQL_TYPE_LONGLONG;b[1].buffer=&uid;
    b[2].buffer_type=MYSQL_TYPE_LONGLONG;b[2].buffer=&amt;b[3].buffer_type=MYSQL_TYPE_LONG;b[3].buffer=&d;
    mysql_stmt_bind_param(s,b);int e=mysql_stmt_execute(s);mysql_stmt_close(s);if(e)return{false,"订单创建失败"};
    try{RedisClient::instance().setex("pay:order:"+ono,1800LL,"ok");}catch(...){}

    // ── V2 RSA 下单 ──
    std::string t=payType.empty()?"alipay":payType;
    std::string ip=clientIp.empty()?"127.0.0.1":clientIp;
    std::string ts=std::to_string((long long)time(nullptr));
    std::vector<std::pair<std::string,std::string>> pr={
        {"pid",m_appId},{"method","jump"},{"type",t},{"out_trade_no",ono},
        {"notify_url",m_notifyUrl},{"return_url",m_returnUrl},
        {"name",nm},{"money",money},{"clientip",ip},{"timestamp",ts}};
    std::string ss=buildSignStr(pr);
    std::string sign=rsaSignSha256(ss);
    if(sign.empty())return{false,"签名密钥缺失"};
    printf("[VPay] sign_src: %s\n[VPay] sign(RSA): %s\n",ss.c_str(),sign.c_str());
    std::ostringstream bd; bool first=true;
    for(auto &kv:pr){ if(!first)bd<<"&"; first=false; bd<<kv.first<<"="<<StringUtils::urlEncode(kv.second); }
    bd<<"&sign="<<StringUtils::urlEncode(sign)<<"&sign_type=RSA";
    std::string resp=httpPost(m_apiUrl+"/api/pay/create",bd.str());
    printf("[VPay] resp: %s\n",resp.c_str());

    std::string code=StringUtils::jsonVal(resp,"code");
    if(code!="0"){
        std::string msg=StringUtils::jsonVal(resp,"msg");
        if(msg.empty()) msg="平台下单失败";
        printf("[VPay] create order FAILED: %s\n",msg.c_str());
        return{false,msg};
    }
    std::string tn=StringUtils::jsonVal(resp,"trade_no");
    std::string payurl=StringUtils::jsonVal(resp,"pay_info");
    std::string ptype=StringUtils::jsonVal(resp,"pay_type");
    std::string kind=(ptype=="qrcode")?"qr":"url";
    for(size_t p=0;(p=payurl.find("\\/",p))!=std::string::npos;p+=1)payurl.replace(p,2,"/");
    if(payurl.empty()){
        std::string msg="平台未返回支付链接";
        printf("[VPay] create order FAILED: %s\n",msg);
        return{false,msg};
    }

    // jump 链接 → 服务端穿透拿真实支付码 (任一步失败自动回退 jump 链接)
    if(ptype=="jump"&&payurl.rfind("http",0)==0){
        std::string real=resolveRealPayCode(payurl);
        if(!real.empty()){payurl=real;kind="qr";}
    }

    // 回填平台订单号 (便于退款/对账)
    if(!tn.empty()){
        char etn[256]; mysql_real_escape_string(db, etn, tn.c_str(), tn.size());
        char qb[512]; snprintf(qb,sizeof(qb),"UPDATE payment_orders SET zp_trade_no='%s' WHERE id=%llu",etn,oid);
        mysql_query(db,qb);
    }
    auto jesc=[&](const std::string &s){std::string o;for(char c:s){if(c=='"'||c=='\\')o+='\\';o+=c;}return o;};
    std::ostringstream r;r<<"{\"pay_url\":\""<<jesc(payurl)<<"\",\"trade_no\":\""<<tn
        <<"\",\"order_id\":\""<<ono<<"\",\"type\":\""<<kind<<"\"}";
    return{true,r.str()};
}

// ── 幂等补账: 订单 paid 后延长订阅 + 加余额 ──
// 并发安全: 进程内互斥 + status='pending' 原子抢占, 回调/查单/对账并发到达也只发货一次
static std::mutex s_applyMtx;
bool VPayService::applyPayment(uint64_t oid, const std::string &tradeNo){
    std::lock_guard<std::mutex> lk(s_applyMtx);
    auto g=m_pool.acquire(5000);if(!g)return false;MYSQL*db=g->get();
    uint64_t du=0,amt=0;uint32_t dd=0;char ds[32]={};bool fd=false;char qb[512];
    snprintf(qb,sizeof(qb),"SELECT user_id,duration_days,status,amount FROM payment_orders WHERE id=%llu",oid);
    mysql_query(db,qb);MYSQL_RES*res=mysql_store_result(db);
    if(res&&mysql_num_rows(res)>0){MYSQL_ROW rw=mysql_fetch_row(res);du=std::stoull(rw[0]?rw[0]:"0");dd=(uint32_t)std::stoul(rw[1]?rw[1]:"0");if(rw[2]){std::strncpy(ds,rw[2],31);ds[31]='\0';}amt=std::stoull(rw[3]?rw[3]:"0");fd=true;}
    if(res)mysql_free_result(res);
    if(!fd)return false;
    if(std::string(ds)=="paid")return true;   // 幂等
    char etn[256]; mysql_real_escape_string(db, etn, tradeNo.c_str(), tradeNo.size());
    // 先抢占 pending→paid (原子), 抢占失败说明已被其他路径发货 → 直接幂等返回
    snprintf(qb,sizeof(qb),"UPDATE payment_orders SET status='paid',zp_trade_no='%s' WHERE id=%llu AND status='pending'",etn,oid);
    mysql_query(db,qb);
    if (mysql_affected_rows(db) == 0) {
        printf("[VPay] order %llu already applied by another path, skip\n",(unsigned long long)oid);
        return true;
    }
    // 只有抢占成功者才写订阅/余额
    snprintf(qb,sizeof(qb),"UPDATE users SET subscription_expires=DATE_ADD(GREATEST(COALESCE(subscription_expires,NOW()),NOW()),INTERVAL %u DAY), balance=balance+%llu WHERE id=%llu",dd,amt,du);
    mysql_query(db,qb);
    try{RedisClient::instance().del("sub:"+std::to_string(du));RedisClient::instance().del("pay:order:"+std::to_string(oid));}catch(...){}
    printf("[VPay] order %llu -> paid, user %llu +%u days +%llu balance\n",(unsigned long long)oid,(unsigned long long)du,dd,(unsigned long long)amt);
    return true;
}

// ── 对账扫描用: 未支付订单ID列表 (雪花ID作时间下界) ──
std::vector<uint64_t> VPayService::pendingOrdersSince(uint64_t cutoffId, int limit){
    std::vector<uint64_t> out;
    auto g=m_pool.acquire(5000);if(!g)return out;MYSQL*db=g->get();
    char qb[256];
    snprintf(qb,sizeof(qb),"SELECT id FROM payment_orders WHERE status='pending' AND id>%llu ORDER BY id ASC LIMIT %d",(unsigned long long)cutoffId,limit);
    if(mysql_query(db,qb)!=0)return out;
    MYSQL_RES*res=mysql_store_result(db);
    if(res){MYSQL_ROW rw;while((rw=mysql_fetch_row(res))){if(rw[0])out.push_back(std::stoull(rw[0]));}mysql_free_result(res);}
    return out;
}

// ── RSA-SHA256 验签 (平台公钥 rsa_plat_pub.pem) ──
bool VPayService::rsaVerifySha256(const std::string &data, const std::string &b64sig) const {
    static const char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto val=[&](char c)->int{for(int i=0;i<64;++i)if(T[i]==c)return i;return -1;};
    std::string sig;int buf=0,bits=0;
    for(char c:b64sig){if(c=='=')break;int v=val(c);if(v<0)continue;buf=(buf<<6)|v;bits+=6;if(bits>=8){bits-=8;sig.push_back((char)((buf>>bits)&0xFF));}}
    if(sig.empty())return false;

    std::string pem=readFile("rsa_plat_pub.pem");
    if(pem.empty()){printf("[VPay] rsa_plat_pub.pem missing\n");return false;}
    BIO*b=BIO_new_mem_buf(pem.data(),(int)pem.size());
    EVP_PKEY*pk=PEM_read_bio_PUBKEY(b,nullptr,nullptr,nullptr);BIO_free(b);
    if(!pk){printf("[VPay] platform pubkey parse FAILED\n");return false;}
    EVP_MD_CTX*c=EVP_MD_CTX_new();
    bool ok=EVP_DigestVerifyInit(c,nullptr,EVP_sha256(),nullptr,pk)==1
          && EVP_DigestVerifyUpdate(c,data.data(),data.size())==1
          && EVP_DigestVerifyFinal(c,(const unsigned char*)sig.data(),sig.size())==1;
    EVP_MD_CTX_free(c);EVP_PKEY_free(pk);
    return ok;
}

// ── 回调 (GET, 仅 RSA): 全字段验签 + trade_status/pid/金额/时间戳校验 ──
bool VPayService::handleCallback(const std::string &q){
    auto all=splitQuery(q);
    if(all.empty())return false;
    std::string sign, stype;
    std::vector<std::pair<std::string,std::string>> pr;
    for(auto &kv:all){
        if(kv.first=="sign"){sign=kv.second;continue;}
        if(kv.first=="sign_type"){stype=kv.second;continue;}
        pr.push_back(kv);
    }
    if(stype!="RSA"){printf("[VPay] callback sign_type=%s rejected (RSA only)\n",stype.c_str());return false;}
    if(sign.empty())return false;

    // 时间戳新鲜度 (±10min 防重放)
    std::string tsStr;
    for(auto &kv:pr) if(kv.first=="timestamp") tsStr=kv.second;
    if(!tsStr.empty()){
        long long ts=0; try{ts=std::stoll(tsStr);}catch(...){}
        long long now=(long long)time(nullptr);
        if(ts==0||now-ts>600||ts-now>600){
            printf("[VPay] callback timestamp out of range (%lld vs now %lld)\n",ts,now);
            return false;
        }
    }

    // 平台公钥验签 (全部字段参与, 兼容平台扩展字段)
    std::string ss=buildSignStr(pr);
    if(!rsaVerifySha256(ss,sign)){
        printf("[VPay] callback RSA verify FAILED\n");
        return false;
    }

    // pid / 状态 / 订单号检查
    std::string pid, st, on, tn;
    for(auto &kv:pr){
        if(kv.first=="pid")pid=kv.second;
        else if(kv.first=="trade_status")st=kv.second;
        else if(kv.first=="out_trade_no")on=kv.second;
        else if(kv.first=="trade_no")tn=kv.second;
    }
    if(pid!=m_appId){printf("[VPay] callback pid mismatch: %s\n",pid.c_str());return false;}
    if(st!="TRADE_SUCCESS"||on.empty())return false;

    // 金额核对 (按分比较, 防金额篡改)
    uint64_t oid=0;
    try{oid=std::stoull(on);}catch(...){return false;}
    auto o=queryOrder(oid);
    if(!o)return false;
    std::string money;
    for(auto &kv:pr) if(kv.first=="money") money=kv.second;
    if(!money.empty()){
        uint64_t cents=0; bool valid=true;
        size_t dot=money.find('.');
        std::string yuan=(dot==std::string::npos)?money:money.substr(0,dot);
        std::string fen=(dot==std::string::npos)?"":money.substr(dot+1);
        if(yuan.empty())valid=false;
        for(char c:yuan) if(!isdigit((unsigned char)c)) valid=false;
        for(char c:fen) if(!isdigit((unsigned char)c)) valid=false;
        if(valid){
            if(fen.empty())fen="00";
            else if(fen.size()==1)fen+="0";
            else if(fen.size()>2){valid=false;}
        }
        if(valid){try{cents=std::stoull(yuan)*100+std::stoull(fen);}catch(...){valid=false;}}
        if(!valid||cents!=o->amount){
            printf("[VPay] callback money mismatch: got %s expect %llu\n",money.c_str(),(unsigned long long)o->amount);
            return false;
        }
    }
    return applyPayment(oid,tn);
}

// ── 主动查单: 本地未支付 → POST /api/pay/query → 已付则补账 (回调丢失兜底) ──
bool VPayService::syncOrderStatus(uint64_t oid){
    auto o=queryOrder(oid);
    if(!o)return false;
    if(o->status=="paid")return true;
    std::string ts=std::to_string((long long)time(nullptr));
    std::vector<std::pair<std::string,std::string>> pr={
        {"pid",m_appId},{"out_trade_no",std::to_string(oid)},{"timestamp",ts}};
    std::string ss=buildSignStr(pr);
    std::string sign=rsaSignSha256(ss);
    if(sign.empty())return false;
    std::ostringstream bd; bool first=true;
    for(auto &kv:pr){ if(!first)bd<<"&"; first=false; bd<<kv.first<<"="<<StringUtils::urlEncode(kv.second); }
    bd<<"&sign="<<StringUtils::urlEncode(sign)<<"&sign_type=RSA";
    std::string resp=httpPost(m_apiUrl+"/api/pay/query",bd.str());
    printf("[VPay] pay/query resp: %s\n",resp.c_str());
    if(StringUtils::jsonVal(resp,"code")!="0")return false;
    if(StringUtils::jsonVal(resp,"status")!="1")return false;   // 1=平台已支付
    std::string tn=StringUtils::jsonVal(resp,"trade_no");
    return applyPayment(oid,tn);
}

std::optional<PayOrder> VPayService::queryOrder(uint64_t oid){auto g=m_pool.acquire(5000);if(!g)return{};MYSQL*db=g->get();char qb[256];
    snprintf(qb,sizeof(qb),"SELECT id,user_id,amount,duration_days,status,COALESCE(zp_trade_no,'')FROM payment_orders WHERE id=%llu",oid);mysql_query(db,qb);MYSQL_RES*res=mysql_store_result(db);if(!res||mysql_num_rows(res)==0){if(res)mysql_free_result(res);return{};}MYSQL_ROW rw=mysql_fetch_row(res);PayOrder o;o.orderId=std::stoull(rw[0]?rw[0]:"0");o.userId=std::stoull(rw[1]?rw[1]:"0");o.amount=std::stoull(rw[2]?rw[2]:"0");o.durationDays=(uint32_t)std::stoul(rw[3]?rw[3]:"0");o.status=rw[4]?rw[4]:"";o.tradeNo=rw[5]?rw[5]:"";mysql_free_result(res);return o;}
