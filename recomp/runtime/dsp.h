// The audio processor (DSP) as the game sees it: two mailboxes, its bits of the control
// register, and the program it runs. It starts in its boot ROM, which takes the game's
// own program and hands over to it. Programs are told apart by a checksum of their code,
// as Dolphin does (HW/DSPHLE/UCodes/UCodes.cpp), and each known one has a C version.
#pragma once
#include <stdint.h>

// The control register bits the processor itself owns, as Dolphin's DSP.h lays them out.
// The interrupt flags and their enables belong to hardware.c.
#define DSP_CONTROL_RESET 0x0001u
#define DSP_CONTROL_ASSERT_INTERRUPT 0x0002u
#define DSP_CONTROL_HALT 0x0004u
#define DSP_CONTROL_INIT_CODE 0x0400u
#define DSP_CONTROL_INIT 0x0800u
#define DSP_CONTROL_OWN (DSP_CONTROL_RESET | DSP_CONTROL_ASSERT_INTERRUPT | DSP_CONTROL_HALT | DSP_CONTROL_INIT_CODE | DSP_CONTROL_INIT)

// Mail the processor sends about its task, and the game's answers (UCodes.h).
#define DSP_MAIL_INIT 0xdcd10000u
#define DSP_MAIL_RESUME 0xdcd10001u
#define DSP_MAIL_YIELD 0xdcd10002u
#define DSP_MAIL_DONE 0xdcd10003u
#define DSP_MAIL_SYNC 0xdcd10004u
#define DSP_MAIL_FRAME_END 0xdcd10005u
#define TASK_MAIL_MASK 0xffff0000u
#define TASK_MAIL_TO_DSP 0xcdd10000u
#define TASK_RESUME 0xcdd10000u
#define TASK_NEW_PROGRAM 0xcdd10001u
#define TASK_RESET 0xcdd10002u
#define TASK_CONTINUE 0xcdd10003u

// Checksums that aren't programs: the boot ROM, the start-up program the OS runs, none.
#define PROGRAM_ROM 0x00000000u
#define PROGRAM_INIT 0x00000001u
#define PROGRAM_NONE 0xffffffffu

// A program behind the mailbox. Each keeps its own state; start begins it afresh.
typedef struct {
  const char *name;
  void (*start)(void);
  void (*mail)(uint32_t mail);  // A mail from the game.
  void (*update)(void);         // Every millisecond.
} DspProgram;

void dsp_start(void);
void dsp_update(uint64_t now);
uint64_t dsp_next_due(void);  // When the next delayed interrupt is due, or 0 if none is.

// hardware.c passes these registers on.
int dsp_read(uint32_t address, int bytes, uint32_t *value);
int dsp_write(uint32_t address, uint32_t value, int bytes);
uint32_t dsp_control_read(void);
uint32_t dsp_control_write(uint32_t value);  // Returns the processor's bits as they now stand.

// For programs.
uint32_t dsp_program_crc(void);
void dsp_push_mail(uint32_t mail, int interrupt, uint32_t cycles_later);
// Sent once the program has worked that many CPU cycles, behind any mail already sent.
void dsp_push_mail_later(uint32_t mail, int interrupt, uint32_t cycles_later);
int dsp_mail_pending(void);
void dsp_mail_interrupt(uint32_t cycles_later);  // CPU cycles from now.
void dsp_set_program(uint32_t crc);              // Starts it afresh, as a reset does.
void dsp_new_program_mail(uint32_t mail);        // One of the ten mails that describe the next program.
int dsp_taking_new_program(void);                // Whether the mails now are those ten.
void dsp_expect_new_program(void);               // After TASK_NEW_PROGRAM.
int dsp_needs_resume_mail(void);                 // Once, for a program swapped back in.
uint32_t dsp_hash(const uint8_t *data, uint32_t length);  // Dolphin's HashEctor.

// For sound recordings: the C version now behind the mailbox (0 if none), and whether the
// interpreter has the mailboxes.
const char *dsp_c_version(void);
int dsp_interpreting(void);

// hardware.c: sets the processor's interrupt flag.
void dsp_interrupt(void);

// Programs with a C version.
extern const DspProgram ax_program;  // GameCube and Wii.
