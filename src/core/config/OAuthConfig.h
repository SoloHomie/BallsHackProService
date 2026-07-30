#pragma once
#include "Env.h"

namespace OAuthConfig {
    constexpr const char *REDIRECT_URI = "https://api.solohomie.top/api/auth/oauth/callback";

    inline std::string OAUTH_PROXY() { return Env::get("BH_OAUTH_PROXY", ""); }

    namespace GitHub {
        inline std::string CLIENT_ID()     { return Env::required("BH_OAUTH_GITHUB_ID");     }
        inline std::string CLIENT_SECRET() { return Env::required("BH_OAUTH_GITHUB_SECRET"); }
        constexpr const char *AUTH_URL  = "https://github.com/login/oauth/authorize";
        constexpr const char *TOKEN_URL = "https://github.com/login/oauth/access_token";
        constexpr const char *USER_API  = "https://api.github.com/user";
    }

    namespace Discord {
        inline std::string CLIENT_ID()     { return Env::required("BH_OAUTH_DISCORD_ID");     }
        inline std::string CLIENT_SECRET() { return Env::required("BH_OAUTH_DISCORD_SECRET"); }
        constexpr const char *AUTH_URL  = "https://discord.com/oauth2/authorize";
        constexpr const char *TOKEN_URL = "https://discord.com/api/oauth2/token";
        constexpr const char *USER_API  = "https://discord.com/api/users/@me";
    }
}
