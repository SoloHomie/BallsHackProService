#include "TlsContext.h"
#include <iostream>

TlsContext::~TlsContext() {
    if (m_cert)  X509_free(m_cert);
    if (m_pkey)  EVP_PKEY_free(m_pkey);
    if (m_ctx)   SSL_CTX_free(m_ctx);
}

bool TlsContext::init() {
    // Init OpenSSL (idempotent in OpenSSL 1.1+)
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    // Create SSL context (TLS server, TLS 1.2+)
    m_ctx = SSL_CTX_new(TLS_server_method());
    if (!m_ctx) {
        std::cerr << "[TLS] SSL_CTX_new failed" << std::endl;
        return false;
    }

    // Enforce TLS 1.2 minimum
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);

    // Generate self-signed certificate (in-memory, no files)
    if (!createSelfSignedCert()) {
        std::cerr << "[TLS] Failed to create self-signed certificate" << std::endl;
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return false;
    }

    // Load cert + key into context
    if (SSL_CTX_use_certificate(m_ctx, m_cert) != 1) {
        std::cerr << "[TLS] SSL_CTX_use_certificate failed" << std::endl;
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return false;
    }
    if (SSL_CTX_use_PrivateKey(m_ctx, m_pkey) != 1) {
        std::cerr << "[TLS] SSL_CTX_use_PrivateKey failed" << std::endl;
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return false;
    }

    // Key + cert are now owned by SSL_CTX — release our refs
    X509_free(m_cert); m_cert = nullptr;
    EVP_PKEY_free(m_pkey); m_pkey = nullptr;

    m_valid = true;
    std::cout << "[TLS] Self-signed certificate created, TLS 1.2+ ready" << std::endl;
    return true;
}

bool TlsContext::createSelfSignedCert() {
    // ── Generate RSA 2048-bit key ────────────────────
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) return false;

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);

    // ── Build X509 certificate ──────────────────────
    X509* cert = X509_new();
    if (!cert) { EVP_PKEY_free(pkey); return false; }

    // Version 3
    X509_set_version(cert, 2);

    // Serial number
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);

    // Validity: now to now + 365 days
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600);

    // Set public key
    X509_set_pubkey(cert, pkey);

    // Subject + Issuer = CN=localhost
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(cert, name);

    // Self-sign
    if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }

    m_pkey = pkey;
    m_cert = cert;
    return true;
}
