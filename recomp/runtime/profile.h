// What a title's code did, kept between runs in the title's own data folder, so the next
// build can use it: which functions ran how often (profile.txt), and code the translator
// missed (discovered.txt).
#pragma once
#include <stddef.h>

// Loads the counts saved by earlier runs and saves the running total every few seconds.
// Call once the title is loaded and the folders are set.
void profile_start(void);

// Saves now, such as before the console hands over to another title.
void profile_save(void);

// The path of `name` in this title's data folder, WII_RECOMP_TITLE_DATA/<title ID>, made
// if needed. 0 if there's no folder to use.
int title_data_path(const char *name, char *path, size_t size);
