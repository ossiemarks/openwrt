#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>
#include "ubus.h"
#include "state.h"

static struct ubus_context *ctx;
static struct blob_buf b;
static pthread_t thread;
static volatile bool *run_flag;
static char obj_name[32];

static int csi_status(struct ubus_context *c, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
    blob_buf_init(&b, 0);
    pthread_mutex_lock(&g_state.lock);
    blobmsg_add_u8(&b, "present", g_state.present);
    blobmsg_add_double(&b, "presence_confidence", g_state.presence_confidence);
    blobmsg_add_u32(&b, "person_count", g_state.person_count);
    blobmsg_add_u32(&b, "frames_total", g_state.frames_total);
    blobmsg_add_double(&b, "frame_rate", g_state.frame_rate);
    pthread_mutex_unlock(&g_state.lock);
    ubus_send_reply(c, req, b.head);
    return 0;
}

static int csi_presence(struct ubus_context *c, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
    blob_buf_init(&b, 0);
    pthread_mutex_lock(&g_state.lock);
    blobmsg_add_u8(&b, "present", g_state.present);
    blobmsg_add_double(&b, "confidence", g_state.presence_confidence);
    blobmsg_add_u32(&b, "count", g_state.person_count);
    pthread_mutex_unlock(&g_state.lock);
    ubus_send_reply(c, req, b.head);
    return 0;
}

static int csi_vitals(struct ubus_context *c, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
    blob_buf_init(&b, 0);
    pthread_mutex_lock(&g_state.lock);
    blobmsg_add_u32(&b, "zone", g_state.vitals_zone);
    blobmsg_add_u8(&b, "feasible", g_state.vitals_feasible);
    if (g_state.vitals_feasible) {
        blobmsg_add_u32(&b, "respiration_bpm", g_state.respiration_bpm);
        blobmsg_add_u32(&b, "heart_rate_bpm", g_state.heart_rate_bpm);
        blobmsg_add_double(&b, "confidence", g_state.vitals_confidence);
    } else {
        blobmsg_add_string(&b, "reason", g_state.vitals_reason);
    }
    pthread_mutex_unlock(&g_state.lock);
    ubus_send_reply(c, req, b.head);
    return 0;
}

static const struct ubus_method csi_methods[] = {
    UBUS_METHOD_NOARG("status", csi_status),
    UBUS_METHOD_NOARG("presence", csi_presence),
    UBUS_METHOD_NOARG("vitals", csi_vitals),
};

static struct ubus_object_type csi_obj_type =
    UBUS_OBJECT_TYPE("csi", csi_methods);

static struct ubus_object csi_object = {
    .name = obj_name,
    .type = &csi_obj_type,
    .methods = csi_methods,
    .n_methods = ARRAY_SIZE(csi_methods),
};

static void *ubus_loop(void *arg)
{
    (void)arg;
    while (run_flag && *run_flag) {
        ctx = ubus_connect(NULL);
        if (!ctx) {
            usleep(500000);
            continue;
        }
        ubus_add_uloop(ctx);
        if (ubus_add_object(ctx, &csi_object)) {
            ubus_free(ctx);
            ctx = NULL;
            usleep(500000);
            continue;
        }
        while (run_flag && *run_flag) {
            uloop_run_timeout(200);
        }
        ubus_free(ctx);
        ctx = NULL;
    }
    return NULL;
}

int ubus_start(const csi_config_t *cfg, volatile bool *running)
{
    if (!cfg->ubus_enabled)
        return 0;
    snprintf(obj_name, sizeof(obj_name), "%s", cfg->ubus_object);
    run_flag = running;
    uloop_init();
    return pthread_create(&thread, NULL, ubus_loop, NULL);
}

void ubus_stop(void)
{
    if (run_flag)
        pthread_join(thread, NULL);
    uloop_done();
}
