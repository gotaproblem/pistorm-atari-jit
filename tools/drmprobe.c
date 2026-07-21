/* drmprobe - enumerate DRM/KMS planes and pixel formats on the Pi 4 (vc4).
 *
 * Purpose: decide whether the fVDI natfeat surface (guest pixels stored as
 * bytes 00,R,G,B = DRM fourcc BGRX8888 / BX24) can be scanned out directly
 * by the HVS, which would let the emulator present frames with a plain
 * dirty-row memcpy + pageflip: no CPU swizzle, no GL texture upload, no GPU
 * composite pass.
 *
 * Build (on the Pi; libdrm-dev is already installed for the emulator build):
 *   gcc -O2 -o drmprobe tools/drmprobe.c -I/usr/include/libdrm -ldrm
 *
 * Run:
 *   ./drmprobe            (all queries are read-only; safe to run any time,
 *                          with or without the emulator active)
 *
 * Interesting lines in the output are marked with '***'.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static const char *fourcc_str(uint32_t f, char buf[8])
{
    buf[0] = (char)(f & 0xff);
    buf[1] = (char)((f >> 8) & 0xff);
    buf[2] = (char)((f >> 16) & 0xff);
    buf[3] = (char)((f >> 24) & 0xff);
    buf[4] = 0;
    return buf;
}

static const char *plane_type_str(uint64_t t)
{
    switch (t) {
    case 0: return "OVERLAY";
    case 1: return "PRIMARY";
    case 2: return "CURSOR";
    default: return "?";
    }
}

int main(void)
{
    char card[32];
    int fd = -1;

    for (int i = 0; i < 4; i++) {
        snprintf(card, sizeof card, "/dev/dri/card%d", i);
        fd = open(card, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmModeRes *res_try = drmModeGetResources(fd);
        if (res_try && res_try->count_crtcs > 0) {
            drmModeFreeResources(res_try);
            break;              /* this card has a display pipeline (vc4) */
        }
        if (res_try)
            drmModeFreeResources(res_try);
        close(fd);              /* v3d render node etc. - keep looking */
        fd = -1;
    }
    if (fd < 0) {
        fprintf(stderr, "no usable /dev/dri/cardN with CRTCs found\n");
        return 1;
    }
    printf("device: %s\n", card);

    drmVersionPtr ver = drmGetVersion(fd);
    if (ver) {
        printf("driver: %s (%s)\n", ver->name, ver->desc);
        drmFreeVersion(ver);
    }

    /* Without this cap the kernel hides non-legacy planes. */
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    uint64_t cap_dumb = 0;
    drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap_dumb);
    printf("*** dumb buffers: %s\n", cap_dumb ? "supported" : "NOT supported");

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed\n");
        return 1;
    }
    printf("crtcs=%d connectors=%d encoders=%d\n",
           res->count_crtcs, res->count_connectors, res->count_encoders);

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            printf("connector %u: CONNECTED, %dx%d@%d preferred\n",
                   c->connector_id,
                   c->modes[0].hdisplay, c->modes[0].vdisplay,
                   c->modes[0].vrefresh);
        drmModeFreeConnector(c);
    }

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) {
        fprintf(stderr, "drmModeGetPlaneResources failed\n");
        return 1;
    }

    int bgrx_planes = 0;
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p)
            continue;

        /* plane type property */
        uint64_t ptype = 99;
        drmModeObjectProperties *props =
            drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
                if (prop) {
                    if (strcmp(prop->name, "type") == 0)
                        ptype = props->prop_values[j];
                    drmModeFreeProperty(prop);
                }
            }
            drmModeFreeObjectProperties(props);
        }

        int has_bx24 = 0, has_xr24 = 0, has_rg16 = 0;
        for (uint32_t j = 0; j < p->count_formats; j++) {
            if (p->formats[j] == DRM_FORMAT_BGRX8888) has_bx24 = 1;
            if (p->formats[j] == DRM_FORMAT_XRGB8888) has_xr24 = 1;
            if (p->formats[j] == DRM_FORMAT_RGB565)   has_rg16 = 1;
        }

        printf("plane %u type=%s crtc_mask=0x%x formats=%u%s%s%s\n",
               p->plane_id, plane_type_str(ptype), p->possible_crtcs,
               p->count_formats,
               has_bx24 ? "  ***BGRX8888***" : "",
               has_xr24 ? " XRGB8888" : "",
               has_rg16 ? " RGB565" : "");

        if (has_bx24)
            bgrx_planes++;

        /* full format list, one line, for the record */
        printf("  ");
        for (uint32_t j = 0; j < p->count_formats; j++) {
            char b[8];
            printf("%s ", fourcc_str(p->formats[j], b));
        }
        printf("\n");
        drmModeFreePlane(p);
    }

    printf("\n*** verdict: %d plane(s) accept BGRX8888 (fVDI surface byte order)\n",
           bgrx_planes);
    printf("*** %s\n", bgrx_planes
           ? "direct scanout of the fVDI surface format is POSSIBLE"
           : "no BGRX8888 plane: DRM path would still need the swizzle pass");

    drmModeFreePlaneResources(pr);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}
