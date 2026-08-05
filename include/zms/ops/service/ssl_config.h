#ifndef ZMS_SERVICE_SSL_CONFIG_H
#define ZMS_SERVICE_SSL_CONFIG_H

#include "zms/ops/service/config.h"
#include "ztk/net/ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 将 ZMS [ssl] 配置填入 ztk_ssl_config（仅当 ZTK_HAVE_OPENSSL 时有效） */
static inline void zms_ssl_config_from_zms(ztk_ssl_config_t *out, const zms_config *cfg)
{
    if (!out) {
        return;
    }
    out->ca_file = NULL;
    out->verify_peer = 0;
    out->client_cert_file = NULL;
    out->client_key_file = NULL;
    if (!cfg) {
        return;
    }
    if (cfg->ssl.ca_file[0]) {
        out->ca_file = cfg->ssl.ca_file;
    }
    out->verify_peer = cfg->ssl.verify_peer;
    if (cfg->ssl.client_cert[0]) {
        out->client_cert_file = cfg->ssl.client_cert;
    }
    if (cfg->ssl.client_key[0]) {
        out->client_key_file = cfg->ssl.client_key;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SERVICE_SSL_CONFIG_H */
