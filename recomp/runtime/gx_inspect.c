// Looks inside a frame, for finding what paints what. Through the control pipe, "dump"
// writes every draw of the next frame to a file and "skip <first> <last>" leaves out a
// range of draws, so a fault on screen can be narrowed to one draw while the game runs.
#include <stdio.h>
#include <stdlib.h>

#include "gx_backend.h"
#include "gx_inspect.h"

enum {
  BP_GEN_MODE = 0, BP_Z_MODE = 0x40, BP_BLEND = 0x41, BP_ALPHA_TEST = 0xf3,
  BP_TEXTURE_IMAGE = 0x88, BP_TEXTURE_IMAGE_HIGH = 0xa8, BP_TEXTURE_ADDRESS = 0x94, BP_TEXTURE_ADDRESS_HIGH = 0xb4,
  CP_DESCRIPTOR_LOW = 0x50, CP_DESCRIPTOR_HIGH = 0x60, CP_FORMAT_A = 0x70, XF_COLOR_CONTROL = 0x100e, XF_TEXTURE_GENERATION = 0x1040,
  VERTICES_SHOWN = 3,
};

static int dump_wanted, dumping, skip_first = -1, skip_last = -1, draw_number;
static FILE *dump_file;

void gx_inspect_dump(void) { dump_wanted = 1; }

void gx_inspect_skip(int first, int last) {
  skip_first = first;
  skip_last = last;
}

void gx_inspect_frame(void) {
  if (dumping) {
    dumping = 0;
    fclose(dump_file);
    fprintf(stderr, "inspect: wrote %d draws\n", draw_number);
  }
  if (dump_wanted) {
    const char *path = getenv("WII_RECOMP_GX_INSPECT");
    dump_file = fopen(path ? path : "frame.txt", "w");
    dump_wanted = 0;
    dumping = dump_file != 0;
  }
  draw_number = 0;
}

static void dump_draw(const TevKey *tev, unsigned maps, int format, int primitive, int count, const Vertex *vertices, int skipped) {
  fprintf(dump_file, "draw %d%s prim %d verts %d format %d desc %05x %04x vat %08x stages %d gens %u colors %x\n", draw_number,
          skipped ? " SKIPPED" : "", primitive, count, format, gx.cp[CP_DESCRIPTOR_LOW], gx.cp[CP_DESCRIPTOR_HIGH], gx.cp[CP_FORMAT_A + format],
          tev->stages, gx.bp[BP_GEN_MODE] & 15, gx.xf[XF_COLOR_CONTROL]);
  fprintf(dump_file, "  z %06x blend %06x alpha test %06x cull %u\n", gx.bp[BP_Z_MODE], gx.bp[BP_BLEND], gx.bp[BP_ALPHA_TEST], (gx.bp[BP_GEN_MODE] >> 14) & 3);
  for (int map = 0; map < MAX_TEXTURE_MAPS; map++) {
    if (!(maps & (1u << map))) continue;
    uint32_t image = gx.bp[map < 4 ? BP_TEXTURE_IMAGE + map : BP_TEXTURE_IMAGE_HIGH + map - 4];
    uint32_t address = gx.bp[map < 4 ? BP_TEXTURE_ADDRESS + map : BP_TEXTURE_ADDRESS_HIGH + map - 4] & 0xffffff;
    fprintf(dump_file, "  map %d format %u size %ux%u address %08x%s\n", map, (image >> 20) & 15, (image & 0x3ff) + 1, ((image >> 10) & 0x3ff) + 1,
            address << 5, gx_copy_find(map) ? " FROM COPY" : "");
  }
  for (int stage = 0; stage < tev->stages; stage++) {
    const TevStage *s = &tev->stage[stage];
    fprintf(dump_file, "  stage %d tex %d map %d coord %d indirect %d raster %d color %x%x%x%x bias %d sub %d scale %d to %d alpha %x%x%x%x bias %d scale %d to %d\n",
            stage, s->texture_on, s->map, s->coordinate, s->indirect, s->raster, s->color_in[0], s->color_in[1], s->color_in[2], s->color_in[3],
            s->color_bias, s->color_subtract, s->color_scale, s->color_target, s->alpha_in[0], s->alpha_in[1], s->alpha_in[2], s->alpha_in[3],
            s->alpha_bias, s->alpha_scale, s->alpha_target);
  }
  for (int i = 0; i < (int)(gx.xf[0x103f] & 15) && i < MAX_TEXTURE_MAPS; i++) fprintf(dump_file, "  coordinate %d %08x\n", i, gx.xf[XF_TEXTURE_GENERATION + i]);
  for (int i = 0; i < VERTICES_SHOWN && i < count; i++) {
    const Vertex *v = &vertices[i];
    fprintf(dump_file, "  vertex %d pos %.3f %.3f %.3f normal %.3f %.3f %.3f uv %.3f %.3f color %.2f %.2f %.2f %.2f matrix %d\n", i, v->position[0], v->position[1], v->position[2],
            v->normal[0], v->normal[1], v->normal[2], v->uv[0][0], v->uv[0][1], v->color[0][0], v->color[0][1], v->color[0][2], v->color[0][3], v->matrix[VERTEX_MATRIX_POSITION]);
  }
}

int gx_inspect_draw(const TevKey *tev, unsigned maps, int format, int primitive, int count, const Vertex *vertices) {
  int number = draw_number, skipped = number >= skip_first && number <= skip_last;
  if (dumping) dump_draw(tev, maps, format, primitive, count, vertices, skipped);
  draw_number++;
  return !skipped;
}
