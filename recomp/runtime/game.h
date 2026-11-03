// What differs between games, written by the translator into generated/game.c.
#pragma once
#include <stdint.h>

extern const uint32_t game_ax_frame_done;     // Called when the audio processor finishes a frame.
extern const uint32_t game_calibration_flag;  // 0 in a game with no Balance Board.
extern const uint32_t game_title_high, game_title_low;
extern const char game_data_folder[];
extern const uint32_t game_remote_blocks, game_attachment_add, game_attachment_xor;  // Where the game keeps its attachment tables.
extern const int game_virtual_bluetooth;  // Whether the game talks to bluetooth.c rather than direct input.
extern const int game_fifo_breakpoints;  // Whether the chip honours FIFO breakpoints: see gx_fifo.c.
