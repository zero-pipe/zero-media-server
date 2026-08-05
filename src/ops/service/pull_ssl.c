#include "zms/ops/service/pull_ssl.h"
#include "zms/ops/service/ssl_config.h"
#include "ztk/net/ssl.h"
#include <stdlib.h>

static struct ztk_ssl_ctx *s_pull_ssl;
static int s_pull_ssl_tried;

struct ztk_ssl_ctx *zms_pull_ssl_ctx(const zms_config *cfg)
{
#if !defined(ZTK_HAVE_OPENSSL) || !ZTK_HAVE_OPENSSL
    (void)cfg;
    return NULL;
#else
    if (s_pull_ssl || s_pull_ssl_tried) {
        return s_pull_ssl;
    }
    s_pull_ssl_tried = 1;
    ztk_ssl_config_t sc;
    zms_ssl_config_from_zms(&sc, cfg);
    s_pull_ssl = ztk_ssl_ctx_create(&sc);
    return s_pull_ssl;
#endif
}
