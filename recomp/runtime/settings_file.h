// A small reader for settings files in the INI form: [section] lines, key = value lines,
// and # comments, on lines of their own or after a value. Nothing is skipped quietly: the
// caller answers each key, and a line it doesn't accept is an error naming that line.
#pragma once
#include <stddef.h>

// Called for each key, with its section ("" before the first). Returns 0 if the key or
// its value isn't one it knows, after writing why into `why`.
typedef int (*SettingsKey)(void *context, const char *section, const char *key, const char *value, char *why, size_t why_bytes);

// Returns 0, or the number of the first line that's wrong, with why in `error`.
int settings_parse(const char *text, SettingsKey on_key, void *context, char *error, size_t error_bytes);

// The parts of a comma-separated value, trimmed, into `parts`; returns how many, or -1 if
// there are more than `most`.
int settings_list(const char *value, char parts[][32], int most);
