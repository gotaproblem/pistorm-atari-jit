/*
 * webbench.c - phase 0(d) of the APJ-OS browser: what psweb's frame path costs.
 *
 * Renders a page with WPE WebKit off-screen through WPEBackend-fdo's SHM
 * path - no display, no compositor, no GL - converts every frame to the
 * fVDI 32bpp format the Atari wants (00 RR GG BB), and times it. Then it
 * scrolls for five seconds and counts frames, which is the number that
 * says whether this can feel like a browser.
 *
 * This is the same shape psweb will have: export_shm_buffer -> convert ->
 * hand on -> release -> frame_complete.
 *
 *   ./webbench <url> [seconds] [outdir] [WxH]
 *
 * Env worth trying:
 *   WEBKIT_SKIA_ENABLE_CPU_RENDERING=0  tile painting on the GPU (V3D)
 *   WEBKIT_SKIA_CPU_PAINTING_THREADS=n  CPU painter worker count
 *   WEBBENCH_NOJS=1                     load with JavaScript off
 *   WEBBENCH_FILTER=rules.json          WebKit content blocker rules
 *
 * Writes first.ppm and last.ppm (plain P6, any viewer opens them).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <glib.h>
#include <gio/gio.h>
#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>
#include <wayland-server.h>

static int VIEW_W = 1920;  /* override with the WxH argument */
static int VIEW_H = 1016;  /* 1080 minus a toolbar and a status strip */
#define SCROLL_MS 5000
#define TICK_MS 16

static struct {
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    WebKitWebView *view;
    GMainLoop *loop;

    uint32_t *dst;            /* the "TT-RAM buffer" */
    int dst_w, dst_h;

    int frames, scroll_frames, gpu_frames;
    double convert_total_ms, convert_max_ms;
    double bytes_total;

    gboolean ack_pending, scrolling, saved_first;
    double t0, t_load, t_first_frame, t_scroll_start;

    char outdir[512];
    char url[2048];
    int timeout_s;
} A;


static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static void load_now(void)
{
    A.t0 = now_ms();
    printf("loading %s at %dx%d (SHM path, no display)\n", A.url, VIEW_W, VIEW_H);
    webkit_web_view_load_uri(A.view, A.url);
}

/* ARGB8888 little-endian (what the web process gives us) -> the fVDI 32bpp
 * byte order 00 RR GG BB. This is the loop psweb runs on core 1. */
static void convert_frame(const uint8_t *src, int stride, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *) (src + (size_t) y * stride);
        uint8_t *d = (uint8_t *) (A.dst + (size_t) y * A.dst_w);
        for (int x = 0; x < w; x++) {
            uint32_t p = s[x];
            d[0] = 0;
            d[1] = (uint8_t) (p >> 16);
            d[2] = (uint8_t) (p >> 8);
            d[3] = (uint8_t) p;
            d += 4;
        }
    }
}

static void save_ppm(const char *name)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s", A.outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", A.dst_w, A.dst_h);
    for (int i = 0; i < A.dst_w * A.dst_h; i++) {
        const uint8_t *p = (const uint8_t *) &A.dst[i];
        fwrite(p + 1, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static void on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer)
{
    (void) data;
    struct wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    if (shm) {
        wl_shm_buffer_begin_access(shm);
        const uint8_t *src = wl_shm_buffer_get_data(shm);
        int w = wl_shm_buffer_get_width(shm);
        int h = wl_shm_buffer_get_height(shm);
        int stride = wl_shm_buffer_get_stride(shm);

        if (w > A.dst_w) w = A.dst_w;
        if (h > A.dst_h) h = A.dst_h;

        double t = now_ms();
        convert_frame(src, stride, w, h);
        double ms = now_ms() - t;
        wl_shm_buffer_end_access(shm);

        A.frames++;
        A.convert_total_ms += ms;
        if (ms > A.convert_max_ms) A.convert_max_ms = ms;
        A.bytes_total += (double) w * h * 4.0;
        if (A.scrolling) A.scroll_frames++;
        if (A.t_first_frame == 0.0) A.t_first_frame = now_ms();

        if (!A.saved_first && A.t_load != 0.0) {
            save_ppm("first.ppm");
            A.saved_first = TRUE;
        }
    }

    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(A.exportable, buffer);
    A.ack_pending = TRUE;
}

/* With GPU tile painting (WEBKIT_SKIA_ENABLE_CPU_RENDERING=0) the web
 * process hands back GL buffers instead of shared memory. We cannot read
 * those without EGL, but we must still release them and acknowledge the
 * frame, or the engine stalls after the first one - which is exactly what
 * a run with no dmabuf handler looks like. Counting them separates "the
 * engine drew nothing" from "the engine drew, in a form we can't copy". */
static void on_export_buffer_resource(void *data, struct wl_resource *resource)
{
    (void) data;
    A.gpu_frames++;
    if (A.scrolling) A.scroll_frames++;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(A.exportable, resource);
    A.ack_pending = TRUE;
}

static void on_export_dmabuf_resource(void *data,
                                      struct wpe_view_backend_exportable_fdo_dmabuf_resource *res)
{
    (void) data;
    A.gpu_frames++;
    if (A.scrolling) A.scroll_frames++;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(A.exportable, res->buffer_resource);
    A.ack_pending = TRUE;
}

/* The guest's poll loop is what paces psweb; here a timer stands in for it. */
static gboolean on_tick(gpointer user)
{
    (void) user;
    if (A.ack_pending) {
        A.ack_pending = FALSE;
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(A.exportable);
    }
    return G_SOURCE_CONTINUE;
}

static void report_and_quit(void)
{
    save_ppm("last.ppm");

    double load_ms = A.t_load ? A.t_load - A.t0 : -1.0;
    double first_ms = A.t_first_frame ? A.t_first_frame - A.t0 : -1.0;
    double avg = A.frames ? A.convert_total_ms / A.frames : 0.0;
    double scroll_s = SCROLL_MS / 1000.0;

    printf("\n--- webbench ---------------------------------------------\n");
    printf("view                 %dx%d\n", A.dst_w, A.dst_h);
    printf("load finished        %.0f ms\n", load_ms);
    printf("first frame          %.0f ms\n", first_ms);
    printf("frames total         %d shared-memory", A.frames);
    if (A.gpu_frames)
        printf(", %d GPU (not readable without EGL)", A.gpu_frames);
    printf("\n");
    printf("frames while scrolling %d in %.0f s  = %.1f fps\n",
           A.scroll_frames, scroll_s, A.scroll_frames / scroll_s);
    printf("convert per frame    %.1f ms avg, %.1f ms worst\n", avg, A.convert_max_ms);
    printf("pixels converted     %.0f MB total\n", A.bytes_total / 1e6);
    printf("----------------------------------------------------------\n");
    printf("A frame has to be converted (above), copied into TT-RAM and\n");
    printf("blitted by fVDI. PSPDF measured that last pair at ~5 ms warm.\n");

    g_main_loop_quit(A.loop);
}

static gboolean on_scroll_tick(gpointer user)
{
    (void) user;
    if (now_ms() - A.t_scroll_start > SCROLL_MS) {
        A.scrolling = FALSE;
        report_and_quit();
        return G_SOURCE_REMOVE;
    }

    struct wpe_input_axis_2d_event ev;
    memset(&ev, 0, sizeof ev);
    ev.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion;
    ev.base.time = (uint32_t) now_ms();
    ev.base.x = VIEW_W / 2;
    ev.base.y = VIEW_H / 2;
    ev.x_axis = 0.0;
    ev.y_axis = -120.0 * 3.0;          /* three wheel clicks down */
    wpe_view_backend_dispatch_axis_event(A.backend, &ev.base);
    return G_SOURCE_CONTINUE;
}

static gboolean start_scrolling(gpointer user)
{
    (void) user;
    printf("scrolling for %d s...\n", SCROLL_MS / 1000);
    A.scrolling = TRUE;
    A.scroll_frames = 0;
    A.t_scroll_start = now_ms();
    g_timeout_add(TICK_MS, on_scroll_tick, NULL);
    return G_SOURCE_REMOVE;
}

static void on_filter_ready(GObject *source, GAsyncResult *result, gpointer user)
{
    GError *error = NULL;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_from_file_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
    if (filter) {
        webkit_user_content_manager_add_filter(WEBKIT_USER_CONTENT_MANAGER(user), filter);
        webkit_user_content_filter_unref(filter);
        printf("content filter active\n");
    } else {
        fprintf(stderr, "content filter failed: %s\n", error ? error->message : "?");
        g_clear_error(&error);
    }
    load_now();
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user)
{
    (void) view; (void) user;
    if (event == WEBKIT_LOAD_FINISHED && A.t_load == 0.0) {
        A.t_load = now_ms();
        printf("loaded in %.0f ms\n", A.t_load - A.t0);
        g_timeout_add(1000, start_scrolling, NULL);
    }
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
                               const char *uri, GError *error, gpointer user)
{
    (void) view; (void) event; (void) user;
    fprintf(stderr, "load failed: %s: %s\n", uri, error ? error->message : "?");
    g_main_loop_quit(A.loop);
    return TRUE;
}

static gboolean on_timeout(gpointer user)
{
    (void) user;
    fprintf(stderr, "timed out after %d s\n", A.timeout_s);
    report_and_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <url> [timeout-seconds] [outdir] [WxH]\n", argv[0]);
        return 2;
    }
    snprintf(A.url, sizeof A.url, "%s", argv[1]);
    const char *filter_path = getenv("WEBBENCH_FILTER");
    A.timeout_s = (argc > 2) ? atoi(argv[2]) : 60;
    snprintf(A.outdir, sizeof A.outdir, "%s", (argc > 3) ? argv[3] : ".");
    if (argc > 4) {
        int w = 0, h = 0;
        if (sscanf(argv[4], "%dx%d", &w, &h) == 2 && w > 63 && h > 63) {
            VIEW_W = w;
            VIEW_H = h;
        } else {
            fprintf(stderr, "bad size '%s', keeping %dx%d\n", argv[4], VIEW_W, VIEW_H);
        }
    }

    A.dst_w = VIEW_W;
    A.dst_h = VIEW_H;
    A.dst = calloc((size_t) A.dst_w * A.dst_h, 4);
    if (!A.dst) { fprintf(stderr, "out of memory for the frame buffer\n"); return 1; }

    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    if (!wpe_fdo_initialize_shm()) {
        fprintf(stderr, "wpe_fdo_initialize_shm() failed - no SHM rendering path\n");
        return 1;
    }

    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
        .export_shm_buffer = on_export_shm_buffer,
    };
    A.exportable = wpe_view_backend_exportable_fdo_create(&client, NULL, VIEW_W, VIEW_H);
    if (!A.exportable) { fprintf(stderr, "exportable_fdo_create failed\n"); return 1; }
    A.backend = wpe_view_backend_exportable_fdo_get_view_backend(A.exportable);

    WebKitWebViewBackend *wk_backend = webkit_web_view_backend_new(A.backend, NULL, NULL);
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    A.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                          "backend", wk_backend,
                                          "user-content-manager", ucm, NULL));

    if (getenv("WEBBENCH_NOJS")) {
        webkit_settings_set_enable_javascript(webkit_web_view_get_settings(A.view), FALSE);
        printf("JavaScript disabled\n");
    }
    g_signal_connect(A.view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(A.view, "load-failed", G_CALLBACK(on_load_failed), NULL);

    A.loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(TICK_MS, on_tick, NULL);
    g_timeout_add_seconds(A.timeout_s, on_timeout, NULL);

    if (filter_path && *filter_path) {
        char store_dir[640];
        snprintf(store_dir, sizeof store_dir, "%s/filters", A.outdir);
        WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(store_dir);
        GFile *file = g_file_new_for_path(filter_path);
        printf("compiling content filter %s ...\n", filter_path);
        webkit_user_content_filter_store_save_from_file(store, "webbench", file, NULL,
                                                        on_filter_ready, ucm);
        g_object_unref(file);
    } else {
        load_now();
    }

    g_main_loop_run(A.loop);
    free(A.dst);
    return 0;
}
