/*
 * psweb.c - the web engine service for the PiSTorm Atari (psweb-design.md).
 *
 * A separate process from the emulator, on purpose: WebKit spawns its own
 * processes and anything born on the emulator's 68k thread never runs; a
 * crash here takes a tab, not the Atari; and it runs as an ordinary user.
 *
 * WPE WebKit renders off-screen through WPEBackend-fdo's shared-memory
 * path (measured in tools/webbench: CPU painting beats the V3D here, and it
 * is the only path that hands us plain pixels). Each frame is converted to
 * the guest's fVDI pixel format and placed in a shared-memory surface; the
 * emulator's PSWEB NatFeat copies it into TT-RAM when the guest asks. One
 * frame in flight, ever: the next one is only released to WebKit once the
 * emulator has consumed the last, so the engine runs at the guest's pace.
 *
 *   psweb                       listens on $PSWEB_SOCK (default /tmp/psweb.sock),
 *                               or on the socket systemd hands it (LISTEN_FDS)
 *   PSWEB_CPUS=2                affinity mask (hex) for us and the web processes
 *   PSWEB_FILTER=rules.json     content blocker rules (WebKit JSON format)
 *   PSWEB_MEM_MB=700            web process memory limit (default 40% of RAM);
 *                               the process is killed at 1.5x and WEBGEM shows
 *                               "crashed" - the cgroup cannot do this on a Pi
 *                               booted with cgroup_disable=memory
 *   PSWEB_IDLE_S=600            exit after this long with no client (0 = never);
 *                               under psweb.socket the next connect restarts us
 *   PSWEB_CACHE=full            keep WebKit's desktop caching (old web processes
 *                               stay for back/forward); default is lean
 *   PSWEB_DEBUG=1               chatter
 *
 * Talks the protocol in platforms/atari/web/psweb_proto.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>
#include <wayland-server.h>

#include "../platforms/atari/web/psweb_proto.h"

/* --------------------------------------------------------------- state -- */

static int g_debug;
#define DBG(...) do { if (g_debug) { fprintf(stderr, "[psweb] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static GMainLoop *g_loop;
static int g_listen_fd = -1;
static int g_shm_fd = -1;
static int g_listen_inherited;      /* from systemd: never unlink it */
static int g_client_fd = -1;
static guint g_client_src;
static WebKitWebContext *g_ctx;
static int g_lean = 1;              /* PSWEB_CACHE: lean (default) or full */
static int g_idle_s;                /* exit after this long without a client */
static gint64 g_idle_since;         /* monotonic us of the last disconnect */

/* shared memory */
static char g_shm_name[PSWEB_SHM_NAME_MAX];
static uint8_t *g_shm;
static size_t g_shm_size;
static struct psweb_surface *g_surf;
static uint8_t *g_pixels;

/* the one view (v1) */
static struct {
    int active;
    int w, h, bpp;
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    WebKitWebView *view;
    WebKitUserContentManager *ucm;

    uint8_t *staging;            /* latest frame, guest format         */
    int staging_w, staging_h, staging_stride;
    int staging_fresh;           /* a frame waits for the surface      */
    int ack_pending;             /* WebKit waits for frame_complete    */
    uint32_t dropped;

    uint32_t flags;
    uint32_t pointer_mods;       /* buttons held, for drags            */
    int js_on, blocker_on;
} V;

static WebKitUserContentFilter *g_filter;   /* compiled blocker, or NULL */

/* read buffer for commands */
static uint8_t g_inbuf[sizeof(struct psweb_cmd) + PSWEB_STR_MAX + 16];
static size_t g_inlen;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ------------------------------------------------------------ events ---- */

/* fd >= 0 rides along as SCM_RIGHTS: the HELLO carries the surface's
 * descriptor, so the emulator maps it whoever it runs as - a name in
 * /dev/shm and its mode bits are only a fallback for an old emulator */
static void send_evt_fd(uint32_t type, int32_t a, int32_t b, const char *str, int fd)
{
    if (g_client_fd < 0)
        return;
    struct psweb_evt e;
    e.type = type;
    e.view = 1;
    e.a = a;
    e.b = b;
    e.len = str ? (uint32_t) strnlen(str, PSWEB_STR_MAX - 1) : 0;

    struct iovec iov[2] = {
        { &e, sizeof e },
        { (void *) str, e.len },
    };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov;
    mh.msg_iovlen = str ? 2 : 1;
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cm;
    if (fd >= 0) {
        memset(&cm, 0, sizeof cm);
        mh.msg_control = cm.buf;
        mh.msg_controllen = sizeof cm.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    if (sendmsg(g_client_fd, &mh, MSG_NOSIGNAL) < 0)
        fprintf(stderr, "[psweb] event %u send failed: %s\n", type, strerror(errno));
}

static void send_evt(uint32_t type, int32_t a, int32_t b, const char *str)
{
    send_evt_fd(type, a, b, str, -1);
}

static void send_flags(void)
{
    uint32_t f = V.flags & ~(PSWEB_FL_CAN_BACK | PSWEB_FL_CAN_FWD | PSWEB_FL_SECURE |
                             PSWEB_FL_JS_OFF | PSWEB_FL_BLOCKER);
    if (V.view) {
        if (webkit_web_view_can_go_back(V.view)) f |= PSWEB_FL_CAN_BACK;
        if (webkit_web_view_can_go_forward(V.view)) f |= PSWEB_FL_CAN_FWD;
        const char *uri = webkit_web_view_get_uri(V.view);
        if (uri && g_str_has_prefix(uri, "https://")) f |= PSWEB_FL_SECURE;
    }
    if (!V.js_on) f |= PSWEB_FL_JS_OFF;
    if (V.blocker_on && g_filter) f |= PSWEB_FL_BLOCKER;
    V.flags = f;
    send_evt(PSWEB_EVT_FLAGS, (int32_t) f, 0, NULL);
}

/* --------------------------------------------------------- frame path --- */

/* ARGB8888 (little-endian words) -> the guest's format */
static void convert(const uint8_t *src, int src_stride, int w, int h)
{
    if (V.bpp == 32) {
        for (int y = 0; y < h; y++) {
            const uint32_t *s = (const uint32_t *) (src + (size_t) y * src_stride);
            uint8_t *d = V.staging + (size_t) y * V.staging_stride;
            for (int x = 0; x < w; x++) {
                uint32_t p = s[x];
                d[0] = 0;
                d[1] = (uint8_t) (p >> 16);
                d[2] = (uint8_t) (p >> 8);
                d[3] = (uint8_t) p;
                d += 4;
            }
        }
    } else {                                    /* 16: big-endian RGB565 */
        for (int y = 0; y < h; y++) {
            const uint32_t *s = (const uint32_t *) (src + (size_t) y * src_stride);
            uint8_t *d = V.staging + (size_t) y * V.staging_stride;
            for (int x = 0; x < w; x++) {
                uint32_t p = s[x];
                uint16_t v = (uint16_t) (((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
                d[0] = (uint8_t) (v >> 8);
                d[1] = (uint8_t) v;
                d += 2;
            }
        }
    }
}

/* the surface is free when the emulator has consumed what is in it */
static int surface_free(void)
{
    return g_surf && g_surf->consumed == g_surf->serial;
}

static void publish(void)
{
    double t = now_us();
    g_surf->w = (uint32_t) V.staging_w;
    g_surf->h = (uint32_t) V.staging_h;
    g_surf->bpp = (uint32_t) V.bpp;
    g_surf->stride = (uint32_t) V.staging_stride;
    memcpy(g_pixels, V.staging, (size_t) V.staging_stride * V.staging_h);
    g_surf->n_damage = 1;
    g_surf->damage[0][0] = 0;
    g_surf->damage[0][1] = 0;
    g_surf->damage[0][2] = V.staging_w;
    g_surf->damage[0][3] = V.staging_h;
    g_surf->frames_dropped = V.dropped;
    g_surf->paint_us = (uint32_t) (now_us() - t);
    __sync_synchronize();
    g_surf->serial++;
    V.staging_fresh = 0;
}

static void on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer)
{
    (void) data;
    struct wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    if (shm && V.staging) {
        wl_shm_buffer_begin_access(shm);
        int w = wl_shm_buffer_get_width(shm);
        int h = wl_shm_buffer_get_height(shm);
        int stride = wl_shm_buffer_get_stride(shm);
        if (w > V.w) w = V.w;
        if (h > V.h) h = V.h;
        if (V.staging_fresh)
            V.dropped++;               /* the guest never saw the last one */
        V.staging_w = w;
        V.staging_h = h;
        V.staging_stride = w * (V.bpp / 8);
        convert(wl_shm_buffer_get_data(shm), stride, w, h);
        wl_shm_buffer_end_access(shm);
        V.staging_fresh = 1;
    }
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(V.exportable, buffer);
    V.ack_pending = 1;
}

/* GPU-path buffers (only if someone forces GPU painting): release, never read */
static void on_export_buffer_resource(void *data, struct wl_resource *resource)
{
    (void) data;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(V.exportable, resource);
    V.ack_pending = 1;
}

static void on_export_dmabuf_resource(void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *res)
{
    (void) data;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(V.exportable, res->buffer_resource);
    V.ack_pending = 1;
}

/* Runs every few ms. Hands a waiting frame to the surface once the emulator
 * has taken the previous one, and only then lets WebKit draw the next. That
 * is the whole pacing scheme. */
static gboolean on_tick(gpointer user)
{
    (void) user;
    if (!V.active)
        return G_SOURCE_CONTINUE;
    if (V.staging_fresh && surface_free())
        publish();
    if (V.ack_pending && !V.staging_fresh) {
        V.ack_pending = 0;
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(V.exportable);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------ WebKit signals -- */

static void on_notify_title(GObject *o, GParamSpec *p, gpointer u)
{
    (void) o; (void) p; (void) u;
    const char *t = webkit_web_view_get_title(V.view);
    send_evt(PSWEB_EVT_TITLE, 0, 0, t ? t : "");
}

static void on_notify_uri(GObject *o, GParamSpec *p, gpointer u)
{
    (void) o; (void) p; (void) u;
    const char *s = webkit_web_view_get_uri(V.view);
    send_evt(PSWEB_EVT_URI, 0, 0, s ? s : "");
    send_flags();
}

static void on_notify_progress(GObject *o, GParamSpec *p, gpointer u)
{
    (void) o; (void) p; (void) u;
    send_evt(PSWEB_EVT_PROGRESS, (int32_t) (webkit_web_view_get_estimated_load_progress(V.view) * 1000.0), 0, NULL);
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent ev, gpointer u)
{
    (void) view; (void) u;
    if (ev == WEBKIT_LOAD_STARTED)
        V.flags |= PSWEB_FL_LOADING;
    else if (ev == WEBKIT_LOAD_FINISHED)
        V.flags &= ~PSWEB_FL_LOADING;
    V.flags &= ~PSWEB_FL_CRASHED;
    send_flags();
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent ev, const char *uri, GError *err, gpointer u)
{
    (void) view; (void) ev; (void) u;
    DBG("load failed %s: %s", uri, err ? err->message : "?");
    V.flags &= ~PSWEB_FL_LOADING;
    send_flags();
    return FALSE;                 /* let WebKit show its error page */
}

static void on_mouse_target(WebKitWebView *view, WebKitHitTestResult *hit, guint mods, gpointer u)
{
    (void) view; (void) mods; (void) u;
    const char *link = webkit_hit_test_result_context_is_link(hit) ? webkit_hit_test_result_get_link_uri(hit) : NULL;
    send_evt(PSWEB_EVT_LINK, 0, 0, link ? link : "");
}

static gboolean on_web_process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason why, gpointer u)
{
    (void) view; (void) u;
    DBG("web process terminated (%d)", (int) why);
    V.flags |= PSWEB_FL_CRASHED;
    V.flags &= ~PSWEB_FL_LOADING;
    send_evt(PSWEB_EVT_CRASHED, (int32_t) why, 0, NULL);
    send_flags();
    return TRUE;
}

/* Native dialogs are the guest's job later (design 5.6). For v1, script
 * dialogs are dismissed so a page can never wedge the engine. */
static gboolean on_script_dialog(WebKitWebView *view, WebKitScriptDialog *dlg, gpointer u)
{
    (void) view; (void) u;
    if (webkit_script_dialog_get_dialog_type(dlg) == WEBKIT_SCRIPT_DIALOG_CONFIRM ||
        webkit_script_dialog_get_dialog_type(dlg) == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM)
        webkit_script_dialog_confirm_set_confirmed(dlg, TRUE);
    return TRUE;
}

static gboolean on_permission(WebKitWebView *view, WebKitPermissionRequest *req, gpointer u)
{
    (void) view; (void) u;
    webkit_permission_request_deny(req);
    return TRUE;
}

/* ---------------------------------------------------------- the view ---- */

static void apply_blocker(void)
{
    if (!V.ucm || !g_filter)
        return;
    if (V.blocker_on)
        webkit_user_content_manager_add_filter(V.ucm, g_filter);
    else
        webkit_user_content_manager_remove_filter(V.ucm, g_filter);
}

static void view_free(void)
{
    if (!V.active)
        return;
    if (V.view) {
        g_object_unref(V.view);       /* also drops the backend it owns */
        V.view = NULL;
    }
    if (V.ucm) {
        g_object_unref(V.ucm);
        V.ucm = NULL;
    }
    free(V.staging);
    V.staging = NULL;
    V.exportable = NULL;              /* destroyed with the WebKit backend */
    V.active = 0;
    V.staging_fresh = 0;
    V.ack_pending = 0;
}

static void on_backend_destroyed(gpointer data)
{
    (void) data;
    /* WebKit is done with the wpe_view_backend: drop the fdo wrapper too */
    if (V.exportable) {
        wpe_view_backend_exportable_fdo_destroy(V.exportable);
        V.exportable = NULL;
    }
}

static int view_new(int w, int h, int bpp)
{
    if (w < 64 || h < 64 || w > PSWEB_MAX_W || h > PSWEB_MAX_H || (bpp != 16 && bpp != 32))
        return -1;
    view_free();

    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
        .export_shm_buffer = on_export_shm_buffer,
    };
    V.exportable = wpe_view_backend_exportable_fdo_create(&client, NULL, (uint32_t) w, (uint32_t) h);
    if (!V.exportable)
        return -1;
    V.backend = wpe_view_backend_exportable_fdo_get_view_backend(V.exportable);
    /* say so explicitly, so a later VIEW_STATE has something to remove */
    wpe_view_backend_add_activity_state(V.backend, wpe_view_activity_state_visible |
                                        wpe_view_activity_state_focused | wpe_view_activity_state_in_window);

    WebKitWebViewBackend *wkb = webkit_web_view_backend_new(V.backend, on_backend_destroyed, NULL);
    V.ucm = webkit_user_content_manager_new();
    V.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                          "backend", wkb,
                                          "web-context", g_ctx,
                                          "user-content-manager", V.ucm, NULL));
    V.w = w;
    V.h = h;
    V.bpp = bpp;
    V.staging = malloc((size_t) PSWEB_MAX_W * PSWEB_MAX_H * 4);
    V.staging_fresh = 0;
    V.ack_pending = 0;
    V.dropped = 0;
    V.flags = 0;
    V.pointer_mods = 0;
    V.active = 1;

    g_surf->w = (uint32_t) w;
    g_surf->h = (uint32_t) h;
    g_surf->bpp = (uint32_t) bpp;
    g_surf->stride = (uint32_t) (w * bpp / 8);
    g_surf->n_damage = 0;

    WebKitSettings *st = webkit_web_view_get_settings(V.view);
    webkit_settings_set_enable_javascript(st, V.js_on ? TRUE : FALSE);
    webkit_settings_set_enable_developer_extras(st, FALSE);
    webkit_settings_set_enable_page_cache(st, g_lean ? FALSE : TRUE);
    apply_blocker();

    g_signal_connect(V.view, "notify::title", G_CALLBACK(on_notify_title), NULL);
    g_signal_connect(V.view, "notify::uri", G_CALLBACK(on_notify_uri), NULL);
    g_signal_connect(V.view, "notify::estimated-load-progress", G_CALLBACK(on_notify_progress), NULL);
    g_signal_connect(V.view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(V.view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(V.view, "mouse-target-changed", G_CALLBACK(on_mouse_target), NULL);
    g_signal_connect(V.view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(V.view, "script-dialog", G_CALLBACK(on_script_dialog), NULL);
    g_signal_connect(V.view, "permission-request", G_CALLBACK(on_permission), NULL);

    DBG("view %dx%d @%d", w, h, bpp);
    return 1;
}

static void view_resize(int w, int h)
{
    if (!V.active || w < 64 || h < 64 || w > PSWEB_MAX_W || h > PSWEB_MAX_H)
        return;
    V.w = w;
    V.h = h;
    wpe_view_backend_dispatch_set_size(V.backend, (uint32_t) w, (uint32_t) h);
}

/* What the user typed: a URL, a host name, or words for the search engine. */
static char g_search[PSWEB_STR_MAX + 1] = "https://duckduckgo.com/?q=%s";   /* a whole command string fits */

static void view_load(const char *text)
{
    if (!V.active || !text) {
        fprintf(stderr, "[psweb] load %s: no view\n", text ? text : "(null)");
        return;
    }
    char url[PSWEB_STR_MAX + 64];
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "file:")) {
        snprintf(url, sizeof url, "%s", text);
    } else if (!strchr(text, ' ') && strchr(text, '.')) {
        snprintf(url, sizeof url, "https://%s", text);
    } else {
        char *esc = g_uri_escape_string(text, NULL, FALSE);
        char *q = strstr(g_search, "%s");
        if (q)
            snprintf(url, sizeof url, "%.*s%s%s", (int) (q - g_search), g_search, esc, q + 2);
        else
            snprintf(url, sizeof url, "%s%s", g_search, esc);
        g_free(esc);
    }
    fprintf(stderr, "[psweb] load %s\n", url);
    webkit_web_view_load_uri(V.view, url);
}

/* ------------------------------------------------------------- input ---- */

static uint32_t mods_from_kstate(int ks)
{
    uint32_t m = 0;
    if (ks & (PSWEB_KS_LSHIFT | PSWEB_KS_RSHIFT)) m |= wpe_input_keyboard_modifier_shift;
    if (ks & PSWEB_KS_CTRL) m |= wpe_input_keyboard_modifier_control;
    if (ks & PSWEB_KS_ALT) m |= wpe_input_keyboard_modifier_alt;
    return m;
}

static void input_pointer(int kind, int x, int y, int button, int ks)
{
    if (!V.active)
        return;
    struct wpe_input_pointer_event ev;
    memset(&ev, 0, sizeof ev);
    ev.time = (uint32_t) (now_us() / 1000.0);
    ev.x = x;
    ev.y = y;
    uint32_t bit = button == 1 ? wpe_input_pointer_modifier_button1 :
                   button == 2 ? wpe_input_pointer_modifier_button2 :
                   button == 3 ? wpe_input_pointer_modifier_button3 : 0;
    if (kind == 0) {
        ev.type = wpe_input_pointer_event_type_motion;
    } else {
        ev.type = wpe_input_pointer_event_type_button;
        ev.button = (uint32_t) button;      /* WebKit: 1 left, 2 right, 3 middle */
        ev.state = kind == 1 ? 1 : 0;
        if (kind == 1) V.pointer_mods |= bit; else V.pointer_mods &= ~bit;
    }
    ev.modifiers = mods_from_kstate(ks) | V.pointer_mods;
    wpe_view_backend_dispatch_pointer_event(V.backend, &ev);
}

/* dy > 0 scrolls the page down (wheel towards you). WebKit takes one wheel
 * tick per 2D motion event whatever the magnitude, so notches are sent one
 * by one; a pixel amount goes as a smooth event. */
static void input_scroll(int dx, int dy, int x, int y, int flags)
{
    if (!V.active)
        return;
    struct wpe_input_axis_2d_event ev;
    memset(&ev, 0, sizeof ev);
    ev.base.time = (uint32_t) (now_us() / 1000.0);
    ev.base.x = x;
    ev.base.y = y;
    if (flags & 1) {
        ev.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth;
        ev.x_axis = -(double) dx;
        ev.y_axis = -(double) dy;
        wpe_view_backend_dispatch_axis_event(V.backend, &ev.base);
        return;
    }
    int nx = dx / 120, ny = dy / 120;
    ev.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion;
    for (int i = 0; i < abs(ny); i++) {
        ev.x_axis = 0.0;
        ev.y_axis = ny > 0 ? -120.0 : 120.0;
        wpe_view_backend_dispatch_axis_event(V.backend, &ev.base);
    }
    for (int i = 0; i < abs(nx); i++) {
        ev.x_axis = nx > 0 ? -120.0 : 120.0;
        ev.y_axis = 0.0;
        wpe_view_backend_dispatch_axis_event(V.backend, &ev.base);
    }
}

/* Atari scancodes that are not characters -> X keysyms (wpe/keysyms.h values) */
static uint32_t keysym_for_scan(int scan)
{
    switch (scan) {
        case 0x01: return 0xff1b;   /* Esc          */
        case 0x0e: return 0xff08;   /* Backspace    */
        case 0x0f: return 0xff09;   /* Tab          */
        case 0x1c: return 0xff0d;   /* Return       */
        case 0x72: return 0xff8d;   /* Enter (KP)   */
        case 0x53: return 0xffff;   /* Delete       */
        case 0x52: return 0xff63;   /* Insert       */
        case 0x47: return 0xff50;   /* ClrHome -> Home */
        case 0x48: return 0xff52;   /* Up           */
        case 0x50: return 0xff54;   /* Down         */
        case 0x4b: return 0xff51;   /* Left         */
        case 0x4d: return 0xff53;   /* Right        */
        case 0x62: return 0xff55;   /* Help -> PgUp */
        case 0x61: return 0xff56;   /* Undo -> PgDn */
        case 0x3b: return 0xffbe;   /* F1           */
        case 0x3c: return 0xffbf;
        case 0x3d: return 0xffc0;
        case 0x3e: return 0xffc1;
        case 0x3f: return 0xffc2;
        case 0x40: return 0xffc3;
        case 0x41: return 0xffc4;
        case 0x42: return 0xffc5;
        case 0x43: return 0xffc6;
        case 0x44: return 0xffc7;   /* F10          */
        default:   return 0;
    }
}

/* Atari ST charset for the characters a UK/US keyboard produces above 0x7f:
 * enough for v1, the full table lives in pspdf.cpp on the host side */
static uint32_t keysym_for_atari_char(unsigned c)
{
    if (c >= 0x20 && c < 0x7f)
        return c;                       /* Latin-1 keysyms == ASCII */
    switch (c) {
        case 0x9c: return 0xa3;         /* pound sign  */
        case 0xf8: return 0xb0;         /* degree      */
        case 0xbd: return 0xa9;         /* copyright   */
        default:   return 0;
    }
}

static void input_key(int down, int aeskey, int ks)
{
    if (!V.active)
        return;
    int scan = (aeskey >> 8) & 0xff;
    unsigned ch = (unsigned) (aeskey & 0xff);
    uint32_t sym = keysym_for_scan(scan);
    if (!sym && ch) {
        /* Ctrl+letter arrives as a control code: give WebKit the letter */
        if ((ks & PSWEB_KS_CTRL) && ch < 0x20)
            ch = ch + 0x60;
        sym = keysym_for_atari_char(ch);
    }
    /* ClrHome with Shift = End */
    if (scan == 0x47 && (ks & (PSWEB_KS_LSHIFT | PSWEB_KS_RSHIFT)))
        sym = 0xff57;
    if (!sym)
        return;

    struct wpe_input_keyboard_event ev;
    memset(&ev, 0, sizeof ev);
    ev.time = (uint32_t) (now_us() / 1000.0);
    ev.key_code = sym;
    ev.hardware_key_code = 0;
    ev.pressed = down ? true : false;
    ev.modifiers = mods_from_kstate(ks);
    wpe_view_backend_dispatch_keyboard_event(V.backend, &ev);
}

static void input_text(const char *s)
{
    for (const unsigned char *p = (const unsigned char *) s; p && *p; p++) {
        uint32_t sym = keysym_for_atari_char(*p);
        if (!sym) continue;
        struct wpe_input_keyboard_event ev;
        memset(&ev, 0, sizeof ev);
        ev.time = (uint32_t) (now_us() / 1000.0);
        ev.key_code = sym;
        ev.pressed = true;
        wpe_view_backend_dispatch_keyboard_event(V.backend, &ev);
        ev.pressed = false;
        wpe_view_backend_dispatch_keyboard_event(V.backend, &ev);
    }
}

/* ---------------------------------------------------------- settings ---- */

static void setting(int key, int value, const char *str)
{
    switch (key) {
        case PSWEB_SET_JAVASCRIPT:
            V.js_on = value ? 1 : 0;
            if (V.view)
                webkit_settings_set_enable_javascript(webkit_web_view_get_settings(V.view), V.js_on);
            break;
        case PSWEB_SET_IMAGES:
            if (V.view)
                webkit_settings_set_auto_load_images(webkit_web_view_get_settings(V.view), value ? TRUE : FALSE);
            break;
        case PSWEB_SET_UA:
            if (V.view)
                webkit_settings_set_user_agent(webkit_web_view_get_settings(V.view), value
                    ? "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15"
                    : NULL);
            break;
        case PSWEB_SET_SEARCH:
            if (str && *str)
                snprintf(g_search, sizeof g_search, "%s", str);
            break;
        case PSWEB_SET_ZOOM:
            if (V.view && value >= 25 && value <= 400)
                webkit_web_view_set_zoom_level(V.view, value / 100.0);
            break;
        case PSWEB_SET_BLOCKER:
            V.blocker_on = value ? 1 : 0;
            apply_blocker();
            break;
        default:
            break;
    }
    send_flags();
}

/* -------------------------------------------------------- commands ------ */

static void handle_cmd(const struct psweb_cmd *c, const char *str)
{
    switch (c->type) {
        case PSWEB_CMD_HELLO:
            fprintf(stderr, "[psweb] handshake: emulator speaks protocol %d, sending the surface %s (%zu bytes)\n",
                    c->a, g_shm_name, g_shm_size);
            send_evt_fd(PSWEB_EVT_HELLO, PSWEB_PROTO_VERSION, (int32_t) g_shm_size, g_shm_name, g_shm_fd);
            break;
        case PSWEB_CMD_VIEW_NEW: {
            int rc = view_new(c->a, c->b, c->c);
            fprintf(stderr, "[psweb] view %dx%d %d bpp: %s\n", c->a, c->b, c->c, rc > 0 ? "created" : "FAILED");
            send_evt(PSWEB_EVT_VIEW, rc, 0, NULL);
            send_flags();
            break;
        }
        case PSWEB_CMD_VIEW_FREE:
            view_free();
            break;
        case PSWEB_CMD_VIEW_SIZE:
            view_resize(c->a, c->b);
            break;
        case PSWEB_CMD_VIEW_STATE: {
            /* bit0 visible, bit1 focused (bit2 topped rides with focused).
             * Not visible = WebKit stops painting and throttles the page's
             * timers: the JIT's share of the L2 comes back while the
             * window is behind another or iconified. */
            if (!V.active || !V.backend) break;
            uint32_t want = 0, have = wpe_view_backend_get_activity_state(V.backend);
            if (c->a & 1) want |= wpe_view_activity_state_visible | wpe_view_activity_state_in_window;
            if (c->a & 6) want |= wpe_view_activity_state_focused;
            if (have & ~want) wpe_view_backend_remove_activity_state(V.backend, have & ~want);
            if (want & ~have) wpe_view_backend_add_activity_state(V.backend, want & ~have);
            if ((have ^ want) & wpe_view_activity_state_visible)
                fprintf(stderr, "[psweb] view %s\n", (c->a & 1) ? "visible: painting" : "hidden: paused");
            break;
        }
        case PSWEB_CMD_LOAD:
            view_load(str);
            break;
        case PSWEB_CMD_NAV:
            if (!V.active) break;
            switch (c->a) {
                case 0: webkit_web_view_go_back(V.view); break;
                case 1: webkit_web_view_go_forward(V.view); break;
                case 2: webkit_web_view_reload(V.view); break;
                case 3: webkit_web_view_reload_bypass_cache(V.view); break;
                case 4: webkit_web_view_stop_loading(V.view); break;
                default: break;
            }
            break;
        case PSWEB_CMD_POINTER:
            input_pointer(c->a, c->b, c->c, c->d, c->e);
            break;
        case PSWEB_CMD_SCROLL:
            input_scroll(c->a, c->b, c->c, c->d, c->e);
            break;
        case PSWEB_CMD_KEY:
            input_key(c->a, c->b, c->c);
            break;
        case PSWEB_CMD_TEXT:
            if (str) input_text(str);
            break;
        case PSWEB_CMD_ZOOM:
            setting(PSWEB_SET_ZOOM, c->a, NULL);
            break;
        case PSWEB_CMD_SETTING:
            setting(c->a, c->b, str);
            break;
        case PSWEB_CMD_QUIT:
            g_main_loop_quit(g_loop);
            break;
        default:
            DBG("unknown command %u", c->type);
            break;
    }
}

static void client_close(void)
{
    if (g_client_src) {
        g_source_remove(g_client_src);
        g_client_src = 0;
    }
    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    g_inlen = 0;
    view_free();
    g_idle_since = g_get_monotonic_time();
    fprintf(stderr, "[psweb] emulator disconnected\n");
}

/* No client for PSWEB_IDLE_S: give the memory back. psweb.socket brings
 * us back on the next connect, so the guest notices nothing but a slower
 * first page. */
static gboolean on_idle_check(gpointer user)
{
    (void) user;
    if (g_idle_s > 0 && g_client_fd < 0 &&
        g_get_monotonic_time() - g_idle_since > (gint64) g_idle_s * G_USEC_PER_SEC) {
        fprintf(stderr, "[psweb] idle for %d s, exiting\n", g_idle_s);
        g_main_loop_quit(g_loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_client_readable(gint fd, GIOCondition cond, gpointer user)
{
    (void) user;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        client_close();
        return G_SOURCE_REMOVE;
    }
    ssize_t n = read(fd, g_inbuf + g_inlen, sizeof g_inbuf - g_inlen);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return G_SOURCE_CONTINUE;
        client_close();
        return G_SOURCE_REMOVE;
    }
    g_inlen += (size_t) n;

    for (;;) {
        if (g_inlen < sizeof(struct psweb_cmd))
            break;
        struct psweb_cmd c;
        memcpy(&c, g_inbuf, sizeof c);
        if (c.len > PSWEB_STR_MAX) {
            DBG("bad string length %u, dropping client", c.len);
            client_close();
            return G_SOURCE_REMOVE;
        }
        size_t need = sizeof c + c.len;
        if (g_inlen < need)
            break;
        char str[PSWEB_STR_MAX + 1];
        memcpy(str, g_inbuf + sizeof c, c.len);
        str[c.len] = 0;
        handle_cmd(&c, c.len ? str : NULL);
        memmove(g_inbuf, g_inbuf + need, g_inlen - need);
        g_inlen -= need;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_listen(gint fd, GIOCondition cond, gpointer user)
{
    (void) cond; (void) user;
    int c = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0)
        return G_SOURCE_CONTINUE;
    if (g_client_fd >= 0) {
        DBG("second client refused");
        close(c);
        return G_SOURCE_CONTINUE;
    }
    g_client_fd = c;
    g_inlen = 0;
    g_client_src = g_unix_fd_add(c, G_IO_IN | G_IO_HUP | G_IO_ERR, on_client_readable, NULL);
    fprintf(stderr, "[psweb] emulator connected\n");
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------ setup ----- */

static int shm_create(void)
{
    snprintf(g_shm_name, sizeof g_shm_name, "/psweb-%d", (int) getpid());
    shm_unlink(g_shm_name);
    int fd = shm_open(g_shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return -1;
    }
    /* the emulator maps this read-write (it writes the consumed serial)
     * and under psweb.service it is another user: 0644 left it stuck on
     * "Connecting to psweb..." with EACCES on its side. fchmod beats the
     * umask. */
    fchmod(fd, 0666);
    size_t pix = (size_t) PSWEB_MAX_W * PSWEB_MAX_H * 4;
    size_t hdr = 4096;
    g_shm_size = hdr + pix;
    if (ftruncate(fd, (off_t) g_shm_size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    g_shm = mmap(NULL, g_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_shm == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    g_shm_fd = fd;                      /* kept: handed to the emulator in HELLO */
    memset(g_shm, 0, hdr);
    struct psweb_shm_hdr *h = (struct psweb_shm_hdr *) g_shm;
    h->magic = PSWEB_SHM_MAGIC;
    h->version = PSWEB_PROTO_VERSION;
    h->size = (uint32_t) g_shm_size;
    h->surface_off = 256;
    h->pixels_off = (uint32_t) hdr;
    h->pixels_bytes = (uint32_t) pix;
    g_surf = (struct psweb_surface *) (g_shm + h->surface_off);
    g_pixels = g_shm + hdr;
    return 0;
}

/* systemd socket activation: LISTEN_PID/LISTEN_FDS name fd 3 as ours */
static int listen_inherited(void)
{
    const char *pid = getenv("LISTEN_PID"), *n = getenv("LISTEN_FDS");
    if (!pid || !n || (pid_t) atol(pid) != getpid() || atoi(n) < 1)
        return -1;
    int fd = 3;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -1;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    g_listen_inherited = 1;
    return fd;
}

static int listen_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *) &sa, sizeof sa) < 0 || listen(fd, 1) < 0) {
        perror(path);
        close(fd);
        return -1;
    }
    chmod(path, 0666);          /* the emulator runs as root anyway */
    return fd;
}

static void set_affinity(void)
{
    const char *e = getenv("PSWEB_CPUS");
    unsigned long mask = (e && *e) ? strtoul(e, NULL, 16) : 0x2;   /* core 1 */
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
        if (mask & (1UL << i)) CPU_SET(i, &set);
    if (CPU_COUNT(&set) && sched_setaffinity(0, sizeof set, &set) < 0)
        perror("sched_setaffinity");
}

/* The blocker. Compiling 68k EasyList rules into WebKit's DFA takes a
 * minute on a Pi 4 and a few hundred MB, so the compiled form lives in the
 * filter store and is only rebuilt when the JSON changes (a stamp of its
 * mtime and size sits beside it). `psweb --compile-filter` does that
 * rebuild on its own, for install-full.sh. */
static int g_compile_only;
static char g_stamp_path[600];
static char g_stamp_now[64];

static void filter_done(void)
{
    if (g_compile_only)
        g_main_loop_quit(g_loop);
}

static void on_filter_compiled(GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GError *err = NULL;
    g_filter = webkit_user_content_filter_store_save_from_file_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), res, &err);
    if (g_filter) {
        FILE *f = fopen(g_stamp_path, "w");
        if (f) {
            fputs(g_stamp_now, f);
            fclose(f);
        }
        fprintf(stderr, "[psweb] content blocker compiled\n");
        apply_blocker();
        send_flags();
    } else {
        fprintf(stderr, "[psweb] content blocker failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    filter_done();
}

static void on_filter_loaded(GObject *src, GAsyncResult *res, gpointer user)
{
    GError *err = NULL;
    g_filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), res, &err);
    if (g_filter) {
        fprintf(stderr, "[psweb] content blocker loaded from the store\n");
        apply_blocker();
        send_flags();
        filter_done();
        return;
    }
    g_clear_error(&err);
    fprintf(stderr, "[psweb] compiling the content blocker (first run, or the rules changed)\n");
    GFile *f = g_file_new_for_path((const char *) user);
    webkit_user_content_filter_store_save_from_file(WEBKIT_USER_CONTENT_FILTER_STORE(src), "psweb", f, NULL,
                                                    on_filter_compiled, NULL);
    g_object_unref(f);
}

static int load_filter(void)
{
    static char path_buf[512];
    const char *path = getenv("PSWEB_FILTER");
    if (!path || !*path)
        path = "/etc/psweb/adblock.json";
    struct stat st_json;
    if (stat(path, &st_json) != 0) {
        fprintf(stderr, "[psweb] no content blocker rules at %s\n", path);
        return 0;
    }
    snprintf(path_buf, sizeof path_buf, "%s", path);
    char store[512];
    snprintf(store, sizeof store, "%s/psweb/filters", g_get_user_cache_dir());
    g_mkdir_with_parents(store, 0700);
    snprintf(g_stamp_path, sizeof g_stamp_path, "%s/psweb.stamp", store);
    snprintf(g_stamp_now, sizeof g_stamp_now, "%lld %lld\n",
             (long long) st_json.st_mtime, (long long) st_json.st_size);

    char have[64] = "";
    FILE *f = fopen(g_stamp_path, "r");
    if (f) {
        if (!fgets(have, sizeof have, f))
            have[0] = 0;
        fclose(f);
    }
    WebKitUserContentFilterStore *st = webkit_user_content_filter_store_new(store);
    if (strcmp(have, g_stamp_now) == 0) {
        webkit_user_content_filter_store_load(st, "psweb", NULL, on_filter_loaded, path_buf);
    } else {
        fprintf(stderr, "[psweb] compiling the content blocker from %s\n", path);
        GFile *gf = g_file_new_for_path(path);
        webkit_user_content_filter_store_save_from_file(st, "psweb", gf, NULL, on_filter_compiled, NULL);
        g_object_unref(gf);
    }
    return 1;
}

static gboolean on_sigterm(gpointer user)
{
    (void) user;
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    g_debug = getenv("PSWEB_DEBUG") != NULL;
    if (argc > 1 && strcmp(argv[1], "--compile-filter") == 0) {
        /* compile $PSWEB_FILTER into the store and exit: no socket, no view */
        g_compile_only = 1;
        g_loop = g_main_loop_new(NULL, FALSE);
        if (!load_filter())
            return 1;
        g_main_loop_run(g_loop);
        return g_filter ? 0 : 1;
    }
    set_affinity();
    setenv("WEBKIT_SKIA_CPU_PAINTING_THREADS", "1", 0);   /* one core is all we have */

    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    if (!wpe_fdo_initialize_shm()) {
        fprintf(stderr, "[psweb] wpe_fdo_initialize_shm failed\n");
        return 1;
    }
    if (shm_create() < 0)
        return 1;

    const char *sock = getenv("PSWEB_SOCK");
    if (!sock || !*sock)
        sock = PSWEB_SOCK_DEFAULT;
    g_listen_fd = listen_inherited();
    if (g_listen_fd < 0)
        g_listen_fd = listen_socket(sock);
    if (g_listen_fd < 0)
        return 1;

    /* One web context for every view, with a memory ceiling the web
     * process polices itself: WebKit sheds caches at the conservative and
     * strict thresholds and is killed at the kill threshold. On this Pi the
     * kernel cgroup limit is unavailable (cgroup_disable=memory), so this
     * is the wall between a heavy page and the emulator's own memory. */
    {
        const char *e = getenv("PSWEB_MEM_MB");
        long mb = (e && *e) ? atol(e) : 0;
        if (mb <= 0) {
            long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
            mb = (pages > 0 && psz > 0) ? (long) ((double) pages * psz / (1024.0 * 1024.0) * 0.4) : 512;
        }
        WebKitMemoryPressureSettings *mps = webkit_memory_pressure_settings_new();
        webkit_memory_pressure_settings_set_memory_limit(mps, (guint) mb);
        /* order matters: each must sit below the next (defaults 0.33/0.5) */
        webkit_memory_pressure_settings_set_kill_threshold(mps, 1.5);
        webkit_memory_pressure_settings_set_strict_threshold(mps, 0.8);
        webkit_memory_pressure_settings_set_conservative_threshold(mps, 0.5);
        webkit_memory_pressure_settings_set_poll_interval(mps, 5.0);
        g_ctx = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
                                                "memory-pressure-settings", mps, NULL));
        webkit_memory_pressure_settings_free(mps);
        /* WPE always swaps web processes on a cross-site navigation and
         * keeps the old one alive holding the previous page for back and
         * forward: on a 2 GB Pi that was three WebProcesses, 930 MB
         * resident, for one window (top, 16 Sep). The "document browser"
         * cache model and no page cache make the old process go as soon
         * as the swap is done; back reloads instead. PSWEB_CACHE=full
         * restores the desktop-browser behaviour. */
        const char *cm = getenv("PSWEB_CACHE");
        g_lean = !(cm && strcmp(cm, "full") == 0);
        webkit_web_context_set_cache_model(g_ctx, g_lean ? WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER
                                                         : WEBKIT_CACHE_MODEL_WEB_BROWSER);
        fprintf(stderr, "[psweb] web process memory limit %ld MB (killed at %ld MB), %s caches\n",
                mb, mb * 3 / 2, g_lean ? "lean" : "full");
    }
    {
        const char *e = getenv("PSWEB_IDLE_S");
        g_idle_s = (e && *e) ? atoi(e) : 600;
        g_idle_since = g_get_monotonic_time();
    }

    V.js_on = 1;
    V.blocker_on = 1;
    load_filter();

    g_loop = g_main_loop_new(NULL, FALSE);
    g_unix_fd_add(g_listen_fd, G_IO_IN, on_listen, NULL);
    g_timeout_add(4, on_tick, NULL);
    g_timeout_add_seconds(10, on_idle_check, NULL);
    g_unix_signal_add(SIGTERM, on_sigterm, NULL);
    g_unix_signal_add(SIGINT, on_sigterm, NULL);

    fprintf(stderr, "[psweb] ready on %s, shm %s (%zu bytes)\n",
            g_listen_inherited ? "the systemd socket" : sock, g_shm_name, g_shm_size);
    g_main_loop_run(g_loop);

    client_close();
    close(g_listen_fd);
    if (!g_listen_inherited)
        unlink(sock);
    shm_unlink(g_shm_name);
    return 0;
}
