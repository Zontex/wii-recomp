// Picture copies. The game draws a scene, then copies the picture into memory to use as
// a texture. Here the picture stays on the graphics card, filed by its address, and a
// texture at that address uses the card's copy while memory still holds it.
//
// Memory gets the copy's real bytes, encoded as the chip does (gx_encode.c), but only
// when something other than our texture cache reads them. The copy's pages are held
// (memory_watch.h): reading or writing them faults, from the game, the runtime or the
// kernel, and only then is the copy read back from the card, encoded and written, and
// the access goes on. Reading back means waiting for the card, so at a place the game has
// read lately each new copy is read back as it's made, without waiting, and stays held
// until a touch finds its bytes there; a place nothing reads for a while stops. A touch of
// something else on a copy's first or last page, which games do every frame, doesn't
// wait either: the copy's bytes on that page land with the next picture shown, in place
// of only the bytes nothing has written since, so a late copy never undoes a write. A
// copy a later one only partly covers stays held apart, on a texture of its own, and both
// land in order when something reads there. Only a touch of a copy made in the same
// picture waits for the card, and says so.
// WII_RECOMP_COPIES_ON_CARD=1 leaves memory alone, as before copies reached it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "debug.h"
#include "gaps.h"
#include "gx_backend.h"
#include "gx_encode.h"
#include "memory_watch.h"

enum {
  BP_COPY_ORIGIN = 0x49, BP_COPY_SIZE = 0x4a, BP_COPY_TARGET = 0x4b, BP_COPY_STRIDE = 0x4d, BP_PIXEL_FORMAT = 0x43, BP_FILTER_LOW = 0x53,
  BP_FILTER_HIGH = 0x54, MAX_COPIES = 512, PIXEL_FORMAT_MASK = 7,
  BP_TEXTURE_IMAGE = 0x88, BP_TEXTURE_IMAGE_HIGH = 0xa8, BP_TEXTURE_ADDRESS = 0x94, BP_TEXTURE_ADDRESS_HIGH = 0xb4,
  HALF_SIZE_BIT = 9, FORMAT_SHIFT = 3, FORMAT_MASK = 15, COORDINATE_MASK = 0x3ff, COORDINATE_BITS = 10, TEXTURE_SIZE_MASK = 0x3ff,
  TEXTURE_FORMAT_SHIFT = 20, TEXTURE_FORMAT_MASK = 15, ADDRESS_SHIFT = 5, CLAMP_TOP_BIT = 0, CLAMP_BOTTOM_BIT = 1, GAMMA_SHIFT = 7,
  INTENSITY_BIT = 15, AUTOMATIC_BIT = 16, FILTER_BITS = 6, FILTER_MASK = 63, FILTER_WHOLE = 64,
  TEXTURE_I4 = 0, TEXTURE_I8 = 1, TEXTURE_IA4 = 2, TEXTURE_IA8 = 3, TEXTURE_RGB565 = 4, TEXTURE_RGB5A3 = 5, TEXTURE_RGBA8 = 6, TEXTURE_C4 = 8, TEXTURE_C8 = 9,
  TEXTURE_C14X2 = 10, TEXTURE_CMPR = 14, TEXTURE_FORMATS = 16,
};
enum { MEMORY_NONE, MEMORY_HELD, MEMORY_WRITTEN };
// A place stops being read back as each copy there is made once nothing has read one
// for this many pictures.
enum { MAX_OLDER = 256, LEARNED_FRAMES = 600 };

// One copy as it lies in memory. A place's current copy is what textures there draw; an
// older one still held, that a later copy only partly covers, keeps its own texture until
// it lands or is covered whole.
typedef struct {
  uint32_t address;
  GxTexture *texture, *depth_texture;
  int from_depth, live, older;
  uint32_t width, height, format, row_bytes, rows, stride;
  CopyEncoding encoding;
  int memory;                // MEMORY_HELD while its bytes are only on the card.
  unsigned read_frame;       // Plus 1: the picture its bytes were last read by the game in.
  unsigned ticket;           // Its read back, while it's on its way.
  uint32_t made;             // In the order copies were made, so overlapping ones land oldest first.
  uint64_t watched, hash;    // Memory once written: the watch's count, or with none, the bytes' hash.
  unsigned checked_frame;    // The frame, plus 1, its memory was last checked in, when unwatched.
  // Pages let go before its bytes landed, for something beside it: what each held then, so
  // a byte the game has written there since isn't written over when the copy lands.
  uint8_t *before, *before_taken;
  size_t before_size;
} Copy;

static Copy copies[MAX_COPIES], older[MAX_OLDER];
static int copy_count;
static uint32_t copies_made;
static struct { unsigned held, landed_early, waited, kept_apart; } counts;

// Textures an older copy gave back, usable again from the next picture, as the backend
// may still be holding on to one for this one.
// Each keeps the size it was last copied at, so one of the size wanted is taken first
// and the card keeps its image rather than making another.
typedef struct { GxTexture *texture; uint32_t width, height; } Spare;
static Spare spare[2][MAX_OLDER], freed[2][MAX_OLDER];
static int spare_count[2], freed_count[2];

static GxTexture *take_texture(int depth, uint32_t width, uint32_t height) {
  int chosen = spare_count[depth] - 1;
  for (int i = 0; i < spare_count[depth]; i++) {
    if (spare[depth][i].width == width && spare[depth][i].height == height) chosen = i;
  }
  if (chosen < 0) return gx_backend->texture_create();
  GxTexture *texture = spare[depth][chosen].texture;
  spare[depth][chosen] = spare[depth][--spare_count[depth]];
  return texture;
}

static void give_texture(int depth, GxTexture *texture, uint32_t width, uint32_t height) {
  if (texture && freed_count[depth] < MAX_OLDER) freed[depth][freed_count[depth]++] = (Spare){texture, width, height};
}

// What each copy format lands in memory as, which is always one of the texture formats.
static const uint8_t base_formats[COPY_FORMATS] = {
  [COPY_R4] = TEXTURE_I4, [COPY_R8_LOW] = TEXTURE_I8, [COPY_RA4] = TEXTURE_IA4, [COPY_RA8] = TEXTURE_IA8, [COPY_RGB565] = TEXTURE_RGB565,
  [COPY_RGB5A3] = TEXTURE_RGB5A3, [COPY_RGBA8] = TEXTURE_RGBA8, [COPY_A8] = TEXTURE_I8, [COPY_R8] = TEXTURE_I8, [COPY_G8] = TEXTURE_I8, [COPY_B8] = TEXTURE_I8,
  [COPY_RG8] = TEXTURE_IA8, [COPY_GB8] = TEXTURE_IA8, [13] = TEXTURE_RGBA8, [14] = TEXTURE_RGBA8, [15] = TEXTURE_RGBA8,
};
static const uint8_t texel_bits[TEXTURE_FORMATS] = {
  [TEXTURE_I4] = 4, [TEXTURE_I8] = 8, [TEXTURE_IA4] = 8, [TEXTURE_IA8] = 16, [TEXTURE_RGB565] = 16, [TEXTURE_RGB5A3] = 16, [TEXTURE_RGBA8] = 32,
  [TEXTURE_C4] = 4, [TEXTURE_C8] = 8, [TEXTURE_C14X2] = 16, [TEXTURE_CMPR] = 4,
};

static uint32_t copy_format(uint32_t control) {
  uint32_t code = (control >> FORMAT_SHIFT) & FORMAT_MASK;
  return code / 2 + (code & 1) * 8;
}

static uint32_t copy_start(const Copy *copy) { return copy->address << ADDRESS_SHIFT; }
// Its last row ends with the row's own bytes, not the stride.
static uint32_t copy_end(const Copy *copy) { return copy_start(copy) + (copy->rows - 1) * copy->stride + copy->row_bytes; }
static uint32_t copy_bytes(const Copy *copy) { return copy_end(copy) - copy_start(copy); }
static int overlaps(const Copy *copy, uint32_t start, uint32_t end) { return copy_start(copy) < end && start < copy_end(copy); }
static GxTexture *texture_of(const Copy *copy) { return copy->from_depth ? copy->depth_texture : copy->texture; }

static uint32_t page_size(void) {
  static uint32_t bytes;
  if (!bytes) bytes = (uint32_t)sysconf(_SC_PAGESIZE);
  return bytes;
}

static int copy_log(void) {
  static int log = -1;
  if (log < 0) log = getenv("WII_RECOMP_COPY_LOG") != 0;
  return log;
}

// Every copy still held, places' and older ones alike.
#define EACH_HELD(copy) \
  for (int each_ = 0; each_ < copy_count + MAX_OLDER; each_++) \
    for (Copy *copy = each_ < copy_count ? &copies[each_] : &older[each_ - copy_count]; copy; copy = 0) \
      if (copy->memory == MEMORY_HELD)

// Notes what every page of the copy that's no longer held holds now, once.
static void take_before(Copy *copy) {
  uint32_t page_bytes = page_size(), first = copy_start(copy) & ~(page_bytes - 1), pages = (copy_end(copy) - first + page_bytes - 1) / page_bytes;
  if (copy->before_size < copy_bytes(copy)) {
    copy->before = realloc(copy->before, copy->before_size = copy_bytes(copy));
    copy->before_taken = realloc(copy->before_taken, copy->before_size / page_bytes + 2);
    memset(copy->before_taken, 0, copy->before_size / page_bytes + 2);
  }
  const uint8_t *memory = host(copy_start(copy) | 0x80000000u);
  for (uint32_t i = 0; i < pages; i++) {
    uint32_t page = first + i * page_bytes;
    if (copy->before_taken[i] || watch_held(page, 1)) continue;
    uint32_t from = page > copy_start(copy) ? page - copy_start(copy) : 0, to = page + page_bytes - copy_start(copy);
    if (to > copy_bytes(copy)) to = copy_bytes(copy);
    memcpy(copy->before + from, memory + from, to - from);
    copy->before_taken[i] = 1;
  }
}

static int was_let_go(const Copy *copy) {
  if (!copy->before_taken) return 0;
  for (size_t i = 0; i < copy->before_size / page_size() + 2; i++) {
    if (copy->before_taken[i]) return 1;
  }
  return 0;
}

// Writes the copy's encoded rows. Pages still held take them whole; on one let go early,
// only bytes unchanged since then take the copy's.
static void land(Copy *copy, uint8_t *memory, const uint8_t *encoded, const uint8_t *held) {
  uint32_t page_bytes = page_size(), first = copy_start(copy) & ~(page_bytes - 1);
  // Notes made for a smaller copy than this one say nothing of it.
  int taken = copy->before_taken && copy->before_size >= copy_bytes(copy);
  for (uint32_t row = 0; row < copy->rows; row++) {
    uint32_t at = row * copy->stride, end = at + copy->row_bytes;
    const uint8_t *from = encoded + (size_t)row * copy->row_bytes - at;
    while (at < end) {
      uint32_t page = (copy_start(copy) + at - first) / page_bytes, page_end = (page + 1) * page_bytes + first - copy_start(copy);
      uint32_t stop = page_end < end ? page_end : end;
      if (held[page]) {
        memcpy(memory + at, from + at, stop - at);
      } else if (taken && copy->before_taken[page]) {
        for (uint32_t i = at; i < stop; i++) {
          if (memory[i] == copy->before[i]) memory[i] = from[i];
        }
      }
      at = stop;
    }
  }
}

// Eight bytes at a time: every byte counts, as any write must show.
static uint64_t hash_bytes(const uint8_t *data, uint32_t bytes) {
  uint64_t value = 1469598103934665603ull, word;
  uint32_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    memcpy(&word, data + i, sizeof word);
    value = (value ^ word) * 1099511628211ull;
  }
  for (; i < bytes; i++) value = (value ^ data[i]) * 1099511628211ull;
  return value;
}

// An older copy, done with: its texture goes back for another.
static void drop(Copy *copy) {
  copy->memory = MEMORY_NONE;
  copy->ticket = 0;
  if (!copy->older) return;
  give_texture(copy->from_depth, texture_of(copy), copy->width, copy->height);
  copy->texture = copy->depth_texture = 0;
  copy->older = 0;
}

// Starts reading a copy back, at its own size.
static unsigned read_copy(Copy *copy) {
  if (!gx_backend->read_texture) return 0;
  return gx_backend->read_texture(texture_of(copy), (int)copy->width, (int)copy->height, copy->encoding.half);
}

// The copy's pixels, as the card read them back, encoded into memory's rows. Waits for the
// card only when the read back isn't there yet, which is named.
static const uint8_t *encode(Copy *copy, const char *why) {
  unsigned ticket = copy->ticket;
  copy->ticket = 0;
  int width = 0, height = 0;
  const uint8_t *read = 0;
  if (ticket && gx_backend->read_ready(ticket)) read = gx_backend->read_finish(ticket, &width, &height);
  if (!read) {
    // Touched in the picture it was made in, or never read back: only now can it be.
    if (!ticket) ticket = read_copy(copy);
    counts.waited++;
    if (copy_log() || counts.waited <= 8) {
      fprintf(stderr, "copies: waited for the card for the copy at %08x (%ux%u), %s in frame %u\n", copy_start(copy) | 0x80000000u, copy->width,
              copy->height, why, gx_frame_number());
    }
    read = ticket ? gx_backend->read_finish(ticket, &width, &height) : 0;
  }
  if (!read || !width || !height) return 0;
  // The texture keeps the picture's full detail. Each texel takes the pixel at its middle,
  // or for a halved copy the mean of all it covers, which holds the whole 2 by 2 box.
  static uint32_t *pixels;
  static uint8_t *encoded;
  static size_t pixels_size, encoded_size;
  size_t need = (size_t)copy->width * copy->height, encoded_need = (size_t)copy->rows * copy->row_bytes;
  if (need > pixels_size) pixels = realloc(pixels, (pixels_size = need) * sizeof *pixels);
  if (encoded_need > encoded_size) encoded = realloc(encoded, encoded_size = encoded_need);
  int boxed = copy->encoding.half && !copy->from_depth;
  // Read back at the copy's own size, color is used as it came.
  int direct = !copy->from_depth && (uint32_t)width == copy->width && (uint32_t)height == copy->height;
  for (uint32_t y = 0; y < copy->height && !direct; y++) {
    uint32_t top = (uint32_t)((uint64_t)y * (uint64_t)height / copy->height), bottom = (uint32_t)((uint64_t)(y + 1) * (uint64_t)height / copy->height);
    uint32_t middle_y = (uint32_t)(((uint64_t)y * 2 + 1) * (uint64_t)height / (copy->height * 2));
    for (uint32_t x = 0; x < copy->width; x++) {
      uint32_t left = (uint32_t)((uint64_t)x * (uint64_t)width / copy->width), right = (uint32_t)((uint64_t)(x + 1) * (uint64_t)width / copy->width);
      uint32_t middle_x = (uint32_t)(((uint64_t)x * 2 + 1) * (uint64_t)width / (copy->width * 2)), value = 0;
      if (copy->from_depth) {
        float depth;
        memcpy(&depth, read + ((size_t)middle_y * (uint32_t)width + middle_x) * 4, sizeof depth);
        value = gx_depth_24(depth);
      } else if (boxed && right > left && bottom > top) {
        uint32_t sum[4] = {0, 0, 0, 0}, count = (right - left) * (bottom - top);
        for (uint32_t from_y = top; from_y < bottom; from_y++) {
          for (uint32_t from_x = left; from_x < right; from_x++) {
            const uint8_t *pixel = read + ((size_t)from_y * (uint32_t)width + from_x) * 4;
            for (int i = 0; i < 4; i++) sum[i] += pixel[i];
          }
        }
        for (int i = 0; i < 4; i++) value |= (sum[i] / count) << (i * 8);
      } else {
        memcpy(&value, read + ((size_t)middle_y * (uint32_t)width + middle_x) * 4, sizeof value);
      }
      pixels[(size_t)y * copy->width + x] = value;
    }
  }
  CopyEncoding encoding = copy->encoding;
  encoding.half = 0;
  encoding.stride = copy->row_bytes;
  CopySource source = {pixels, (int)copy->width, (int)copy->height, 0, 0};
  if (direct) source.pixels = (const uint32_t *)read;
  gx_encode_copy(&encoding, &source, encoded);
  return encoded;
}

// Lands a copy with every held one it overlaps, oldest first, so a later copy's bytes win
// where they meet. Each copy's pages are seen as held or not before any lands, as landing
// lets them go. Pages of others still held that this opened are held again, so data
// beside a copy being read doesn't let its neighbours go.
enum { MOST_LANDING = 64 };
static void write_back(Copy *copy, const char *why) {
  if (copy->memory != MEMORY_HELD) return;
  uint32_t start = copy_start(copy), end = copy_end(copy), page_bytes = page_size();
  Copy *group[MOST_LANDING];
  uint8_t *encoded[MOST_LANDING], *held[MOST_LANDING];
  int count = 0;
  EACH_HELD(other) {
    if (overlaps(other, start, end) && count < MOST_LANDING) group[count++] = other;
  }
  for (int i = 1; i < count; i++) {  // Oldest first.
    for (int j = i; j > 0 && group[j]->made < group[j - 1]->made; j--) {
      Copy *swap = group[j];
      group[j] = group[j - 1], group[j - 1] = swap;
    }
  }
  for (int i = 0; i < count; i++) {
    Copy *one = group[i];
    if (copy_log()) fprintf(stderr, "copies: %08x %ux%u lands, frame %u, %s\n", copy_start(one), one->width, one->height, gx_frame_number(), why);
    uint32_t first = copy_start(one) & ~(page_bytes - 1), pages = (copy_end(one) - first + page_bytes - 1) / page_bytes;
    held[i] = malloc(pages);
    // Unwatched memory was never held, and the copy lands as it's made.
    for (uint32_t page = 0; page < pages; page++) held[i][page] = (uint8_t)(!watch_enabled() || watch_held(first + page * page_bytes, 1));
    uint32_t at = copy_start(one) | 0x80000000u;
    const uint8_t *bytes = host(at) && host(at + copy_bytes(one) - 1) ? encode(one, why) : 0;
    encoded[i] = bytes ? malloc((size_t)one->rows * one->row_bytes) : 0;
    if (bytes) memcpy(encoded[i], bytes, (size_t)one->rows * one->row_bytes);
  }
  for (int i = 0; i < count; i++) watch_release(copy_start(group[i]) | 0x80000000u, copy_bytes(group[i]));
  for (int i = 0; i < count; i++) {
    Copy *one = group[i];
    uint32_t at = copy_start(one) | 0x80000000u, bytes = copy_bytes(one);
    if (!encoded[i]) {
      // The card no longer holds it, or this backend can't read it back.
      gap_hit("pixel.copy_to_texture", copy_start(one));
      one->live = 0;
      drop(one);
    } else {
      uint8_t *memory = host(at);
      land(one, memory, encoded[i], held[i]);
      if (one->before_taken) memset(one->before_taken, 0, one->before_size / page_bytes + 2);
      if (one->older) {
        drop(one);
      } else {
        one->memory = MEMORY_WRITTEN;
        one->watched = watch_range(at, bytes);
        if (one->watched == WATCH_NONE) one->hash = hash_bytes(memory, bytes);  // Only needed unwatched.
        one->checked_frame = gx_frame_number() + 1;
      }
    }
    free(encoded[i]);
    free(held[i]);
  }
  EACH_HELD(other) {
    uint32_t first = start & ~(page_bytes - 1), last = (end + page_bytes - 1) & ~(page_bytes - 1);
    if (overlaps(other, first, last) && !was_let_go(other)) watch_hold(copy_start(other), copy_bytes(other));
  }
}

// The game read a place's copy, so new copies there are read back as they're made.
static void learn(Copy *copy, const char *how) {
  Copy *place = copy;
  if (copy->older) {
    place = 0;
    for (int i = 0; i < copy_count && !place; i++) {
      if (copies[i].address == copy->address) place = &copies[i];
    }
    if (!place) return;
  }
  if (!place->read_frame && copy_log()) {
    fprintf(stderr, "copies: the one at %08x (%ux%u, format %u) was %s, so it's read back as made\n", copy_start(place) | 0x80000000u,
            place->width, place->height, place->encoding.format, how);
  }
  place->read_frame = gx_frame_number() + 1;
}

static int learned(const Copy *place) { return place->read_frame && gx_frame_number() + 1 - place->read_frame < LEARNED_FRAMES; }

// A held copy whose page is opened for something beside it, not its own bytes: data
// sharing a page with a copy is touched every frame in some games, and waiting for the
// card each time would cost a frame. Its read back goes on without waiting, and lands as
// a picture is shown; until then its bytes on that page are the ones there before.
static void expose(Copy *copy, const char *why, uint32_t at) {
  if (copy_log() && !was_let_go(copy)) {
    fprintf(stderr, "copies: %08x %ux%u format %u exposed %s %08x, lr %08x, frame %u\n", copy_start(copy), copy->width, copy->height,
            copy->encoding.format, why, at, cpu.lr, gx_frame_number());
  }
  take_before(copy);
  if (!copy->ticket) copy->ticket = read_copy(copy);
  gap_note("pixel.copy_to_texture", copy_start(copy));  // Its bytes on a shared page may be late.
}

// Any held copy with a page out of step: let go by memory_watch.c with no copy taking it.
static void settle_pages(void) {
  uint32_t page_bytes = page_size();
  EACH_HELD(copy) {
    for (uint32_t page = copy_start(copy) & ~(page_bytes - 1); page < copy_end(copy); page += page_bytes) {
      if (!watch_held(page, 1)) {
        expose(copy, "with its page let go at", page);
        break;
      }
    }
  }
}

// Something touched a held page, on the thread that owns the backend. A copy whose own
// bytes were touched is filled in now; others on the page are read back without waiting.
static void touched_work(void *argument) {
  uint32_t page_bytes = page_size(), at = *(const uint32_t *)argument & 0x3fffffffu, page = at & ~(page_bytes - 1);
  EACH_HELD(copy) {
    if (overlaps(copy, at, at + 1)) {
      learn(copy, "touched");
      write_back(copy, "touched");
    }
  }
  // The page is let go first, so what the others hold on it can be noted.
  watch_release(page | 0x80000000u, page_bytes);
  EACH_HELD(copy) {
    if (overlaps(copy, page, page + page_bytes)) expose(copy, "by a touch at", at);
  }
  settle_pages();
}

static void touched(uint32_t physical) { gx_run_on_graphics(touched_work, &physical); }

void gx_copy_prepare_read(uint32_t address, uint32_t bytes) {
  uint32_t start = address & 0x3fffffffu, end = start + bytes;
  EACH_HELD(copy) {
    if (overlaps(copy, start, end)) {
      learn(copy, "read as a texture");
      write_back(copy, "read as a texture");
    }
  }
  settle_pages();
}

// Whether a copy and every held one landing with it have their read backs there, so
// landing waits for nothing. Any without one starts one, for the next picture.
static int read_back_with_others(Copy *copy) {
  int ready = 1;
  EACH_HELD(other) {
    if (!overlaps(other, copy_start(copy), copy_end(copy))) continue;
    if (!other->ticket) other->ticket = read_copy(other);
    if (other->ticket && !gx_backend->read_ready(other->ticket)) ready = 0;  // None at all can't be waited for.
  }
  return ready;
}

void gx_copy_frame_shown(void) {
  // A copy let go early lands once its read back is there; the rest wait, held, for a touch.
  EACH_HELD(copy) {
    if (copy->ticket && was_let_go(copy) && read_back_with_others(copy)) {
      write_back(copy, "let go early, read back already");
      counts.landed_early++;
    }
  }
  settle_pages();
  for (int depth = 0; depth < 2; depth++) {
    while (freed_count[depth] && spare_count[depth] < MAX_OLDER) spare[depth][spare_count[depth]++] = freed[depth][--freed_count[depth]];
  }
  unsigned frame = gx_frame_number();
  if (frame <= 3 || frame % 300 == 0) {
    int places = 0;
    for (int i = 0; i < copy_count; i++) places += learned(&copies[i]);
    fprintf(stderr, "copies: %u held, %u kept apart from a later one, %u landed early, %u waited for, %d places read back as made\n",
            counts.held, counts.kept_apart, counts.landed_early, counts.waited, places);
  }
}

// Single-channel copies repeat that channel, as the game reads them as intensity.
static GxChannels channels_for(uint32_t control) {
  uint32_t format = copy_format(control);
  GxChannels channels = {GX_CHANNEL_RED, GX_CHANNEL_GREEN, GX_CHANNEL_BLUE, GX_CHANNEL_ALPHA};
  if (format == COPY_R4 || format == COPY_R8_LOW || format == COPY_R8) channels.green = channels.blue = channels.alpha = GX_CHANNEL_RED;
  else if (format == COPY_RA4 || format == COPY_RA8) channels.green = channels.blue = GX_CHANNEL_RED;
  else if (format == COPY_A8) channels.red = channels.green = channels.blue = GX_CHANNEL_ALPHA;
  else if (format == COPY_G8) channels.red = channels.blue = channels.alpha = GX_CHANNEL_GREEN;
  else if (format == COPY_B8) channels.red = channels.green = channels.alpha = GX_CHANNEL_BLUE;
  // An IA8 texture's intensity is red and its alpha green: RG8 is green over red, GB8 blue over green.
  else if (format == COPY_RG8) channels.green = channels.blue = GX_CHANNEL_RED, channels.alpha = GX_CHANNEL_GREEN;
  else if (format == COPY_GB8) channels.red = channels.blue = GX_CHANNEL_GREEN, channels.alpha = GX_CHANNEL_BLUE;
  // A picture with no alpha copies it as full, as in Dolphin's TextureConverterShaderGen.cpp.
  if (!gx_target_has_alpha()) {
    uint8_t *each[4] = {&channels.red, &channels.green, &channels.blue, &channels.alpha};
    for (int i = 0; i < 4; i++) {
      if (*each[i] == GX_CHANNEL_ALPHA) *each[i] = GX_CHANNEL_ONE;
    }
  }
  return channels;
}

void gx_copy_rectangle(int *x, int *y, int *width, int *height) {
  uint32_t origin = gx.bp[BP_COPY_ORIGIN], size = gx.bp[BP_COPY_SIZE];
  *x = (int)(origin & COORDINATE_MASK);
  *y = (int)((origin >> COORDINATE_BITS) & COORDINATE_MASK);
  *width = (int)(size & COORDINATE_MASK) + 1;
  *height = (int)((size >> COORDINATE_BITS) & COORDINATE_MASK) + 1;
}

// What decides a copy's bytes, from the registers. Behaviour Dolphin's two encoders
// disagree on, or leave unknown, is named and left out.
static CopyEncoding encoding_for(uint32_t control, int from_depth, uint32_t width, uint32_t height, uint32_t stride) {
  CopyEncoding e;
  memset(&e, 0, sizeof e);
  uint32_t format = copy_format(control);
  e.format = (uint8_t)format;
  e.depth = (uint8_t)from_depth;
  e.pixel_format = (uint8_t)(gx.bp[BP_PIXEL_FORMAT] & PIXEL_FORMAT_MASK);
  e.half = (control >> HALF_SIZE_BIT) & 1;
  e.clamp_top = (control >> CLAMP_TOP_BIT) & 1;
  e.clamp_bottom = (control >> CLAMP_BOTTOM_BIT) & 1;
  e.gamma = (control >> GAMMA_SHIFT) & 3;
  uint64_t filter = gx.bp[BP_FILTER_LOW] | (uint64_t)gx.bp[BP_FILTER_HIGH] << 24;
  for (int i = 0; i < 7; i++) e.filter[i] = (uint8_t)((filter >> (i * FILTER_BITS)) & FILTER_MASK);
  e.width = width, e.height = height, e.stride = stride;
  int intensity_format = format == COPY_R4 || format == COPY_R8_LOW || format == COPY_R8 || format == COPY_RA4 || format == COPY_RA8;
  if (((control >> INTENSITY_BIT) & 1) && ((control >> AUTOMATIC_BIT) & 1)) {
    if (intensity_format && !from_depth) e.intensity = 1;
    else gap_hit("pixel.copy_intensity_rgb_to_yuv", format | (uint32_t)from_depth << 8);
  }
  // Depth has no alpha for these, and Dolphin's software encoder has no answer for them.
  if (from_depth && (format == COPY_RA4 || format == COPY_RA8 || format == COPY_RGB565 || format == COPY_RGB5A3 || format == COPY_A8)) {
    gap_hit("pixel.depth_copy", format);
  }
  return e;
}

// Works out the shape a copy has in memory. Says no if it has none.
static int fit_copy(Copy *copy, uint32_t control, uint32_t width, uint32_t height, uint32_t stride) {
  uint32_t format = copy_format(control), across, rows, block_bytes;
  if (!gx_copy_shape(format, width, height, &across, &rows, &block_bytes)) {
    gap_hit("gx.0x52_trigger_copy", format);
    return 0;
  }
  copy->format = base_formats[format];
  copy->width = width;
  copy->height = height;
  copy->stride = stride;
  copy->rows = rows;
  copy->row_bytes = across * block_bytes;
  return copy->rows && copy->row_bytes;
}

static Copy *copy_for(uint32_t address) {
  for (int i = 0; i < copy_count; i++) {
    if (copies[i].address == address) return &copies[i];
  }
  if (copy_count == MAX_COPIES) return 0;
  Copy *copy = &copies[copy_count++];
  copy->address = address;
  copy->texture = gx_backend->texture_create();
  copy->depth_texture = gx_backend->texture_create();
  return copy;
}

// A place's held copy that a new one there won't cover: it keeps its pixels, and texture,
// apart, and the place gets another texture for the new copy.
static void keep_apart(Copy *copy, uint32_t width, uint32_t height) {
  Copy *apart = 0;
  for (int i = 0; i < MAX_OLDER && !apart; i++) {
    if (older[i].memory == MEMORY_NONE) apart = &older[i];
  }
  if (!apart) {
    // Too many waiting at once: the oldest lands now, and that's named.
    gap_hit("limit.picture_copies_kept_apart", MAX_OLDER);
    Copy *oldest = &older[0];
    for (int i = 1; i < MAX_OLDER; i++) {
      if (older[i].made < oldest->made) oldest = &older[i];
    }
    write_back(oldest, "pushed out by a newer one");
    apart = oldest;
  }
  uint8_t *before = apart->before, *before_taken = apart->before_taken;
  size_t before_size = apart->before_size;
  *apart = *copy;
  apart->older = 1;
  apart->live = 0;
  // The snapshot of pages let go stays with the copy it belongs to; the place takes the spare.
  copy->before = before, copy->before_taken = before_taken, copy->before_size = before_size;
  if (copy->before_taken) memset(copy->before_taken, 0, copy->before_size / page_size() + 2);
  if (copy->from_depth) copy->depth_texture = take_texture(1, width, height);
  else copy->texture = take_texture(0, width, height);
  copy->memory = MEMORY_NONE;
  copy->ticket = 0;
  counts.kept_apart++;
}

void gx_copy_to_texture(uint32_t control, int from_depth) {
  static int resolver_set;
  if (!resolver_set) {
    resolver_set = 1;
    watch_set_resolver(touched);
  }
  GxRectangle from;
  gx_copy_rectangle(&from.x, &from.y, &from.width, &from.height);
  // Dolphin's hardware tests: a copy that starts past the picture has nothing to copy, one
  // running past its edge is cut there, and with a stride of 0 nothing is copied at all.
  uint32_t stride = gx.bp[BP_COPY_STRIDE] << ADDRESS_SHIFT;
  if (from.x >= EFB_WIDTH || from.y >= EFB_HEIGHT || !stride) return;
  if (from.x + from.width > EFB_WIDTH) from.width = EFB_WIDTH - from.x;
  if (from.y + from.height > EFB_HEIGHT) from.height = EFB_HEIGHT - from.y;
  int half = (control >> HALF_SIZE_BIT) & 1;
  uint32_t copy_width = (uint32_t)(half ? from.width / 2 : from.width), copy_height = (uint32_t)(half ? from.height / 2 : from.height);
  uint32_t address = gx.bp[BP_COPY_TARGET] & 0xffffff;
  Copy shape = {.address = address};
  if (!fit_copy(&shape, control, copy_width, copy_height, stride)) return;
  if (stride < shape.row_bytes) {
    debug_unsupported("gx.0x4d_copy_stride", "picture copy with a stride below its row", stride);
    return;
  }
  Copy *copy = copy_for(address);
  if (!copy) {
    // Every place is taken: the first is given to this one, its held copy kept apart.
    gap_note("limit.picture_copy_places", MAX_COPIES);
    copy = &copies[0];
    if (copy->memory == MEMORY_HELD) keep_apart(copy, shape.width, shape.height);
    copy->read_frame = 0;
  }
  // Older copies this one lands on: one it covers whole is simply replaced. Any other still
  // held keeps its bytes outside this one, so it stays held, apart, until something reads
  // there or it's covered too, and this one lands after it.
  uint32_t start = copy_start(&shape), end = copy_end(&shape);
  for (int i = 0; i < copy_count + MAX_OLDER; i++) {
    Copy *other = i < copy_count ? &copies[i] : &older[i - copy_count];
    if (!other->live && other->memory != MEMORY_HELD) continue;
    if (other != copy && !overlaps(other, start, end)) continue;
    // Every byte of it is written again: it lies inside this one, which has no gaps, or
    // which has the same gaps, as a copy made again in the same place does.
    uint32_t inside = copy_start(other) - start;
    int covered = copy_start(other) >= start && copy_end(other) <= end
               && (shape.stride == shape.row_bytes || (other->stride == shape.stride && inside % shape.stride + other->row_bytes <= shape.row_bytes));
    if (other->memory == MEMORY_HELD) {
      if (covered) drop(other);
      else if (other == copy) keep_apart(copy, shape.width, shape.height);
    }
    other->live = 0;
  }
  copy->address = address;
  copy->format = shape.format, copy->width = shape.width, copy->height = shape.height;
  copy->stride = shape.stride, copy->rows = shape.rows, copy->row_bytes = shape.row_bytes;
  copy->encoding = encoding_for(control, from_depth, copy_width, copy_height, stride);
  // Copies keep the buffer's full detail. Shrinking them would blend the edges of a
  // cut-out picture with whatever color surrounds it.
  int width = (int)copy_width * EFB_SCALE, height = (int)copy_height * EFB_SCALE;
  copy->from_depth = from_depth;
  if (from_depth) gx_backend->copy_depth(copy->depth_texture, &from, width, height);
  else gx_backend->copy_color(copy->texture, &from, width, height, channels_for(control), half);
  copy->live = 1;
  copy->ticket = 0;
  copy->made = ++copies_made;
  if (copy->before_taken) memset(copy->before_taken, 0, copy->before_size / page_size() + 2);
  // Leaving memory as it was, for measuring what landing costs.
  static int on_card = -1;
  if (on_card < 0) on_card = getenv("WII_RECOMP_COPIES_ON_CARD") != 0;
  if (on_card) {
    copy->memory = MEMORY_WRITTEN;
    copy->watched = watch_range(start | 0x80000000u, end - start);
    if (copy->watched == WATCH_NONE) copy->hash = hash_bytes(host(start | 0x80000000u), end - start);
    gap_note("pixel.copy_to_texture", 0x0ca7d);
    return;
  }
  copy->memory = MEMORY_HELD;
  counts.held++;
  if (!watch_hold(start, end - start)) write_back(copy, "as memory can't be held");
  else if (learned(copy)) copy->ticket = read_copy(copy);
}

static int memory_unchanged(Copy *copy) {
  if (copy->memory != MEMORY_WRITTEN) return copy->memory == MEMORY_HELD;
  uint32_t start = copy_start(copy) | 0x80000000u, bytes = copy_bytes(copy);
  if (copy->watched != WATCH_NONE) return watch_count(start, bytes) == copy->watched;
  // Pages written frame after frame aren't watched: the bytes are hashed once a frame.
  if (copy->checked_frame == gx_frame_number() + 1) return 1;
  copy->checked_frame = gx_frame_number() + 1;
  return hash_bytes(host(start), bytes) == copy->hash;
}

// The copy a map's texture could be. Whatever shape a texture has, the first question is
// whether the copy is still in memory, as a new texture there is no concern of ours. If
// it is, the game is reading the copy some other way, which only real pixels could show.
static Copy *usable_copy(int map) {
  uint32_t address = gx.bp[map < 4 ? BP_TEXTURE_ADDRESS + map : BP_TEXTURE_ADDRESS_HIGH + map - 4] & 0xffffff;
  uint32_t image = gx.bp[map < 4 ? BP_TEXTURE_IMAGE + map : BP_TEXTURE_IMAGE_HIGH + map - 4];
  for (int i = 0; i < copy_count; i++) {
    Copy *copy = &copies[i];
    if (copy->address != address || !copy->live) continue;
    if (!memory_unchanged(copy)) {
      copy->live = 0;
      return 0;
    }
    uint32_t format = (image >> TEXTURE_FORMAT_SHIFT) & TEXTURE_FORMAT_MASK;
    uint32_t width = (image & TEXTURE_SIZE_MASK) + 1, height = ((image >> COORDINATE_BITS) & TEXTURE_SIZE_MASK) + 1;
    // Read any other way, memory has the real bytes, as the texture cache finds there.
    if (format == TEXTURE_CMPR || texel_bits[format] != texel_bits[copy->format] || width != copy->width || height != copy->height
        || copy->stride != copy->row_bytes || format == TEXTURE_C4 || format == TEXTURE_C8 || format == TEXTURE_C14X2 || format != copy->format) {
      return 0;
    }
    // The card's copy has no filter, gamma or YUV, though memory does. Waiting for the
    // card to read it back here would stall every frame, so the card's copy is drawn.
    const CopyEncoding *e = &copy->encoding;
    int plain_filter = !e->filter[0] && !e->filter[1] && !e->filter[5] && !e->filter[6] && e->filter[2] + e->filter[3] + e->filter[4] == FILTER_WHOLE;
    if (!plain_filter) debug_unsupported("pixel.copy_filter_aa_and_deflicker", "picture copy drawn without its vertical filter", e->filter[3]);
    if (e->gamma) debug_unsupported("pixel.gamma_on_xfb_copy_1_0_1_7_2_2", "picture copy drawn without its gamma", e->gamma);
    if (e->intensity) debug_unsupported("pixel.copy_intensity_rgb_to_yuv", "picture copy drawn as red, not as YUV", e->format);
    return copy;
  }
  return 0;
}

GxTexture *gx_copy_find(int map) {
  Copy *copy = usable_copy(map);
  if (!copy) return 0;
  return copy->from_depth ? copy->depth_texture : copy->texture;
}

int gx_copy_is_depth(int map) {
  Copy *copy = usable_copy(map);
  return copy && copy->from_depth;
}
