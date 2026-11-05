// The INI reader settings_file.h describes.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "settings_file.h"

enum { LINE_BYTES = 512, NAME_BYTES = 64 };

static char *trim(char *text) {
  while (isspace((unsigned char)*text)) text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
  return text;
}

int settings_parse(const char *text, SettingsKey on_key, void *context, char *error, size_t error_bytes) {
  char section[NAME_BYTES] = "", line[LINE_BYTES];
  int number = 0;
  while (*text) {
    number++;
    size_t length = strcspn(text, "\n");
    if (length >= sizeof line) {
      snprintf(error, error_bytes, "the line is longer than %d characters", LINE_BYTES - 1);
      return number;
    }
    memcpy(line, text, length);
    line[length] = 0;
    text += length + (text[length] == '\n');
    char *comment = strchr(line, '#');
    if (comment) *comment = 0;
    char *content = trim(line);
    if (!*content) continue;
    if (*content == '[') {
      char *close = strchr(content, ']');
      if (!close || *trim(close + 1)) {
        snprintf(error, error_bytes, "a section is written [name]");
        return number;
      }
      *close = 0;
      char *name = trim(content + 1);
      if (!*name || strlen(name) >= sizeof section) {
        snprintf(error, error_bytes, "a section needs a name");
        return number;
      }
      strcpy(section, name);
      continue;
    }
    char *equals = strchr(content, '=');
    if (!equals) {
      snprintf(error, error_bytes, "a setting is written name = value");
      return number;
    }
    *equals = 0;
    char *key = trim(content), *value = trim(equals + 1);
    if (!*key) {
      snprintf(error, error_bytes, "a setting needs a name before its =");
      return number;
    }
    char why[256] = "";
    if (!on_key(context, section, key, value, why, sizeof why)) {
      snprintf(error, error_bytes, "%s", why[0] ? why : "not a setting this file has");
      return number;
    }
  }
  return 0;
}

int settings_list(const char *value, char parts[][32], int most) {
  int count = 0;
  const char *at = value;
  while (*at) {
    size_t length = strcspn(at, ",");
    char part[LINE_BYTES];
    snprintf(part, sizeof part, "%.*s", (int)length, at);
    char *trimmed = trim(part);
    if (*trimmed) {
      if (count == most) return -1;
      snprintf(parts[count++], 32, "%s", trimmed);
    }
    at += length + (at[length] == ',');
  }
  return count;
}
