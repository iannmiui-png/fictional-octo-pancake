/* test_frontend.c -- a minimal libretro frontend, enough to drive the core.
 *
 *   ./test_frontend core.so content.png [--frames N] [--type "line"] ...
 *
 * The text it prints is read back out of the framebuffer by matching each 8x8
 * cell against the font, not by asking the core what it printed. That keeps
 * the renderer inside the test: if a glyph is drawn wrong, or drawn in the
 * wrong cell, the comparison fails.
 *
 * Typed lines are injected when a frame comes back byte-identical to the one
 * before it, which is the frontend's only visible sign that the core has
 * stopped producing output and is waiting on the keyboard.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "font8x8.h"

#define COLS 80
#define ROWS 50

static uint32_t *frame;
static size_t frame_px;
static unsigned fw, fh;
static retro_keyboard_event_t kb;
static const char *opt_speed = "fast", *opt_palette = "white";

static void logfn(enum retro_log_level lv, const char *fmt, ...)
{
   va_list ap;
   (void)lv;
   va_start(ap, fmt);
   fprintf(stderr, "[core] ");
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static bool environ_cb(unsigned cmd, void *data)
{
   switch (cmd) {
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
      case RETRO_ENVIRONMENT_SET_VARIABLES:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_MESSAGE:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE: {
         struct retro_variable *v = (struct retro_variable *)data;
         if (!strcmp(v->key, "aheui_speed"))        v->value = opt_speed;
         else if (!strcmp(v->key, "aheui_palette")) v->value = opt_palette;
         else return false;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = logfn;
         return true;
      case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
         kb = ((struct retro_keyboard_callback *)data)->callback;
         return true;
      default:
         return false;
   }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
   size_t n = (size_t)width * height;
   if (!data) return;
   if (n != frame_px) { free(frame); frame = (uint32_t *)malloc(n * 4); frame_px = n; }
   fw = width; fh = height;
   {
      unsigned y;
      for (y = 0; y < height; y++)
         memcpy(frame + (size_t)y * width, (const char *)data + y * pitch, (size_t)width * 4);
   }
}

static void audio_dummy_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_dummy_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll(void) {}
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id)
{ (void)p; (void)d; (void)i; (void)id; return 0; }

/* Reverse the font: ASCII wins any tie, so a duplicated glyph reads back as
 * the character a person would have typed. */
static int glyph_to_char(const unsigned char *bits)
{
   int c;
   for (c = 32; c < 127; c++) if (!memcmp(font8x8[c], bits, 8)) return c;
   for (c = 0; c < 256; c++)  if (!memcmp(font8x8[c], bits, 8)) return c;
   return '?';
}

static void screen_text(char out[ROWS][COLS + 1])
{
   unsigned cy, cx, gy, gx;
   uint32_t bg;
   /* The background is the majority colour, not whatever happens to sit at
      pixel 0. A shade glyph in the top-left cell starts on a foreground pixel,
      and taking that as the background inverts every glyph on the screen. */
   {
      size_t i, same = 0;
      uint32_t other = frame[0];
      for (i = 0; i < frame_px; i++) {
         if (frame[i] == frame[0]) same++;
         else other = frame[i];
      }
      bg = (same * 2 >= frame_px) ? frame[0] : other;
   }
   for (cy = 0; cy < ROWS; cy++) {
      for (cx = 0; cx < COLS; cx++) {
         unsigned char bits[8];
         for (gy = 0; gy < 8; gy++) {
            unsigned char b = 0;
            for (gx = 0; gx < 8; gx++)
               if (frame[(size_t)(cy * 8 + gy) * fw + cx * 8 + gx] != bg) b |= (unsigned char)(1 << gx);
            bits[gy] = b;
         }
         out[cy][cx] = (char)glyph_to_char(bits);
      }
      out[cy][COLS] = 0;
      { int e = COLS - 1; while (e >= 0 && out[cy][e] == ' ') out[cy][e--] = 0; }
   }
}

/* argv is UTF-8; a real frontend hands the core a Unicode codepoint, so decode
   rather than passing raw bytes. Typing a CP437 glyph has to arrive as the
   codepoint that draws it, not as the byte that happens to encode it here. */
static void type_line(const char *s)
{
   if (!kb) return;
   while (*s) {
      unsigned char c = (unsigned char)*s;
      uint32_t cp;
      if (c < 0x80)        { cp = c; s += 1; }
      else if (c < 0xE0)   { cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); s += 2; }
      else if (c < 0xF0)   { cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu); s += 3; }
      else                 { cp = '?'; s += 4; }
      kb(true, cp < 128 ? cp : 0, cp, 0);
   }
   kb(true, RETROK_RETURN, 0, 0);
}

int main(int argc, char **argv)
{
   void *lib;
   void (*p_init)(void), (*p_deinit)(void), (*p_run)(void);
   void (*p_set_env)(retro_environment_t), (*p_set_video)(retro_video_refresh_t);
   void (*p_set_as)(retro_audio_sample_t), (*p_set_ab)(retro_audio_sample_batch_t);
   void (*p_set_ip)(retro_input_poll_t), (*p_set_is)(retro_input_state_t);
   bool (*p_load)(const struct retro_game_info *);
   void (*p_get_si)(struct retro_system_info *);
   void (*p_get_av)(struct retro_system_av_info *);
   unsigned (*p_api)(void);
   struct retro_system_info si;
   struct retro_system_av_info av;
   struct retro_game_info gi;
   const char *lines[8];
   int nlines = 0, frames = 600, i, f, injected = 0, cycles = 0;
   FILE *fp;
   char *buf;
   long sz;
   static char text[ROWS][COLS + 1];
   uint32_t *prev = NULL;

   if (argc < 3) { fprintf(stderr, "usage: %s core.so content [--frames N] [--type LINE]\n", argv[0]); return 2; }
   for (i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--type") && i + 1 < argc && nlines < 8) lines[nlines++] = argv[++i];
      else if (!strcmp(argv[i], "--speed") && i + 1 < argc) opt_speed = argv[++i];
      else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
   }

   lib = dlopen(argv[1], RTLD_NOW);
   if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define SYM(v, n) do { *(void **)&v = dlsym(lib, n); if (!v) { fprintf(stderr, "missing %s\n", n); return 1; } } while (0)
   SYM(p_init, "retro_init"); SYM(p_deinit, "retro_deinit"); SYM(p_run, "retro_run");
   SYM(p_set_env, "retro_set_environment"); SYM(p_set_video, "retro_set_video_refresh");
   SYM(p_set_as, "retro_set_audio_sample"); SYM(p_set_ab, "retro_set_audio_sample_batch");
   SYM(p_set_ip, "retro_set_input_poll"); SYM(p_set_is, "retro_set_input_state");
   SYM(p_load, "retro_load_game"); SYM(p_get_si, "retro_get_system_info");
   SYM(p_get_av, "retro_get_system_av_info"); SYM(p_api, "retro_api_version");
#undef SYM

   if (p_api() != RETRO_API_VERSION) { fprintf(stderr, "api version mismatch\n"); return 1; }
   p_get_si(&si);
   fprintf(stderr, "core: %s %s [%s]\n", si.library_name, si.library_version, si.valid_extensions);

   p_set_env(environ_cb);
   p_init();
   p_set_video(video_cb); p_set_as(audio_dummy_sample); p_set_ab(audio_dummy_batch);
   p_set_ip(input_poll); p_set_is(input_state);

   fp = fopen(argv[2], "rb");
   if (!fp) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
   fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
   buf = (char *)malloc((size_t)sz);
   if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
   fclose(fp);

   memset(&gi, 0, sizeof gi);
   gi.path = argv[2]; gi.data = buf; gi.size = (size_t)sz;
   if (!p_load(&gi)) {
      fprintf(stderr, "retro_load_game failed\n");
      p_deinit(); free(buf); dlclose(lib);
      return 1;
   }
   p_get_av(&av);
   fprintf(stderr, "av: %ux%u @ %.1f fps\n", av.geometry.base_width,
           av.geometry.base_height, av.timing.fps);

   for (f = 0; f < frames; f++) {
      p_run();
      if (!frame) continue;
      if (prev && injected < nlines && !memcmp(prev, frame, frame_px * 4)) {
         type_line(lines[injected++]);           /* screen settled: it wants input */
      }
      if (!prev) prev = (uint32_t *)malloc(frame_px * 4);
      memcpy(prev, frame, frame_px * 4);
   }

   screen_text(text);
   for (i = 0; i < ROWS; i++) printf("%s\n", text[i]);
   {
      void (*p_unload)(void);
      *(void **)&p_unload = dlsym(lib, "retro_unload_game");
      if (p_unload) p_unload();
      if (cycles) {                      /* load the same content again */
         int k;
         for (k = 0; k < cycles; k++) {
            if (!p_load(&gi)) { fprintf(stderr, "reload %d failed\n", k); return 1; }
            for (f = 0; f < 5; f++) p_run();
            if (p_unload) p_unload();
         }
         fprintf(stderr, "unload/reload cycles: %d ok\n", cycles);
      }
   }
   p_deinit();
   free(buf); free(prev); free(frame);
   dlclose(lib);
   return 0;
}
