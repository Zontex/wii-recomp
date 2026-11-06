// Sound recordings (dsp_record.c): what passes between the game and the audio processor
// while the interpreter runs the game's real program, for recomp/tools/sound_bench.py to
// play through a C version with no game running.
#pragma once
#include <stdint.h>

// The file, little-endian. A header, the interpreter's whole state at the start (dsp_lle,
// as this build lays it out), then events in the order they happened, each a kind byte
// and its fields. Main memory reads hold only the bytes that differ from what the
// recording already gave for that address, so replaying them in order rebuilds the memory
// the program saw.
#define DSP_RECORD_MAGIC 0x52535257u  // "WRSR"
#define DSP_RECORD_VERSION 2u
#define DSP_RECORD_FROM_BOOT 1u       // Started before the game's program: in the boot ROM.

typedef struct {
  uint32_t magic, version, flags;
  uint32_t start_crc, end_crc;  // The program's checksum at the start and the end.
  uint32_t state_bytes;         // sizeof(DspLle), checked on replay.
  uint64_t start_time, end_time;
  uint32_t mails, events;       // CPU to DSP mails, and all events.
  uint32_t reserved[4];
} DspRecordHeader;

enum {
  DSP_EVENT_TIME = 1,        // u64 time base, before any event at a new time.
  DSP_EVENT_RUN,             // u32 cycles: the interpreter ran, after the events it caused.
  DSP_EVENT_CPU_HIGH,        // u16: the game wrote its mailbox's high half.
  DSP_EVENT_CPU_LOW,         // u16: and the low half, which sends the mail.
  DSP_EVENT_READ_LOW,        // u8 1 for the game's mailbox, 0 the processor's, u32 what it held: the game read a low half.
  DSP_EVENT_CONTROL,         // u16: the game wrote the control register, after the events it caused.
  DSP_EVENT_MEMORY_READ,     // u32 physical address, u32 count, the bytes.
  DSP_EVENT_MEMORY_WRITE,    // The same, for what the program wrote.
  DSP_EVENT_MAIL,            // u32: the program put a mail in its mailbox.
  DSP_EVENT_INTERRUPT,       // The program interrupted the game.
};

void dsp_record_request(int frames, const char *file);  // The control pipe's record-sound; any thread.
void dsp_record_start(void);                             // At power-on, for WII_RECOMP_SOUND_RECORD.
void dsp_record_update(uint64_t now);                    // Starts and stops recordings.

// dsp.c tells the recorder what the game does to the interpreter, while one runs.
extern int dsp_recording;
void dsp_record_run(uint32_t cycles);
void dsp_record_cpu_write(int high, uint16_t value);
void dsp_record_read_low(int cpu, uint32_t mail);  // The mailbox as it was, full bit and all.
void dsp_record_control(uint16_t value);
