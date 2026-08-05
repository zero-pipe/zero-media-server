#include "zms/egress/egress_segment_recorder.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define ZMS_SEGMENT_OPS_MAX 8
#define ZMS_SEGMENT_REC_SLOTS 4
#define ZMS_SEGMENT_REC_NAME_MAX 16

typedef struct zms_segment_rec_slot {
    char name[ZMS_SEGMENT_REC_NAME_MAX];
    void *rec;
} zms_segment_rec_slot;

typedef struct zms_segment_sidecar {
    zms_segment_rec_slot slots[ZMS_SEGMENT_REC_SLOTS];
} zms_segment_sidecar;

static const zms_segment_recorder_ops *g_ops[ZMS_SEGMENT_OPS_MAX];
static int g_ops_count;

static zms_segment_sidecar *segment_sidecar_get(const zms_media_source *src)
{
    return src ? (zms_segment_sidecar *)src->segment_sidecar : NULL;
}

static zms_segment_sidecar *segment_sidecar_ensure(zms_media_source *src)
{
    zms_segment_sidecar *bag;

    if (!src) {
        return NULL;
    }
    bag = (zms_segment_sidecar *)src->segment_sidecar;
    if (bag) {
        return bag;
    }
    bag = (zms_segment_sidecar *)calloc(1, sizeof(*bag));
    if (!bag) {
        return NULL;
    }
    src->segment_sidecar = bag;
    return bag;
}

static void segment_sidecar_free_if_empty(zms_media_source *src)
{
    zms_segment_sidecar *bag;
    int i;

    if (!src || !src->segment_sidecar) {
        return;
    }
    bag = (zms_segment_sidecar *)src->segment_sidecar;
    for (i = 0; i < ZMS_SEGMENT_REC_SLOTS; ++i) {
        if (bag->slots[i].rec && bag->slots[i].name[0]) {
            return;
        }
    }
    free(bag);
    src->segment_sidecar = NULL;
}

void zms_segment_recorder_register(const zms_segment_recorder_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->name[0]) {
        return;
    }
    for (i = 0; i < g_ops_count; ++i) {
        if (g_ops[i] && strcmp(g_ops[i]->name, ops->name) == 0) {
            g_ops[i] = ops;
            return;
        }
    }
    if (g_ops_count >= ZMS_SEGMENT_OPS_MAX) {
        return;
    }
    g_ops[g_ops_count++] = ops;
}

const zms_segment_recorder_ops *zms_segment_recorder_find_ops(const char *name)
{
    int i;

    if (!name) {
        return NULL;
    }
    for (i = 0; i < g_ops_count; ++i) {
        if (g_ops[i] && strcmp(g_ops[i]->name, name) == 0) {
            return g_ops[i];
        }
    }
    return NULL;
}

void *zms_media_source_segment_rec_get(const zms_media_source *src, const char *name)
{
    zms_segment_sidecar *bag;
    int i;

    bag = segment_sidecar_get(src);
    if (!bag || !name || !name[0]) {
        return NULL;
    }
    for (i = 0; i < ZMS_SEGMENT_REC_SLOTS; ++i) {
        if (bag->slots[i].rec && bag->slots[i].name[0] &&
            strcmp(bag->slots[i].name, name) == 0) {
            return bag->slots[i].rec;
        }
    }
    return NULL;
}

ztk_err_t zms_media_source_segment_rec_set(zms_media_source *src, const char *name, void *rec)
{
    zms_segment_sidecar *bag;
    int i;
    int free_slot = -1;

    if (!src || !name || !name[0]) {
        return ZTK_ERR_INVALID;
    }
    if (strlen(name) >= ZMS_SEGMENT_REC_NAME_MAX) {
        return ZTK_ERR_INVALID;
    }

    bag = segment_sidecar_get(src);
    if (!bag) {
        if (!rec) {
            return ZTK_OK;
        }
        bag = segment_sidecar_ensure(src);
        if (!bag) {
            return ZTK_ERR_NOMEM;
        }
    }

    for (i = 0; i < ZMS_SEGMENT_REC_SLOTS; ++i) {
        if (bag->slots[i].name[0] && strcmp(bag->slots[i].name, name) == 0) {
            bag->slots[i].rec = rec;
            if (!rec) {
                bag->slots[i].name[0] = '\0';
                segment_sidecar_free_if_empty(src);
            }
            return ZTK_OK;
        }
        if (!bag->slots[i].rec && free_slot < 0) {
            free_slot = i;
        }
    }

    if (!rec) {
        return ZTK_OK;
    }
    if (free_slot < 0) {
        return ZTK_ERR_NOMEM;
    }

    strncpy(bag->slots[free_slot].name, name, sizeof(bag->slots[free_slot].name) - 1);
    bag->slots[free_slot].name[sizeof(bag->slots[free_slot].name) - 1] = '\0';
    bag->slots[free_slot].rec = rec;
    return ZTK_OK;
}

ztk_err_t zms_segment_recorder_create_live(zms_media_source *src, const char *name,
                                           const void *opts)
{
    const zms_segment_recorder_ops *ops;
    void *rec = NULL;
    ztk_err_t err;

    if (!src || !name) {
        return ZTK_ERR_INVALID;
    }
    /* VOD 源用 vod_buffer；直播分片录制器需 gop_queue。 */
    if (src->vod_buffer) {
        return ZTK_ERR_INVALID;
    }
    if (!src->gop_queue) {
        return ZTK_ERR_INVALID;
    }
    if (zms_media_source_segment_rec_get(src, name)) {
        return ZTK_OK;
    }

    ops = zms_segment_recorder_find_ops(name);
    if (!ops || !ops->create_live) {
        return ZTK_ERR_INVALID;
    }

    err = ops->create_live(src, opts, &rec);
    if (err != ZTK_OK || !rec) {
        return err != ZTK_OK ? err : ZTK_ERR_INVALID;
    }

    if (zms_media_source_segment_rec_set(src, name, rec) != ZTK_OK) {
        if (ops->destroy) {
            ops->destroy(rec);
        }
        return ZTK_ERR_NOMEM;
    }
    return ZTK_OK;
}

void zms_segment_recorder_destroy_live(zms_media_source *src, const char *name)
{
    const zms_segment_recorder_ops *ops;
    void *rec;

    if (!src || !name) {
        return;
    }
    rec = zms_media_source_segment_rec_get(src, name);
    if (!rec) {
        return;
    }
    (void)zms_media_source_segment_rec_set(src, name, NULL);
    ops = zms_segment_recorder_find_ops(name);
    if (ops && ops->destroy) {
        ops->destroy(rec);
    }
}

void zms_segment_recorder_destroy_all(zms_media_source *src)
{
    zms_segment_sidecar *bag;
    char names[ZMS_SEGMENT_REC_SLOTS][ZMS_SEGMENT_REC_NAME_MAX];
    int n = 0;
    int i;

    if (!src) {
        return;
    }
    bag = segment_sidecar_get(src);
    if (!bag) {
        return;
    }
    for (i = 0; i < ZMS_SEGMENT_REC_SLOTS; ++i) {
        if (bag->slots[i].rec && bag->slots[i].name[0]) {
            strncpy(names[n], bag->slots[i].name, ZMS_SEGMENT_REC_NAME_MAX - 1);
            names[n][ZMS_SEGMENT_REC_NAME_MAX - 1] = '\0';
            ++n;
        }
    }
    for (i = 0; i < n; ++i) {
        zms_segment_recorder_destroy_live(src, names[i]);
    }
}

ztk_err_t zms_segment_recorder_ensure_live(zms_media_source *src, const char *name,
                                           ztk_poller *poller, const void *opts)
{
    ztk_err_t err;

    if (!src || !name) {
        return ZTK_ERR_INVALID;
    }
    /* 内建由 zms_modules_register_all() 注册（组合根）。 */
    if (!zms_media_source_segment_rec_get(src, name)) {
        err = zms_segment_recorder_create_live(src, name, opts);
        if (err != ZTK_OK) {
            return err;
        }
    }
    zms_segment_recorder_bind_timer(src, name, poller);
    return zms_media_source_segment_rec_get(src, name) ? ZTK_OK : ZTK_ERR_INVALID;
}

void zms_segment_recorder_bind_timer(zms_media_source *src, const char *name, ztk_poller *poller)
{
    const zms_segment_recorder_ops *ops;
    void *rec;

    if (!src || !name || !poller) {
        return;
    }
    rec = zms_media_source_segment_rec_get(src, name);
    if (!rec) {
        return;
    }
    ops = zms_segment_recorder_find_ops(name);
    if (ops && ops->bind_timer) {
        ops->bind_timer(rec, poller);
    }
}

void zms_segment_recorder_tick(zms_media_source *src, const char *name)
{
    const zms_segment_recorder_ops *ops;
    void *rec;

    if (!src || !name) {
        return;
    }
    rec = zms_media_source_segment_rec_get(src, name);
    if (!rec) {
        return;
    }
    ops = zms_segment_recorder_find_ops(name);
    if (ops && ops->tick) {
        ops->tick(rec);
    }
}

void zms_segment_recorder_touch(zms_media_source *src, const char *name)
{
    const zms_segment_recorder_ops *ops;
    void *rec;

    if (!src || !name) {
        return;
    }
    rec = zms_media_source_segment_rec_get(src, name);
    if (!rec) {
        return;
    }
    ops = zms_segment_recorder_find_ops(name);
    if (ops && ops->touch) {
        ops->touch(rec);
    }
}
