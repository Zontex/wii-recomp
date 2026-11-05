// profile.c: run counts saved per title, added to on the next run, and started again when
// the program changes. Run once per stage, with WII_RECOMP_TITLE_DATA set: the test reads
// the file between runs.
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "profile.h"
#include "title.h"

static void nothing(void) {}
static const TitleFunction functions[] = {{0x80004000u, nothing}, {0x80004100u, nothing}, {0x80004200u, nothing}};
static uint32_t counts[3];
static Title made_up = {.title_high = 0x00010004, .title_low = 0x54455354, .program_check = 0x1234,
                        .functions = functions, .function_count = 3, .counts = counts};
const Title *title = &made_up;

int main(int count, char **arguments) {
  const char *stage = count > 1 ? arguments[1] : "";
  if (!strcmp(stage, "first")) memcpy(counts, (uint32_t[]){5, 0, 7}, sizeof counts);
  if (!strcmp(stage, "second")) memcpy(counts, (uint32_t[]){1, 2, 0}, sizeof counts);
  if (!strcmp(stage, "other")) {
    made_up.program_check = 0x5678;
    memcpy(counts, (uint32_t[]){0, 0, 3}, sizeof counts);
  }
  char path[4096];
  CHECK(title_data_path("x.txt", path, sizeof path));
  CHECK(strstr(path, "/00010004-54455354/x.txt") != 0);
  profile_start();
  profile_save();
  return finish();
}
