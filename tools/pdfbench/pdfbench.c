/*
 * pdfbench.c - phase 0 of the PDF viewer (see pdf-viewer-design.md)
 *
 * Host-only. Measures what the PSPDF NatFeat would cost on the Pi:
 *
 *   open     poppler parsing the file
 *   render   poppler + cairo drawing a page at the viewer's zoom
 *   convert  cairo ARGB32 -> fVDI 32bpp (00 RR GG BB), i.e. a bswap per pixel
 *   fetch    memcpy of one screen-sized viewport out of the page bitmap
 *            (what FETCH does into the guest's TT-RAM buffer)
 *
 * Nothing here talks to the guest; it is a plain Linux program so the numbers
 * can be taken on the Pi before any emulator code is written.
 *
 *   make
 *   taskset -c 1 ./pdfbench [options] file.pdf...
 *
 * Options:
 *   -w N     render width in pixels (default 1920, "fit width" at 1080p)
 *   -n N     pages to render per file (default 5, spread through the file)
 *   -a       render every page (overrides -n)
 *   -v WxH   viewport size for the fetch test (default 1920x1080)
 *   -f TEXT  also time a whole-document search for TEXT
 *   -p FILE  write the first rendered page to FILE as .png, to eyeball it
 *   -q       one summary line per file, no per-page table
 *   -m FILE  write a synthetic 200-page test PDF and exit (no PDFs needed)
 *
 * A directory argument is scanned for *.pdf / *.PDF. With no argument at all
 * the current directory is scanned.
 */
#include <poppler.h>
#include <cairo.h>
#include <cairo-pdf.h>
#include <glib.h>
#include <stdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* cairo ARGB32 (native endian 0xAARRGGBB) -> fVDI 32bpp, bytes 00 RR GG BB.
 * The real thing would do this with NEON; this is the scalar floor. */
static void convert_xrgb(const uint32_t *src, uint32_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = __builtin_bswap32(src[i] & 0x00ffffffu);
}

struct stats {
    double min, max, sum;
    int n;
};

static void stat_add(struct stats *s, double v)
{
    if (!s->n || v < s->min) s->min = v;
    if (!s->n || v > s->max) s->max = v;
    s->sum += v;
    s->n++;
}

static double stat_avg(const struct stats *s)
{
    return s->n ? s->sum / s->n : 0.0;
}


/* ------------------------------------------------------------------ */
/* -m: write a synthetic test PDF, so the benchmark can run on a Pi    */
/* with no PDFs on it. Text-heavy pages with some vector work.         */
static int make_test_pdf(const char *path, int pages)
{
    const double W = 595.0, H = 842.0;          /* A4 in points */
    cairo_surface_t *s = cairo_pdf_surface_create(path, W, H);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "%s: cannot write\n", path);
        cairo_surface_destroy(s);
        return -1;
    }
    cairo_t *cr = cairo_create(s);

    for (int p = 0; p < pages; p++) {
        char line[128];

        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 18);
        cairo_move_to(cr, 57, 70);
        snprintf(line, sizeof line, "pdfbench synthetic page %d", p + 1);
        cairo_show_text(cr, line);

        cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        for (int l = 0; l < 44; l++) {
            cairo_move_to(cr, 57, 110 + l * 14);
            snprintf(line, sizeof line,
                     "%02d  The quick brown fox jumps over the lazy dog, "
                     "0123456789, page %d, line %d of this needle test.",
                     l + 1, p + 1, l + 1);
            cairo_show_text(cr, line);
        }

        /* some vector work so the page is not pure text */
        for (int i = 0; i < 24; i++) {
            cairo_set_source_rgb(cr, (i % 3) / 3.0, (i % 5) / 5.0, (i % 7) / 7.0);
            cairo_set_line_width(cr, 0.6 + (i % 4) * 0.4);
            cairo_arc(cr, 300 + 120 * ((i % 2) ? 1 : -1), 740, 8 + i * 2,
                      0, 6.2831853);
            cairo_stroke(cr);
        }

        cairo_show_page(cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(s);
    printf("wrote %s (%d pages)\n", path, pages);
    return 0;
}

static int is_pdf_name(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".pdf"));
}

/* Add one path to the list: a file as-is, a directory walked for PDFs
 * (subdirectories included, depth-limited). When a directory holds no PDF,
 * the names it did contain are printed, which usually explains why. */
static void add_file(const char *path, char ***list, int *n, int *cap)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *list = realloc(*list, (size_t)*cap * sizeof(char *));
    }
    (*list)[(*n)++] = strdup(path);
}

static void add_path_depth(const char *path, char ***list, int *n, int *cap,
                           int depth)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        add_file(path, list, n, cap);
        return;
    }

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return;
    }

    int before = *n, shown = 0, others = 0;
    struct dirent *e;
    char seen[6][64];

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char full[4096];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);

        struct stat es;
        if (stat(full, &es) != 0) continue;

        if (S_ISDIR(es.st_mode)) {
            if (depth > 0)
                add_path_depth(full, list, n, cap, depth - 1);
            continue;
        }
        if (is_pdf_name(e->d_name)) {
            add_file(full, list, n, cap);
            continue;
        }
        if (shown < 6)
            snprintf(seen[shown++], sizeof seen[0], "%s", e->d_name);
        others++;
    }
    closedir(d);

    if (*n == before) {
        fprintf(stderr, "%s: no *.pdf here", path);
        if (others) {
            fprintf(stderr, " (%d other file%s:", others, others == 1 ? "" : "s");
            for (int i = 0; i < shown; i++)
                fprintf(stderr, " %s", seen[i]);
            fprintf(stderr, "%s)", others > shown ? " ..." : "");
        }
        fprintf(stderr, "\n");
    }
}

static void add_path(const char *path, char ***list, int *n, int *cap)
{
    add_path_depth(path, list, n, cap, 4);   /* 4 levels of subdirectories */
}

int main(int argc, char **argv)
{
    int want_w = 1920, want_pages = 5, all_pages = 0, quiet = 0;
    int vp_w = 1920, vp_h = 1080;
    const char *find = NULL, *png = NULL, *make = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "w:n:av:f:p:m:qh")) != -1) {
        switch (opt) {
        case 'w': want_w = atoi(optarg); break;
        case 'n': want_pages = atoi(optarg); break;
        case 'a': all_pages = 1; break;
        case 'v': if (sscanf(optarg, "%dx%d", &vp_w, &vp_h) != 2) {
                      fprintf(stderr, "bad -v, want WxH\n"); return 2;
                  }
                  break;
        case 'f': find = optarg; break;
        case 'p': png = optarg; break;
        case 'q': quiet = 1; break;
        case 'm': make = optarg; break;
        default:
            fprintf(stderr,
                "usage: %s [-w px] [-n pages] [-a] [-v WxH] [-f text]\n"
                "       [-p out.png] [-q] [-m test.pdf] [file.pdf | dir]...\n"
                "\nexamples:\n"
                "  %s -m /tmp/test.pdf          make a 200-page test PDF\n"
                "  taskset -c 1 %s /tmp/test.pdf\n"
                "  taskset -c 1 %s -f needle ~/pdfs\n",
                argv[0], argv[0], argv[0], argv[0]);
            return 2;
        }
    }
    if (make)
        return make_test_pdf(make, 200);

    char **files = NULL;
    int nfiles = 0, cap = 0;

    if (optind >= argc) {
        add_path(".", &files, &nfiles, &cap);   /* no argument: scan cwd */
        if (!nfiles) {
            fprintf(stderr,
                "no PDF given, and no *.pdf in the current directory.\n"
                "Make one to test with:  %s -m /tmp/test.pdf\n"
                "then:                   taskset -c 1 %s /tmp/test.pdf\n",
                argv[0], argv[0]);
            return 2;
        }
        printf("no file given - using the %d PDF(s) in the current directory\n",
               nfiles);
    } else {
        for (int i = optind; i < argc; i++)
            add_path(argv[i], &files, &nfiles, &cap);
        if (!nfiles) {
            fprintf(stderr, "nothing to do: no readable PDF in those paths\n");
            return 2;
        }
        printf("%d PDF(s) to benchmark\n", nfiles);
    }

    printf("pdfbench: poppler %s, width %d px, viewport %dx%d\n",
           poppler_get_version(), want_w, vp_w, vp_h);

    for (int a = 0; a < nfiles; a++) {
        const char *path = files[a];
        GError *err = NULL;
        /* g_filename_to_uri() insists on an absolute path */
        char *abs = realpath(path, NULL);
        char *uri = g_filename_to_uri(abs ? abs : path, NULL, &err);
        if (!uri) {
            fprintf(stderr, "%s: %s\n", path, err ? err->message : "bad path");
            g_clear_error(&err);
            free(abs);
            continue;
        }

        double t0 = now_ms();
        PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &err);
        double open_ms = now_ms() - t0;
        g_free(uri);
        free(abs);
        if (!doc) {
            fprintf(stderr, "%s: %s\n", path, err ? err->message : "open failed");
            g_clear_error(&err);
            continue;
        }

        int npages = poppler_document_get_n_pages(doc);
        int step = 1, todo = npages;
        if (!all_pages && want_pages > 0 && want_pages < npages) {
            todo = want_pages;
            step = npages / want_pages;
        }

        printf("\n%s\n  %d pages, open %.1f ms\n", path, npages, open_ms);
        if (!quiet)
            printf("  %5s %10s %8s %8s %8s %8s\n",
                   "page", "pixels", "MPix", "render", "conv", "ms/MPix");

        struct stats render = {0}, conv = {0}, fetch = {0};
        size_t peak_bytes = 0;

        for (int i = 0; i < todo; i++) {
            int idx = i * step;
            if (idx >= npages) break;

            PopplerPage *pg = poppler_document_get_page(doc, idx);
            if (!pg) continue;

            double pw, ph;
            poppler_page_get_size(pg, &pw, &ph);
            double scale = want_w / pw;
            int w = (int)(pw * scale + 0.5);
            int h = (int)(ph * scale + 0.5);

            cairo_surface_t *surf =
                cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
                fprintf(stderr, "  page %d: cairo surface %dx%d failed\n",
                        idx + 1, w, h);
                cairo_surface_destroy(surf);
                g_object_unref(pg);
                continue;
            }
            cairo_t *cr = cairo_create(surf);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_paint(cr);
            cairo_scale(cr, scale, scale);

            t0 = now_ms();
            poppler_page_render(pg, cr);
            cairo_surface_flush(surf);
            double render_ms = now_ms() - t0;

            size_t stride = cairo_image_surface_get_stride(surf);
            size_t npix = (size_t)w * h;
            uint32_t *dst = malloc(npix * 4);
            if (!dst) { fprintf(stderr, "  out of memory\n"); return 1; }

            t0 = now_ms();
            for (int y = 0; y < h; y++)
                convert_xrgb((const uint32_t *)(cairo_image_surface_get_data(surf)
                                                + (size_t)y * stride),
                             dst + (size_t)y * w, w);
            double conv_ms = now_ms() - t0;

            /* FETCH: copy one viewport out of the page, row by row, exactly as
             * the NatFeat would copy into the guest buffer. */
            int cw = vp_w < w ? vp_w : w;
            int chh = vp_h < h ? vp_h : h;
            uint32_t *vp = malloc((size_t)cw * chh * 4);
            if (!vp) { fprintf(stderr, "  out of memory\n"); return 1; }
            t0 = now_ms();
            for (int y = 0; y < chh; y++)
                memcpy(vp + (size_t)y * cw, dst + (size_t)y * w, (size_t)cw * 4);
            double fetch_ms = now_ms() - t0;

            double mpix = npix / 1e6;
            stat_add(&render, render_ms);
            stat_add(&conv, conv_ms);
            stat_add(&fetch, fetch_ms);
            if (npix * 4 > peak_bytes) peak_bytes = npix * 4;

            if (!quiet) {
                char dims[32];
                snprintf(dims, sizeof dims, "%dx%d", w, h);
                printf("  %5d %10s %8.2f %8.1f %8.1f %8.1f\n",
                       idx + 1, dims, mpix, render_ms, conv_ms,
                       mpix > 0 ? render_ms / mpix : 0.0);
            }

            if (png && i == 0) {
                cairo_surface_write_to_png(surf, png);
                printf("  wrote %s\n", png);
            }

            free(vp);
            free(dst);
            cairo_destroy(cr);
            cairo_surface_destroy(surf);
            g_object_unref(pg);
        }

        printf("  render  avg %.1f ms  min %.1f  max %.1f  (%d pages)\n",
               stat_avg(&render), render.min, render.max, render.n);
        printf("  convert avg %.1f ms   fetch avg %.2f ms   page buffer %.1f MB\n",
               stat_avg(&conv), stat_avg(&fetch), peak_bytes / 1048576.0);

        if (find) {
            int hits = 0;
            t0 = now_ms();
            for (int i = 0; i < npages; i++) {
                PopplerPage *pg = poppler_document_get_page(doc, i);
                if (!pg) continue;
                GList *l = poppler_page_find_text(pg, find);
                hits += g_list_length(l);
                g_list_free_full(l, (GDestroyNotify)poppler_rectangle_free);
                g_object_unref(pg);
            }
            printf("  find \"%s\": %d hits in %d pages, %.1f ms total\n",
                   find, hits, npages, now_ms() - t0);
        }

        g_object_unref(doc);
    }

    return 0;
}
