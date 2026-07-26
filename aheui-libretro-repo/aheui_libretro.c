/* aheui_libretro.c -- a libretro core that runs pure-Hangul Aheui programs.
 *
 * Content may be:
 *   .png          an Aheui grid stored as pixel residues mod 40
 *   .aheui .txt   an Aheui grid as UTF-8 text
 *   .b .bf        Brainfuck, compiled to an Aheui grid on load
 *
 * No external dependencies: DEFLATE, PNG and the font are all in-tree, because
 * a core that needs zlib at the wrong version is a core nobody can build.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "aheui_tables.h"
#include "font8x8.h"

/* ---------------------------------------------------------------- logging */
static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static void logf_(enum retro_log_level lv, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   if (log_cb) log_cb(lv, "%s", buf);
   else fprintf(stderr, "[aheui] %s", buf);
}

/* --------------------------------------------------------------- inflate */
/* Enough DEFLATE to read a PNG: stored, fixed and dynamic Huffman blocks.
 * The output size is known from IHDR, so the sink is a fixed buffer. */
typedef struct { unsigned short counts[16]; unsigned short symbols[288]; } huff;
typedef struct {
   const unsigned char *src, *end;
   unsigned int bitbuf; int bitcnt;
   unsigned char *dst, *dstend, *start;
} inf;

static void huff_build(huff *h, const unsigned char *len, int n)
{
   int i, sum = 0, offs[16];
   memset(h->counts, 0, sizeof h->counts);
   for (i = 0; i < n; i++) h->counts[len[i]]++;
   h->counts[0] = 0;
   for (i = 0; i < 16; i++) { offs[i] = sum; sum += h->counts[i]; }
   for (i = 0; i < n; i++) if (len[i]) h->symbols[offs[len[i]]++] = (unsigned short)i;
}

static int getbit(inf *d)
{
   if (!d->bitcnt) {
      if (d->src >= d->end) return -1;
      d->bitbuf = *d->src++;
      d->bitcnt = 8;
   }
   { int v = d->bitbuf & 1; d->bitbuf >>= 1; d->bitcnt--; return v; }
}

static int getbits(inf *d, int n)
{
   int v = 0, i;
   for (i = 0; i < n; i++) {
      int b = getbit(d);
      if (b < 0) return -1;
      v |= b << i;
   }
   return v;
}

static int huff_decode(inf *d, const huff *h)
{
   int sum = 0, cur = 0, len = 0;
   do {
      int b = getbit(d);
      if (b < 0) return -1;
      cur = 2 * cur + b;
      if (++len > 15) return -1;
      sum += h->counts[len];
      cur -= h->counts[len];
   } while (cur >= 0);
   return h->symbols[sum + cur];
}

static const unsigned short len_base[29] = {
   3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const unsigned char len_extra[29] = {
   0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const unsigned short dist_base[30] = {
   1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
   1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const unsigned char dist_extra[30] = {
   0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static const unsigned char clc_order[19] = {
   16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

static int inf_block(inf *d, const huff *lt, const huff *dt)
{
   for (;;) {
      int sym = huff_decode(d, lt);
      if (sym < 0) return -1;
      if (sym == 256) return 0;
      if (sym < 256) {
         if (d->dst >= d->dstend) return -1;
         *d->dst++ = (unsigned char)sym;
      } else {
         int len, dist, ds, e;
         sym -= 257;
         if (sym >= 29) return -1;
         e = getbits(d, len_extra[sym]);
         if (e < 0) return -1;
         len = len_base[sym] + e;
         ds = huff_decode(d, dt);
         if (ds < 0 || ds >= 30) return -1;
         e = getbits(d, dist_extra[ds]);
         if (e < 0) return -1;
         dist = dist_base[ds] + e;
         /* A corrupt stream can name a distance reaching behind the output,
            which is a wild read, not merely wrong data. */
         if (dist > (long)(d->dst - d->start)) return -1;
         if (d->dst + len > d->dstend) return -1;
         {
            unsigned char *p = d->dst - dist;
            int i;
            for (i = 0; i < len; i++) *d->dst++ = *p++;
         }
      }
   }
}

static void fixed_trees(huff *lt, huff *dt)
{
   unsigned char l[288];
   int i;
   for (i = 0;   i < 144; i++) l[i] = 8;
   for (i = 144; i < 256; i++) l[i] = 9;
   for (i = 256; i < 280; i++) l[i] = 7;
   for (i = 280; i < 288; i++) l[i] = 8;
   huff_build(lt, l, 288);
   for (i = 0; i < 30; i++) l[i] = 5;
   huff_build(dt, l, 30);
}

static int dynamic_trees(inf *d, huff *lt, huff *dt)
{
   unsigned char lens[288 + 32];
   huff clc;
   int hlit, hdist, hclen, i, n;
   unsigned char cl[19];

   hlit  = getbits(d, 5); hdist = getbits(d, 5); hclen = getbits(d, 4);
   if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
   hlit += 257; hdist += 1; hclen += 4;

   memset(cl, 0, sizeof cl);
   for (i = 0; i < hclen; i++) {
      int v = getbits(d, 3);
      if (v < 0) return -1;
      cl[clc_order[i]] = (unsigned char)v;
   }
   huff_build(&clc, cl, 19);

   n = 0;
   while (n < hlit + hdist) {
      int sym = huff_decode(d, &clc);
      if (sym < 0) return -1;
      if (sym < 16) { lens[n++] = (unsigned char)sym; }
      else if (sym == 16) {
         int r = getbits(d, 2);
         unsigned char prev;
         if (r < 0 || n == 0) return -1;
         prev = lens[n - 1];
         for (r += 3; r && n < hlit + hdist; r--) lens[n++] = prev;
      } else if (sym == 17) {
         int r = getbits(d, 3);
         if (r < 0) return -1;
         for (r += 3; r && n < hlit + hdist; r--) lens[n++] = 0;
      } else {
         int r = getbits(d, 7);
         if (r < 0) return -1;
         for (r += 11; r && n < hlit + hdist; r--) lens[n++] = 0;
      }
   }
   huff_build(lt, lens, hlit);
   huff_build(dt, lens + hlit, hdist);
   return 0;
}

/* Returns bytes written, or -1. Input is a zlib stream (PNG's IDAT). */
static long zlib_inflate(const unsigned char *src, size_t srclen,
                         unsigned char *dst, size_t dstlen)
{
   inf d;
   int final;
   if (srclen < 2) return -1;
   d.src = src + 2; d.end = src + srclen;      /* skip the zlib header */
   d.bitbuf = 0; d.bitcnt = 0;
   d.dst = dst; d.dstend = dst + dstlen; d.start = dst;
   do {
      int type;
      huff lt, dt;
      final = getbit(&d);
      type  = getbits(&d, 2);
      if (final < 0 || type < 0) return -1;
      if (type == 0) {
         unsigned len;
         d.bitcnt = 0;                          /* stored: byte-align */
         if (d.src + 4 > d.end) return -1;
         len = (unsigned)d.src[0] | ((unsigned)d.src[1] << 8);
         d.src += 4;
         if (d.src + len > d.end || d.dst + len > d.dstend) return -1;
         memcpy(d.dst, d.src, len);
         d.src += len; d.dst += len;
      } else if (type == 1) {
         fixed_trees(&lt, &dt);
         if (inf_block(&d, &lt, &dt) < 0) return -1;
      } else if (type == 2) {
         if (dynamic_trees(&d, &lt, &dt) < 0) return -1;
         if (inf_block(&d, &lt, &dt) < 0) return -1;
      } else return -1;
   } while (!final);
   return (long)(d.dst - dst);
}

/* -------------------------------------------------------------------- PNG */
static unsigned rd32(const unsigned char *p)
{
   return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
          ((unsigned)p[2] << 8) | (unsigned)p[3];
}

/* Decodes to raw samples, one byte per channel, exactly as the browser
 * console's hand-rolled decoder does. Canvas is not an option there and
 * neither is a full PNG library here; both need the bytes unmolested. */
static unsigned char *png_decode(const unsigned char *buf, size_t len, size_t *out_len)
{
   size_t off = 8, idat_cap = 0, idat_len = 0;
   unsigned W = 0, H = 0;
   int ch = 4, depth = 0, ctype = 0;
   unsigned char *idat = NULL, *raw = NULL, *dec = NULL;
   size_t stride, need;
   long got;
   unsigned y;

   if (len < 8 || buf[0] != 0x89 || buf[1] != 'P' || buf[2] != 'N' || buf[3] != 'G') {
      logf_(RETRO_LOG_ERROR, "not a PNG\n");
      return NULL;
   }
   while (off + 8 <= len) {
      unsigned clen = rd32(buf + off);
      const char *type = (const char *)buf + off + 4;
      const unsigned char *data = buf + off + 8;
      if (off + 12 + clen > len) break;
      if (!memcmp(type, "IHDR", 4)) {
         W = rd32(data); H = rd32(data + 4);
         depth = data[8]; ctype = data[9];
         if (data[12]) { logf_(RETRO_LOG_ERROR, "interlaced PNG unsupported\n"); return NULL; }
         ch = ctype == 6 ? 4 : ctype == 2 ? 3 : ctype == 4 ? 2 : 1;
      } else if (!memcmp(type, "IDAT", 4)) {
         if (idat_len + clen > idat_cap) {
            idat_cap = (idat_len + clen) * 2 + 4096;
            idat = (unsigned char *)realloc(idat, idat_cap);
            if (!idat) return NULL;
         }
         memcpy(idat + idat_len, data, clen);
         idat_len += clen;
      } else if (!memcmp(type, "IEND", 4)) break;
      off += 12 + clen;
   }
   if (!W || !H || !idat) { free(idat); logf_(RETRO_LOG_ERROR, "no image data\n"); return NULL; }
   if (depth != 8) { free(idat); logf_(RETRO_LOG_ERROR, "bit depth %d unsupported\n", depth); return NULL; }
   /* Dimensions come from the file. Bound them before they reach malloc, and
      check the product rather than trusting it not to wrap. */
   if (W > (1u << 20) || H > (1u << 24)) {
      free(idat); logf_(RETRO_LOG_ERROR, "implausible image size %ux%u\n", W, H); return NULL;
   }
   stride = (size_t)W * ch;
   if (stride + 1 > (size_t)-1 / H || (stride + 1) * H > (size_t)512 * 1024 * 1024) {
      free(idat); logf_(RETRO_LOG_ERROR, "image too large: %ux%u x%d\n", W, H, ch); return NULL;
   }
   need = (stride + 1) * H;
   dec = (unsigned char *)malloc(need);
   if (!dec) { free(idat); return NULL; }
   got = zlib_inflate(idat, idat_len, dec, need);
   free(idat);
   if (got < 0 || (size_t)got < need) {
      logf_(RETRO_LOG_ERROR, "inflate failed (%ld of %lu)\n", got, (unsigned long)need);
      free(dec);
      return NULL;
   }

   raw = (unsigned char *)malloc(stride * H);
   if (!raw) { free(dec); return NULL; }
   for (y = 0; y < H; y++) {
      const unsigned char *line = dec + (size_t)y * (stride + 1);
      int f = line[0];
      unsigned char *o = raw + (size_t)y * stride;
      size_t x;
      line++;
      for (x = 0; x < stride; x++) {
         int a = x >= (size_t)ch ? o[x - ch] : 0;
         int b = y > 0 ? o[(ptrdiff_t)x - (ptrdiff_t)stride] : 0;
         int c = (x >= (size_t)ch && y > 0) ? o[(ptrdiff_t)x - (ptrdiff_t)stride - ch] : 0;
         int v = line[x];
         switch (f) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: {
               int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
               v += (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
               break;
            }
            default: break;
         }
         o[x] = (unsigned char)v;
      }
   }
   free(dec);
   *out_len = stride * H;
   return raw;
}

/* ------------------------------------------------------------------- grid */
typedef struct {
   unsigned short *cells;      /* codepoints, packed row-major and ragged */
   size_t ncells, cap;
   int *rowStart;
   unsigned short *rowLen;
   int rows, rowcap, W;
} grid;

static void grid_free(grid *g)
{
   free(g->cells); free(g->rowStart); free(g->rowLen);
   memset(g, 0, sizeof *g);
}

static int grid_row(grid *g, const unsigned short *row, int len)
{
   if (g->rows == g->rowcap) {
      int nc = g->rowcap ? g->rowcap * 2 : 1024;
      int *rs = (int *)realloc(g->rowStart, (size_t)nc * sizeof *rs);
      unsigned short *rl = (unsigned short *)realloc(g->rowLen, (size_t)nc * sizeof *rl);
      if (!rs || !rl) return 0;
      g->rowStart = rs; g->rowLen = rl; g->rowcap = nc;
   }
   if (g->ncells + (size_t)len > g->cap) {
      size_t nc = (g->ncells + len) * 2 + 4096;
      unsigned short *c = (unsigned short *)realloc(g->cells, nc * sizeof *c);
      if (!c) return 0;
      g->cells = c; g->cap = nc;
   }
   g->rowStart[g->rows] = (int)g->ncells;
   g->rowLen[g->rows] = (unsigned short)len;
   if (len > g->W) g->W = len;
   if (len) memcpy(g->cells + g->ncells, row, (size_t)len * sizeof *row);
   g->ncells += (size_t)len;
   g->rows++;
   return 1;
}

#define CELL(g, x, y) ((x) < (g)->rowLen[y] ? (g)->cells[(g)->rowStart[y] + (x)] : 0x20)

/* residue bytes -> grid */
static int grid_from_png(grid *g, const unsigned char *raw, size_t len)
{
   unsigned short row[4096];
   int n = 0;
   size_t i;
   memset(g, 0, sizeof *g);
   for (i = 0; i < len; i++) {
      int d = raw[i] % AH_BASE;
      if (d == AH_TERM) break;
      if (d == 0) {                              /* newline */
         if (!grid_row(g, row, n)) return 0;
         n = 0;
      } else if (d < AH_NALPHA && n < (int)(sizeof row / sizeof *row)) {
         row[n++] = ah_alphabet[d];
      }                                          /* residue 38 indexes nothing */
   }
   if (n && !grid_row(g, row, n)) return 0;
   return g->rows > 0;
}

/* UTF-8 text -> grid (any Aheui program, not only ones this core could bake) */
static int grid_from_text(grid *g, const unsigned char *p, size_t len)
{
   unsigned short *row = (unsigned short *)malloc(65536 * sizeof *row);
   int n = 0;
   size_t i = 0;
   memset(g, 0, sizeof *g);
   if (!row) return 0;
   if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;  /* BOM */
   for (; i < len; i++) {
      unsigned c = p[i];
      if (c == '\r') continue;                              /* CRLF, lone CR */
      if (c >= 0xF0) { i += 3; continue; }                  /* outside the BMP */
      else if (c >= 0xE0) {
         if (i + 2 >= len) break;
         c = ((c & 0x0F) << 12) | ((p[i+1] & 0x3F) << 6) | (p[i+2] & 0x3F);
         i += 2;
      } else if (c >= 0xC0) {
         if (i + 1 >= len) break;
         c = ((c & 0x1F) << 6) | (p[i+1] & 0x3F);
         i += 1;
      }
      if (c == '\n') {
         if (!grid_row(g, row, n)) { free(row); return 0; }
         n = 0;
      } else if (n < 65536) {
         row[n++] = (unsigned short)c;
      }
   }
   if (n && !grid_row(g, row, n)) { free(row); return 0; }
   free(row);
   return g->rows > 0;
}

/* Brainfuck -> grid, the same scaffold the Python and JS compilers emit */
static const unsigned short *const *leaf_for(char t)
{
   int i;
   for (i = 0; ah_leaves[i].rows; i++) if (ah_leaves[i].tok == t) return ah_leaves[i].rows;
   return NULL;
}

static int emit_set(grid *g, unsigned short base, int col, unsigned short ch)
{
   unsigned short row[512];        /* depth ~250, not the 27 a 64-cell row allowed */
   int i;
   if (col >= (int)(sizeof row / sizeof *row)) return 0;
   row[0] = base;
   for (i = 1; i <= col; i++) row[i] = 0x20;
   row[col] = ch;
   return grid_row(g, row, col + 1);
}

static int bf_emit(grid *g, const char *t, const int *match, int lo, int hi, int depth)
{
   int i;
   for (i = lo; i < hi; i++) {
      const unsigned short *const *blk = leaf_for(t[i]);
      if (blk) {
         int r;
         for (r = 0; blk[r]; r++) {
            int n = 0;
            while (blk[r][n]) n++;
            if (!grid_row(g, blk[r], n)) return 0;
         }
         continue;
      }
      if (t[i] != '[') continue;
      {
         int j = match[i], skip = 8 + 2 * depth, back = skip + 1;
         unsigned short one;
         one = AH_DUP_SKIP2;   if (!grid_row(g, &one, 1)) return 0;
         if (!emit_set(g, AH_NOOP_RIGHT, skip, AH_NOOP_DOWN)) return 0;
         one = AH_CHECK_DOWN;  if (!grid_row(g, &one, 1)) return 0;
         if (!emit_set(g, AH_NOOP_DOWN, back, AH_NOOP_LEFT)) return 0;
         if (j == i + 1) { one = AH_NOOP_DOWN; if (!grid_row(g, &one, 1)) return 0; }
         else if (!bf_emit(g, t, match, i + 1, j, depth + 1)) return 0;
         one = AH_DUP_SKIP2;   if (!grid_row(g, &one, 1)) return 0;
         if (!emit_set(g, AH_NOOP_RIGHT, back, AH_NOOP_UP)) return 0;
         one = AH_CHECK_UP;    if (!grid_row(g, &one, 1)) return 0;
         if (!emit_set(g, AH_NOOP_DOWN, skip, AH_NOOP_LEFT)) return 0;
         i = j;
      }
   }
   return 1;
}

static int grid_from_bf(grid *g, const unsigned char *src, size_t len)
{
   char *t = (char *)malloc(len + 1);
   int *match = NULL, *stack = NULL;
   int n = 0, sp = 0, ok = 0;
   size_t i;
   unsigned short one;

   memset(g, 0, sizeof *g);
   if (!t) return 0;
   for (i = 0; i < len; i++)
      if (strchr("+-.,<>[]", src[i]) && src[i]) t[n++] = (char)src[i];
   t[n] = 0;
   match = (int *)calloc((size_t)n + 1, sizeof *match);
   stack = (int *)calloc((size_t)n + 1, sizeof *stack);
   if (!match || !stack) goto done;
   for (i = 0; i < (size_t)n; i++) {
      if (t[i] == '[') stack[sp++] = (int)i;
      else if (t[i] == ']') {
         if (!sp) { logf_(RETRO_LOG_ERROR, "unmatched ]\n"); goto done; }
         sp--; match[stack[sp]] = (int)i; match[i] = stack[sp];
      }
   }
   if (sp) { logf_(RETRO_LOG_ERROR, "unmatched [\n"); goto done; }

   one = AH_BLOCK_BEGIN;
   if (!grid_row(g, &one, 1)) goto done;
   if (!bf_emit(g, t, match, 0, n, 0)) goto done;
   one = AH_BLOCK_HALT;
   if (!grid_row(g, &one, 1)) goto done;
   ok = g->rows > 0;
done:
   free(t); free(match); free(stack);
   return ok;
}

/* --------------------------------------------------------------- Aheui VM */
#define NSTACK 28
#define QUEUE  21

typedef struct { long long *v; int len, cap, head; } stk;

typedef struct {
   grid g;
   stk st[NSTACK];
   int cur, x, y, dx, dy;
   int done, waiting;
   unsigned long long steps;
   int **skip;                 /* per column: sorted occupied rows, or NULL */
   int *skipn;
   signed char *cho;           /* decomposition, indexed by codepoint-0xAC00 */
   unsigned char *ju, *jo;
   int in[4096], inhead, intail;
} vm;

static const int STROKES[28] = {
   0,2,4,4,2,5,5,3,5,7,9,9,7,9,9,8,4,4,6,2,4,0,3,4,3,4,4,0 };

static void stk_push(stk *s, long long v)
{
   if (s->len == s->cap) {
      int nc = s->cap ? s->cap * 2 : 64;
      long long *p = (long long *)realloc(s->v, (size_t)nc * sizeof *p);
      if (!p) return;
      s->v = p; s->cap = nc;
   }
   s->v[s->len++] = v;
}

static int stk_count(const stk *s) { return s->len - s->head; }

static long long stk_pop(stk *s, int queue)
{
   if (queue) return s->v[s->head++];
   return s->v[--s->len];
}

static long long stk_peek(const stk *s, int queue)
{
   return queue ? s->v[s->head] : s->v[s->len - 1];
}

static void vm_free(vm *m)
{
   int i;
   for (i = 0; i < NSTACK; i++) free(m->st[i].v);
   if (m->skip) { for (i = 0; i < m->g.W; i++) free(m->skip[i]); free(m->skip); }
   free(m->skipn); free(m->cho); free(m->ju); free(m->jo);
   grid_free(&m->g);
   memset(m, 0, sizeof *m);
}

#define HB 0xAC00
#define NSYL (19 * 21 * 28)

static int vm_prepare(vm *m)
{
   int x, y;
   m->cho = (signed char *)malloc(NSYL);
   m->ju  = (unsigned char *)malloc(NSYL);
   m->jo  = (unsigned char *)malloc(NSYL);
   if (!m->cho || !m->ju || !m->jo) return 0;
   for (x = 0; x < NSYL; x++) {
      m->cho[x] = (signed char)(x / 588);
      m->ju[x]  = (unsigned char)((x % 588) / 28);
      m->jo[x]  = (unsigned char)(x % 28);
   }
   /* Skip tables, built row-major: a run of blanks cannot change direction or
    * touch storage, so a vertical glide may jump to the next occupied row.
    * Dense columns are left untabled; their glides are short anyway. */
   m->skip  = (int **)calloc((size_t)m->g.W, sizeof *m->skip);
   m->skipn = (int *)calloc((size_t)m->g.W, sizeof *m->skipn);
   if (!m->skip || !m->skipn) return 0;
   {
      int *count = (int *)calloc((size_t)m->g.W, sizeof *count);
      int *fill;
      if (!count) return 0;
      for (y = 0; y < m->g.rows; y++)
         for (x = 0; x < m->g.W; x++) {
            unsigned c = CELL(&m->g, x, y);
            if (c - HB < NSYL) count[x]++;
         }
      for (x = 0; x < m->g.W; x++)
         if (count[x] && count[x] <= m->g.rows * 3 / 10) {
            m->skip[x] = (int *)malloc((size_t)count[x] * sizeof(int));
            m->skipn[x] = count[x];
            if (!m->skip[x]) { free(count); return 0; }
         }
      fill = (int *)calloc((size_t)m->g.W, sizeof *fill);
      if (!fill) { free(count); return 0; }
      for (y = 0; y < m->g.rows; y++)
         for (x = 0; x < m->g.W; x++) {
            unsigned c = CELL(&m->g, x, y);
            if (m->skip[x] && c - HB < NSYL) m->skip[x][fill[x]++] = y;
         }
      free(count); free(fill);
   }
   m->x = m->y = 0; m->dx = 1; m->dy = 0; m->cur = 0;
   return 1;
}

static void advance(vm *m)
{
   m->x += m->dx; m->y += m->dy;
   if (m->y < 0) m->y = m->g.rows - 1; else if (m->y >= m->g.rows) m->y = 0;
   if (m->x < 0) m->x = m->g.W - 1;    else if (m->x >= m->g.W) m->x = 0;
}

static int input_get(vm *m)
{
   if (m->inhead == m->intail) return -1;
   { int c = m->in[m->inhead]; m->inhead = (m->inhead + 1) % 4096; return c; }
}

static void vm_run(vm *m, long budget, void (*emit)(int));

/* ---------------------------------------------------------- terminal */
#define COLS 80
#define ROWS 50
#define FBW  (COLS * 8)
#define FBH  (ROWS * 8)

static unsigned char term[ROWS][COLS];
static int term_x, term_y;
static uint32_t *fb;
static char line[256];
static int line_n;
static int cursor_phase;

static void term_clear(void)
{
   memset(term, ' ', sizeof term);
   term_x = term_y = 0;
}

static void term_scroll(void)
{
   memmove(term[0], term[1], sizeof term - COLS);
   memset(term[ROWS - 1], ' ', COLS);
   term_y = ROWS - 1;
}

static void term_put(int c)
{
   if (c == '\n') { term_x = 0; if (++term_y >= ROWS) term_scroll(); return; }
   if (c == '\r') { term_x = 0; return; }
   if (c == '\b') { if (term_x) term_x--; return; }
   if (c == '\t') { term_x = (term_x + 8) & ~7; if (term_x >= COLS) { term_x = 0; if (++term_y >= ROWS) term_scroll(); } return; }
   term[term_y][term_x] = (unsigned char)c;
   if (++term_x >= COLS) { term_x = 0; if (++term_y >= ROWS) term_scroll(); }
}

/* ----------------------------------------------------------- core state */
static vm machine;
static int have_content;
static unsigned opt_steps = 500000;
static uint32_t fg_col = 0x00E8E8E8, bg_col = 0x00101010;
static int16_t silence[2 * 1024];

static void emit_char(int c) { term_put(c & 0xFF); }

static void emit_number(long long n, void (*emit)(int))
{
   char b[24];
   int i = 0;
   int neg = n < 0;
   unsigned long long u = neg ? 0ULL - (unsigned long long)n
                              : (unsigned long long)n;
   do { b[i++] = (char)('0' + (int)(u % 10ULL)); u /= 10ULL; } while (u);
   if (neg) b[i++] = '-';
   while (i--) emit(b[i]);
}

static void vm_run(vm *m, long budget, void (*emit)(int))
{
   const grid *g = &m->g;
   while (!m->done && budget-- > 0) {
      unsigned s = CELL(g, m->x, m->y);
      unsigned o = s - HB;
      int c, v, j, ok = 1;
      stk *S;

      m->steps++;
      if (o >= NSYL) {                              /* blank: glide */
         int n = 4, moved = 0;
         while (n-- > 0) {
            advance(m);
            { unsigned t = CELL(g, m->x, m->y) - HB; if (t < NSYL) { moved = 1; break; } }
         }
         if (moved) continue;
         if (m->dx == 0 && m->dy != 0 && m->skip[m->x] && (m->dy == 1 || m->dy == -1)) {
            const int *t = m->skip[m->x];
            int lo = 0, hi = m->skipn[m->x] - 1, res = -1;
            if (m->dy > 0) {
               while (lo <= hi) { int mid = (lo + hi) / 2; if (t[mid] > m->y) { res = t[mid]; hi = mid - 1; } else lo = mid + 1; }
               m->y = res >= 0 ? res : t[0];
            } else {
               while (lo <= hi) { int mid = (lo + hi) / 2; if (t[mid] < m->y) { res = t[mid]; lo = mid + 1; } else hi = mid - 1; }
               m->y = res >= 0 ? res : t[m->skipn[m->x] - 1];
            }
         } else advance(m);
         continue;
      }

      c = m->cho[o]; v = m->ju[o]; j = m->jo[o];
      switch (v) {
         case 0:  m->dx =  1; m->dy =  0; break;
         case 2:  m->dx =  2; m->dy =  0; break;
         case 4:  m->dx = -1; m->dy =  0; break;
         case 6:  m->dx = -2; m->dy =  0; break;
         case 8:  m->dx =  0; m->dy = -1; break;
         case 12: m->dx =  0; m->dy = -2; break;
         case 13: m->dx =  0; m->dy =  1; break;
         case 17: m->dx =  0; m->dy =  2; break;
         case 18: m->dy = -m->dy; break;
         case 19: m->dx = -m->dx; m->dy = -m->dy; break;
         case 20: m->dx = -m->dx; break;
         default: break;
      }

      /* Arithmetic goes through unsigned, where wraparound is defined. A
         grid is just data and may hold values whose sum or product overflows;
         signed overflow would be undefined behaviour, not a wrong answer.
         Division and remainder are guarded separately because LLONG_MIN / -1
         traps rather than wrapping. */
      S = &m->st[m->cur];
      { int q = (m->cur == QUEUE);
        switch (c) {
         case 2:
            if (stk_count(S) < 2) ok = 0;
            else { long long a = stk_pop(S, q), b = stk_pop(S, q);
                   stk_push(S, a == 0 ? 0 :
                            a == -1 ? (long long)(0ULL - (unsigned long long)b) : b / a); }
            break;
         case 3:
            if (stk_count(S) < 2) ok = 0;
            else { long long a = stk_pop(S, q), b = stk_pop(S, q);
                   stk_push(S, (long long)((unsigned long long)b + (unsigned long long)a)); }
            break;
         case 4:
            if (stk_count(S) < 2) ok = 0;
            else { long long a = stk_pop(S, q), b = stk_pop(S, q);
                   stk_push(S, (long long)((unsigned long long)b * (unsigned long long)a)); }
            break;
         case 5:
            if (stk_count(S) < 2) ok = 0;
            else { long long a = stk_pop(S, q), b = stk_pop(S, q);
                   stk_push(S, (a == 0 || a == -1) ? 0 : ((b % a) + a) % a); }
            break;
         case 6:
            if (!stk_count(S)) ok = 0;
            else {
               long long n = stk_pop(S, q);
               if (j == 21) emit_number(n, emit);
               else if (j == 27) emit((int)n);
            }
            break;
         case 7:
            if (j == 27) {
               int ic = input_get(m);
               if (ic < 0) { m->waiting = 1; m->steps--; return; }   /* park, do not spin */
               m->waiting = 0;
               stk_push(S, ic);
            } else if (j != 21) stk_push(S, STROKES[j]);
            break;
         case 8: if (!stk_count(S)) ok = 0; else stk_push(S, stk_peek(S, q)); break;
         case 9: m->cur = j; break;
         case 10: if (!stk_count(S)) ok = 0; else { long long n = stk_pop(S, q); if (j != 27) stk_push(&m->st[j], n); } break;
         case 12: if (stk_count(S) < 2) ok = 0; else { long long a = stk_pop(S, q), b = stk_pop(S, q); stk_push(S, b >= a ? 1 : 0); } break;
         case 14: if (!stk_count(S)) ok = 0; else { long long n = stk_pop(S, q); if (n == 0) { m->dx = -m->dx; m->dy = -m->dy; } } break;
         case 15:
            if (stk_count(S) < 2) ok = 0;
            else { long long t = S->v[S->len - 1]; S->v[S->len - 1] = S->v[S->len - 2]; S->v[S->len - 2] = t; }
            break;
         case 16:
            if (stk_count(S) < 2) ok = 0;
            else { long long a = stk_pop(S, q), b = stk_pop(S, q);
                   stk_push(S, (long long)((unsigned long long)b - (unsigned long long)a)); }
            break;
         case 17: if (!stk_count(S)) ok = 0; else { long long n = stk_peek(S, q); if (j != 27) stk_push(&m->st[j], n); } break;
         case 18: m->done = 1; break;
         default: break;
        }
      }
      if (m->done) break;
      if (!ok) { m->dx = -m->dx; m->dy = -m->dy; }
      advance(m);
   }
}

/* ------------------------------------------------------------ rendering */
static void draw_glyph(int cx, int cy, unsigned char ch, uint32_t fg, uint32_t bg)
{
   int gy, gx;
   for (gy = 0; gy < 8; gy++) {
      unsigned char bits = font8x8[ch][gy];
      uint32_t *p = fb + (size_t)(cy * 8 + gy) * FBW + cx * 8;
      for (gx = 0; gx < 8; gx++) p[gx] = (bits >> gx) & 1 ? fg : bg;
   }
}

static void render(void)
{
   int y, x;
   for (y = 0; y < ROWS; y++)
      for (x = 0; x < COLS; x++) {
         draw_glyph(x, y, term[y][x], fg_col, bg_col);
      }
   /* the line being typed, drawn over the terminal rather than into it */
   {
      int cx = term_x, cy = term_y, i;
      for (i = 0; i < line_n && cx < COLS; i++, cx++)
         draw_glyph(cx, cy, (unsigned char)line[i], fg_col, bg_col);
      if (cx < COLS && ((cursor_phase >> 4) & 1))
         draw_glyph(cx, cy, 0xDB, fg_col, bg_col);
   }
}

/* -------------------------------------------------------- core options */
static void check_variables(void)
{
   struct retro_variable var;

   var.key = "aheui_speed"; var.value = NULL;
   if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if      (!strcmp(var.value, "slow"))   opt_steps = 50000;
      else if (!strcmp(var.value, "normal")) opt_steps = 500000;
      else if (!strcmp(var.value, "fast"))   opt_steps = 4000000;
      else                                   opt_steps = 20000000;
   }
   var.key = "aheui_palette"; var.value = NULL;
   if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if      (!strcmp(var.value, "green")) { fg_col = 0x0033FF66; bg_col = 0x00001100; }
      else if (!strcmp(var.value, "amber")) { fg_col = 0x00FFB000; bg_col = 0x00120A00; }
      else                                  { fg_col = 0x00E8E8E8; bg_col = 0x00101010; }
   }
}

static void keyboard_event(bool down, unsigned keycode, uint32_t ch, uint16_t mod)
{
   (void)mod;
   if (!down) return;
   if (keycode == RETROK_RETURN || keycode == RETROK_KP_ENTER) {
      int i;
      for (i = 0; i < line_n; i++) term_put(line[i]);
      term_put('\n');
      for (i = 0; i < line_n; i++) {
         int nt = (machine.intail + 1) % 4096;
         if (nt == machine.inhead) break;
         machine.in[machine.intail] = (unsigned char)line[i];
         machine.intail = nt;
      }
      { int nt = (machine.intail + 1) % 4096;
        if (nt != machine.inhead) { machine.in[machine.intail] = '\n'; machine.intail = nt; } }
      line_n = 0;
      machine.waiting = 0;
      return;
   }
   if (keycode == RETROK_BACKSPACE) { if (line_n) line_n--; return; }
   if (line_n >= (int)sizeof line - 1) return;
   if (ch >= 32 && ch < 127) { line[line_n++] = (char)ch; return; }
   if (ch) {
      int b;
      for (b = 0x80; b < 256; b++)
         if (cp437_unicode[b] == ch) { line[line_n++] = (char)b; return; }
      for (b = 1; b < 32; b++)
         if (cp437_unicode[b] == ch) { line[line_n++] = (char)b; return; }
   }
}

/* --------------------------------------------------------- libretro API */
void retro_init(void)
{
   fb = (uint32_t *)calloc((size_t)FBW * FBH, sizeof *fb);
   memset(silence, 0, sizeof silence);
   term_clear();
}

void retro_deinit(void)
{
   free(fb); fb = NULL;
   vm_free(&machine);
   have_content = 0;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof *info);
   info->library_name     = "Aheui";
   info->library_version  = "0.1";
   info->valid_extensions = "png|aheui|txt|b|bf";
   info->need_fullpath    = false;
   info->block_extract    = false;   /* let the frontend unzip and pass the file */
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof *info);
   info->geometry.base_width   = FBW;
   info->geometry.base_height  = FBH;
   info->geometry.max_width    = FBW;
   info->geometry.max_height   = FBH;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 44100.0;
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "aheui_speed",   "Steps per frame; slow|normal|fast|unlimited" },
      { "aheui_palette", "Palette; white|green|amber" },
      { NULL, NULL }
   };
   bool no_game = false;      /* there is nothing to run without content */
   env_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   {
      struct retro_log_callback lc;
      if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc)) log_cb = lc.log;
   }
   {
      struct retro_keyboard_callback kc;
      kc.callback = keyboard_event;
      cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kc);
   }
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_reset(void)
{
   int i;
   for (i = 0; i < NSTACK; i++) { machine.st[i].len = 0; machine.st[i].head = 0; }
   machine.x = machine.y = 0; machine.dx = 1; machine.dy = 0;
   machine.cur = 0; machine.done = 0; machine.waiting = 0; machine.steps = 0;
   machine.inhead = machine.intail = 0;
   line_n = 0;
   term_clear();
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   const unsigned char *p;
   size_t n;
   int ok = 0;

   if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
      logf_(RETRO_LOG_ERROR, "XRGB8888 not supported by the frontend\n");
      return false;
   }
   check_variables();
   vm_free(&machine);
   term_clear();
   if (!game || !game->data || !game->size) {
      logf_(RETRO_LOG_INFO, "no content; nothing to run\n");
      return false;
   }
   p = (const unsigned char *)game->data;
   n = game->size;

   if (n > 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
      size_t rawlen = 0;
      unsigned char *raw = png_decode(p, n, &rawlen);
      if (!raw) return false;
      ok = grid_from_png(&machine.g, raw, rawlen);
      free(raw);
   } else {
      /* Hangul anywhere means it is already a grid; otherwise Brainfuck. */
      size_t i;
      int hangul = 0;
      for (i = 0; i + 2 < n && i < 65536; i++)
         if (p[i] >= 0xEA && p[i] <= 0xED) { hangul = 1; break; }
      ok = hangul ? grid_from_text(&machine.g, p, n)
                  : grid_from_bf(&machine.g, p, n);
   }
   if (!ok) {
      logf_(RETRO_LOG_ERROR, "could not build a grid from this content\n");
      vm_free(&machine);
      return false;
   }
   if (!vm_prepare(&machine)) {
      logf_(RETRO_LOG_ERROR, "out of memory preparing the VM\n");
      vm_free(&machine);
      return false;
   }

   logf_(RETRO_LOG_INFO, "grid %d rows x %d cols\n", machine.g.rows, machine.g.W);
   have_content = 1;
   return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{ (void)type; (void)info; (void)num; return false; }

void retro_unload_game(void) { vm_free(&machine); have_content = 0; }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

/* Save states would have to capture 28 growable stacks plus the position and
 * direction. Not implemented; a size of 0 tells the frontend so rather than
 * handing it something that would restore into a wrong machine. */
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void)index; (void)enabled; (void)code; }

void retro_run(void)
{
   bool updated = false;

   if (input_poll_cb) input_poll_cb();
   if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

   if (have_content && !machine.done && !machine.waiting)
      vm_run(&machine, (long)opt_steps, emit_char);

   cursor_phase++;
   render();
   if (video_cb) video_cb(fb, FBW, FBH, (size_t)FBW * sizeof *fb);
   if (audio_batch_cb) audio_batch_cb(silence, 735);
}
