/* SPDX-License-Identifier: MIT
 *
 * stbox_psgfx_hook.c - hook profiles for PSGFX in STBOX (see stbox_psgfx.h).
 *
 * The expression compiler runs on the host when a profile loads. The
 * evaluator and stbox_psgfx_fire() run on core 3 inside the box's
 * instruction hook, under ipl_task's rule: memory only, no syscalls, no
 * locks, bounded work - no loops beyond the fixed hook/arg/opcode counts,
 * reads clamped to box RAM, a full ring drops the record and counts it.
 */
#include "stbox_psgfx.h"
#include "stbox.h"
#include "../../../third_party/musashi/m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

stbox_psgfx_rec stbox_psgfx_ring[STBOX_PSGFX_RING];
volatile uint32_t stbox_psgfx_head, stbox_psgfx_tail, stbox_psgfx_drops;
const ph_table *volatile stbox_psgfx_table;
volatile int stbox_psgfx_armed;

enum { O_NUM, O_REG, O_B, O_W, O_L, O_SW, O_NEG, O_ADD, O_SUB, O_MUL, O_AND, O_OR, O_SHL, O_SHR, O_EQ, O_NE, O_LT, O_GE };

typedef struct { const char *s; ph_expr *e; const char *err; } P;

static void emit(P *p, int op, int32_t v)
{
    if (p->e->n >= PH_MAXCODE) { p->err = "expression too long"; return; }
    p->e->c[p->e->n].op = op; p->e->c[p->e->n].v = v; p->e->n++;
}
static void ws(P *p) { while (*p->s == ' ' || *p->s == '\t') p->s++; }
static void expr(P *p);
static void prim(P *p)
{
    ws(p);
    const char *s = p->s;
    if (*s == '(') { p->s++; expr(p); ws(p); if (*p->s != ')') p->err = "missing )"; else p->s++; return; }
    if (*s == '-') { p->s++; prim(p); emit(p, O_NEG, 0); return; }
    if ((s[0] == 'B' || s[0] == 'W' || s[0] == 'L') && s[1] == '[') {
        int op = s[0] == 'B' ? O_B : s[0] == 'W' ? O_W : O_L;
        p->s += 2; expr(p); ws(p); if (*p->s != ']') { p->err = "missing ]"; return; } p->s++;
        emit(p, op, 0); return;
    }
    if (!strncmp(s, "sw(", 3)) { p->s += 3; expr(p); ws(p); if (*p->s != ')') { p->err = "missing )"; return; } p->s++; emit(p, O_SW, 0); return; }
    if ((s[0] == 'd' || s[0] == 'a') && s[1] >= '0' && s[1] <= '7' && !isalnum((unsigned char)s[2])) {
        emit(p, O_REG, (s[0] == 'a' ? 8 : 0) + s[1] - '0'); p->s += 2; return;
    }
    if (isdigit((unsigned char)*s)) { char *e; long v = strtol(s, &e, 0); p->s = e; emit(p, O_NUM, (int32_t)v); return; }
    p->err = "bad token";
}
static void term(P *p) { prim(p); for (;;) { ws(p); if (*p->s == '*') { p->s++; prim(p); emit(p, O_MUL, 0); } else break; } }
static void sum(P *p)
{
    term(p);
    for (;;) { ws(p);
        if (*p->s == '+') { p->s++; term(p); emit(p, O_ADD, 0); }
        else if (*p->s == '-') { p->s++; term(p); emit(p, O_SUB, 0); }
        else break; }
}
static void shift(P *p)
{
    sum(p);
    for (;;) { ws(p);
        if (!strncmp(p->s, "<<", 2)) { p->s += 2; sum(p); emit(p, O_SHL, 0); }
        else if (!strncmp(p->s, ">>", 2)) { p->s += 2; sum(p); emit(p, O_SHR, 0); }
        else break; }
}
static void cmp(P *p)
{
    shift(p);
    for (;;) { ws(p);
        if (!strncmp(p->s, "==", 2)) { p->s += 2; shift(p); emit(p, O_EQ, 0); }
        else if (!strncmp(p->s, "!=", 2)) { p->s += 2; shift(p); emit(p, O_NE, 0); }
        else if (!strncmp(p->s, ">=", 2)) { p->s += 2; shift(p); emit(p, O_GE, 0); }
        else if (*p->s == '<' && p->s[1] != '<') { p->s++; shift(p); emit(p, O_LT, 0); }
        else break; }
}
static void expr(P *p)
{
    cmp(p);
    for (;;) { ws(p);
        if (*p->s == '&') { p->s++; cmp(p); emit(p, O_AND, 0); }
        else if (*p->s == '|') { p->s++; cmp(p); emit(p, O_OR, 0); }
        else break; }
}
static int compile(const char *src, ph_expr *e, const char **end, const char **err)
{
    P p = {src, e, NULL}; e->n = 0; expr(&p); *end = p.s; *err = p.err;
    return p.err ? -1 : 0;
}

static inline uint32_t rd8(const uint8_t *m, uint32_t sz, uint32_t a) { a &= 0xffffff; return a < sz ? m[a] : 0; }
static int32_t eval(const ph_expr *e, const uint32_t r[16], const uint8_t *m, uint32_t sz)
{
    int32_t st[PH_MAXCODE]; int sp = 0;
    for (int i = 0; i < e->n; i++) {
        const ph_ins *c = &e->c[i]; int32_t b;
        switch (c->op) {
        case O_NUM: st[sp++] = c->v; break;
        case O_REG: st[sp++] = (int32_t)r[c->v]; break;
        case O_B: st[sp - 1] = (int32_t)rd8(m, sz, st[sp - 1]); break;
        case O_W: { uint32_t a = st[sp - 1]; st[sp - 1] = (int16_t)((rd8(m, sz, a) << 8) | rd8(m, sz, a + 1)); break; }
        case O_L: { uint32_t a = st[sp - 1]; st[sp - 1] = (int32_t)((rd8(m, sz, a) << 24) | (rd8(m, sz, a + 1) << 16) | (rd8(m, sz, a + 2) << 8) | rd8(m, sz, a + 3)); break; }
        case O_SW: st[sp - 1] = (int16_t)(st[sp - 1] & 0xffff); break;
        case O_NEG: st[sp - 1] = -st[sp - 1]; break;
        default:
            b = st[--sp];
            switch (c->op) {
            case O_ADD: st[sp - 1] += b; break;
            case O_SUB: st[sp - 1] -= b; break;
            case O_MUL: st[sp - 1] *= b; break;
            case O_AND: st[sp - 1] &= b; break;
            case O_OR:  st[sp - 1] |= b; break;
            case O_SHL: st[sp - 1] = (int32_t)((uint32_t)st[sp - 1] << (b & 31)); break;
            case O_SHR: st[sp - 1] = (int32_t)((uint32_t)st[sp - 1] >> (b & 31)); break;
            case O_EQ:  st[sp - 1] = st[sp - 1] == b; break;
            case O_NE:  st[sp - 1] = st[sp - 1] != b; break;
            case O_LT:  st[sp - 1] = st[sp - 1] < b; break;
            case O_GE:  st[sp - 1] = st[sp - 1] >= b; break;
            }
        }
    }
    return sp ? st[sp - 1] : 0;
}

int ph_add(ph_table *t, const char *line, char *err, int errlen)
{
    while (*line == ' ') line++;
    if (!*line || *line == '#' || *line == '\n') return 0;
    if (t->n >= PH_MAXHOOK) { snprintf(err, errlen, "too many hooks"); return -1; }
    ph_hook *h = &t->h[t->n]; memset(h, 0, sizeof *h);
    char *e;
    h->pc = strtoul(line, &e, 16) & 0xffffff; h->op = strtol(e, &e, 0);
    const char *s = e;
    for (;;) {
        while (*s == ' ') s++;
        if (!strncmp(s, "expect=", 7)) {
            s += 7; while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) && h->nexpect < 8) {
                unsigned v; sscanf(s, "%2x", &v); h->expect[h->nexpect++] = v; s += 2; }
        } else if (!strncmp(s, "when=", 5)) {
            const char *end, *er;
            if (compile(s + 5, &h->when, &end, &er)) { snprintf(err, errlen, "when: %s", er); return -1; }
            h->has_when = 1; s = end;
        } else break;
    }
    if (*s != ':') { snprintf(err, errlen, "expected ':'"); return -1; }
    s++;
    while (*s && *s != '\n') {
        if (h->nargs >= PH_MAXARGS) { snprintf(err, errlen, "too many args"); return -1; }
        const char *end, *er;
        if (compile(s, &h->arg[h->nargs], &end, &er)) { snprintf(err, errlen, "arg %d: %s at '%.12s'", h->nargs, er, s); return -1; }
        h->nargs++; s = end; while (*s == ' ') s++;
        if (*s == ';') s++;
    }
    t->bitmap[h->pc >> 4] |= 1 << ((h->pc >> 1) & 7);
    t->n++;
    return 1;
}

void stbox_psgfx_fire(uint32_t pc)
{
    const ph_table *t = stbox_psgfx_table;
    pc &= 0xffffff;
    for (int i = 0; i < t->n; i++) {
        const ph_hook *h = &t->h[i];
        if (h->pc != pc) continue;
        uint32_t r[16];
        for (int k = 0; k < 16; k++)
            r[k] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + k));
        const uint8_t *ram = stbox_shared.ram; uint32_t sz = stbox_shared.ram_size;
        if (h->has_when && !eval(&h->when, r, ram, sz)) return;
        uint32_t hd = stbox_psgfx_head;
        if (hd - stbox_psgfx_tail >= STBOX_PSGFX_RING) { stbox_psgfx_drops++; return; }
        stbox_psgfx_rec *e = &stbox_psgfx_ring[hd & (STBOX_PSGFX_RING - 1)];
        e->vbl = stbox_shared.frame; e->op = (int16_t)h->op; e->n = (int16_t)h->nargs;
        for (int k = 0; k < h->nargs; k++) e->a[k] = eval(&h->arg[k], r, ram, sz);
        __atomic_store_n(&stbox_psgfx_head, hd + 1, __ATOMIC_RELEASE);
        return;
    }
}

int ph_verify(const ph_table *t, const uint8_t *ram, uint32_t sz)
{
    for (int i = 0; i < t->n; i++)
        for (int k = 0; k < t->h[i].nexpect; k++)
            if (rd8(ram, sz, t->h[i].pc + k) != t->h[i].expect[k]) return 0;
    return t->n > 0;
}
