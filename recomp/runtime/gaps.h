// Gaps: a game asked for something we don't do. Each is named by its row ID in
// docs/hardware_inventory.md, such as "ios.setattr", or "limit." and a name for a limit of
// our own. Each ID and detail is logged once and counted. WII_RECOMP_GAPS=<file> keeps
// the list in a file, which recomp/tools/gap_report.py ranks across runs and titles, and
// WII_RECOMP_GAPS_STRICT=1 stops at the first one.
//
// Call these only on a path where the game asked for something we don't do, never for a
// default value, so the list stays short.
#pragma once
#include <stdint.h>

// Logs "gap: <id> 0x<detail>" the first time.
void gap_hit(const char *id, uint32_t detail);

// Counts it without logging, for callers that log in their own words.
void gap_note(const char *id, uint32_t detail);

// Rewrites the file if anything is new. Called every second and at exit.
void gap_flush(void);
