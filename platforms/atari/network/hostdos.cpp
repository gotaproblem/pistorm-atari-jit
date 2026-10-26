// SPDX-License-Identifier: MIT
/*
 * HOSTDOS engine - GEMDOS file calls on a HOSTFS directory, for plain TOS.
 * See hostdos.h for the split between this file and HOSTDOS.PRG.
 *
 * Names. TOS only knows 8.3 upper-case names, the Pi's directories hold
 * anything. Every host directory is mapped once per change (dir mtime) to
 * a table of TOS names: entries that already are valid 8.3 names keep
 * them, upper-cased; the rest - long names, spaces, a second dot, UTF-8 -
 * get "BASE~N.EXT", N counted up in host-name order so a listing is
 * stable from call to call. Dot-files are hidden. Lookups go through the
 * same table, so whatever Fsfirst showed can be opened, and a name a
 * program types is truncated to 8.3 first, as TOS itself does.
 *
 * Writes go to the host file directly (honouring the cfg's `ro`): new
 * files and folders are created under the 8.3 name the program used.
 */
#include "hostdos.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define E_OK     0
#define ERROR    (-1)
#define EWRPRO   (-13)
#define EFILNF   (-33)
#define EPTHNF   (-34)
#define ENHNDL   (-35)
#define EACCDN   (-36)
#define EIHNDL   (-37)
#define ENSMEM   (-39)
#define EDRIVE   (-46)
#define ENSAME   (-48)
#define ENMFIL   (-49)
#define ERANGE_T (-64)
#define EPLFMT   (-66)

#define FA_RDONLY 0x01
#define FA_HIDDEN 0x02
#define FA_SYSTEM 0x04
#define FA_LABEL  0x08
#define FA_DIR    0x10
#define FA_ARCH   0x20

#define NDRIVES HOSTDOS_DRIVES
#define MAX_SEARCHES 32
#define MAX_PRG_BYTES (64u << 20)

static hostdos_drive_t g_drives[HOSTDOS_DRIVES];
static hostdos_mem_t g_mem;
static int (*g_debug)(void);
static bool g_mint;           /* hostfs.xfs mounted since the last INSTALL */

#define LOG(...) do { if (g_debug && g_debug()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* guest memory                                                        */
/* ------------------------------------------------------------------ */

static uint32_t rd32(uint32_t a)
{
  uint8_t b[4];
  g_mem.read(a, b, 4);
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint16_t rd16(uint32_t a)
{
  uint8_t b[2];
  g_mem.read(a, b, 2);
  return (uint16_t)((b[0] << 8) | b[1]);
}

static void wr32(uint32_t a, uint32_t v)
{
  uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
  g_mem.write(a, b, 4);
}

static void wr16(uint32_t a, uint16_t v)
{
  uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
  g_mem.write(a, b, 2);
}

static std::string rdstr(uint32_t a, size_t max = 256)
{
  std::string s;
  if (!a)
    return s;
  for (size_t i = 0; i < max; i++) {
    uint8_t c;
    g_mem.read(a + (uint32_t)i, &c, 1);
    if (!c)
      break;
    s += (char)c;
  }
  return s;
}

static void wrzero(uint32_t a, uint32_t n)
{
  static const uint8_t z[16384] = { 0 };
  while (n) {
    uint32_t k = n > sizeof z ? (uint32_t)sizeof z : n;
    g_mem.write(a, z, k);
    a += k;
    n -= k;
  }
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

static uint16_t dos_time(time_t t)
{
  struct tm tm;
  localtime_r(&t, &tm);
  return (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

static uint16_t dos_date(time_t t)
{
  struct tm tm;
  localtime_r(&t, &tm);
  int y = tm.tm_year - 80;
  if (y < 0) y = 0;
  if (y > 127) y = 127;
  return (uint16_t)((y << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

static time_t from_dos(uint16_t time, uint16_t date)
{
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = ((date >> 9) & 0x7F) + 80;
  tm.tm_mon = ((date >> 5) & 0x0F) - 1;
  tm.tm_mday = date & 0x1F;
  tm.tm_hour = (time >> 11) & 0x1F;
  tm.tm_min = (time >> 5) & 0x3F;
  tm.tm_sec = (time & 0x1F) * 2;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

/* ------------------------------------------------------------------ */
/* 8.3 names                                                           */
/* ------------------------------------------------------------------ */

static bool tos_char_ok(unsigned char c)
{
  if (isalnum(c))
    return c < 0x80;
  return strchr("_-!#$%&'()@^`{}~", c) != NULL && c != 0;
}

/* already a valid 8.3 name (any case)? */
static bool valid83(const std::string &n)
{
  size_t dot = n.find('.');
  std::string base = dot == std::string::npos ? n : n.substr(0, dot);
  std::string ext = dot == std::string::npos ? "" : n.substr(dot + 1);
  if (base.empty() || base.size() > 8 || ext.size() > 3)
    return false;
  if (dot != std::string::npos && ext.empty())
    return false;
  for (unsigned char c : base) if (!tos_char_ok(c)) return false;
  for (unsigned char c : ext) if (!tos_char_ok(c)) return false;
  return true;
}

static std::string upper(std::string s)
{
  for (char &c : s)
    c = (char)toupper((unsigned char)c);
  return s;
}

/* a name as a program typed it, cut to 8.3 the way GEMDOS does */
static std::string trunc83(const std::string &in)
{
  if (in == "." || in == "..")
    return in;
  std::string s = upper(in);
  size_t dot = s.find('.');
  std::string base = dot == std::string::npos ? s : s.substr(0, dot);
  std::string ext;
  if (dot != std::string::npos) {
    ext = s.substr(dot + 1);
    size_t d2 = ext.find('.');
    if (d2 != std::string::npos)
      ext = ext.substr(0, d2);
  }
  if (base.size() > 8) base.resize(8);
  if (ext.size() > 3) ext.resize(3);
  return ext.empty() ? base : base + "." + ext;
}

static std::string sanitize(const std::string &s)
{
  std::string o;
  for (unsigned char c : s) {
    if (c == ' ' || c == '.')
      continue;
    o += tos_char_ok(c) ? (char)toupper(c) : '_';
  }
  return o;
}

static std::string mangle(const std::string &host, int n)
{
  size_t dot = host.rfind('.');
  std::string base = (dot == std::string::npos || dot == 0) ? host : host.substr(0, dot);
  std::string ext = (dot == std::string::npos || dot == 0) ? "" : host.substr(dot + 1);
  base = sanitize(base);
  ext = sanitize(ext);
  if (base.empty())
    base = "_";
  if (ext.size() > 3)
    ext.resize(3);
  char suf[12];
  snprintf(suf, sizeof suf, "~%d", n);
  size_t keep = 8 - strlen(suf);
  if (base.size() > keep)
    base.resize(keep);
  base += suf;
  return ext.empty() ? base : base + "." + ext;
}

/* 11-character FCB match, as GEMDOS does it: '*' fills the rest of its
 * field with '?', so "*" alone only matches names without an extension */
static void fcb(const std::string &n, char out[11])
{
  memset(out, ' ', 11);
  size_t dot = n.find('.');
  std::string base = dot == std::string::npos ? n : n.substr(0, dot);
  std::string ext = dot == std::string::npos ? "" : n.substr(dot + 1);
  for (size_t i = 0, j = 0; i < base.size() && j < 8; i++) {
    if (base[i] == '*') { while (j < 8) out[j++] = '?'; break; }
    out[j++] = base[i];
  }
  for (size_t i = 0, j = 0; i < ext.size() && j < 3; i++) {
    if (ext[i] == '*') { while (j < 3) out[8 + j++] = '?'; break; }
    out[8 + j++] = ext[i];
  }
}

static bool match83(const std::string &pat, const std::string &name)
{
  if (name == "." || name == "..")
    return pat == "*.*" || pat == name;
  char p[11], n[11];
  fcb(upper(pat), p);
  fcb(name, n);
  for (int i = 0; i < 11; i++)
    if (p[i] != '?' && p[i] != n[i])
      return false;
  return true;
}

static bool has_wild(const std::string &s)
{
  return s.find_first_of("*?") != std::string::npos;
}

/* ------------------------------------------------------------------ */
/* directory map                                                       */
/* ------------------------------------------------------------------ */

struct ent { std::string tos, host; };

struct dmap {
  long long mtime;
  std::vector<ent> e;
};

static std::map<std::string, dmap> g_dcache;

static void dcache_drop(void) { g_dcache.clear(); }

static long long mtime_ns(const struct stat &st)
{
  return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

static const std::vector<ent> *map_dir(const std::string &dir)
{
  struct stat st;
  if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return NULL;
  auto it = g_dcache.find(dir);
  if (it != g_dcache.end() && it->second.mtime == mtime_ns(st))
    return &it->second.e;

  DIR *d = opendir(dir.c_str());
  if (!d)
    return NULL;
  std::vector<std::string> names;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.')
      continue;                       /* ".", "..", dot-files */
    struct stat cs;
    std::string p = dir + "/" + de->d_name;
    if (stat(p.c_str(), &cs) != 0)
      continue;                       /* dangling link */
    if (!S_ISREG(cs.st_mode) && !S_ISDIR(cs.st_mode))
      continue;
    names.push_back(de->d_name);
  }
  closedir(d);
  std::sort(names.begin(), names.end());

  std::vector<ent> out;
  std::set<std::string> taken;
  std::vector<bool> done(names.size(), false);
  for (size_t i = 0; i < names.size(); i++) {
    if (!valid83(names[i]))
      continue;
    std::string t = upper(names[i]);
    if (taken.count(t))
      continue;
    taken.insert(t);
    out.push_back({ t, names[i] });
    done[i] = true;
  }
  for (size_t i = 0; i < names.size(); i++) {
    if (done[i])
      continue;
    for (int n = 1; n < 100000; n++) {
      std::string t = mangle(names[i], n);
      if (!taken.count(t)) {
        taken.insert(t);
        out.push_back({ t, names[i] });
        break;
      }
    }
  }
  std::sort(out.begin(), out.end(),
            [](const ent &a, const ent &b) { return a.host < b.host; });

  if (g_dcache.size() >= 32)
    g_dcache.clear();
  dmap &m = g_dcache[dir];
  m.mtime = mtime_ns(st);
  m.e.swap(out);
  return &m.e;
}

static const ent *lookup(const std::string &dir, const std::string &tos)
{
  const std::vector<ent> *m = map_dir(dir);
  if (!m)
    return NULL;
  for (const ent &e : *m)
    if (e.tos == tos)
      return &e;
  return NULL;
}

/* ------------------------------------------------------------------ */
/* drives, paths                                                       */
/* ------------------------------------------------------------------ */

static std::vector<std::string> g_cwd[NDRIVES];

static bool drive_ok(int d)
{
  return d >= 0 && d < NDRIVES &&
         g_drives[d].enabled && g_drives[d].path[0];
}

static bool drive_ro(int d)
{
  return g_drives[d].readonly;
}

static std::string root_of(int d)
{
  std::string r = g_drives[d].path;
  while (r.size() > 1 && r.back() == '/')
    r.pop_back();
  return r;
}

/* GEMDOS path -> TOS components, cwd applied, "." / ".." folded, each
 * cut to 8.3. A leading "X:" is skipped (the guest already chose the
 * drive from it). */
static std::vector<std::string> parse(int d, const std::string &gem)
{
  std::string s = gem;
  if (s.size() >= 2 && s[1] == ':')
    s = s.substr(2);
  std::vector<std::string> c;
  if (s.empty() || (s[0] != '\\' && s[0] != '/'))
    c = g_cwd[d];
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find_first_of("\\/", i);
    if (j == std::string::npos)
      j = s.size();
    std::string comp = s.substr(i, j - i);
    if (comp.empty() || comp == ".") {
    } else if (comp == "..") {
      if (!c.empty())
        c.pop_back();
    } else {
      c.push_back(trunc83(comp));
    }
    i = j + 1;
  }
  return c;
}

/* host directory for the first n components; "" + *err if a step is
 * missing or not a folder */
static std::string walk(int d, const std::vector<std::string> &c, size_t n, int *err)
{
  std::string h = root_of(d);
  for (size_t i = 0; i < n; i++) {
    const ent *e = lookup(h, c[i]);
    if (!e) { *err = EPTHNF; return ""; }
    h += "/" + e->host;
    struct stat st;
    if (stat(h.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { *err = EPTHNF; return ""; }
  }
  *err = 0;
  return h;
}

/* existing object (file or folder); the root itself counts */
static int resolve(int d, const std::string &gem, std::string *host, struct stat *st,
                   std::vector<std::string> *comps = NULL)
{
  std::vector<std::string> c = parse(d, gem);
  if (comps)
    *comps = c;
  int err;
  if (c.empty()) {
    *host = root_of(d);
    return stat(host->c_str(), st) == 0 ? 0 : EPTHNF;
  }
  std::string dir = walk(d, c, c.size() - 1, &err);
  if (err)
    return err;
  const ent *e = lookup(dir, c.back());
  if (!e)
    return EFILNF;
  *host = dir + "/" + e->host;
  return stat(host->c_str(), st) == 0 ? 0 : EFILNF;
}

/* parent folder + 8.3 leaf for something about to be created; *exist set
 * to the host path of a same-named entry, if any */
static int resolve_new(int d, const std::string &gem, std::string *dir,
                       std::string *leaf, std::string *exist)
{
  std::vector<std::string> c = parse(d, gem);
  if (c.empty())
    return EACCDN;
  int err;
  *dir = walk(d, c, c.size() - 1, &err);
  if (err)
    return err;
  *leaf = c.back();
  if (*leaf == "." || *leaf == ".." || has_wild(*leaf))
    return EACCDN;
  const ent *e = lookup(*dir, *leaf);
  *exist = e ? *dir + "/" + e->host : "";
  return 0;
}

/* TOS's read-only bit is the file's write permission bits, not access():
 * the emulator runs as root on the Pi, and root may write anything */
static bool is_ro(const struct stat &st)
{
  return !S_ISDIR(st.st_mode) && (st.st_mode & 0222) == 0;
}

static int errno_tos(int e, int dflt)
{
  switch (e) {
    case ENOENT:    return dflt;
    case ENOTDIR:   return EPTHNF;
    case EACCES:
    case EPERM:
    case EEXIST:
    case ENOTEMPTY:
    case EISDIR:    return EACCDN;
    case EROFS:     return EWRPRO;
    case EMFILE:
    case ENFILE:    return ENHNDL;
    case ENOMEM:    return ENSMEM;
    case EXDEV:     return ENSAME;
  }
  return ERROR;
}

/* ------------------------------------------------------------------ */
/* handles, searches, pending Pexec                                    */
/* ------------------------------------------------------------------ */

struct hfile {
  bool used;
  int fd;
  int drive;
  bool canwrite;
  uint32_t owner;
  std::string path;
};

static hfile g_files[HOSTDOS_MAX_FILES];

struct hit {
  std::string name;
  uint8_t attr;
  uint16_t time, date;
  uint32_t size;
};

struct search {
  uint32_t id;
  std::vector<hit> hits;
  size_t pos;
};

static search g_search[MAX_SEARCHES];
static uint32_t g_next_search = 1;

static struct {
  bool valid;
  std::string path;
  uint32_t tlen, dlen, blen, slen, flags;
  uint16_t absflag;
} g_pend;

static hfile *file_of(int h)
{
  int i = h - HOSTDOS_HANDLE_BASE;
  if (i < 0 || i >= HOSTDOS_MAX_FILES || !g_files[i].used)
    return NULL;
  return &g_files[i];
}

static int file_new(int fd, int d, bool w, uint32_t owner, const std::string &path)
{
  for (int i = 0; i < HOSTDOS_MAX_FILES; i++) {
    if (!g_files[i].used) {
      g_files[i].used = true;
      g_files[i].fd = fd;
      g_files[i].drive = d;
      g_files[i].canwrite = w;
      g_files[i].owner = owner;
      g_files[i].path = path;
      return HOSTDOS_HANDLE_BASE + i;
    }
  }
  close(fd);
  return ENHNDL;
}

static void file_close(hfile *f)
{
  if (f->used && f->fd >= 0)
    close(f->fd);
  f->used = false;
  f->fd = -1;
  f->path.clear();
}

static void reset_all(void)
{
  for (hfile &f : g_files)
    file_close(&f);
  for (search &s : g_search) {
    s.id = 0;
    s.hits.clear();
  }
  for (auto &c : g_cwd)
    c.clear();
  g_pend.valid = false;
  dcache_drop();
}

/* ------------------------------------------------------------------ */
/* the calls                                                           */
/* ------------------------------------------------------------------ */

static hit make_hit(const std::string &name, const struct stat &st)
{
  hit h;
  h.name = name;
  h.attr = S_ISDIR(st.st_mode) ? FA_DIR : 0;
  if (is_ro(st))
    h.attr |= FA_RDONLY;
  h.time = dos_time(st.st_mtime);
  h.date = dos_date(st.st_mtime);
  h.size = S_ISDIR(st.st_mode) ? 0 :
           (st.st_size > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)st.st_size);
  return h;
}

static void dta_fill(uint32_t dta, uint32_t id, const hit &h)
{
  uint8_t b[44];
  memset(b, 0, sizeof b);
  b[0] = (uint8_t)(HOSTDOS_DTA_MAGIC >> 24);
  b[1] = (uint8_t)(HOSTDOS_DTA_MAGIC >> 16);
  b[2] = (uint8_t)(HOSTDOS_DTA_MAGIC >> 8);
  b[3] = (uint8_t)HOSTDOS_DTA_MAGIC;
  b[4] = (uint8_t)(id >> 24); b[5] = (uint8_t)(id >> 16);
  b[6] = (uint8_t)(id >> 8);  b[7] = (uint8_t)id;
  b[21] = h.attr;
  b[22] = (uint8_t)(h.time >> 8); b[23] = (uint8_t)h.time;
  b[24] = (uint8_t)(h.date >> 8); b[25] = (uint8_t)h.date;
  b[26] = (uint8_t)(h.size >> 24); b[27] = (uint8_t)(h.size >> 16);
  b[28] = (uint8_t)(h.size >> 8);  b[29] = (uint8_t)h.size;
  strncpy((char *)b + 30, h.name.c_str(), 13);
  g_mem.write(dta, b, sizeof b);
}

static int32_t do_fsfirst(int d, uint32_t a, uint32_t dta)
{
  std::string spec = rdstr(rd32(a + 2));
  uint16_t attr = rd16(a + 6);
  if (!dta)
    return EFILNF;
  if ((attr & 0xFF) == FA_LABEL)
    return EFILNF;                    /* no volume labels here */

  std::vector<std::string> c = parse(d, spec);
  if (c.empty())
    return EFILNF;
  int err;
  std::string dir = walk(d, c, c.size() - 1, &err);
  if (err)
    return err;
  std::string pat = c.back();

  search *s = &g_search[0];
  for (search &x : g_search)          /* free slot, else the oldest */
    if (x.id == 0 || x.id < s->id) { s = &x; if (x.id == 0) break; }
  s->hits.clear();
  s->pos = 0;

  struct stat st;
  if (c.size() > 1 && (attr & FA_DIR) && stat(dir.c_str(), &st) == 0) {
    if (match83(pat, ".")) s->hits.push_back(make_hit(".", st));
    if (match83(pat, "..")) s->hits.push_back(make_hit("..", st));
  }
  const std::vector<ent> *m = map_dir(dir);
  if (m) {
    for (const ent &e : *m) {
      if (!match83(pat, e.tos))
        continue;
      std::string h = dir + "/" + e.host;
      if (stat(h.c_str(), &st) != 0)
        continue;
      if (S_ISDIR(st.st_mode) && !(attr & FA_DIR))
        continue;
      s->hits.push_back(make_hit(e.tos, st));
    }
  }
  LOG("[HOSTDOS] Fsfirst %c: '%s' attr=%02X -> %zu\n", 'A' + d, spec.c_str(), attr,
      s->hits.size());
  if (s->hits.empty()) {
    s->id = 0;
    return EFILNF;
  }
  s->id = g_next_search++;
  if (g_next_search == 0)
    g_next_search = 1;
  dta_fill(dta, s->id, s->hits[0]);
  s->pos = 1;
  return 0;
}

static int32_t do_fsnext(uint32_t dta)
{
  if (!dta || rd32(dta) != HOSTDOS_DTA_MAGIC)
    return ENMFIL;
  uint32_t id = rd32(dta + 4);
  for (search &s : g_search) {
    if (s.id != id || id == 0)
      continue;
    if (s.pos >= s.hits.size()) {
      s.id = 0;
      s.hits.clear();
      return ENMFIL;
    }
    dta_fill(dta, id, s.hits[s.pos++]);
    return 0;
  }
  return ENMFIL;
}

static int32_t do_fopen(int d, uint32_t a, uint32_t bp)
{
  std::string name = rdstr(rd32(a + 2));
  int mode = rd16(a + 6) & 3;
  std::string host;
  struct stat st;
  int err = resolve(d, name, &host, &st);
  if (err)
    return err;
  if (S_ISDIR(st.st_mode))
    return EFILNF;
  if (mode != 0 && drive_ro(d))
    return EWRPRO;
  int fl = mode == 0 ? O_RDONLY : mode == 1 ? O_WRONLY : O_RDWR;
  int fd = open(host.c_str(), fl);
  if (fd < 0)
    return errno_tos(errno, EFILNF);
  int h = file_new(fd, d, mode != 0, bp, host);
  LOG("[HOSTDOS] Fopen '%s' mode %d -> %d (%s)\n", name.c_str(), mode, h, host.c_str());
  return h;
}

static int32_t do_fcreate(int d, uint32_t a, uint32_t bp)
{
  std::string name = rdstr(rd32(a + 2));
  uint16_t attr = rd16(a + 6);
  if (attr & (FA_LABEL | FA_DIR))
    return EACCDN;
  if (drive_ro(d))
    return EWRPRO;
  std::string dir, leaf, exist;
  int err = resolve_new(d, name, &dir, &leaf, &exist);
  if (err)
    return err;
  std::string host = exist.empty() ? dir + "/" + leaf : exist;
  if (!exist.empty()) {
    struct stat st;
    if (stat(host.c_str(), &st) == 0 && (S_ISDIR(st.st_mode) || is_ro(st)))
      return EACCDN;
  }
  int fd = open(host.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return errno_tos(errno, EPTHNF);
  if (attr & FA_RDONLY)
    fchmod(fd, 0444);
  dcache_drop();
  int h = file_new(fd, d, true, bp, host);
  LOG("[HOSTDOS] Fcreate '%s' -> %d (%s)\n", name.c_str(), h, host.c_str());
  return h;
}

static int32_t do_fclose(uint32_t a)
{
  hfile *f = file_of((int16_t)rd16(a + 2));
  if (!f)
    return EIHNDL;
  file_close(f);
  return 0;
}

static int32_t do_fread(uint32_t a)
{
  hfile *f = file_of((int16_t)rd16(a + 2));
  uint32_t count = rd32(a + 4);
  uint32_t buf = rd32(a + 8);
  if (!f)
    return EIHNDL;
  static uint8_t chunk[65536];
  uint32_t total = 0;
  while (total < count) {
    uint32_t want = count - total;
    if (want > sizeof chunk)
      want = sizeof chunk;
    ssize_t got = read(f->fd, chunk, want);
    if (got < 0)
      return total ? (int32_t)total : ERROR;
    if (got == 0)
      break;
    g_mem.write(buf + total, chunk, (uint32_t)got);
    total += (uint32_t)got;
  }
  return (int32_t)total;
}

static int32_t do_fwrite(uint32_t a)
{
  hfile *f = file_of((int16_t)rd16(a + 2));
  uint32_t count = rd32(a + 4);
  uint32_t buf = rd32(a + 8);
  if (!f)
    return EIHNDL;
  if (!f->canwrite)
    return EACCDN;
  static uint8_t chunk[65536];
  uint32_t total = 0;
  while (total < count) {
    uint32_t want = count - total;
    if (want > sizeof chunk)
      want = sizeof chunk;
    g_mem.read(buf + total, chunk, want);
    ssize_t put = write(f->fd, chunk, want);
    if (put < 0)
      return total ? (int32_t)total : errno_tos(errno, EACCDN);
    total += (uint32_t)put;
    if ((uint32_t)put < want)
      break;                          /* disk full: short count, as TOS */
  }
  return (int32_t)total;
}

static int32_t do_fseek(uint32_t a)
{
  int32_t off = (int32_t)rd32(a + 2);
  hfile *f = file_of((int16_t)rd16(a + 6));
  int mode = rd16(a + 8);
  if (!f)
    return EIHNDL;
  struct stat st;
  if (fstat(f->fd, &st) != 0)
    return ERROR;
  off_t cur = lseek(f->fd, 0, SEEK_CUR);
  off_t base = mode == 0 ? 0 : mode == 1 ? cur : mode == 2 ? st.st_size : -1;
  if (base < 0)
    return ERROR;
  off_t pos = base + off;
  if (pos < 0 || pos > st.st_size)
    return ERANGE_T;
  if (lseek(f->fd, pos, SEEK_SET) < 0)
    return ERROR;
  return (int32_t)pos;
}

static int32_t do_fdatime(uint32_t a)
{
  uint32_t tp = rd32(a + 2);
  hfile *f = file_of((int16_t)rd16(a + 6));
  int wflag = rd16(a + 8);
  if (!f)
    return EIHNDL;
  if (wflag) {
    if (drive_ro(f->drive))
      return EWRPRO;
    time_t t = from_dos(rd16(tp), rd16(tp + 2));
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = t;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    if (futimens(f->fd, ts) != 0)
      return errno_tos(errno, EACCDN);
    return 0;
  }
  struct stat st;
  if (fstat(f->fd, &st) != 0)
    return ERROR;
  wr16(tp, dos_time(st.st_mtime));
  wr16(tp + 2, dos_date(st.st_mtime));
  return 0;
}

static int32_t do_fdelete(int d, uint32_t a)
{
  std::string name = rdstr(rd32(a + 2));
  std::string host;
  struct stat st;
  int err = resolve(d, name, &host, &st);
  if (err)
    return err;
  if (S_ISDIR(st.st_mode))
    return EFILNF;
  if (drive_ro(d))
    return EWRPRO;
  if (is_ro(st))
    return EACCDN;
  if (unlink(host.c_str()) != 0)
    return errno_tos(errno, EFILNF);
  dcache_drop();
  LOG("[HOSTDOS] Fdelete %s\n", host.c_str());
  return 0;
}

static int32_t do_dcreate(int d, uint32_t a)
{
  std::string name = rdstr(rd32(a + 2));
  if (drive_ro(d))
    return EWRPRO;
  std::string dir, leaf, exist;
  int err = resolve_new(d, name, &dir, &leaf, &exist);
  if (err)
    return err;
  if (!exist.empty())
    return EACCDN;
  if (mkdir((dir + "/" + leaf).c_str(), 0755) != 0)
    return errno_tos(errno, EPTHNF);
  dcache_drop();
  return 0;
}

static int32_t do_ddelete(int d, uint32_t a)
{
  std::string name = rdstr(rd32(a + 2));
  std::string host;
  struct stat st;
  std::vector<std::string> c;
  int err = resolve(d, name, &host, &st, &c);
  if (err)
    return EPTHNF;
  if (!S_ISDIR(st.st_mode) || c.empty())
    return EPTHNF;
  if (drive_ro(d))
    return EWRPRO;
  if (rmdir(host.c_str()) != 0)
    return errno_tos(errno, EPTHNF);
  dcache_drop();
  return 0;
}

static int32_t do_frename(int d, uint32_t a)
{
  std::string oldn = rdstr(rd32(a + 4));
  std::string newn = rdstr(rd32(a + 8));
  if (newn.size() >= 2 && newn[1] == ':' &&
      toupper((unsigned char)newn[0]) - 'A' != d)
    return ENSAME;
  std::string host;
  struct stat st;
  int err = resolve(d, oldn, &host, &st);
  if (err)
    return err;
  if (drive_ro(d))
    return EWRPRO;
  std::string dir, leaf, exist;
  err = resolve_new(d, newn, &dir, &leaf, &exist);
  if (err)
    return err;
  if (!exist.empty())
    return EACCDN;
  if (rename(host.c_str(), (dir + "/" + leaf).c_str()) != 0)
    return errno_tos(errno, EPTHNF);
  dcache_drop();
  return 0;
}

static int32_t do_fattrib(int d, uint32_t a)
{
  std::string name = rdstr(rd32(a + 2));
  int wflag = rd16(a + 6);
  int attr = rd16(a + 8);
  std::string host;
  struct stat st;
  int err = resolve(d, name, &host, &st);
  if (err)
    return err;
  int cur = S_ISDIR(st.st_mode) ? FA_DIR : 0;
  if (is_ro(st))
    cur |= FA_RDONLY;
  if (!wflag)
    return cur;
  if (drive_ro(d))
    return EWRPRO;
  if (S_ISDIR(st.st_mode))
    return cur;
  mode_t m = st.st_mode & 07777;
  m = (attr & FA_RDONLY) ? (m & ~(mode_t)0222) : (m | S_IWUSR);
  if (chmod(host.c_str(), m) != 0)
    return errno_tos(errno, EACCDN);
  return (attr & FA_RDONLY) ? FA_RDONLY : 0;
}

static int32_t do_dsetpath(int d, uint32_t a)
{
  std::string name = rdstr(rd32(a + 2));
  if (name.empty() || (name.size() == 2 && name[1] == ':'))
    return EPTHNF;
  std::string host;
  struct stat st;
  std::vector<std::string> c;
  int err = resolve(d, name, &host, &st, &c);
  if (err || !S_ISDIR(st.st_mode))
    return EPTHNF;
  g_cwd[d] = c;
  return 0;
}

static int32_t do_dgetpath(int d, uint32_t a)
{
  uint32_t buf = rd32(a + 2);
  std::string s;
  for (const std::string &c : g_cwd[d])
    s += "\\" + c;
  if (s.size() > 127)
    s.resize(127);
  g_mem.write(buf, s.c_str(), (uint32_t)s.size() + 1);
  return 0;
}

static int32_t do_dfree(int d, uint32_t a)
{
  uint32_t buf = rd32(a + 2);
  struct statvfs v;
  const uint32_t secsiz = 512, clsiz = 2, clbytes = secsiz * clsiz;
  const uint64_t maxcl = 0x7FFFFFFFull / clbytes;   /* free bytes fit an int32 */
  uint64_t total = 0, avail = 0;
  if (statvfs(root_of(d).c_str(), &v) == 0) {
    total = (uint64_t)v.f_blocks * v.f_frsize / clbytes;
    avail = (uint64_t)v.f_bavail * v.f_frsize / clbytes;
  }
  if (total > maxcl) total = maxcl;
  if (avail > total) avail = total;
  if (drive_ro(d)) avail = 0;
  wr32(buf, (uint32_t)avail);
  wr32(buf + 4, (uint32_t)total);
  wr32(buf + 8, secsiz);
  wr32(buf + 12, clsiz);
  return 0;
}

static int32_t do_pexec(int d, uint32_t a)
{
  int mode = rd16(a + 2);
  if (mode != 0 && mode != 3)
    return (int32_t)HOSTDOS_PASS;
  std::string name = rdstr(rd32(a + 4));
  std::string host;
  struct stat st;
  int err = resolve(d, name, &host, &st);
  if (err)
    return err;
  if (S_ISDIR(st.st_mode))
    return EFILNF;
  FILE *fp = fopen(host.c_str(), "rb");
  if (!fp)
    return errno_tos(errno, EFILNF);
  uint8_t h[28];
  size_t got = fread(h, 1, sizeof h, fp);
  fclose(fp);
  auto be32 = [&](int o) {
    return ((uint32_t)h[o] << 24) | ((uint32_t)h[o + 1] << 16) | ((uint32_t)h[o + 2] << 8) | h[o + 3];
  };
  if (got != sizeof h || h[0] != 0x60 || h[1] != 0x1A)
    return EPLFMT;
  g_pend.valid = true;
  g_pend.path = host;
  g_pend.tlen = be32(2);
  g_pend.dlen = be32(6);
  g_pend.blen = be32(10);
  g_pend.slen = be32(14);
  g_pend.flags = be32(22);
  g_pend.absflag = (uint16_t)((h[26] << 8) | h[27]);
  LOG("[HOSTDOS] Pexec %d %s text %u data %u bss %u flags %08X\n", mode,
      host.c_str(), g_pend.tlen, g_pend.dlen, g_pend.blen, g_pend.flags);
  return (int32_t)HOSTDOS_PEXEC;
}

static int32_t do_load(uint32_t bp)
{
  if (!g_pend.valid)
    return EPLFMT;
  g_pend.valid = false;

  uint32_t lowtpa = rd32(bp), hitpa = rd32(bp + 4);
  uint64_t img = (uint64_t)g_pend.tlen + g_pend.dlen;
  uint64_t need = 0x100 + img + g_pend.blen;
  if (img > MAX_PRG_BYTES || lowtpa + need > hitpa)
    return ENSMEM;

  FILE *fp = fopen(g_pend.path.c_str(), "rb");
  if (!fp)
    return EFILNF;
  std::vector<uint8_t> file;
  {
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
      file.insert(file.end(), buf, buf + n);
      if (file.size() > MAX_PRG_BYTES + (64u << 20))
        break;
    }
  }
  fclose(fp);
  if (file.size() < 28 + img)
    return EPLFMT;

  std::vector<uint8_t> im(file.begin() + 28, file.begin() + 28 + (size_t)img);
  uint32_t tbase = lowtpa + 0x100;

  if (g_pend.absflag == 0) {
    uint64_t ro = 28 + img + g_pend.slen;
    if (ro + 4 <= file.size()) {
      auto patch = [&](uint64_t off) -> bool {
        if (off + 4 > im.size() || (off & 1))
          return false;
        uint32_t v = ((uint32_t)im[off] << 24) | ((uint32_t)im[off + 1] << 16) |
                     ((uint32_t)im[off + 2] << 8) | im[off + 3];
        v += tbase;
        im[off] = (uint8_t)(v >> 24); im[off + 1] = (uint8_t)(v >> 16);
        im[off + 2] = (uint8_t)(v >> 8); im[off + 3] = (uint8_t)v;
        return true;
      };
      uint64_t pos = ((uint32_t)file[ro] << 24) | ((uint32_t)file[ro + 1] << 16) |
                     ((uint32_t)file[ro + 2] << 8) | file[ro + 3];
      if (pos) {
        if (!patch(pos))
          return EPLFMT;
        for (uint64_t i = ro + 4; i < file.size(); i++) {
          uint8_t b = file[i];
          if (b == 0)
            break;
          if (b == 1) { pos += 254; continue; }
          pos += b;
          if (!patch(pos))
            return EPLFMT;
        }
      }
    }
  }

  g_mem.write(tbase, im.data(), (uint32_t)im.size());
  uint32_t bbase = tbase + (uint32_t)img;
  if (g_pend.flags & 1)
    wrzero(bbase, g_pend.blen);                       /* fastload: BSS only */
  else
    wrzero(bbase, hitpa - bbase);                     /* BSS and heap */

  wr32(bp + 8, tbase);
  wr32(bp + 12, g_pend.tlen);
  wr32(bp + 16, tbase + g_pend.tlen);
  wr32(bp + 20, g_pend.dlen);
  wr32(bp + 24, bbase);
  wr32(bp + 28, g_pend.blen);
  LOG("[HOSTDOS] loaded %s at %08X (TPA %08X-%08X)\n", g_pend.path.c_str(), tbase,
      lowtpa, hitpa);
  return 0;
}

static int32_t dispatch(uint32_t a, int d, uint32_t bp, uint32_t dta)
{
  uint16_t fn = rd16(a);

  /* handle- and DTA-based calls: the guest only sends ours */
  switch (fn) {
    case 0x3E: return do_fclose(a);
    case 0x3F: return do_fread(a);
    case 0x40: return do_fwrite(a);
    case 0x42: return do_fseek(a);
    case 0x57: return do_fdatime(a);
    case 0x4F: return do_fsnext(dta);
  }

  if (!drive_ok(d))
    return (int32_t)HOSTDOS_PASS;

  switch (fn) {
    case 0x36: return do_dfree(d, a);
    case 0x39: return do_dcreate(d, a);
    case 0x3A: return do_ddelete(d, a);
    case 0x3B: return do_dsetpath(d, a);
    case 0x3C: return do_fcreate(d, a, bp);
    case 0x3D: return do_fopen(d, a, bp);
    case 0x41: return do_fdelete(d, a);
    case 0x43: return do_fattrib(d, a);
    case 0x47: return do_dgetpath(d, a);
    case 0x4B: return do_pexec(d, a);
    case 0x4E: return do_fsfirst(d, a, dta);
    case 0x56: return do_frename(d, a);
  }
  return (int32_t)HOSTDOS_PASS;
}

/* ------------------------------------------------------------------ */
/* entry points                                                        */
/* ------------------------------------------------------------------ */

extern "C" void hostdos_init(const hostdos_drive_t *drives,
                             const hostdos_mem_t *mem, int (*debug_enabled)(void))
{
  reset_all();
  memset(g_drives, 0, sizeof g_drives);
  if (drives)
    memcpy(g_drives, drives, sizeof g_drives);
  if (mem)
    g_mem = *mem;
  g_debug = debug_enabled;
  g_mint = false;
}

extern "C" void hostdos_note_mint(void)
{
  if (!g_mint)
    reset_all();
  g_mint = true;
}

extern "C" uint32_t hostdos_call(uint32_t subid, const uint32_t *p)
{
  if (!g_mem.read || !g_mem.write)
    return (uint32_t)EDRIVE;
  switch (subid) {
    case HOSTDOS_OP_VERSION:
      return HOSTDOS_API_VERSION;

    case HOSTDOS_OP_INSTALL:
    {
      reset_all();
      g_mint = false;
      uint32_t bits = 0;
      for (int d = 0; d < NDRIVES; d++)
        if (drive_ok(d))
          bits |= 1u << d;
      LOG("[HOSTDOS] install -> drive bits %08X\n", bits);
      return bits;
    }

    case HOSTDOS_OP_DISPATCH:
    {
      if (g_mint)
        return HOSTDOS_PASS;
      int32_t r = dispatch(p[0], (int)p[1], p[2], p[3]);
      LOG("[HOSTDOS] fn %02X drive %c -> %d\n", rd16(p[0]),
          (p[1] < NDRIVES) ? 'A' + (int)p[1] : '?', r);
      return (uint32_t)r;
    }

    case HOSTDOS_OP_PTERM:
      for (hfile &f : g_files)
        if (f.used && f.owner == p[0]) {
          LOG("[HOSTDOS] Pterm %08X closes %s\n", p[0], f.path.c_str());
          file_close(&f);
        }
      return 0;

    case HOSTDOS_OP_PRGFLAGS:
      return g_pend.valid ? g_pend.flags : 0;

    case HOSTDOS_OP_LOAD:
      return (uint32_t)do_load(p[0]);
  }
  return (uint32_t)ERROR;
}

extern "C" int hostdos_gemdos_to_host(const char *gem, char *out, size_t outsz)
{
  if (!gem || !gem[0] || gem[1] != ':')
    return 0;
  int d = toupper((unsigned char)gem[0]) - 'A';
  if (!drive_ok(d))
    return 0;
  std::string host;
  struct stat st;
  if (resolve(d, gem, &host, &st) != 0)
    return 0;
  if (host.size() + 1 > outsz)
    return 0;
  memcpy(out, host.c_str(), host.size() + 1);
  return 1;
}
