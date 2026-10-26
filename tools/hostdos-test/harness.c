/*
 * HOSTDOS host test bench (any Linux host, no Pi, no ROM).
 *
 * Musashi runs PSHOSTFS.PRG for real - installed by its own code, trap #1
 * hooked through the vector - over a small fake GEMDOS standing in for the
 * ROM. The HOSTDOS NatFeat is the real engine, platforms/atari/network/
 * hostdos.cpp, on a temporary folder. TEST.PRG then makes GEMDOS calls and
 * records each result; the bench checks them and the folder.
 *
 * The fake ROM answers every call that should not reach HOSTDOS with a
 * marker (-777 path call, -778 handle call, -779 Fsnext, -780 drive
 * call), so a call that went the wrong way shows. Its Pexec 4/5/6/7 and
 * Pterm follow TOS: 5/7 make a basepage owned by the caller, 6 hands
 * ownership to the child, Pterm frees what the leaver owns and resumes
 * the parent after its Pexec trap.
 *
 *   ./hostdos-bench PSHOSTFS.PRG TEST.PRG CHILD.PRG [68000|68030] [gemdos-ver]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "m68k.h"
#include "platforms/atari/network/hostdos.h"

#define RAMSIZE   0x01000000u
static uint8_t ram[RAMSIZE];

#define OSHDR     0x0800u
#define RUNP      0x0900u
#define FAKEDOS   0x0A00u   /* 7FFF rte */
#define FAKEXBIOS 0x0A04u   /* 7FFC rte */
#define FAKERET   0x0A08u   /* 7FFE rte */
#define FAULT     0x0A0Cu   /* 7FFD */
#define SSP_TOP   0x00008000u
#define SUPSTK_TOP (SSP_TOP - 0x100)   /* GEMDOS system stack top: every process starts here */
#define RESULTS   0x00008000u
#define HEAP_LO   0x00010000u
#define HEAP_HI   0x00F00000u

static int longframe;
static uint16_t gemver;
static int stop_flag, stop_code;
static int nf_id_base = 0x00100000;    /* feature index 0 -> id 1<<20 */
static char outbuf[4096];
static int outlen;

/* ---- memory ------------------------------------------------------- */
static unsigned rd8(unsigned a) { a &= 0xFFFFFF; return ram[a]; }
static unsigned rd16(unsigned a) { return (rd8(a) << 8) | rd8(a + 1); }
static unsigned rd32(unsigned a) { return (rd16(a) << 16) | rd16(a + 2); }
static void wr8(unsigned a, unsigned v) { a &= 0xFFFFFF; ram[a] = (uint8_t)v; }
static void wr16(unsigned a, unsigned v) { wr8(a, v >> 8); wr8(a + 1, v); }
static void wr32(unsigned a, unsigned v) { wr16(a, v >> 16); wr16(a + 2, v); }

unsigned int m68k_read_memory_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return rd32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { wr16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { wr32(a, v); }

static void mem_read(uint32_t a, void *d, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) ((uint8_t *)d)[i] = (uint8_t)rd8(a + i);
}
static void mem_write(uint32_t a, const void *s, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) wr8(a + i, ((const uint8_t *)s)[i]);
}

/* ---- fake memory manager ------------------------------------------- */
typedef struct { uint32_t addr, size, owner; int used; } blk_t;
static blk_t blk[256];
static uint32_t heap_next = HEAP_LO;

static uint32_t m_alloc(uint32_t size, uint32_t owner)
{
  size = (size + 15) & ~15u;
  for (int i = 0; i < 256; i++)
    if (!blk[i].used) {
      if (heap_next + size > HEAP_HI) return 0;
      blk[i].addr = heap_next; blk[i].size = size; blk[i].owner = owner; blk[i].used = 1;
      heap_next += size;
      memset(ram + blk[i].addr, 0xA5, size);   /* dirty: the loader must clear */
      return blk[i].addr;
    }
  return 0;
}
static int bad_mfree;
static int m_free(uint32_t a)
{
  for (int i = 0; i < 256; i++)
    if (blk[i].used && blk[i].addr == a) { blk[i].used = 0; return 0; }
  bad_mfree++;
  return -40;
}
static void m_free_owner(uint32_t owner)
{
  for (int i = 0; i < 256; i++)
    if (blk[i].used && blk[i].owner == owner) blk[i].used = 0;
}
static void m_chown(uint32_t a, uint32_t owner)
{
  for (int i = 0; i < 256; i++)
    if (blk[i].used && blk[i].addr == a) blk[i].owner = owner;
}
static int m_count(void)
{
  int n = 0;
  for (int i = 0; i < 256; i++) n += blk[i].used;
  return n;
}

/* ---- processes ----------------------------------------------------- */
typedef struct {
  uint32_t regs[16], usp, ssp, parent, child;
  int mode;
  int from_super;   /* the Pexec go came from supervisor mode */
  uint8_t frame[8]; /* a user-mode caller's trap frame, kept as GEMDOS does */
} ctx_t;
static ctx_t ctx[16];
static int nctx;
static uint32_t cur_drive = 2;          /* C: */

static uint32_t run(void) { return rd32(RUNP); }

static uint32_t make_bp(uint32_t cmd, uint32_t owner)
{
  uint32_t size = 0x40000;
  uint32_t bp = m_alloc(size, owner);
  uint32_t env = m_alloc(64, owner);
  memset(ram + bp, 0, 0x100);
  memset(ram + env, 0, 64);
  wr32(bp + 0, bp);
  wr32(bp + 4, bp + size);
  wr32(bp + 0x20, bp + 0x80);           /* p_dta */
  wr32(bp + 0x24, run());               /* p_parent */
  wr32(bp + 0x2C, env);
  if (cmd)
    for (int i = 0; i < 128; i++) wr8(bp + 0x80 + i, rd8(cmd + i));
  return bp;
}

/* frame at A7 (we are at FAKEDOS, supervisor): where are the args? */
static uint32_t args_of_frame(uint32_t sp)
{
  uint16_t sr = rd16(sp);
  if (sr & 0x2000)
    return sp + 6 + (longframe ? 2 : 0);
  return m68k_get_reg(NULL, M68K_REG_USP);
}

static void push_frame_and_go(uint32_t ssp, uint32_t pc, uint16_t sr)
{
  uint32_t fs = longframe ? 8 : 6;
  ssp -= fs;
  wr16(ssp, sr);
  wr32(ssp + 2, pc);
  if (longframe) wr16(ssp + 6, 0x0080);
  m68k_set_reg(M68K_REG_A7, ssp);
  m68k_set_reg(M68K_REG_PC, FAKEDOS + 2);   /* the rte */
}

static void start_child(uint32_t bp, int mode)
{
  ctx_t *c = &ctx[nctx++];
  for (int i = 0; i < 16; i++) c->regs[i] = m68k_get_reg(NULL, M68K_REG_D0 + i);
  c->usp = m68k_get_reg(NULL, M68K_REG_USP);
  c->ssp = m68k_get_reg(NULL, M68K_REG_A7);
  c->from_super = (rd16(c->ssp) & 0x2000) != 0;   /* caller's SR in the frame */
  /* A user-mode caller's context lives in its PD/user stack in GEMDOS, so
   * the child cannot touch it; keep its frame. A supervisor-mode caller's
   * frame stays where it is, on the caller's own stack - as in GEMDOS. */
  for (int i = 0; i < 8; i++) c->frame[i] = (uint8_t)rd8(c->ssp + i);
  c->parent = run();
  c->child = bp;
  c->mode = mode;
  if (mode == 6) {
    m_chown(bp, bp);
    m_chown(rd32(bp + 0x2C), bp);
  }
  wr32(RUNP, bp);
  uint32_t usp = rd32(bp + 4) - 8;
  wr32(usp + 4, bp);
  m68k_set_reg(M68K_REG_USP, usp);
  /* GEMDOS starts every process with the supervisor stack reset to the top
   * of its system stack (EmuTOS proc_go(): other_sp = &supstk[SUPSIZ], after
   * Atari's GEMDOS) - NOT below the parent's frames. So a child's traps and
   * interrupts overwrite whatever its parent left there, e.g. the parent's
   * own trap #1 frame. Field case 26 Oct 2026: !RUNME.TOS -> COREMARK.TOS,
   * HOSTDOS returned to RUNME through a frame COREMARK had overwritten with
   * its Pterm trap frame and landed back in COREMARK after the Pterm. */
  push_frame_and_go(SUPSTK_TOP, rd32(bp + 8), 0x0000);
}

static void terminate(int code, int keep)
{
  uint32_t me = run();
  if (!keep)
    m_free_owner(me);
  if (nctx == 0) {
    stop_flag = 1;
    stop_code = code;
    m68k_end_timeslice();
    return;
  }
  ctx_t *c = &ctx[--nctx];
  if (c->from_super) {
    /* Real TOS, Pexec 4/6 issued from SUPERVISOR mode (HOSTDOS runs the
     * child from inside its trap #1 handler): field case 26 Oct 2026, a
     * program on the host drive that Pexec'd another one crashed when the
     * child ended (illegal instruction at $0000000C: rts through a stale
     * user stack). Model the worst TOS may do here - only the supervisor
     * stack comes back; the USP stays the child's and d1-d7/a0-a6 are
     * whatever the child left - so HOSTDOS must keep them itself. */
    for (int i = 1; i < 15; i++)
      m68k_set_reg(M68K_REG_D0 + i, 0xDEAD0000u | (uint32_t)i);
  } else {
    for (int i = 0; i < 16; i++) m68k_set_reg(M68K_REG_D0 + i, c->regs[i]);
    m68k_set_reg(M68K_REG_USP, c->usp);
    for (int i = 0; i < 8; i++) wr8(c->ssp + i, c->frame[i]);
  }
  m68k_set_reg(M68K_REG_A7, c->ssp);
  wr32(RUNP, c->parent);
  m68k_set_reg(M68K_REG_D0, (uint32_t)code);
  m68k_set_reg(M68K_REG_PC, FAKEDOS + 2);
}

static void put_str(uint32_t a)
{
  for (;; a++) {
    int c = rd8(a);
    if (!c) break;
    if (outlen < (int)sizeof outbuf - 1) outbuf[outlen++] = (char)c;
    putchar(c);
  }
}

/* path names a drive other than ours? (fake ROM: everything not passed
 * is a failure marker) */
static void fake_gemdos(void)
{
  uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
  uint32_t a = args_of_frame(sp);
  uint16_t fn = rd16(a);
  int32_t r = -32;
  switch (fn) {
    case 0x00: terminate(0, 0); return;
    case 0x4C: terminate((int16_t)rd16(a + 2), 0); return;
    case 0x31: terminate((int16_t)rd16(a + 6), 1); return;
    case 0x02: putchar(rd16(a + 2)); r = 0; break;
    case 0x09: put_str(rd32(a + 2)); r = 0; break;
    case 0x0E: cur_drive = rd16(a + 2); r = (int32_t)rd32(0x4C2); break;
    case 0x19: r = (int32_t)cur_drive; break;
    case 0x1A: wr32(run() + 0x20, rd32(a + 2)); r = 0; break;
    case 0x2F: r = (int32_t)rd32(run() + 0x20); break;
    case 0x30: r = (int32_t)(((gemver & 0xFF) << 8) | (gemver >> 8)); break;
    case 0x48: r = (int32_t)m_alloc(rd32(a + 2), run()); break;
    case 0x49: r = m_free(rd32(a + 2)); break;
    case 0x4B: {
      uint16_t mode = rd16(a + 2);
      if (mode == 5 || mode == 7) {
        if (mode == 7 && gemver < 0x19) { r = -32; break; }
        r = (int32_t)make_bp(rd32(a + 8), run());
      } else if (mode == 4 || mode == 6) {
        if (mode == 6 && gemver < 0x15) { r = -32; break; }
        start_child(rd32(a + 8), mode);
        return;
      } else {
        r = -777;                       /* Pexec 0/3 of a TOS drive */
      }
      break;
    }
    case 0x3C: case 0x3D: case 0x41: case 0x43: case 0x39: case 0x3A:
    case 0x3B: case 0x4E: case 0x56:
      r = -777; break;
    case 0x3E: case 0x3F: case 0x40: case 0x42: case 0x57:
      r = -778; break;
    case 0x4F: r = -779; break;
    case 0x7F: r = m_count(); break;        /* bench probe: live blocks */
    case 0x36: case 0x47: r = -780; break;
  }
  m68k_set_reg(M68K_REG_D0, (uint32_t)r);
}

static void fake_xbios(void)
{
  uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
  uint32_t a = args_of_frame(sp);
  if (rd16(a) == 38) {                  /* Supexec: call it, rts -> FAKERET */
    sp -= 4;
    wr32(sp, FAKERET);
    m68k_set_reg(M68K_REG_A7, sp);
    m68k_set_reg(M68K_REG_PC, rd32(a + 2));
    return;
  }
  m68k_set_reg(M68K_REG_D0, 0);
}

void stbox_pc_hook(unsigned int pc) { (void)pc; }  /* m68kconf.h STBOX hook */

/* ---- NatFeats ------------------------------------------------------- */
static int dbg(void) { return getenv("HOSTDOS_DEBUG") != NULL; }

int bench_illg(int op)
{
  uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
  switch (op) {
    case 0x7300: {
      char name[32];
      int i;
      for (i = 0; i < 31 && rd8(rd32(sp + 4) + i); i++) name[i] = (char)rd8(rd32(sp + 4) + i);
      name[i] = 0;
      m68k_set_reg(M68K_REG_D0, strcmp(name, "HOSTDOS") ? 0 : (uint32_t)nf_id_base);
      return 1;
    }
    case 0x7301: {
      uint32_t id = rd32(sp + 4), p[4];
      for (int i = 0; i < 4; i++) p[i] = rd32(sp + 8 + 4 * i);
      uint32_t r = (id & 0xFFF00000u) == (uint32_t)nf_id_base ? hostdos_call(id & 0xFFFFF, p) : 0;
      m68k_set_reg(M68K_REG_D0, r);
      return 1;
    }
    case 0x7FFF: fake_gemdos(); return 1;
    case 0x7FFC: fake_xbios(); return 1;
    case 0x7FFE: return 1;                       /* Supexec routine returned */
    case 0x7FFD:
      printf("\nFAULT: unexpected exception, frame SR %04X PC %08X\n", rd16(sp), rd32(sp + 2));
      stop_flag = 1; stop_code = -9999;
      m68k_end_timeslice();
      return 1;
  }
  return 0;
}

/* ---- loading ---------------------------------------------------------- */
static uint32_t load_prg(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  static uint8_t buf[1 << 20];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  uint32_t tlen = (buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5];
  uint32_t dlen = (buf[6] << 24) | (buf[7] << 16) | (buf[8] << 8) | buf[9];
  uint32_t blen = (buf[10] << 24) | (buf[11] << 16) | (buf[12] << 8) | buf[13];
  if (n < 28 + tlen + dlen) { fprintf(stderr, "short prg\n"); exit(2); }
  uint32_t bp = make_bp(0, 0);
  m_chown(bp, bp);
  m_chown(rd32(bp + 0x2C), bp);
  uint32_t tb = bp + 0x100;
  memcpy(ram + tb, buf + 28, tlen + dlen);      /* bench PRGs are PC-relative */
  memset(ram + tb + tlen + dlen, 0, blen);
  wr32(bp + 8, tb); wr32(bp + 12, tlen);
  wr32(bp + 16, tb + tlen); wr32(bp + 20, dlen);
  wr32(bp + 24, tb + tlen + dlen); wr32(bp + 28, blen);
  return bp;
}

static int run_prg(uint32_t bp)
{
  wr32(RUNP, bp);
  uint32_t usp = rd32(bp + 4) - 8;
  wr32(usp + 4, bp);
  stop_flag = 0;
  m68k_set_reg(M68K_REG_SR, 0x2700);
  m68k_set_reg(M68K_REG_A7, SSP_TOP - 0x100);
  m68k_set_reg(M68K_REG_USP, usp);
  push_frame_and_go(SUPSTK_TOP, rd32(bp + 8), 0x0000);
  for (int i = 0; i < 2000 && !stop_flag; i++)
    m68k_execute(100000);
  if (!stop_flag) { printf("\nTIMEOUT pc=%08X\n", m68k_get_reg(NULL, M68K_REG_PC)); return -9998; }
  return stop_code;
}

static void setup_machine(int cpu)
{
  memset(ram, 0, sizeof ram);
  for (int v = 0; v < 256; v++) wr32(v * 4, FAULT);
  wr32(0x84, FAKEDOS);                  /* trap #1 */
  wr32(0xB8, FAKEXBIOS);                /* trap #14 */
  wr16(FAKEDOS, 0x7FFF); wr16(FAKEDOS + 2, 0x4E73);
  wr16(FAKEXBIOS, 0x7FFC); wr16(FAKEXBIOS + 2, 0x4E73);
  wr16(FAKERET, 0x7FFE); wr16(FAKERET + 2, 0x4E73);
  wr16(FAULT, 0x7FFD); wr16(FAULT + 2, 0x4E72); wr16(FAULT + 4, 0x2700);
  wr32(0, SSP_TOP); wr32(4, FAULT);
  wr32(0x4C2, 0x0000000Cu);             /* C: D: */
  wr32(0x4F2, OSHDR);
  wr32(OSHDR + 8, OSHDR);               /* os_beg */
  wr16(OSHDR + 2, 0x0206);
  wr32(OSHDR + 0x28, RUNP);
  wr16(0x59E, longframe ? 1 : 0);
  wr32(0x5A0, 0);
  m68k_init();
  m68k_set_cpu_type(cpu);
  m68k_pulse_reset();
  heap_next = HEAP_LO;
  memset(blk, 0, sizeof blk);
  nctx = 0;
  cur_drive = 2;
}

/* ---- the run ----------------------------------------------------------- */
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                              else if (getenv("BENCH_VERBOSE")) { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static int open_fds(void)
{
  int n = 0;
  DIR *d = opendir("/proc/self/fd");
  struct dirent *e;
  while ((e = readdir(d))) n++;
  closedir(d);
  return n;
}

static void writef(const char *p, const char *s)
{
  FILE *f = fopen(p, "wb");
  fputs(s, f);
  fclose(f);
}

int main(int argc, char **argv)
{
  if (argc < 4) { fprintf(stderr, "usage: %s PSHOSTFS.PRG TEST.PRG CHILD.PRG [68000|68030] [gemver]\n", argv[0]); return 2; }
  int cpu = M68K_CPU_TYPE_68000;
  longframe = 0;
  if (argc > 4 && !strcmp(argv[4], "68030")) { cpu = M68K_CPU_TYPE_68030; longframe = 1; }
  gemver = argc > 5 ? (uint16_t)strtol(argv[5], NULL, 16) : 0x19;

  char root[] = "/tmp/hostdos-bench-XXXXXX";
  if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
  char p[512], ro[256];
  snprintf(p, sizeof p, "%s/s", root); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/s/hello.txt", root); writef(p, "Hello\n");
  snprintf(p, sizeof p, "%s/s/A Long Name.text", root); writef(p, "long");
  snprintf(p, sizeof p, "%s/s/Readme.TXT", root); writef(p, "one");
  snprintf(p, sizeof p, "%s/s/readme.txt", root); writef(p, "two");
  snprintf(p, sizeof p, "%s/s/.hidden", root); writef(p, "x");
  snprintf(p, sizeof p, "%s/s/sub", root); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/s/sub/inner.dat", root); writef(p, "inner");
  snprintf(p, sizeof p, "cp '%s' '%s/s/CHILD.PRG'", argv[3], root);
  if (system(p) != 0) return 2;
  snprintf(ro, sizeof ro, "%s/r", root); mkdir(ro, 0755);

  hostdos_drive_t drv[HOSTDOS_DRIVES];
  memset(drv, 0, sizeof drv);
  drv[18].enabled = true; snprintf(drv[18].path, sizeof drv[18].path, "%s/s", root);
  drv[17].enabled = true; drv[17].readonly = true; snprintf(drv[17].path, sizeof drv[17].path, "%s", ro);
  drv[3].enabled = true; snprintf(drv[3].path, sizeof drv[3].path, "%s", ro);  /* D: exists on TOS */
  hostdos_mem_t mem = { mem_read, mem_write };
  hostdos_init(drv, &mem, dbg);

  setup_machine(cpu);
  int fd0 = open_fds();

  printf("--- install (%s, GEMDOS %02X)\n", longframe ? "68030 long frames" : "68000", gemver);
  int r = run_prg(load_prg(argv[1]));
  CHECK(r == 0, "PSHOSTFS.PRG exits via Ptermres (got %d)", r);
  CHECK(strstr(outbuf, "R: S:") != NULL, "install line lists R: S: and not D: ('%s')", outbuf);
  CHECK(rd32(0x4C2) == (0x0Cu | (1u << 17) | (1u << 18)), "_drvbits %08X", rd32(0x4C2));
  CHECK(rd32(0x84) != FAKEDOS, "trap #1 hooked");
  outlen = 0; outbuf[0] = 0;
  r = run_prg(load_prg(argv[1]));
  outbuf[outlen] = 0;
  CHECK(r == 0 && strstr(outbuf, "already installed"), "second PSHOSTFS.PRG refuses ('%s')", outbuf);
  int blocks_after_install = m_count();

  printf("--- TEST.PRG\n");
  uint32_t tbp = load_prg(argv[2]);
  r = run_prg(tbp);
  CHECK(r == 7, "TEST.PRG ran to its end (Pterm 7, got %d)", r);

  /* results: see test.S for the slot list */
#define R(i) ((int32_t)rd32(RESULTS + 4 * (i)))
  CHECK(R(0) == 0, "Fsfirst S:\\*.* -> 0 (%d)", R(0));
  {
    char names[1024]; int k = 0;
    for (uint32_t a = RESULTS + 0x400; k < 1000; a++) { char c = (char)rd8(a); names[k++] = c ? c : ','; if (!c && !rd8(a + 1)) break; }
    names[k] = 0;
    printf("listing: %s\n", names);
    CHECK(strstr(names, "HELLO.TXT,") && strstr(names, "ALONGN~1.TEX,") && strstr(names, "README.TXT,") &&
          strstr(names, "README~1.TXT,") && strstr(names, "SUB,") && strstr(names, "CHILD.PRG,") &&
          !strstr(names, "HIDDEN"), "listing has the 8.3 names, no dot-file");
  }
  CHECK(R(1) == -49, "Fsnext at the end -> ENMFIL (%d)", R(1));
  CHECK(R(2) >= 64 && R(2) < 96, "Fopen S:\\HELLO.TXT -> handle (%d)", R(2));
  CHECK(R(3) == 6, "Fread -> 6 (%d)", R(3));
  CHECK(!memcmp(ram + RESULTS + 0x300, "Hello\n", 6), "Fread data");
  CHECK(R(4) == 0, "Fclose (%d)", R(4));
  CHECK(R(5) >= 64, "Fopen mangled name ALONGN~1.TEX (%d)", R(5));
  CHECK(R(6) == 0, "Dsetpath \\SUB on S: (%d)", R(6));
  CHECK(!strcmp((char *)ram + RESULTS + 0x380, "\\SUB"), "Dgetpath -> '%s'", (char *)ram + RESULTS + 0x380);
  CHECK(R(54) == 0, "Dsetpath \"\" -> E_OK, as GEMDOS (%d)", R(54));
  CHECK(R(55) == 0, "Dsetpath \"S:\" -> E_OK, as GEMDOS (%d)", R(55));
  CHECK(!strcmp((char *)ram + RESULTS + 0x200, "\\SUB"), "Dsetpath \"\"/\"S:\" kept the folder ('%s')",
        (char *)ram + RESULTS + 0x200);
  CHECK(R(7) >= 64, "Fopen INNER.DAT relative (%d)", R(7));
  CHECK(R(8) >= 64, "Fcreate NEW.TXT (%d)", R(8));
  CHECK(R(9) == 5, "Fwrite 5 (%d)", R(9));
  CHECK(R(10) == -64, "Fseek past EOF -> ERANGE (%d)", R(10));
  CHECK(R(11) == 2, "Fseek 2 (%d)", R(11));
  CHECK(R(12) == 0, "Frename NEW.TXT RENAMED.TXT (%d)", R(12));
  CHECK(R(13) == 1, "Fattrib set read-only -> 1 (%d)", R(13));
  CHECK(R(14) == -36, "Fdelete read-only -> EACCDN (%d)", R(14));
  CHECK(R(15) == 0, "Fattrib clear (%d)", R(15));
  CHECK(R(16) == 0, "Fdelete (%d)", R(16));
  CHECK(R(17) == 0 && R(18) == -36 && R(19) == 0, "Dcreate / again / Ddelete (%d %d %d)", R(17), R(18), R(19));
  CHECK(R(20) == 0 && rd32(RESULTS + 0x3C0 + 8) == 512, "Dfree S: (%d, secsiz %u)", R(20), rd32(RESULTS + 0x3C8));
  CHECK(R(21) == -777, "Fopen C:\\FOO reaches the ROM (%d)", R(21));
  CHECK(R(22) == -777, "Fopen FOO on current C: reaches the ROM (%d)", R(22));
  CHECK(R(23) == -780, "Dfree current C: reaches the ROM (%d)", R(23));
  CHECK(R(24) == -778, "Fread handle 3 reaches the ROM (%d)", R(24));
  CHECK(R(25) == 42, "Pexec 0 S:\\CHILD.PRG -> child's 42 (%d)", R(25));
  CHECK(R(40) == 0x12345678, "child: relocated data read (%08X)", R(40));
  CHECK(R(41) == 0, "child: BSS cleared (%08X)", R(41));
  CHECK(R(42) == 1, "child: p_tbase matches its code (%d)", R(42));
  CHECK(R(43) >= 64, "child: Fopen INNER.DAT in the inherited S:\\SUB (%d)", R(43));
  CHECK(R(44) == 'X', "child: command line reached it (%c)", R(44));
  CHECK(R(26) > 0, "Pexec 3 -> basepage (%08X)", R(26));
  CHECK(R(27) == 42, "Pexec 4 of it -> 42 (%d)", R(27));
  CHECK(R(28) == 0 && R(29) == 0, "caller Mfree env / basepage (%d %d)", R(28), R(29));
  CHECK(R(30) >= 64 && R(31) == 0, "supervisor-mode Fopen/Fclose (%d %d)", R(30), R(31));
  CHECK(R(32) == -779, "Fsnext with a TOS DTA reaches the ROM (%d)", R(32));
  CHECK(R(33) == -33, "Fsfirst no match -> EFILNF (%d)", R(33));
  CHECK(R(34) == -33, "Fopen a folder -> EFILNF (%d)", R(34));
  CHECK(R(35) == -13, "Fcreate on a ro drive -> EWRPRO (%d)", R(35));
  CHECK(R(36) == -777, "Fopen D: (TOS's own drive) reaches the ROM (%d)", R(36));
  CHECK(R(37) == -33, "Pexec 0 of a missing S: program -> EFILNF (%d)", R(37));
  CHECK(R(38) >= 64, "Fopen left open for Pterm (%d)", R(38));
  CHECK(R(39) == -778, "Fread handle 100 (not ours) reaches the ROM (%d)", R(39));
  CHECK(R(45) == R(46), "Pexec 0 leaves no block behind (%d before, %d after)", R(45), R(46));
  CHECK(R(50) == R(51), "Pexec 0 from a program: its stack pointer survives (%08X before, %08X after)", R(50), R(51));
  /* two levels: TEST.PRG -> (HOSTDOS) CHILD.PRG "X" -> (HOSTDOS) CHILD.PRG "Y",
   * the !RUNME.TOS -> COREMARK.TOS shape of 26 Oct 2026 */
  CHECK(rd32(RESULTS + 0x1E0) == 42, "nested: CHILD.PRG's own Pexec of CHILD.PRG -> 42 (%d)",
        (int32_t)rd32(RESULTS + 0x1E0));
  CHECK(rd32(RESULTS + 0x1E4) == rd32(RESULTS + 0x1E8),
        "nested: the launching child's stack pointer survives (%08X before, %08X after)",
        rd32(RESULTS + 0x1E4), rd32(RESULTS + 0x1E8));
  CHECK(rd32(RESULTS + 0x1EC) == 0x7777D7D7u && rd32(RESULTS + 0x1F0) == 0xA6A6A6A6u,
        "nested: the launching child's d3-d7/a3-a6 survive (d7 %08X a6 %08X)",
        rd32(RESULTS + 0x1EC), rd32(RESULTS + 0x1F0));
  CHECK(R(52) == 0x7777D7D7u && R(53) == 0xA6A6A6A6u,
        "Pexec 0 from a program: its d3-d7/a3-a6 survive (d7 %08X a6 %08X)", R(52), R(53));
  CHECK(bad_mfree == 0, "no Mfree of a block that does not exist (%d)", bad_mfree);

  struct stat st;
  snprintf(p, sizeof p, "%s/s/sub/RENAMED.TXT", root);
  CHECK(stat(p, &st) != 0, "RENAMED.TXT deleted on the host");
  snprintf(p, sizeof p, "%s/s/NEWDIR", root);
  CHECK(stat(p, &st) != 0, "NEWDIR removed on the host");
  CHECK(open_fds() == fd0, "no host fd left open after TEST.PRG's Pterm (%d vs %d)", open_fds(), fd0);
  CHECK(m_count() == blocks_after_install, "every child block freed (%d blocks, %d after install)", m_count(), blocks_after_install);

  /* under MiNT it must not install at all */
  setup_machine(cpu);
  wr32(0x5A0, 0x6000); wr32(0x6000, 0x4D694E54); wr32(0x6004, 0x0100); wr32(0x6008, 0);
  outlen = 0;
  r = run_prg(load_prg(argv[1]));
  outbuf[outlen] = 0;
  CHECK(r == 0 && strstr(outbuf, "MiNT") && rd32(0x84) == FAKEDOS, "refuses under MiNT, trap #1 untouched");

  snprintf(p, sizeof p, "rm -rf '%s'", root);
  if (system(p) != 0) fprintf(stderr, "cleanup failed\n");
  printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
  return fails ? 1 : 0;
}
