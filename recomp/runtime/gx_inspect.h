// See gx_inspect.c.
#pragma once
#include "gx.h"

void gx_inspect_dump(void);
void gx_inspect_skip(int first, int last);
void gx_inspect_frame(void);
int gx_inspect_draw(const TevKey *tev, unsigned maps, int format, int primitive, int count, const Vertex *vertices);
