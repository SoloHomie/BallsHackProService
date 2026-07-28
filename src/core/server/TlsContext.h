#pragma once
// ── TLS server context (OpenSSL) ──────────────────────
// Manages SSL_CTX with a self-signed certificate.
// For production, run behind Caddy/Nginx reverse proxy
// and disable TLS with SUREITY_TLS_DISABLE=1.
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>

class TlsContext {
public:
    TlsContext() = default;
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    bool init();
    SSL_CTX* sslCtx() { return m_ctx; }
    bool valid() const { return m_valid; }

private:
    bool createSelfSignedCert();

    SSL_CTX*  m_ctx   = nullptr;
    EVP_PKEY* m_pkey  = nullptr;  // in-memory key, freed with context
    X509*     m_cert  = nullptr;  // in-memory cert
    bool      m_valid = false;
};
