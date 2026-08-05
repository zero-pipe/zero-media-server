/**
 * 最小嵌入示例：通过 Stable SDK 门面从 config.ini 启动 ZMS。
 *
 *   hello_server [config.ini]
 */
#include "zms/sdk.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *cfg = (argc > 1) ? argv[1] : "config.ini";
    zms_server *s = zms_server_create_from_ini(cfg, NULL);

    if (!s) {
        fprintf(stderr, "create failed (check %s)\n", cfg);
        return 1;
    }
    if (zms_server_start(s) != ZTK_OK) {
        fprintf(stderr, "start failed\n");
        zms_server_destroy(s);
        return 1;
    }

    zms_server_print_endpoints(s);
    zms_server_install_default_signals(s);
    zms_server_run(s, -1);

    zms_server_stop(s);
    zms_server_destroy(s);
    return 0;
}
