// The Wii's hardware blocks, as seen by the game through register addresses.
#pragma once
#include <stdint.h>

enum {
  IRQ_RESET_SWITCH = 1, IRQ_DI = 2, IRQ_SI = 3, IRQ_EXI = 4, IRQ_AI = 5, IRQ_DSP = 6, IRQ_VI = 8, IRQ_PE_TOKEN = 9, IRQ_PE_FINISH = 10, IRQ_CP = 11, IRQ_IPC = 14,
};

// The running game is a GameCube game, which the Wii plays in its GameCube mode: the game
// reaches the disc drive, controllers, memory cards and audio memory itself, through
// their registers, and its clocks are a GameCube's. Set from the disc it boots (loader.c).
extern int gamecube_mode;
void guest_memory_open_virtual(void);  // GameCube mode: 0x7e000000's 32 MB, as plain memory (guest_memory.c).
// Whether the running title's code counts the processor cycles it runs, as the translator
// builds a GameCube title to (emit.py's charges): what its checks count down is then
// cycles, not checks. Set as the title loads (title.c).
extern int title_counts_work;
// The cycles the title's code has run since this was last asked: any thread (interrupts.c).
uint64_t work_cycles_taken(void);

void hardware_start(void);
void raise_irq(int bit);
void clear_irq(int bit);
int irq_pending(void);
void hardware_describe(void);  // Prints the interrupt and IPC state, for a program that seems stuck.
void interrupts_describe(void);  // How often interrupts were checked for and taken (interrupts.c).
void vi_display_interrupt(int i);  // From vi.c, as the beam reaches its line.
void audio_dma_interrupt(void);
int audio_dma_interrupt_unanswered(void);
void hardware_update(uint64_t now);  // Timed signals: the audio interface's interrupt, and button presses.
void reset_button_press(void);  // Any thread: presses the console's reset button.
// Any thread: presses the power button. Returns whether the program is listening for it;
// if not, the power just goes, as a real Wii's does when its program has stopped.
int power_button_press(void);
uint32_t hw_peek(uint32_t address, int bytes);
void hw_poke(uint32_t address, uint32_t value, int bytes);

// Blocks claim the registers they handle, and return 1 if they did.
// The disc drive's registers, which only a GameCube game uses (di.c). Both answer 0 for
// an address that isn't theirs, or outside GameCube mode.
int di_read(uint32_t address, uint32_t *value);
int di_write(uint32_t address, uint32_t value);
void di_start(void);
int si_read(uint32_t address, uint32_t *value);
void si_poll(void);  // A field begins: in GameCube mode the controller ports are polled (vi.c).
int si_write(uint32_t address, uint32_t value);
int exi_read(uint32_t address, uint32_t *value);
void exi_start(void);  // GameCube mode: puts the memory card in slot A (exi.c).
int exi_write(uint32_t address, uint32_t value);
int ipc_read(uint32_t address, uint32_t *value);
int ipc_write(uint32_t address, uint32_t value);
void ipc_update(void);
void ipc_describe(void);
int graphics_write(uint32_t address, uint32_t value, int bytes);
// A store to the graphics pipe (0xcc008000 to 0xcc008fff), the commonest write by far.
void graphics_pipe_write(uint32_t value, int bytes);
enum { GRAPHICS_PIPE = 0xcc008000u, GRAPHICS_PIPE_BYTES = 0x1000u };
int graphics_read(uint32_t address, uint32_t *value);
void graphics_update(void);
void graphics_retrace(double field_rate);  // A field begins, at this many fields a second (vi.c, on the game's thread).
int graphics_busy(void);  // Commands are still waiting to be run.
uint64_t graphics_next_due(void);  // When the next delayed signal is due, or 0 if none is.
