#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "rest.h"
#include "state.h"

static int listen_fd = -1;
static pthread_t thread;
static volatile bool *run_flag;

static void body_status(char *out, size_t n)
{
    pthread_mutex_lock(&g_state.lock);
    snprintf(out, n,
        "{\"frames_total\":%lu,\"frame_rate\":%.1f,\"present\":%s}",
        g_state.frames_total, g_state.frame_rate,
        g_state.present ? "true" : "false");
    pthread_mutex_unlock(&g_state.lock);
}

static void body_presence(char *out, size_t n)
{
    pthread_mutex_lock(&g_state.lock);
    snprintf(out, n,
        "{\"present\":%s,\"confidence\":%.2f,\"count\":%d}",
        g_state.present ? "true" : "false",
        g_state.presence_confidence, g_state.person_count);
    pthread_mutex_unlock(&g_state.lock);
}

static void body_vitals(char *out, size_t n)
{
    pthread_mutex_lock(&g_state.lock);
    if (g_state.vitals_feasible)
        snprintf(out, n,
            "{\"zone\":%d,\"feasible\":true,\"respiration_bpm\":%d,"
            "\"heart_rate_bpm\":%d,\"confidence\":%.2f,\"window_seconds\":30}",
            g_state.vitals_zone, g_state.respiration_bpm,
            g_state.heart_rate_bpm, g_state.vitals_confidence);
    else
        snprintf(out, n,
            "{\"zone\":%d,\"feasible\":false,\"reason\":\"%s\"}",
            g_state.vitals_zone, g_state.vitals_reason);
    pthread_mutex_unlock(&g_state.lock);
}

static void body_zones(char *out, size_t n)
{
    size_t off = 0;
    off += snprintf(out + off, n - off, "{");
    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < g_config.zone_count && off < n; i++) {
        off += snprintf(out + off, n - off, "%s\"%s\":\"%s\"",
                        i ? "," : "", g_config.zone_names[i],
                        g_state.zone_occupied[i] ? "occupied" : "empty");
    }
    pthread_mutex_unlock(&g_state.lock);
    snprintf(out + off, n - off, "}");
}

static void handle(int fd, const char *path)
{
    char body[1024] = "{}";
    const char *status = "200 OK";

    if (!strcmp(path, "/api/csi/status"))
        body_status(body, sizeof(body));
    else if (!strcmp(path, "/api/csi/presence"))
        body_presence(body, sizeof(body));
    else if (!strcmp(path, "/api/csi/vitals"))
        body_vitals(body, sizeof(body));
    else if (!strcmp(path, "/api/csi/zones"))
        body_zones(body, sizeof(body));
    else
        status = "404 Not Found";

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        status, strlen(body));
    write(fd, hdr, hn);
    write(fd, body, strlen(body));
}

static void *rest_loop(void *arg)
{
    (void)arg;
    while (run_flag && *run_flag) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        char req[1024];
        int fd = accept(listen_fd, (struct sockaddr *)&ca, &cl);

        if (fd < 0)
            continue;

        int r = read(fd, req, sizeof(req) - 1);
        if (r > 0) {
            req[r] = '\0';
            char method[8], path[128];
            if (sscanf(req, "%7s %127s", method, path) == 2)
                handle(fd, path);
        }
        close(fd);
    }
    return NULL;
}

int rest_start(const csi_config_t *cfg, volatile bool *running)
{
    struct sockaddr_in sa;
    int opt = 1;

    if (!cfg->rest_enabled)
        return 0;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return -1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(cfg->rest_port);

    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(listen_fd, 8) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }

    run_flag = running;
    return pthread_create(&thread, NULL, rest_loop, NULL);
}

void rest_stop(void)
{
    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
        listen_fd = -1;
    }
    if (run_flag)
        pthread_join(thread, NULL);
}
