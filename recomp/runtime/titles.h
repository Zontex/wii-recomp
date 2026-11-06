// Installed titles in the console's storage: their metadata (TMD), tickets and
// contents. Plain bytes in and out, with no guest memory, so it can be tested alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
  TMD_HEADER_BYTES = 0x1e4, TMD_CONTENT_BYTES = 36, TMD_MAX_CONTENTS = 512,
  TMD_MAX_BYTES = TMD_HEADER_BYTES + TMD_MAX_CONTENTS * TMD_CONTENT_BYTES,
  TICKET_BYTES = 0x2a4, TICKET_VIEW_BYTES = 0xd8, MAX_TICKETS = 16,
  TMD_VIEW_HEADER_BYTES = 0x5c, TMD_VIEW_CONTENT_BYTES = 16,
  TITLE_PATH_BYTES = 64,
};

typedef struct {
  uint32_t id;
  uint16_t index, type;
  uint64_t size;
  const uint8_t *hash;  // 20 bytes, inside the TMD.
} TmdContent;

// Titles with metadata installed, and titles with a ticket, sorted. Return the count.
int titles_installed(uint64_t *out, int limit);
int titles_owned(uint64_t *out, int limit);

// Reads a title's metadata or all its tickets. Returns the size, or -1 if there are none.
long title_tmd(uint64_t title, uint8_t *out, size_t limit);
long title_tickets(uint64_t title, uint8_t *out, size_t limit);

// Checks a TMD's size against its content count. Returns the content count, or -1.
int tmd_check(const uint8_t *tmd, size_t bytes);
uint64_t tmd_title(const uint8_t *tmd);
uint64_t tmd_ios(const uint8_t *tmd);
uint16_t tmd_title_version(const uint8_t *tmd);
TmdContent tmd_content(const uint8_t *tmd, int number);

// The views programs get: a summary of a TMD, and of a ticket.
size_t tmd_view(const uint8_t *tmd, size_t bytes, uint8_t *out, size_t limit);
size_t tmd_view_bytes(const uint8_t *tmd, size_t bytes);
void ticket_view(const uint8_t *ticket, uint8_t *out);

// Where a content is stored: in the title's own folder, or shared by its hash.
// Returns 1 and the path in storage if it's there.
int title_content_path(uint64_t title, const TmdContent *content, char *out, size_t limit);
