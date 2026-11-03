// The title system: tells the game which title it is and where its saves live.
// Other requests get zeros.
#include <string.h>

#include "game.h"
#include "cpu.h"
#include "ios.h"

enum { GET_DATA_FOLDER = 0x1d, GET_TITLE_ID = 0x20, VECTOR_BYTES = 8 };

int es_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return 0;
  uint32_t code = rd32(request + 12), inputs = rd32(request + 16), outputs = rd32(request + 20);
  uint32_t vectors = rd32(request + 24) | 0x80000000u;
  for (uint32_t i = 0; i < outputs; i++) {
    uint32_t vector = vectors + (inputs + i) * VECTOR_BYTES;
    uint32_t target = rd32(vector), bytes = rd32(vector + 4);
    if (target && bytes) memset(host(target | 0x80000000u), 0, bytes);
    if (code == GET_DATA_FOLDER && i == 0 && bytes > strlen(game_data_folder)) {
      memcpy(host(target | 0x80000000u), game_data_folder, strlen(game_data_folder) + 1);
    }
    if (code == GET_TITLE_ID && i == 0) {
      wr32(target | 0x80000000u, game_title_high);
      wr32((target | 0x80000000u) + 4, game_title_low);
    }
  }
  return 0;
}
