#pragma once
// ── Per-client TLS connection (OpenSSL) ───────────────
// Thin wrapper around SSL*.  Call accept() after creation,
// then read()/write() for encrypted I/O.
#include <openssl/ssl.h>
#include <string>
#include <vector>

class TlsContext;

class TlsConnection {
public:
    TlsConnection(TlsContext &ctx, int sock);
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    // Perform TLS handshake (blocking). Returns true on success.
    bool accept();

    // Read decrypted bytes. Returns number of bytes read, or -1 on error.
    int read(void *buf, int len);

    // Write encrypted bytes. Returns number of bytes written, or -1 on error.
    int write(const void *buf, int len);

    bool valid() const { return m_ssl != nullptr; }

private:
    SSL* m_ssl = nullptr;
};
