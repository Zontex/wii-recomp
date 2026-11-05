// Paired-single loads and stores for the interpreter and anything else not translated
// inline. The work is in cpu.h, shared with translated code.
#include "cpu.h"

void ps_load(uint32_t address, int w, int gqr, int rd) { ps_load_inline(address, w, gqr, rd); }

void ps_store(uint32_t address, int w, int gqr, int rs) { ps_store_inline(address, w, gqr, rs); }
