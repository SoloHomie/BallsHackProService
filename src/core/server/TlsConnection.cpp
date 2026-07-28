#include "TlsConnection.h"
#include "TlsContext.h"
#include <cstring>

TlsConnection::TlsConnection(TlsContext &ctx, int sock)
    : m_ssl(SSL_new(ctx.sslCtx())) {
    if (m_ssl)
        SSL_set_fd(m_ssl, sock);
}

TlsConnection::~TlsConnection() {
    if (m_ssl) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);
    }
}

bool TlsConnection::accept() {
    if (!m_ssl) return false;

    int ret = SSL_accept(m_ssl);
    if (ret == 1) return true;

    // Check for errors
    int err = SSL_get_error(m_ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // Non-blocking would retry; for blocking sockets this shouldn't happen
        return false;
    }
    return false; // handshake failed
}

int TlsConnection::read(void *buf, int len) {
    if (!m_ssl) return -1;
    int n = SSL_read(m_ssl, buf, len);
    if (n <= 0) {
        int err = SSL_get_error(m_ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            return 0; // no data available yet
        return -1;    // error or connection closed
    }
    return n;
}

int TlsConnection::write(const void *buf, int len) {
    if (!m_ssl) return -1;
    int n = SSL_write(m_ssl, buf, len);
    if (n <= 0) return -1;
    return n;
}
