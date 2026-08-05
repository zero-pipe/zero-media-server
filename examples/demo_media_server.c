/**
 * ZMS 综合媒体服务（薄封装，逻辑在 zms_server 门面）。
 *
 *   demo_media_server [--config config.ini] [--log file] [--seconds N]
 */
#include "demo_server_runtime.h"
#include "zms/server.h"
#include "ztk/util/log.h"

#include <stdio.h>
#include <string.h>

static void on_publish(const zms_media_tuple *t, zms_media_origin origin, void *user)
{
    (void)user;
    ztk_debug("[hook] on_media_publish %s/%s origin=%s", t->app, t->stream,
              zms_media_origin_str(origin));
}

static void on_publish_fini(const zms_media_tuple *t, zms_media_origin origin, void *user)
{
    (void)user;
    ztk_debug("[hook] on_media_publish_fini %s/%s origin=%s", t->app, t->stream,
              zms_media_origin_str(origin));
}

static void on_play(const zms_media_tuple *t, const char *player, void *user)
{
    (void)user;
    ztk_debug("[hook] on_media_play %s/%s player=%s", t->app, t->stream, player ? player : "?");
}

static void on_stop(const zms_media_tuple *t, const char *player, void *user)
{
    (void)user;
    ztk_debug("[hook] on_media_stop %s/%s player=%s", t->app, t->stream, player ? player : "?");
}

static void on_reader_changed(const zms_media_tuple *t, int count, void *user)
{
    (void)user;
    ztk_debug("[hook] on_media_reader_changed %s/%s readers=%d", t->app, t->stream, count);
}

static void on_none_reader(const zms_media_tuple *t, void *user)
{
    (void)user;
    ztk_debug("[hook] on_media_none_reader %s/%s", t->app, t->stream);
}

int main(int argc, char **argv)
{
    zms_demo_server_args args;
    zms_server *srv;
    zms_server_hooks hooks;

    zms_demo_server_args_default(&args);
    if (zms_demo_server_parse_args(argc, argv, &args) != 0) {
        zms_demo_server_print_usage(argv[0], NULL);
        return 1;
    }

    srv = zms_server_create_from_ini(args.config_path, args.log_file_override);
    if (!srv) {
        return 1;
    }

    memset(&hooks, 0, sizeof(hooks));
    hooks.on_media_publish = on_publish;
    hooks.on_media_publish_fini = on_publish_fini;
    hooks.on_media_play = on_play;
    hooks.on_media_stop = on_stop;
    hooks.on_media_reader_changed = on_reader_changed;
    hooks.on_media_none_reader = on_none_reader;
    (void)zms_server_set_hooks(srv, &hooks);

    if (zms_server_start(srv) != ZTK_OK) {
        zms_server_destroy(srv);
        return 1;
    }

    zms_server_print_endpoints(srv);
    zms_server_install_default_signals(srv);
    zms_server_run(srv, args.seconds);

    zms_server_stop(srv);
    zms_server_destroy(srv);
    return 0;
}
