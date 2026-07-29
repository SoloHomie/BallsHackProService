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
#include <mysql/mysql.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include "../core/cache/RedisClient.h"
#include <sstream>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <algorithm>

static std::string b64enc(const unsigned char *d, int n) {
    static const char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve(((n+2)/3)*4);
    for(int i=0;i<n;i+=3){unsigned int v=((unsigned int)d[i])<<16;v|=(i+1<n)?((unsigned int)d[i+1])<<8:0;v|=(i+2<n)?(unsigned int)d[i+2]:0;
        o+=T[(v>>18)&0x3F];o+=T[(v>>12)&0x3F];o+=(i+1<n)?T[(v>>6)&0x3F]:'=';o+=(i+2<n)?T[v&0x3F]:'=';}
    return o;
}

const std::vector<PricePlan>& VPayService::plans(){
    static const std::vector<PricePlan> s={
    {"1","日卡",500,1},{"2","周卡",2500,7},{"3","月卡",8800,30},
    {"4","季卡",19800,90},{"5","年卡",28800,365}
};
    return s;
}

std::string VPayService::readFile(const std::string &p) const {std::ifstream f(p);if(!f)return"";std::ostringstream ss;ss<<f.rdbuf();return ss.str();}

VPayService::VPayService(DbPool &pool,const std::string &u,const std::string &id,const std::string &kp,const std::string &mk,const std::string &nu)
    :m_pool(pool),m_apiUrl(u),m_appId(id),m_md5Key(mk),m_notifyUrl(nu){while(!m_apiUrl.empty()&&m_apiUrl.back()=='/')m_apiUrl.pop_back();m_rsaKey=readFile(kp);printf("[VPay] RSA key: %zu\n",m_rsaKey.size());}

std::string VPayService::rsaSign(const std::string &d) const {
    if(m_rsaKey.empty())return"";
    BIO*b=BIO_new_mem_buf(m_rsaKey.data(),(int)m_rsaKey.size());
    EVP_PKEY*pk=PEM_read_bio_PrivateKey(b,nullptr,nullptr,nullptr);BIO_free(b);
    if(!pk)return"";
    EVP_MD_CTX*c=EVP_MD_CTX_new();EVP_SignInit_ex(c,EVP_sha256(),nullptr);
    EVP_SignUpdate(c,d.data(),d.size());unsigned char sig[512];unsigned int sl=0;
    EVP_SignFinal(c,sig,&sl,pk);EVP_MD_CTX_free(c);EVP_PKEY_free(pk);
    return b64enc(sig,(int)sl);
}

std::string VPayService::buildSignStr(const std::vector<std::pair<std::string,std::string>> &p) const {
    std::vector<std::pair<std::string,std::string>> s;
    for(auto &x:p) if(x.first!="sign"&&x.first!="sign_type"&&!x.second.empty()) s.push_back(x);
    std::sort(s.begin(),s.end(),[](auto&a,auto&b){return a.first<b.first;});
    std::ostringstream ss; for(size_t i=0;i<s.size();++i){if(i)ss<<"&";ss<<s[i].first<<"="<<s[i].second;}
    return ss.str();
}

static std::string httpPost(const std::string &url, const std::string &data){
    std::string r;
    std::string cmd="curl -s -X POST \""+url+"\" -d \""+data+"\" --max-time 15";
    FILE*fp=_popen(cmd.c_str(),"r");
    if(fp){char bf[4096];while(fgets(bf,sizeof(bf),fp))r+=bf;_pclose(fp);}
    return r;
}

uint32_t VPayService::daysForProduct(const std::string &pid) const {for(auto &p:plans())if(p.productId==pid)return p.days;return 30;}

std::pair<bool,std::string> VPayService::createOrder(uint64_t uid,const std::string &pid){
    uint64_t oid=Snowflake::generate();std::string ono=std::to_string(oid);uint32_t d=daysForProduct(pid);
    uint64_t amt=3000;std::string nm="订阅";for(auto &p:plans())if(p.productId==pid){amt=p.amount;nm=p.name;break;}
    std::string money=std::to_string(amt/100)+"."+std::to_string(amt%100/10)+std::to_string(amt%10);
    auto g=m_pool.acquire(5000);if(!g)return{false,"DB"};
    MYSQL* db=g->get();MYSQL_STMT*s=mysql_stmt_init(db);
    const char*sql="INSERT INTO payment_orders(id,user_id,amount,duration_days,status)VALUES(?,?,?,?,'pending')";
    mysql_stmt_prepare(s,sql,(unsigned long)strlen(sql));MYSQL_BIND b[4]={};
    b[0].buffer_type=MYSQL_TYPE_LONGLONG;b[0].buffer=&oid;b[1].buffer_type=MYSQL_TYPE_LONGLONG;b[1].buffer=&uid;
    b[2].buffer_type=MYSQL_TYPE_LONGLONG;b[2].buffer=&amt;b[3].buffer_type=MYSQL_TYPE_LONG;b[3].buffer=&d;
    mysql_stmt_bind_param(s,b);int e=mysql_stmt_execute(s);mysql_stmt_close(s);if(e)return{false,"订单创建失败"};
    try{RedisClient::instance().setex("pay:order:"+ono,300LL,"ok");}catch(...){}

    std::vector<std::pair<std::string,std::string>> pr={
        {"clientip","127.0.0.1"},{"device","mobile"},{"money",money},{"name",nm},
        {"notify_url",m_notifyUrl},{"out_trade_no",ono},{"pid",m_appId},{"type","alipay"}};
    std::string ss=buildSignStr(pr);std::string sig=rsaSign(ss);
    printf("[VPay] sign_src: %s\n[VPay] sign: %s\n",ss.c_str(),sig.c_str());
    std::ostringstream bd;bd<<"pid="<<m_appId<<"&type=alipay&out_trade_no="<<ono<<"&notify_url="<<StringUtils::urlEncode(m_notifyUrl)<<"&name="<<StringUtils::urlEncode(nm)<<"&money="<<money<<"&clientip=127.0.0.1&device=mobile&sign="<<StringUtils::urlEncode(sig)<<"&sign_type=RSA";
    std::string resp=httpPost(m_apiUrl+"/mapi.php",bd.str());
    printf("[VPay] resp: %s\n",resp.c_str());
    std::string qr=StringUtils::jsonVal(resp,"qrcode"),pu=StringUtils::jsonVal(resp,"payurl"),tn=StringUtils::jsonVal(resp,"trade_no");
    for(size_t p=0;(p=pu.find("\\/",p))!=std::string::npos;p+=1)pu.replace(p,2,"/");
    for(size_t p=0;(p=qr.find("\\/",p))!=std::string::npos;p+=1)qr.replace(p,2,"/");
    // 返回 JSON: pay_url + trade_no
    std::ostringstream r;r<<"{\"pay_url\":\""<<(qr.empty()?pu:qr)<<"\",\"trade_no\":\""<<tn<<"\",\"order_id\":\""<<ono<<"\",\"type\":\""<<(qr.empty()?"url":"qr")<<"\"}";
    return{true,r.str()};
}

bool VPayService::handleCallback(const std::string &q){
    std::string tn=StringUtils::queryVal(q,"trade_no"),on=StringUtils::queryVal(q,"out_trade_no"),st=StringUtils::queryVal(q,"trade_status"),cs=StringUtils::queryVal(q,"sign");
    if(on.empty()||st!="TRADE_SUCCESS")return false;
    std::vector<std::pair<std::string,std::string>> pr={{"money",StringUtils::queryVal(q,"money")},{"name",StringUtils::queryVal(q,"name")},{"out_trade_no",on},{"pid",StringUtils::queryVal(q,"pid")},{"trade_no",tn},{"trade_status",st},{"type",StringUtils::queryVal(q,"type")}};
    std::string ss=buildSignStr(pr);
    // Verify with MD5 since callback uses MD5 (商户密钥)
    auto md5=[](const std::string &s){unsigned char d[16];unsigned int l=0;EVP_MD_CTX*c=EVP_MD_CTX_new();EVP_DigestInit_ex(c,EVP_md5(),nullptr);EVP_DigestUpdate(c,s.data(),s.size());EVP_DigestFinal_ex(c,d,&l);EVP_MD_CTX_free(c);static const char h[]="0123456789abcdef";std::string r(32,'\0');for(int i=0;i<16;++i){r[i*2]=h[d[i]>>4];r[i*2+1]=h[d[i]&0xF];}return r;};
    // RSA 验签（用平台公钥）
    auto b64dec=[](const std::string &s){
        static int D[256]={};static bool init=false;if(!init){for(int i=0;i<256;++i)D[i]=-1;
        for(int i=0;i<64;++i)D[(int)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]]=i;init=true;}
        std::string o;for(size_t i=0;i<s.size();i+=4){int v0=D[(unsigned char)s[i]],v1=i+1<s.size()?D[(unsigned char)s[i+1]]:-1,
        v2=i+2<s.size()&&s[i+2]!='='?D[(unsigned char)s[i+2]]:-1,v3=i+3<s.size()&&s[i+3]!='='?D[(unsigned char)s[i+3]]:-1;
        if(v0==-1||v1==-1)break;unsigned int v=((unsigned int)v0<<18)|((unsigned int)v1<<12);o+=(unsigned char)(v>>16);
        if(v2!=-1){v|=(unsigned int)v2<<6;o+=(unsigned char)(v>>8);}if(v3!=-1){v|=(unsigned int)v3;o+=(unsigned char)v;}}return o;};
    std::string pk=readFile("rsa_plat_pub.pem");
    if(!pk.empty()){
        std::string sd=b64dec(cs);
        BIO*b2=BIO_new_mem_buf(pk.data(),(int)pk.size());EVP_PKEY*ppk=PEM_read_bio_PUBKEY(b2,nullptr,nullptr,nullptr);BIO_free(b2);
        if(ppk){EVP_MD_CTX*c2=EVP_MD_CTX_new();EVP_VerifyInit_ex(c2,EVP_sha256(),nullptr);EVP_VerifyUpdate(c2,ss.data(),ss.size());
            int ok=EVP_VerifyFinal(c2,(unsigned char*)sd.data(),(unsigned int)sd.size(),ppk);EVP_MD_CTX_free(c2);EVP_PKEY_free(ppk);
            if(ok!=1){printf("[VPay] RSA verify FAILED\n");return false;}
        } else { printf("[VPay] RSA verify: bad pubkey\n"); return false; }
    }
    uint64_t oid=std::stoull(on);
    // 检查 Redis 订单是否过期
    try{auto v=RedisClient::instance().get("pay:order:"+on);if(!v)return false;}catch(...){}
    auto g=m_pool.acquire(5000);if(!g)return false;MYSQL*db=g->get();
    uint64_t du=0,amt=0;uint32_t dd=0;char ds[32]={};bool fd=false;char qb[512];
    snprintf(qb,sizeof(qb),"SELECT user_id,duration_days,status,amount FROM payment_orders WHERE id=%llu",oid);
    mysql_query(db,qb);MYSQL_RES*res=mysql_store_result(db);
    if(res&&mysql_num_rows(res)>0){MYSQL_ROW rw=mysql_fetch_row(res);du=std::stoull(rw[0]?rw[0]:"0");dd=(uint32_t)std::stoul(rw[1]?rw[1]:"0");if(rw[2]){std::strncpy(ds,rw[2],31);ds[31]='\0';}amt=std::stoull(rw[3]?rw[3]:"0");fd=true;}
    if(res)mysql_free_result(res);if(!fd)return false;if(std::string(ds)=="paid")return true;
    char etn[512]; mysql_real_escape_string(db, etn, tn.c_str(), tn.size());
    snprintf(qb,sizeof(qb),"UPDATE payment_orders SET status='paid',zp_trade_no='%s' WHERE id=%llu",etn,oid);mysql_query(db,qb);
    snprintf(qb,sizeof(qb),"UPDATE users SET subscription_expires=DATE_ADD(GREATEST(COALESCE(subscription_expires,NOW()),NOW()),INTERVAL %u DAY), balance=balance+%llu WHERE id=%llu",dd,amt,du);mysql_query(db,qb);
    try{RedisClient::instance().del("sub:"+std::to_string(du));RedisClient::instance().del("pay:order:"+on);}catch(...){}
    return true;
}

std::optional<PayOrder> VPayService::queryOrder(uint64_t oid){auto g=m_pool.acquire(5000);if(!g)return{};MYSQL*db=g->get();char qb[256];
    snprintf(qb,sizeof(qb),"SELECT id,user_id,amount,duration_days,status,COALESCE(zp_trade_no,'')FROM payment_orders WHERE id=%llu",oid);mysql_query(db,qb);MYSQL_RES*res=mysql_store_result(db);if(!res||mysql_num_rows(res)==0){if(res)mysql_free_result(res);return{};}MYSQL_ROW rw=mysql_fetch_row(res);PayOrder o;o.orderId=std::stoull(rw[0]?rw[0]:"0");o.userId=std::stoull(rw[1]?rw[1]:"0");o.amount=std::stoull(rw[2]?rw[2]:"0");o.durationDays=(uint32_t)std::stoul(rw[3]?rw[3]:"0");o.status=rw[4]?rw[4]:"";o.tradeNo=rw[5]?rw[5]:"";mysql_free_result(res);return o;}
