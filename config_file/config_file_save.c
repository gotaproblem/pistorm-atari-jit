// SPDX-License-Identifier: MIT
//
// config_file_save() — writes the settings back to the .cfg without
// destroying it.
//
// The parser throws away comments, ordering and the spelling of every
// value it reads (a bare `ttram` becomes 128M, `fps` is clamped,
// `stram_size` becomes bytes), so dumping struct emulator_config back out
// would quietly rewrite the file's meaning as well as strip its comments.
// This is therefore a LINE EDITOR over the original file, not a
// serialiser: every line the emulator does not manage is copied through
// byte for byte, a managed key that already has a line has that one line
// replaced in place, and a managed key with no line is appended once in a
// clearly marked block at the end.
//
// The original is kept as <file>.bak and the new text is written to
// <file>.tmp and renamed, so an interrupted save cannot leave a
// half-written config behind.

#include "config_file/config_file.h"
#include "platforms/atari/psctrl/psctrl_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXLINE 1024

/* Rendered by psctrl_settings.cpp from the descriptor table: the tunables
 * whose cfg key is simply the descriptor name. Returns 0 if the key is
 * not one of those. */
int psctrl_settings_render_key(const char *key, char *out, unsigned long n);
/* Every key the descriptor table wants written, in table order. */
int psctrl_settings_key_count(void);
const char *psctrl_settings_key_at(int i);

static const char *cpu_name(unsigned t)
{
  static const char *n[] = { "NONE", "68000", "68010", "68020",
                             "68030", "68040", "68060" };
  return (t < sizeof(n) / sizeof(n[0])) ? n[t] : "68000";
}

static const char *card_name(int c)
{
  static const char *n[] = { "NONE", "ET4000AX", "ATI", "MATROX" };
  return (c >= 0 && c < 4) ? n[c] : "NONE";
}

static const char *driver_name(int d)
{
  static const char *n[] = { "NONE", "NOVA", "XVDI", "NVDI", "FVDI" };
  return (d >= 0 && d < 5) ? n[d] : "NONE";
}

/*
 * Render one managed key. Returns 1 and fills out[] with the whole line
 * (no newline), 0 if this key is not managed here.
 *
 * The syntax has to be what load_config_file() accepts, not what looks
 * tidy: `blitter real`, `kbd usb nograb merge mousediv 2`, `vga CARD
 * DRIVER` in upper case, sizes with their K/M suffix.
 */
static int render_boot_key(const char *key, const struct emulator_config *c,
                           char *out, unsigned long n)
{
  if (!strcmp(key, "machine")) {
    static const char *m[] = { "st", "ste", "megast" };
    if (!c->machine_set)
      return 0;
    snprintf(out, n, "machine %s",
             (c->machine_kind >= 0 && c->machine_kind < 3) ? m[c->machine_kind] : "st");
    return 1;
  }
  if (!strcmp(key, "cpu")) {
    snprintf(out, n, "cpu %s", cpu_name((unsigned)c->cpu_type + 1u));
    return 1;
  }
  if (!strcmp(key, "fpu"))            { snprintf(out, n, "fpu %s", c->fpu ? "true" : "false"); return 1; }
  if (!strcmp(key, "mmu"))            { snprintf(out, n, "mmu %s", c->mmu ? "true" : "false"); return 1; }
  if (!strcmp(key, "cpu_compatible")) { snprintf(out, n, "cpu_compatible %s", c->cpu_compatible ? "true" : "false"); return 1; }
  if (!strcmp(key, "shifter")) {
    if (!c->shifter_set)
      return 0;
    snprintf(out, n, "shifter %s", c->shifter_ste ? "ste" : "st");
    return 1;
  }
  if (!strcmp(key, "blitter")) {
    if (!c->blitter)
      snprintf(out, n, "blitter false");
    else if (c->blitter_real)
      snprintf(out, n, "blitter real");
    else
      snprintf(out, n, "blitter true");
    return 1;
  }
  if (!strcmp(key, "stram_size")) {
    snprintf(out, n, "stram_size %uK", (unsigned)(c->stram_size / 1024u));
    return 1;
  }
  if (!strcmp(key, "ttram")) {
    if (!c->ttram)
      snprintf(out, n, "ttram false");
    else
      snprintf(out, n, "ttram %uM", (unsigned)(c->ttram_size / (1024u * 1024u)));
    return 1;
  }
  if (!strcmp(key, "addr32"))       { snprintf(out, n, "addr32 %s", c->addr32 ? "true" : "false"); return 1; }
  if (!strcmp(key, "stram_cache"))  { snprintf(out, n, "stram_cache %s", c->stram_cache ? "true" : "false"); return 1; }
  if (!strcmp(key, "stram_direct")) { snprintf(out, n, "stram_direct %s", c->stram_direct ? "true" : "false"); return 1; }
  if (!strcmp(key, "native_hdmi"))  { snprintf(out, n, "native_hdmi %s", c->native_hdmi ? "true" : "false"); return 1; }
  if (!strcmp(key, "vga")) {
    snprintf(out, n, "vga %s %s", card_name((int)c->graphics.card),
             driver_name((int)c->graphics.driver));
    return 1;
  }
  if (!strcmp(key, "monitor")) {
    static const char *m[] = { "auto", "mono", "colour" };
    snprintf(out, n, "monitor %s",
             (c->monitor_force >= 0 && c->monitor_force < 3) ? m[c->monitor_force] : "auto");
    return 1;
  }
  if (!strcmp(key, "ym2149"))    { snprintf(out, n, "ym2149 %s", c->ym2149 ? "true" : "false"); return 1; }
  if (!strcmp(key, "dma_sound")) { snprintf(out, n, "dma_sound %s", c->dma_sound ? "true" : "false"); return 1; }
  if (!strcmp(key, "ide"))       { snprintf(out, n, "ide %s", c->ide ? "true" : "false"); return 1; }
  if (!strcmp(key, "kbd")) {
    char b[128];
    int p = 0;

    if (!c->kbd_usb) {
      snprintf(out, n, "kbd false");
      return 1;
    }
    p += snprintf(b + p, sizeof(b) - p, "usb");
    if (!c->kbd_grab)
      p += snprintf(b + p, sizeof(b) - p, " nograb");
    if (c->kbd_mode == 1)
      p += snprintf(b + p, sizeof(b) - p, " merge");
    else if (c->kbd_mode == 2)
      p += snprintf(b + p, sizeof(b) - p, " standalone");
    if (c->kbd_mouse_div > 1)
      snprintf(b + p, sizeof(b) - p, " mousediv %d", c->kbd_mouse_div);
    snprintf(out, n, "kbd %s", b);
    return 1;
  }
  if (!strcmp(key, "network"))     { snprintf(out, n, "network %s", c->network_enabled ? "true" : "false"); return 1; }
  if (!strcmp(key, "network_irq")) { snprintf(out, n, "network_irq %u", (unsigned)c->network_irq_level); return 1; }
  if (!strcmp(key, "fps"))         { snprintf(out, n, "fps %d", c->fps); return 1; }
  if (!strcmp(key, "rom")) {
    if (!c->rom.rom_path[0])
      return 0;
    snprintf(out, n, "rom %s", c->rom.rom_path);
    return 1;
  }
  if (!strcmp(key, "stbox_tos")) {
    if (!c->stbox_tos[0])
      return 0;
    snprintf(out, n, "stbox_tos %s", c->stbox_tos);
    return 1;
  }
  if (!strcmp(key, "stbox_plane")) { snprintf(out, n, "stbox_plane %d", c->stbox_plane); return 1; }
  if (!strcmp(key, "jit_cache")) {
    if (!c->jit_cache_set)
      return 0;
    snprintf(out, n, "jit_cache %d", c->jit_cache);
    return 1;
  }
  if (!strcmp(key, "m68k_speed")) {
    if (!c->m68k_speed_set)
      return 0;
    if (c->m68k_speed < 0)
      snprintf(out, n, "m68k_speed max");
    else
      snprintf(out, n, "m68k_speed %d", c->m68k_speed);
    return 1;
  }
  if (!strcmp(key, "cpu_clock_multiplier")) {
    if (!c->cpu_clock_multiplier_set)
      return 0;
    snprintf(out, n, "cpu_clock_multiplier %d", c->cpu_clock_multiplier);
    return 1;
  }
  return 0;
}

static int render_key(const char *key, const struct emulator_config *c,
                      char *out, unsigned long n)
{
  if (render_boot_key(key, c, out, n))
    return 1;
  return psctrl_settings_render_key(key, out, n);
}

/* "[apj-os]" -> "apj-os". Returns 0 for any other line. Local to the
 * saver so it does not depend on the parser's private copy. */
static int save_section_header(const char *line, char *out, unsigned long n)
{
  unsigned long i = 0;

  while (*line == ' ' || *line == '\t')
    line++;
  if (*line != '[')
    return 0;
  line++;
  while (*line && *line != ']' && i + 1 < n)
    out[i++] = *line++;
  out[i] = '\0';
  return *line == ']' && i > 0;
}

/* Append the managed keys that the target block did not already contain,
 * under one marked comment, at the current write position (inside the
 * target section). */
static int flush_unseen(FILE *out, const struct emulator_config *cfg,
                        const char *seen, int nkeys)
{
  char rendered[MAXLINE];
  int wrote = 0, i;

  for (i = 0; i < nkeys; i++) {
    if (seen[i])
      continue;
    if (!render_key(psctrl_settings_key_at(i), cfg, rendered, sizeof(rendered)))
      continue;
    if (!wrote)
      fprintf(out, "# --- written by PSCTRL ---\n");
    fprintf(out, "%s\n", rendered);
    wrote = 1;
  }
  return wrote;
}

/* first whitespace-delimited token of a config line, lower-cased */
static int line_key(const char *line, char *out, unsigned long n)
{
  unsigned long i = 0;

  while (*line == ' ' || *line == '\t')
    line++;
  if (!*line || *line == '#' || *line == '/' || *line == '\r' || *line == '\n')
    return 0;
  while (*line && !isspace((unsigned char)*line) && i + 1 < n)
    out[i++] = (char)tolower((unsigned char)*line++);
  out[i] = '\0';
  return i > 0;
}

/* The emulator runs as root under pistorm.service, so the file it writes
 * would come out root:root 0644 and the user could no longer edit their
 * own config. The new file gets the owner of the directory it lands in
 * (the user's tree), not of the .cfg it replaces - an earlier save may
 * already have left that one root-owned - and mode 0664 on top of
 * whatever the old file had. Run by hand as that user this is a no-op. */
static void take_ownership(FILE *out, const char *path)
{
  struct stat st, dst;
  char dir[1100];
  size_t n;
  const char *slash;
  int fd = fileno(out);
  int have_file, have_dir, rc;
  mode_t mode = 0664;

  have_file = stat(path, &st) == 0;
  if (have_file)
    mode |= st.st_mode & 07777;

  slash = strrchr(path, '/');
  n = slash ? (size_t)(slash - path) : 0;
  if (!slash)
    strcpy(dir, ".");
  else if (n == 0)
    strcpy(dir, "/");
  else if (n < sizeof(dir)) {
    memcpy(dir, path, n);
    dir[n] = '\0';
  } else
    dir[0] = '\0';
  have_dir = dir[0] && stat(dir, &dst) == 0;

  if (have_dir && dst.st_uid != 0)
    rc = fchown(fd, dst.st_uid, dst.st_gid);
  else if (have_file)
    rc = fchown(fd, st.st_uid, st.st_gid);
  else
    rc = 0;
  rc |= fchmod(fd, mode);
  (void)rc;
}

int config_file_save(const char *path, const struct emulator_config *cfg)
{
  char tmp[1100], bak[1100];
  char line[MAXLINE], key[64], rendered[MAXLINE];
  FILE *in, *out;
  int nkeys, i;
  char *seen;

  if (!path || !*path || !cfg)
    return -1;

  nkeys = psctrl_settings_key_count();
  seen = (char *)calloc((size_t)nkeys + 1, 1);
  if (!seen)
    return -1;

  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  snprintf(bak, sizeof(bak), "%s.bak", path);

  out = fopen(tmp, "w");
  if (!out) {
    fprintf(stderr, "[PSCTRL] cannot write %s: %s\n", tmp, strerror(errno));
    free(seen);
    return -1;
  }
  take_ownership(out, path);

  /* One file, several machine blocks. A save must edit only the block the
   * running config was booted from - the boot tool's own rule - or it
   * rewrites the other machines' settings. want is that block; empty means
   * an old sectionless .cfg, and then the whole file is managed, exactly
   * as before. */
  {
    const char *want = emulator_config_section();
    int scoped = want && *want;
    int in_target = !scoped;      /* whole file counts as "in" when unscoped */
    int target_seen = !scoped;
    char sec[32];

    in = fopen(path, "r");
    if (in) {
      while (fgets(line, sizeof(line), in)) {
        if (scoped && save_section_header(line, sec, sizeof(sec))) {
          /* Leaving the target block: drop in whatever it was missing
           * before the next section starts, keeping a blank line between
           * the added keys and that next header. */
          if (in_target && flush_unseen(out, cfg, seen, nkeys))
            fputc('\n', out);
          in_target = (strcasecmp(sec, want) == 0);
          if (in_target)
            target_seen = 1;
          fputs(line, out);                     /* the header, verbatim */
          continue;
        }
        if (!line_key(line, key, sizeof(key))) {
          fputs(line, out);                     /* comment or blank */
          continue;
        }
        /* A managed key outside the target block is another machine's and
         * is left exactly as it is. */
        if (in_target && render_key(key, cfg, rendered, sizeof(rendered))) {
          int dup = 0;

          /* one line per managed key: the first occurrence in the block
           * takes the value, any later copy is dropped */
          for (i = 0; i < nkeys; i++)
            if (!strcmp(psctrl_settings_key_at(i), key)) {
              if (seen[i])
                dup = 1;
              seen[i] = 1;
            }
          if (!dup)
            fprintf(out, "%s\n", rendered);
          continue;
        }
        fputs(line, out);                       /* not ours, or not here */
      }
      fclose(in);
    }

    /* The target block was the last in the file (or the only content, or
     * unscoped): flush what it still lacks at the end. */
    if (in_target)
      flush_unseen(out, cfg, seen, nkeys);
    else if (scoped && !target_seen) {
      /* The file never had this block - add it, then its keys. */
      fprintf(out, "\n[%s]\n", want);
      flush_unseen(out, cfg, seen, nkeys);
    }
  }

  free(seen);
  if (fclose(out) != 0) {
    fprintf(stderr, "[PSCTRL] cannot finish %s: %s\n", tmp, strerror(errno));
    return -1;
  }

  rename(path, bak);                            /* best effort */
  if (rename(tmp, path) != 0) {
    fprintf(stderr, "[PSCTRL] cannot replace %s: %s\n", path, strerror(errno));
    rename(bak, path);
    return -1;
  }
  fprintf(stderr, "[PSCTRL] saved %s (previous kept as %s)\n", path, bak);
  fflush(stderr);
  return 0;
}
