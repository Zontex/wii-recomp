#pragma once
#include <stdint.h>
// Saves the next picture shown to path, as a PPM, with either renderer.
void gx_shot(const char *path);
// For a renderer that reads its picture back itself: whether one is wanted, and the
// picture, top row first, four bytes a pixel, of which the first three are kept.
int gx_shot_wanted(void);
void gx_shot_write(const uint8_t *rgba, int width, int height);
