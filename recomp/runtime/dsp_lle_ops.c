// The audio processor's instructions, ported from Dolphin's Core/DSP/Interpreter
// (DSPInterpreter.cpp, DSPIntArithmetic.cpp, DSPIntBranch.cpp, DSPIntLoadStore.cpp,
// DSPIntMultiplier.cpp, DSPIntExtOps.cpp, DSPIntMisc.cpp, DSPIntTables.cpp) and the opcode
// templates of Core/DSP/DSPTables.cpp, GPL-2.0-or-later.
//
// Accumulators are 40 bits, kept sign-extended in 64. Many instructions in the upper half of
// the opcode space carry an extension in their low byte (low 7 bits for 0x3xxx), a load,
// store or address step that runs alongside. Both see the registers as they were; the
// extension's writes are held back and land after the main instruction's, winning where both
// write the same register. That is Dolphin without PRECISE_BACKLOG, and no real program writes
// one register twice.
#include "dsp_lle.h"

#include "gaps.h"

#define R dsp_lle.r
#define STACK_CALL 0
#define STACK_DATA 1
#define STACK_LOOP_ADDRESS 2
#define STACK_LOOP_COUNTER 3

typedef void (*DspOp)(uint16_t opc);

// Shifts on the unsigned bits, so negative values shift as the hardware's do.
static inline int64_t shl64(int64_t value, unsigned shift) { return (int64_t)((uint64_t)value << shift); }

static inline int sr_set(uint16_t flag) { return (R.sr & flag) != 0; }

// --- Conditions (DSPInterpreter.cpp, CheckCondition). ---

static int is_less(void) { return sr_set(DSP_SR_OVERFLOW) != sr_set(DSP_SR_SIGN); }

static int check_condition(uint8_t condition) {
  int zero = sr_set(DSP_SR_ARITH_ZERO);
  int condition_b = !(sr_set(DSP_SR_OVER_S32) || sr_set(DSP_SR_TOP2BITS)) || zero;
  switch (condition & 0xf) {
  case 0xf: return 1;
  case 0x0: return !is_less();          // GE
  case 0x1: return is_less();           // L
  case 0x2: return !is_less() && !zero;  // G
  case 0x3: return is_less() || zero;   // LE
  case 0x4: return !zero;               // NZ
  case 0x5: return zero;                // Z
  case 0x6: return !sr_set(DSP_SR_CARRY);     // NC
  case 0x7: return sr_set(DSP_SR_CARRY);      // C
  case 0x8: return !sr_set(DSP_SR_OVER_S32);  // Not over 32 bits
  case 0x9: return sr_set(DSP_SR_OVER_S32);
  case 0xa: return !condition_b;
  case 0xb: return condition_b;
  case 0xc: return !sr_set(DSP_SR_LOGIC_ZERO);  // LNZ
  case 0xd: return sr_set(DSP_SR_LOGIC_ZERO);   // LZ
  case 0xe: return sr_set(DSP_SR_OVERFLOW);     // O
  default: return 1;
  }
}

// --- Address registers wrap within a power-of-two window set by their wrap register. ---

static uint16_t increment_ar(uint16_t reg) {
  uint32_t ar = R.ar[reg], wr = R.wr[reg], nar = ar + 1;
  if ((nar ^ ar) > ((wr | 1) << 1)) nar -= wr + 1;
  return (uint16_t)nar;
}

static uint16_t decrement_ar(uint16_t reg) {
  uint32_t ar = R.ar[reg], wr = R.wr[reg], nar = ar + wr;
  if (((nar ^ ar) & ((wr | 1) << 1)) > wr) nar -= wr + 1;
  return (uint16_t)nar;
}

static uint16_t increase_ar(uint16_t reg, int16_t ix_) {
  uint32_t ar = R.ar[reg], wr = R.wr[reg];
  int32_t ix = ix_;
  uint32_t mx = (wr | 1) << 1, nar = ar + (uint32_t)ix, dar = (nar ^ ar ^ (uint32_t)ix) & mx;
  if (ix >= 0) {
    if (dar > wr) nar -= wr + 1;  // Overflow
  } else if ((((nar + wr + 1) ^ nar) & dar) <= wr) {
    nar += wr + 1;  // Underflow, or below the window
  }
  return (uint16_t)nar;
}

static uint16_t decrease_ar(uint16_t reg, int16_t ix_) {
  uint32_t ar = R.ar[reg], wr = R.wr[reg];
  int32_t ix = ix_;
  uint32_t mx = (wr | 1) << 1, nar = ar - (uint32_t)ix, dar = (nar ^ ar ^ ~(uint32_t)ix) & mx;
  if ((uint32_t)ix > 0xffff8000u) {  // ix < 0 and not -0x8000
    if (dar > wr) nar -= wr + 1;
  } else if ((((nar + wr + 1) ^ nar) & dar) <= wr) {
    nar += wr + 1;
  }
  return (uint16_t)nar;
}

// --- Accumulators and the product. ---

static int32_t get_long_acx(int reg) { return (int32_t)((uint32_t)R.ax[reg].h << 16 | R.ax[reg].l); }
static int16_t get_ax_low(int reg) { return (int16_t)R.ax[reg].l; }
static int16_t get_ax_high(int reg) { return (int16_t)R.ax[reg].h; }
static int64_t get_long_acc(int reg) { return (int64_t)R.ac[reg].val; }
static void set_long_acc(int reg, int64_t value) { R.ac[reg].val = (uint64_t)(shl64(value, 24) >> 24); }  // 40-bit sign extension
static int16_t get_acc_mid(int reg) { return (int16_t)R.ac[reg].m; }
static int64_t convert_long_acc(int64_t value) { return shl64(value, 24) >> 24; }

static int64_t round_long_acc(int64_t value) {
  if (value & 0x10000) return (value + 0x8000) & ~(int64_t)0xffff;
  return (value + 0x7fff) & ~(int64_t)0xffff;
}

// The product is kept in four parts, and its value is h:m:l plus m2 at m's level.
static int64_t get_long_product(void) {
  int64_t value = shl64((int8_t)(uint8_t)R.prod.h, 32);
  int64_t low = R.prod.m;
  low += R.prod.m2;
  low = shl64(low, 16);
  low |= R.prod.l;
  return value + low;
}

static int64_t get_long_product_rounded(void) { return round_long_acc(get_long_product()); }

// Not how the real parts behave, which is odd, but programs don't depend on it (Dolphin).
static void set_long_product(int64_t value) { R.prod.val = (uint64_t)value & 0x000000ffffffffffull; }

// sign: 0 signed, 1 unsigned when SET15, 2 mixed (unsigned a, signed b) when SET15.
static int64_t multiply(uint16_t a, uint16_t b, uint8_t sign) {
  int64_t product;
  if (sign == 1 && sr_set(DSP_SR_MUL_UNSIGNED)) product = (uint32_t)a * b;
  else if (sign == 2 && sr_set(DSP_SR_MUL_UNSIGNED)) product = (int32_t)a * (int16_t)b;
  else product = (int32_t)(int16_t)a * (int16_t)b;
  if (!sr_set(DSP_SR_MUL_MODIFY)) product = shl64(product, 1);  // M2: doubled.
  return product;
}

static int64_t multiply_add(uint16_t a, uint16_t b) { return get_long_product() + multiply(a, b, 0); }
static int64_t multiply_sub(uint16_t a, uint16_t b) { return get_long_product() - multiply(a, b, 0); }

// MULX's halves: unsigned for two lows, mixed for a low and a high, signed for two highs.
static int64_t multiply_mulx(uint8_t axh0, uint8_t axh1, uint16_t value1, uint16_t value2) {
  if (axh0 == 0 && axh1 == 0) return multiply(value1, value2, 1);
  if (axh0 == 0 && axh1 == 1) return multiply(value1, value2, 2);  // (u16)$ax0.l * (s16)$ax1.h
  if (axh0 == 1 && axh1 == 0) return multiply(value2, value1, 2);  // (u16)$ax1.l * (s16)$ax0.h
  return multiply(value1, value2, 0);
}

// --- Flags (DSPIntCCUtil.h and the UpdateSR functions). ---

static int is_carry_add(uint64_t value, uint64_t result) { return value > result; }
static int is_carry_subtract(uint64_t value, uint64_t result) { return value >= result; }
static int is_overflow(int64_t value1, int64_t value2, int64_t result) { return ((value1 ^ result) & (value2 ^ result)) < 0; }
static int is_over_s32(int64_t acc) { return acc != (int32_t)acc; }

static void update_sr16(int16_t value, int carry, int overflow, int over_s32) {
  R.sr &= ~DSP_SR_CMP_MASK;
  if (carry) R.sr |= DSP_SR_CARRY;
  if (overflow) R.sr |= DSP_SR_OVERFLOW | DSP_SR_OVERFLOW_STICKY;
  if (value == 0) R.sr |= DSP_SR_ARITH_ZERO;
  if (value < 0) R.sr |= DSP_SR_SIGN;
  if (over_s32) R.sr |= DSP_SR_OVER_S32;
  if (((uint16_t)value >> 14) == 0 || ((uint16_t)value >> 14) == 3) R.sr |= DSP_SR_TOP2BITS;
}

static void update_sr64(int64_t value, int carry, int overflow) {
  R.sr &= ~DSP_SR_CMP_MASK;
  if (carry) R.sr |= DSP_SR_CARRY;
  if (overflow) R.sr |= DSP_SR_OVERFLOW | DSP_SR_OVERFLOW_STICKY;
  if (value == 0) R.sr |= DSP_SR_ARITH_ZERO;
  if (value < 0) R.sr |= DSP_SR_SIGN;
  if (is_over_s32(value)) R.sr |= DSP_SR_OVER_S32;
  if ((value & 0xc0000000) == 0 || (value & 0xc0000000) == 0xc0000000) R.sr |= DSP_SR_TOP2BITS;
}

// result is value1 + value2 (or - value2) already cut to 40 bits.
static void update_sr64_add(int64_t value1, int64_t value2, int64_t result) {
  update_sr64(result, is_carry_add((uint64_t)value1, (uint64_t)result), is_overflow(value1, value2, result));
}

static void update_sr64_sub(int64_t value1, int64_t value2, int64_t result) {
  update_sr64(result, is_carry_subtract((uint64_t)value1, (uint64_t)result), is_overflow(value1, -value2, result));
}

static void update_sr_logic_zero(int value) {
  if (value) R.sr |= DSP_SR_LOGIC_ZERO;
  else R.sr &= ~DSP_SR_LOGIC_ZERO;
}

// --- Registers as instructions name them. ---

static uint16_t op_read_register(int reg_) {
  int reg = reg_ & 0x1f;
  switch (reg) {
  case DSP_REG_ST0: case DSP_REG_ST1: case DSP_REG_ST2: case DSP_REG_ST3:
    return dsp_lle_pop_stack(reg - DSP_REG_ST0);  // Reading a stack register pops it.
  case DSP_REG_AR0: case DSP_REG_AR1: case DSP_REG_AR2: case DSP_REG_AR3: return R.ar[reg - DSP_REG_AR0];
  case DSP_REG_IX0: case DSP_REG_IX1: case DSP_REG_IX2: case DSP_REG_IX3: return R.ix[reg - DSP_REG_IX0];
  case DSP_REG_WR0: case DSP_REG_WR1: case DSP_REG_WR2: case DSP_REG_WR3: return R.wr[reg - DSP_REG_WR0];
  case DSP_REG_ACH0: case DSP_REG_ACH1: return (uint16_t)R.ac[reg - DSP_REG_ACH0].h;
  case DSP_REG_CR: return R.cr;
  case DSP_REG_SR: return R.sr;
  case DSP_REG_PRODL: return R.prod.l;
  case DSP_REG_PRODM: return R.prod.m;
  case DSP_REG_PRODH: return R.prod.h;
  case DSP_REG_PRODM2: return R.prod.m2;
  case DSP_REG_AXL0: case DSP_REG_AXL1: return R.ax[reg - DSP_REG_AXL0].l;
  case DSP_REG_AXH0: case DSP_REG_AXH1: return R.ax[reg - DSP_REG_AXH0].h;
  case DSP_REG_ACL0: case DSP_REG_ACL1: return R.ac[reg - DSP_REG_ACL0].l;
  default: {  // $ac0.m, $ac1.m: saturated in 40-bit mode when the whole doesn't fit in 32 bits.
    int index = reg - DSP_REG_ACM0;
    if (sr_set(DSP_SR_40_MODE_BIT)) {
      int64_t acc = get_long_acc(index);
      if (acc != (int32_t)acc) return acc > 0 ? 0x7fff : 0x8000;
    }
    return R.ac[index].m;
  }
  }
}

static void op_write_register(int reg_, uint16_t value) {
  int reg = reg_ & 0x1f;
  switch (reg) {
  case DSP_REG_ACH0: case DSP_REG_ACH1:
    R.ac[reg - DSP_REG_ACH0].h = (uint32_t)(int32_t)(int8_t)value;  // Sign-extended from 8 bits.
    break;
  case DSP_REG_ST0: case DSP_REG_ST1: case DSP_REG_ST2: case DSP_REG_ST3:
    dsp_lle_store_stack(reg - DSP_REG_ST0, value);
    break;
  case DSP_REG_AR0: case DSP_REG_AR1: case DSP_REG_AR2: case DSP_REG_AR3: R.ar[reg - DSP_REG_AR0] = value; break;
  case DSP_REG_IX0: case DSP_REG_IX1: case DSP_REG_IX2: case DSP_REG_IX3: R.ix[reg - DSP_REG_IX0] = value; break;
  case DSP_REG_WR0: case DSP_REG_WR1: case DSP_REG_WR2: case DSP_REG_WR3: R.wr[reg - DSP_REG_WR0] = value; break;
  case DSP_REG_CR: R.cr = value & 0x00ff; break;
  case DSP_REG_SR: R.sr = value & ~DSP_SR_100; break;
  case DSP_REG_PRODL: R.prod.l = value; break;
  case DSP_REG_PRODM: R.prod.m = value; break;
  case DSP_REG_PRODH: R.prod.h = value & 0x00ff; break;  // Not sign-extended, unlike $acX.h.
  case DSP_REG_PRODM2: R.prod.m2 = value; break;
  case DSP_REG_AXL0: case DSP_REG_AXL1: R.ax[reg - DSP_REG_AXL0].l = value; break;
  case DSP_REG_AXH0: case DSP_REG_AXH1: R.ax[reg - DSP_REG_AXH0].h = value; break;
  case DSP_REG_ACL0: case DSP_REG_ACL1: R.ac[reg - DSP_REG_ACL0].l = value; break;
  case DSP_REG_ACM0: case DSP_REG_ACM1: R.ac[reg - DSP_REG_ACM0].m = value; break;
  }
}

// In 40-bit mode, a load to $acX.m sign-extends over the accumulator and clears $acX.l
// (DSPSpy found this; not in 16-bit mode).
static void conditional_extend_accum(int reg) {
  if (reg != DSP_REG_ACM0 && reg != DSP_REG_ACM1) return;
  if (!sr_set(DSP_SR_40_MODE_BIT)) return;
  int index = reg - DSP_REG_ACM0;
  R.ac[index].h = (R.ac[index].m & 0x8000) ? 0xffffffffu : 0;
  R.ac[index].l = 0;
}

static void write_to_back_log(int i, int idx, uint16_t value) {
  dsp_lle.write_back_log[i] = value;
  dsp_lle.write_back_log_idx[i] = idx;
}

static void apply_write_back_log(void) {
  for (int i = 0; dsp_lle.write_back_log_idx[i] != -1; i++) {  // The last entry always stays -1.
    op_write_register(dsp_lle.write_back_log_idx[i], dsp_lle.write_back_log[i]);
    dsp_lle.write_back_log_idx[i] = -1;
  }
}

// ======================= Arithmetic (DSPIntArithmetic.cpp) =======================

static void op_clr(uint16_t opc) {  // CLR $acR
  uint8_t reg = (opc >> 11) & 1;
  set_long_acc(reg, 0);
  update_sr64(0, 0, 0);
}

static void op_clrl(uint16_t opc) {  // CLRL $acR.l: clears, rounding.
  uint8_t reg = (opc >> 8) & 1;
  int64_t acc = round_long_acc(get_long_acc(reg));
  set_long_acc(reg, acc);
  update_sr64(acc, 0, 0);
}

static void op_andcf(uint16_t opc) {  // ANDCF $acD.m, #I: LZ = ($acD.m & I) == I
  uint8_t reg = (opc >> 8) & 1;
  uint16_t imm = dsp_lle_fetch();
  uint16_t value = (uint16_t)get_acc_mid(reg);
  update_sr_logic_zero((value & imm) == imm);
}

static void op_andf(uint16_t opc) {  // ANDF $acD.m, #I: LZ = ($acD.m & I) == 0
  uint8_t reg = (opc >> 8) & 1;
  uint16_t imm = dsp_lle_fetch();
  uint16_t value = (uint16_t)get_acc_mid(reg);
  update_sr_logic_zero((value & imm) == 0);
}

static void op_tst(uint16_t opc) { update_sr64(get_long_acc((opc >> 11) & 1), 0, 0); }
static void op_tstaxh(uint16_t opc) { update_sr16(get_ax_high((opc >> 8) & 1), 0, 0, 0); }

static void op_cmp(uint16_t opc) {  // CMP: flags of $ac0 - $ac1
  (void)opc;
  int64_t acc0 = get_long_acc(0), acc1 = get_long_acc(1);
  update_sr64_sub(acc0, acc1, convert_long_acc(acc0 - acc1));
}

static void op_cmpaxh(uint16_t opc) {  // CMPAXH $acS, $axR.h
  uint8_t rreg = (opc >> 12) & 1, sreg = (opc >> 11) & 1;
  int64_t acc = get_long_acc(sreg);
  int64_t ax = shl64(get_ax_high(rreg), 16);
  update_sr64_sub(acc, ax, convert_long_acc(acc - ax));
}

static void op_cmpi(uint16_t opc) {  // CMPI $acD, #I: the immediate sits at the middle word.
  uint8_t reg = (opc >> 8) & 1;
  int64_t value = get_long_acc(reg);
  int64_t imm = shl64((int16_t)dsp_lle_fetch(), 16);
  update_sr64_sub(value, imm, convert_long_acc(value - imm));
}

static void op_cmpis(uint16_t opc) {  // CMPIS $acD, #I (8 bits, sign-extended)
  uint8_t areg = (opc >> 8) & 1;
  int64_t acc = get_long_acc(areg);
  int64_t imm = shl64((int8_t)opc, 16);
  update_sr64_sub(acc, imm, convert_long_acc(acc - imm));
}

// The logic instructions on $acD.m.
static void logic_result(uint8_t dreg, uint16_t accm) {
  R.ac[dreg].m = accm;
  update_sr16((int16_t)accm, 0, 0, is_over_s32(get_long_acc(dreg)));
}
static void op_xorr(uint16_t opc) { uint8_t d = (opc >> 8) & 1, s = (opc >> 9) & 1; logic_result(d, R.ac[d].m ^ R.ax[s].h); }
static void op_andr(uint16_t opc) { uint8_t d = (opc >> 8) & 1, s = (opc >> 9) & 1; logic_result(d, R.ac[d].m & R.ax[s].h); }
static void op_orr(uint16_t opc) { uint8_t d = (opc >> 8) & 1, s = (opc >> 9) & 1; logic_result(d, R.ac[d].m | R.ax[s].h); }
static void op_andc(uint16_t opc) { uint8_t d = (opc >> 8) & 1; logic_result(d, R.ac[d].m & R.ac[1 - d].m); }
static void op_orc(uint16_t opc) { uint8_t d = (opc >> 8) & 1; logic_result(d, R.ac[d].m | R.ac[1 - d].m); }
static void op_xorc(uint16_t opc) { uint8_t d = (opc >> 8) & 1; logic_result(d, R.ac[d].m ^ R.ac[1 - d].m); }
static void op_notc(uint16_t opc) { uint8_t d = (opc >> 8) & 1; logic_result(d, R.ac[d].m ^ 0xffff); }
static void op_xori(uint16_t opc) { uint8_t d = (opc >> 8) & 1; uint16_t imm = dsp_lle_fetch(); logic_result(d, R.ac[d].m ^ imm); }
static void op_andi(uint16_t opc) { uint8_t d = (opc >> 8) & 1; uint16_t imm = dsp_lle_fetch(); logic_result(d, R.ac[d].m & imm); }
static void op_ori(uint16_t opc) { uint8_t d = (opc >> 8) & 1; uint16_t imm = dsp_lle_fetch(); logic_result(d, R.ac[d].m | imm); }

// $axS.l, $axS.h, $ax0.h or $ax1.h by number 0x18-0x1b, sign-extended.
static int64_t ax_part(uint8_t sreg) {
  switch (sreg) {
  case DSP_REG_AXL0: case DSP_REG_AXL1: return (int16_t)R.ax[sreg - DSP_REG_AXL0].l;
  case DSP_REG_AXH0: case DSP_REG_AXH1: return (int16_t)R.ax[sreg - DSP_REG_AXH0].h;
  default: return 0;
  }
}

static void add_to(uint8_t dreg, int64_t acc, int64_t value) {
  set_long_acc(dreg, acc + value);
  update_sr64_add(acc, value, get_long_acc(dreg));
}

static void subtract_from(uint8_t dreg, int64_t acc, int64_t value) {
  set_long_acc(dreg, acc - value);
  update_sr64_sub(acc, value, get_long_acc(dreg));
}

static void op_addr(uint16_t opc) {  // ADDR $acD.M, $axS.L
  uint8_t dreg = (opc >> 8) & 1, sreg = ((opc >> 9) & 3) + DSP_REG_AXL0;
  add_to(dreg, get_long_acc(dreg), shl64(ax_part(sreg), 16));
}
static void op_addax(uint16_t opc) { uint8_t d = (opc >> 8) & 1, s = (opc >> 9) & 1; add_to(d, get_long_acc(d), get_long_acx(s)); }
static void op_add(uint16_t opc) { uint8_t d = (opc >> 8) & 1; add_to(d, get_long_acc(d), get_long_acc(1 - d)); }
static void op_addp(uint16_t opc) { uint8_t d = (opc >> 8) & 1; add_to(d, get_long_acc(d), get_long_product()); }

static void op_addaxl(uint16_t opc) {  // ADDAXL $acD, $axS.l: the low half unsigned.
  uint8_t sreg = (opc >> 9) & 1, dreg = (opc >> 8) & 1;
  uint64_t acc = (uint64_t)get_long_acc(dreg);
  uint16_t acx = (uint16_t)get_ax_low(sreg);
  set_long_acc(dreg, (int64_t)(acc + acx));
  update_sr64_add((int64_t)acc, acx, get_long_acc(dreg));
}

static void op_addi(uint16_t opc) {  // ADDI $acD, #I at the middle word
  uint8_t areg = (opc >> 8) & 1;
  int64_t acc = get_long_acc(areg);
  add_to(areg, acc, shl64((int16_t)dsp_lle_fetch(), 16));
}
static void op_addis(uint16_t opc) { uint8_t d = (opc >> 8) & 1; add_to(d, get_long_acc(d), shl64((int8_t)opc, 16)); }
static void op_incm(uint16_t opc) { uint8_t d = (opc >> 8) & 1; add_to(d, get_long_acc(d), 0x10000); }
static void op_inc(uint16_t opc) { uint8_t d = (opc >> 8) & 1; add_to(d, get_long_acc(d), 1); }

static void op_subr(uint16_t opc) {
  uint8_t dreg = (opc >> 8) & 1, sreg = ((opc >> 9) & 3) + DSP_REG_AXL0;
  subtract_from(dreg, get_long_acc(dreg), shl64(ax_part(sreg), 16));
}
static void op_subax(uint16_t opc) { uint8_t d = (opc >> 8) & 1, s = (opc >> 9) & 1; subtract_from(d, get_long_acc(d), get_long_acx(s)); }
static void op_sub(uint16_t opc) { uint8_t d = (opc >> 8) & 1; subtract_from(d, get_long_acc(d), get_long_acc(1 - d)); }
static void op_subp(uint16_t opc) { uint8_t d = (opc >> 8) & 1; subtract_from(d, get_long_acc(d), get_long_product()); }
static void op_decm(uint16_t opc) { uint8_t d = (opc >> 8) & 1; subtract_from(d, get_long_acc(d), 0x10000); }
static void op_dec(uint16_t opc) { uint8_t d = (opc >> 8) & 1; subtract_from(d, get_long_acc(d), 1); }

// NEG: carry only from 0, overflow only from the most negative value, which both stay as they were.
static void op_neg(uint16_t opc) {
  uint8_t dreg = (opc >> 8) & 1;
  int64_t acc = get_long_acc(dreg);
  set_long_acc(dreg, 0 - acc);
  update_sr64_sub(0, acc, get_long_acc(dreg));
}

static void op_abs(uint16_t opc) {
  uint8_t dreg = (opc >> 11) & 1;
  int64_t acc = get_long_acc(dreg);
  if (acc < 0) acc = 0 - acc;
  set_long_acc(dreg, acc);
  update_sr64(get_long_acc(dreg), 0, 0);  // Dolphin wonders whether this is right.
}

static void op_movr(uint16_t opc) {  // MOVR $acD, $axS.R: to the middle, low cleared.
  uint8_t areg = (opc >> 8) & 1, sreg = ((opc >> 9) & 3) + DSP_REG_AXL0;
  int64_t ax = shl64(ax_part(sreg), 16);
  set_long_acc(areg, ax);
  update_sr64(ax, 0, 0);
}

static void op_movax(uint16_t opc) {
  uint8_t dreg = (opc >> 8) & 1, sreg = (opc >> 9) & 1;
  int64_t acx = get_long_acx(sreg);
  set_long_acc(dreg, acx);
  update_sr64(acx, 0, 0);
}

static void op_mov(uint16_t opc) {
  uint8_t dreg = (opc >> 8) & 1;
  int64_t acc = get_long_acc(1 - dreg);
  set_long_acc(dreg, acc);
  update_sr64(acc, 0, 0);
}

static void shift_result(uint8_t reg, int64_t acc) {
  set_long_acc(reg, acc);
  update_sr64(get_long_acc(reg), 0, 0);
}

static void op_lsl16(uint16_t opc) { uint8_t r = (opc >> 8) & 1; shift_result(r, shl64(get_long_acc(r), 16)); }
static void op_lsr16(uint16_t opc) {
  uint8_t r = (opc >> 8) & 1;
  uint64_t acc = (uint64_t)get_long_acc(r) & 0x000000ffffffffffull;  // Drop our 64-bit sign extension.
  shift_result(r, (int64_t)(acc >> 16));
}
static void op_asr16(uint16_t opc) { uint8_t r = (opc >> 11) & 1; shift_result(r, get_long_acc(r) >> 16); }
static void op_lsl(uint16_t opc) { uint8_t r = (opc >> 8) & 1; shift_result(r, shl64(get_long_acc(r), opc & 0x3f)); }
static void op_asl(uint16_t opc) { uint8_t r = (opc >> 8) & 1; shift_result(r, shl64(get_long_acc(r), opc & 0x3f)); }

static void op_lsr(uint16_t opc) {  // LSR $acR, #I: right by 64 - I
  uint8_t r = (opc >> 8) & 1;
  uint64_t acc = (uint64_t)get_long_acc(r) & 0x000000ffffffffffull;
  unsigned shift = (opc & 0x3f) == 0 ? 0 : 0x40 - (opc & 0x3f);
  shift_result(r, (int64_t)(acc >> shift));
}

static void op_asr(uint16_t opc) {
  uint8_t r = (opc >> 8) & 1;
  unsigned shift = (opc & 0x3f) == 0 ? 0 : 0x40 - (opc & 0x3f);
  shift_result(r, get_long_acc(r) >> shift);
}

// The low 7 bits of a register as a signed shift.
static int16_t shift_amount(uint16_t value) {
  if ((value & 0x3f) == 0) return 0;
  if (value & 0x40) return (int16_t)(-0x40 + (value & 0x3f));
  return (int16_t)(value & 0x3f);
}

// LSRN and ASRN shift right for positive amounts; the R and RX forms shift left.
static void logical_shift(uint8_t reg, int16_t shift, int left_for_positive) {
  uint64_t acc = (uint64_t)get_long_acc(reg) & 0x000000ffffffffffull;
  if (!left_for_positive) shift = (int16_t)-shift;
  if (shift > 0) acc <<= shift;
  else if (shift < 0) acc >>= -shift;
  shift_result(reg, (int64_t)acc);
}

static void arithmetic_shift(uint8_t reg, int16_t shift, int left_for_positive) {
  int64_t acc = get_long_acc(reg);
  if (!left_for_positive) shift = (int16_t)-shift;
  if (shift > 0) acc = shl64(acc, (unsigned)shift);
  else if (shift < 0) acc >>= -shift;
  shift_result(reg, acc);
}

static void op_lsrn(uint16_t opc) { (void)opc; logical_shift(0, shift_amount((uint16_t)get_acc_mid(1)), 0); }
static void op_asrn(uint16_t opc) { (void)opc; arithmetic_shift(0, shift_amount((uint16_t)get_acc_mid(1)), 0); }
static void op_lsrnrx(uint16_t opc) { logical_shift((opc >> 8) & 1, shift_amount(R.ax[(opc >> 9) & 1].h), 1); }
static void op_asrnrx(uint16_t opc) { arithmetic_shift((opc >> 8) & 1, shift_amount(R.ax[(opc >> 9) & 1].h), 1); }
static void op_lsrnr(uint16_t opc) { uint8_t d = (opc >> 8) & 1; logical_shift(d, shift_amount((uint16_t)get_acc_mid(1 - d)), 1); }
static void op_asrnr(uint16_t opc) { uint8_t d = (opc >> 8) & 1; arithmetic_shift(d, shift_amount((uint16_t)get_acc_mid(1 - d)), 1); }

// ======================= Branches and loops (DSPIntBranch.cpp) =======================

static void op_call(uint16_t opc) {
  uint16_t dest = dsp_lle_fetch();  // Read whether or not it's taken.
  if (check_condition(opc & 0xf)) {
    dsp_lle_store_stack(STACK_CALL, dsp_lle.pc);
    dsp_lle.pc = dest;
  }
}

static void op_callr(uint16_t opc) {
  if (!check_condition(opc & 0xf)) return;
  uint16_t address = op_read_register((opc >> 5) & 7);
  dsp_lle_store_stack(STACK_CALL, dsp_lle.pc);
  dsp_lle.pc = address;
}

static void op_ifcc(uint16_t opc) {
  if (check_condition(opc & 0xf)) return;
  dsp_lle_skip_instruction();
}

static void op_jcc(uint16_t opc) {
  uint16_t dest = dsp_lle_fetch();
  if (check_condition(opc & 0xf)) dsp_lle.pc = dest;
}

static void op_jmprcc(uint16_t opc) {
  if (!check_condition(opc & 0xf)) return;
  dsp_lle.pc = op_read_register((opc >> 5) & 7);
}

static void op_ret(uint16_t opc) {
  if (!check_condition(opc & 0xf)) return;
  dsp_lle.pc = dsp_lle_pop_stack(STACK_CALL);
}

static void op_rti(uint16_t opc) {  // Takes back SR from $st1 and pc from $st0.
  if (!check_condition(opc & 0xf)) return;
  R.sr = dsp_lle_pop_stack(STACK_DATA);
  dsp_lle.pc = dsp_lle_pop_stack(STACK_CALL);
}

static void op_halt(uint16_t opc) {
  (void)opc;
  dsp_lle.control_reg |= DSP_CR_HALT;
  dsp_lle.pc--;
}

// The loop hardware: when pc passes the address on $st2, the count on $st3 drops; while it's
// above zero pc goes back to $st0, and at zero all three are popped.
void dsp_lle_handle_loop(void) {
  uint16_t call_address = R.st[0], loop_address = R.st[2];
  if (loop_address > 0 && R.st[3] > 0 && dsp_lle.pc - 1 == loop_address) {
    R.st[3]--;
    if (R.st[3] > 0) {
      dsp_lle.pc = call_address;
    } else {
      dsp_lle_pop_stack(STACK_CALL);
      dsp_lle_pop_stack(STACK_LOOP_ADDRESS);
      dsp_lle_pop_stack(STACK_LOOP_COUNTER);
    }
  }
}

// LOOP and LOOPI repeat the next instruction; a count of 0 skips it.
static void repeat_next(uint16_t count) {
  uint16_t loop_pc = dsp_lle.pc;
  if (count) {
    dsp_lle_store_stack(STACK_CALL, dsp_lle.pc);
    dsp_lle_store_stack(STACK_LOOP_ADDRESS, loop_pc);
    dsp_lle_store_stack(STACK_LOOP_COUNTER, count);
  } else {
    dsp_lle_skip_instruction();
  }
}

// BLOOP and BLOOPI repeat up to and including the instruction at the given address.
static void repeat_block(uint16_t count) {
  uint16_t loop_pc = dsp_lle_fetch();
  if (count) {
    dsp_lle_store_stack(STACK_CALL, dsp_lle.pc);
    dsp_lle_store_stack(STACK_LOOP_ADDRESS, loop_pc);
    dsp_lle_store_stack(STACK_LOOP_COUNTER, count);
  } else {
    dsp_lle.pc = loop_pc;
    dsp_lle_skip_instruction();
  }
}

static void op_loop(uint16_t opc) { repeat_next(op_read_register(opc & 0x1f)); }
static void op_loopi(uint16_t opc) { repeat_next(opc & 0xff); }
static void op_bloop(uint16_t opc) { repeat_block(op_read_register(opc & 0x1f)); }
static void op_bloopi(uint16_t opc) { repeat_block(opc & 0xff); }

// ======================= Loads and stores (DSPIntLoadStore.cpp) =======================

// SRSH, SRS and LRS address ($cr << 8) | I.
static uint16_t short_address(uint16_t opc) { return (uint16_t)(R.cr << 8 | (opc & 0xff)); }

static void op_srsh(uint16_t opc) { dsp_lle_write_dmem(short_address(opc), op_read_register(((opc >> 8) & 1) + DSP_REG_ACH0)); }
static void op_srs(uint16_t opc) { dsp_lle_write_dmem(short_address(opc), op_read_register(((opc >> 8) & 3) + DSP_REG_ACL0)); }

static void op_lrs(uint16_t opc) {
  uint8_t reg = ((opc >> 8) & 7) + 0x18;
  op_write_register(reg, dsp_lle_read_dmem(short_address(opc)));
  conditional_extend_accum(reg);
}

static void op_lr(uint16_t opc) {
  uint8_t reg = opc & 0x1f;
  uint16_t address = dsp_lle_fetch();
  op_write_register(reg, dsp_lle_read_dmem(address));
  conditional_extend_accum(reg);
}

static void op_sr(uint16_t opc) {
  uint16_t address = dsp_lle_fetch();
  dsp_lle_write_dmem(address, op_read_register(opc & 0x1f));
}

static void op_si(uint16_t opc) {  // SI @M, #I: M is 8 bits, sign-extended (so 0xffxx).
  uint16_t address = (uint16_t)(int8_t)opc;
  dsp_lle_write_dmem(address, dsp_lle_fetch());
}

// LRR family: 0 stays, 1 decrements, 2 increments, 3 adds $ixS.
static void load_indirect(uint16_t opc, int step) {
  uint8_t sreg = (opc >> 5) & 3, dreg = opc & 0x1f;
  op_write_register(dreg, dsp_lle_read_dmem(op_read_register(sreg)));
  conditional_extend_accum(dreg);
  if (step == 1) R.ar[sreg] = decrement_ar(sreg);
  else if (step == 2) R.ar[sreg] = increment_ar(sreg);
  else if (step == 3) R.ar[sreg] = increase_ar(sreg, (int16_t)R.ix[sreg]);
}

static void store_indirect(uint16_t opc, int step) {
  uint8_t dreg = (opc >> 5) & 3, sreg = opc & 0x1f;
  dsp_lle_write_dmem(R.ar[dreg], op_read_register(sreg));
  if (step == 1) R.ar[dreg] = decrement_ar(dreg);
  else if (step == 2) R.ar[dreg] = increment_ar(dreg);
  else if (step == 3) R.ar[dreg] = increase_ar(dreg, (int16_t)R.ix[dreg]);
}

static void op_lrr(uint16_t opc) { load_indirect(opc, 0); }
static void op_lrrd(uint16_t opc) { load_indirect(opc, 1); }
static void op_lrri(uint16_t opc) { load_indirect(opc, 2); }
static void op_lrrn(uint16_t opc) { load_indirect(opc, 3); }
static void op_srr(uint16_t opc) { store_indirect(opc, 0); }
static void op_srrd(uint16_t opc) { store_indirect(opc, 1); }
static void op_srri(uint16_t opc) { store_indirect(opc, 2); }
static void op_srrn(uint16_t opc) { store_indirect(opc, 3); }

// ILRR family: $acD.m from instruction memory at $arS.
static void load_instruction_memory(uint16_t opc, int step) {
  uint16_t reg = opc & 3, dreg = DSP_REG_ACM0 + ((opc >> 8) & 1);
  R.ac[dreg - DSP_REG_ACM0].m = dsp_lle_read_imem(R.ar[reg]);
  conditional_extend_accum(dreg);
  if (step == 1) R.ar[reg] = decrement_ar(reg);
  else if (step == 2) R.ar[reg] = increment_ar(reg);
  else if (step == 3) R.ar[reg] = increase_ar(reg, (int16_t)R.ix[reg]);
}

static void op_ilrr(uint16_t opc) { load_instruction_memory(opc, 0); }
static void op_ilrrd(uint16_t opc) { load_instruction_memory(opc, 1); }
static void op_ilrri(uint16_t opc) { load_instruction_memory(opc, 2); }
static void op_ilrrn(uint16_t opc) { load_instruction_memory(opc, 3); }

// ======================= Everything else (DSPIntMisc.cpp) =======================

static void op_mrr(uint16_t opc) {
  uint8_t sreg = opc & 0x1f, dreg = (opc >> 5) & 0x1f;
  op_write_register(dreg, op_read_register(sreg));
  conditional_extend_accum(dreg);
}

static void op_lri(uint16_t opc) {
  uint8_t reg = opc & 0x1f;
  op_write_register(reg, dsp_lle_fetch());
  conditional_extend_accum(reg);
}

static void op_lris(uint16_t opc) {  // LRIS $(0x18+D), #I: 8 bits, sign-extended
  uint8_t reg = ((opc >> 8) & 7) + DSP_REG_AXL0;
  op_write_register(reg, (uint16_t)(int8_t)opc);
  conditional_extend_accum(reg);
}

static void op_nx(uint16_t opc) { (void)opc; }  // Nothing, for the sake of an extension.

static void op_dar(uint16_t opc) { R.ar[opc & 3] = decrement_ar(opc & 3); }
static void op_iar(uint16_t opc) { R.ar[opc & 3] = increment_ar(opc & 3); }
static void op_subarn(uint16_t opc) { R.ar[opc & 3] = decrease_ar(opc & 3, (int16_t)R.ix[opc & 3]); }
// Zelda's program depends on this wrapping correctly.
static void op_addarn(uint16_t opc) { R.ar[opc & 3] = increase_ar(opc & 3, (int16_t)R.ix[(opc >> 2) & 3]); }

static void op_sbclr(uint16_t opc) { R.sr &= (uint16_t)~(1u << ((opc & 7) + 6)); }  // Bits 6 to 13.
static void op_sbset(uint16_t opc) { R.sr |= (uint16_t)(1u << ((opc & 7) + 6)); }

static void op_srbith(uint16_t opc) {
  switch ((opc >> 8) & 7) {
  case 2: R.sr &= ~DSP_SR_MUL_MODIFY; break;    // M2
  case 3: R.sr |= DSP_SR_MUL_MODIFY; break;     // M0
  case 4: R.sr &= ~DSP_SR_MUL_UNSIGNED; break;  // CLR15
  case 5: R.sr |= DSP_SR_MUL_UNSIGNED; break;   // SET15
  case 6: R.sr &= ~DSP_SR_40_MODE_BIT; break;   // SET16
  case 7: R.sr |= DSP_SR_40_MODE_BIT; break;    // SET40
  default: break;
  }
}

static void op_nop(uint16_t opc) {
  if (opc == 0) return;  // The real NOP is 0; anything else here is unknown.
  gap_hit("limit.dsp_unknown_opcode", (uint32_t)dsp_lle.pc << 16 | opc);
}

// ======================= The multiplier (DSPIntMultiplier.cpp) =======================

// CLRP: the parts take these values, which sum to 0 in 40 bits. AX reads the parts directly.
static void op_clrp(uint16_t opc) {
  (void)opc;
  R.prod.l = 0x0000;
  R.prod.m = 0xfff0;
  R.prod.h = 0x00ff;
  R.prod.m2 = 0x0010;
}

static void op_tstprod(uint16_t opc) { (void)opc; update_sr64(get_long_product(), 0, 0); }

static void product_to(uint8_t dreg, int64_t acc) {
  set_long_acc(dreg, acc);
  update_sr64(acc, 0, 0);
}
static void op_movp(uint16_t opc) { product_to((opc >> 8) & 1, get_long_product()); }
static void op_movnp(uint16_t opc) { product_to((opc >> 8) & 1, -get_long_product()); }
static void op_movpz(uint16_t opc) { product_to((opc >> 8) & 1, get_long_product_rounded()); }

static void op_addpaxz(uint16_t opc) {  // ADDPAXZ $acD, $axS (Dolphin: still off by one at times)
  uint8_t dreg = (opc >> 8) & 1, sreg = (opc >> 9) & 1;
  int64_t old_product = get_long_product();
  int64_t product = get_long_product_rounded();
  int64_t ax = get_long_acx(sreg);
  set_long_acc(dreg, product + (ax & ~(int64_t)0xffff));
  int64_t result = get_long_acc(dreg);
  update_sr64(result, is_carry_add((uint64_t)old_product, (uint64_t)result), 0);  // No overflow, oddly.
}

static void op_mulaxh(uint16_t opc) {
  (void)opc;
  int16_t value = get_ax_high(0);
  set_long_product(multiply((uint16_t)value, (uint16_t)value, 0));
}

static void op_mul(uint16_t opc) {
  uint8_t sreg = (opc >> 11) & 1;
  set_long_product(multiply((uint16_t)get_ax_high(sreg), (uint16_t)get_ax_low(sreg), 0));
}

// The MUL/MULX/MULC forms that also move the old product to $acR: added (AC), moved (MV) or
// moved and rounded (MVZ).
enum { TO_ACC_ADD, TO_ACC_MOVE, TO_ACC_ROUND };
static int64_t old_product_for(uint8_t rreg, int how) {
  if (how == TO_ACC_ADD) return get_long_acc(rreg) + get_long_product();
  if (how == TO_ACC_MOVE) return get_long_product();
  return get_long_product_rounded();
}
static void new_product_and_acc(uint8_t rreg, int64_t acc, int64_t product) {
  set_long_product(product);
  set_long_acc(rreg, acc);
  update_sr64(get_long_acc(rreg), 0, 0);
}

static void mul_to_acc(uint16_t opc, int how) {
  uint8_t rreg = (opc >> 8) & 1, sreg = (opc >> 11) & 1;
  int64_t acc = old_product_for(rreg, how);
  new_product_and_acc(rreg, acc, multiply((uint16_t)get_ax_low(sreg), (uint16_t)get_ax_high(sreg), 0));
}
static void op_mulac(uint16_t opc) { mul_to_acc(opc, TO_ACC_ADD); }
static void op_mulmv(uint16_t opc) { mul_to_acc(opc, TO_ACC_MOVE); }
static void op_mulmvz(uint16_t opc) { mul_to_acc(opc, TO_ACC_ROUND); }

static uint16_t mulx_value1(uint8_t sreg) { return (uint16_t)(sreg == 0 ? get_ax_low(0) : get_ax_high(0)); }
static uint16_t mulx_value2(uint8_t treg) { return (uint16_t)(treg == 0 ? get_ax_low(1) : get_ax_high(1)); }

static void op_mulx(uint16_t opc) {
  uint8_t treg = (opc >> 11) & 1, sreg = (opc >> 12) & 1;
  set_long_product(multiply_mulx(sreg, treg, mulx_value1(sreg), mulx_value2(treg)));
}

static void mulx_to_acc(uint16_t opc, int how) {
  uint8_t rreg = (opc >> 8) & 1, treg = (opc >> 11) & 1, sreg = (opc >> 12) & 1;
  int64_t acc = old_product_for(rreg, how);
  new_product_and_acc(rreg, acc, multiply_mulx(sreg, treg, mulx_value1(sreg), mulx_value2(treg)));
}
static void op_mulxac(uint16_t opc) { mulx_to_acc(opc, TO_ACC_ADD); }
static void op_mulxmv(uint16_t opc) { mulx_to_acc(opc, TO_ACC_MOVE); }
static void op_mulxmvz(uint16_t opc) { mulx_to_acc(opc, TO_ACC_ROUND); }

static void op_mulc(uint16_t opc) {
  uint8_t treg = (opc >> 11) & 1, sreg = (opc >> 12) & 1;
  set_long_product(multiply((uint16_t)get_acc_mid(sreg), (uint16_t)get_ax_high(treg), 0));
}

static void mulc_to_acc(uint16_t opc, int how) {
  uint8_t rreg = (opc >> 8) & 1, treg = (opc >> 11) & 1, sreg = (opc >> 12) & 1;
  int64_t acc = old_product_for(rreg, how);
  new_product_and_acc(rreg, acc, multiply((uint16_t)get_acc_mid(sreg), (uint16_t)get_ax_high(treg), 0));
}
static void op_mulcac(uint16_t opc) { mulc_to_acc(opc, TO_ACC_ADD); }
static void op_mulcmv(uint16_t opc) { mulc_to_acc(opc, TO_ACC_MOVE); }
static void op_mulcmvz(uint16_t opc) { mulc_to_acc(opc, TO_ACC_ROUND); }

static void op_maddx(uint16_t opc) {
  uint8_t treg = (opc >> 8) & 1, sreg = (opc >> 9) & 1;
  set_long_product(multiply_add(mulx_value1(sreg), mulx_value2(treg)));
}
static void op_msubx(uint16_t opc) {
  uint8_t treg = (opc >> 8) & 1, sreg = (opc >> 9) & 1;
  set_long_product(multiply_sub(mulx_value1(sreg), mulx_value2(treg)));
}
static void op_maddc(uint16_t opc) {
  uint8_t treg = (opc >> 8) & 1, sreg = (opc >> 9) & 1;
  set_long_product(multiply_add((uint16_t)get_acc_mid(sreg), (uint16_t)get_ax_high(treg)));
}
static void op_msubc(uint16_t opc) {
  uint8_t treg = (opc >> 8) & 1, sreg = (opc >> 9) & 1;
  set_long_product(multiply_sub((uint16_t)get_acc_mid(sreg), (uint16_t)get_ax_high(treg)));
}
static void op_madd(uint16_t opc) {
  uint8_t sreg = (opc >> 8) & 1;
  set_long_product(multiply_add((uint16_t)get_ax_low(sreg), (uint16_t)get_ax_high(sreg)));
}
static void op_msub(uint16_t opc) {
  uint8_t sreg = (opc >> 8) & 1;
  set_long_product(multiply_sub((uint16_t)get_ax_low(sreg), (uint16_t)get_ax_high(sreg)));
}

// ======================= Extensions (DSPIntExtOps.cpp) =======================
// They never change pc. They step address registers, wrapping by $wrX.

// LM tested this on a Wii: the same 1 KB area of data memory.
static int same_mem_area(uint16_t a, uint16_t b) { return (a >> 10) == (b >> 10); }

static void ext_nop_ext(uint16_t opc) { (void)opc; }
static void ext_dr(uint16_t opc) { write_to_back_log(0, opc & 3, decrement_ar(opc & 3)); }
static void ext_ir(uint16_t opc) { write_to_back_log(0, opc & 3, increment_ar(opc & 3)); }
static void ext_nr(uint16_t opc) { uint8_t r = opc & 3; write_to_back_log(0, r, increase_ar(r, (int16_t)R.ix[r])); }

static void ext_mv(uint16_t opc) {  // MV $axD.D, $acS.S
  uint8_t sreg = (opc & 3) + DSP_REG_ACL0, dreg = (opc >> 2) & 3;
  write_to_back_log(0, dreg + DSP_REG_AXL0, op_read_register(sreg));
}

static void ext_s(uint16_t opc) {  // S @$arD, $acS.S; $arD++
  uint8_t dreg = opc & 3, sreg = ((opc >> 3) & 3) + DSP_REG_ACL0;
  dsp_lle_write_dmem(R.ar[dreg], op_read_register(sreg));
  write_to_back_log(0, dreg, increment_ar(dreg));
}

static void ext_sn(uint16_t opc) {  // SN @$arD, $acS.S; $arD += $ixD
  uint8_t dreg = opc & 3, sreg = ((opc >> 3) & 3) + DSP_REG_ACL0;
  dsp_lle_write_dmem(R.ar[dreg], op_read_register(sreg));
  write_to_back_log(0, dreg, increase_ar(dreg, (int16_t)R.ix[dreg]));
}

// L and LN: in 40-bit mode a load to $acX.m sign-extends, as conditional_extend_accum.
static void load_ext(uint16_t opc, uint16_t next_ar) {
  uint8_t sreg = opc & 3, dreg = ((opc >> 3) & 7) + DSP_REG_AXL0;
  if (dreg >= DSP_REG_ACM0 && sr_set(DSP_SR_40_MODE_BIT)) {
    uint16_t value = dsp_lle_read_dmem(R.ar[sreg]);
    write_to_back_log(0, dreg - DSP_REG_ACM0 + DSP_REG_ACH0, (value & 0x8000) ? 0xffff : 0x0000);
    write_to_back_log(1, dreg, value);
    write_to_back_log(2, dreg - DSP_REG_ACM0 + DSP_REG_ACL0, 0);
    write_to_back_log(3, sreg, next_ar);
  } else {
    write_to_back_log(0, dreg, dsp_lle_read_dmem(R.ar[sreg]));
    write_to_back_log(1, sreg, next_ar);
  }
}
static void ext_l(uint16_t opc) { load_ext(opc, increment_ar(opc & 3)); }
static void ext_ln(uint16_t opc) { load_ext(opc, increase_ar(opc & 3, (int16_t)R.ix[opc & 3])); }

// LS and SL family: one load and one store through $ar0 and $ar3. LS loads from $ar0 and
// stores to $ar3; SL stores to $ar0 and loads from $ar3. N steps $ar0 by $ix0, M steps $ar3
// by $ix3; the others step by 1.
static void load_store(uint16_t opc, int store_first_to_ar0, int n, int m) {
  uint8_t sreg = (opc & 1) + DSP_REG_ACM0, dreg = ((opc >> 4) & 3) + DSP_REG_AXL0;
  dsp_lle_write_dmem(store_first_to_ar0 ? R.ar[0] : R.ar[3], op_read_register(sreg));
  write_to_back_log(0, dreg, dsp_lle_read_dmem(store_first_to_ar0 ? R.ar[3] : R.ar[0]));
  write_to_back_log(1, DSP_REG_AR3, m ? increase_ar(DSP_REG_AR3, (int16_t)R.ix[3]) : increment_ar(DSP_REG_AR3));
  write_to_back_log(2, DSP_REG_AR0, n ? increase_ar(DSP_REG_AR0, (int16_t)R.ix[0]) : increment_ar(DSP_REG_AR0));
}
static void ext_ls(uint16_t opc) { load_store(opc, 0, 0, 0); }
static void ext_lsn(uint16_t opc) { load_store(opc, 0, 1, 0); }
static void ext_lsm(uint16_t opc) { load_store(opc, 0, 0, 1); }
static void ext_lsnm(uint16_t opc) { load_store(opc, 0, 1, 1); }
static void ext_sl(uint16_t opc) { load_store(opc, 1, 0, 0); }
static void ext_sln(uint16_t opc) { load_store(opc, 1, 1, 0); }
static void ext_slm(uint16_t opc) { load_store(opc, 1, 0, 1); }
static void ext_slnm(uint16_t opc) { load_store(opc, 1, 1, 1); }

// LD family: two loads, through $arS and $ar3. If both point into the same 1 KB area, both
// loads get the value at $arS. (Dolphin doesn't model loads from areas that don't exist.)
static void double_load(uint8_t sreg, int first, int second, int n, int m) {
  write_to_back_log(0, first, dsp_lle_read_dmem(R.ar[sreg]));
  if (same_mem_area(R.ar[sreg], R.ar[3])) write_to_back_log(1, second, dsp_lle_read_dmem(R.ar[sreg]));
  else write_to_back_log(1, second, dsp_lle_read_dmem(R.ar[3]));
  write_to_back_log(2, sreg, n ? increase_ar(sreg, (int16_t)R.ix[sreg]) : increment_ar(sreg));
  write_to_back_log(3, DSP_REG_AR3, m ? increase_ar(DSP_REG_AR3, (int16_t)R.ix[3]) : increment_ar(DSP_REG_AR3));
}

// LD $ax0.D, $ax1.R, @$arS
static void ld_family(uint16_t opc, int n, int m) {
  uint8_t dreg = (opc >> 5) & 1, rreg = (opc >> 4) & 1, sreg = opc & 3;
  double_load(sreg, (dreg << 1) + DSP_REG_AXL0, (rreg << 1) + DSP_REG_AXL1, n, m);
}
// LDAX $axR, @$arS: $axR.h from $arS, $axR.l from $ar3.
static void ldax_family(uint16_t opc, int n, int m) {
  uint8_t sreg = (opc >> 5) & 1, rreg = (opc >> 4) & 1;
  double_load(sreg, rreg + DSP_REG_AXH0, rreg + DSP_REG_AXL0, n, m);
}
static void ext_ld(uint16_t opc) { ld_family(opc, 0, 0); }
static void ext_ldn(uint16_t opc) { ld_family(opc, 1, 0); }
static void ext_ldm(uint16_t opc) { ld_family(opc, 0, 1); }
static void ext_ldnm(uint16_t opc) { ld_family(opc, 1, 1); }
static void ext_ldax(uint16_t opc) { ldax_family(opc, 0, 0); }
static void ext_ldaxn(uint16_t opc) { ldax_family(opc, 1, 0); }
static void ext_ldaxm(uint16_t opc) { ldax_family(opc, 0, 1); }
static void ext_ldaxnm(uint16_t opc) { ldax_family(opc, 1, 1); }

// ======================= Tables =======================

// Dolphin's opcode templates (DSPTables.cpp): opcode, mask, size in words, extended. The first
// that matches wins; nothing matching is a one-word "CW" that isn't extended.
static const struct { uint16_t opcode, mask; uint8_t size, extended; } templates[] = {
  {0x0000, 0xfffc, 1, 0}, {0x0004, 0xfffc, 1, 0}, {0x0008, 0xfffc, 1, 0}, {0x000c, 0xfffc, 1, 0},
  {0x0010, 0xfff0, 1, 0}, {0x0021, 0xffff, 1, 0}, {0x02d0, 0xffff, 1, 0}, {0x02d1, 0xffff, 1, 0},
  {0x02d2, 0xffff, 1, 0}, {0x02d3, 0xffff, 1, 0}, {0x02d4, 0xffff, 1, 0}, {0x02d5, 0xffff, 1, 0},
  {0x02d6, 0xffff, 1, 0}, {0x02d7, 0xffff, 1, 0}, {0x02d8, 0xffff, 1, 0}, {0x02d9, 0xffff, 1, 0},
  {0x02da, 0xffff, 1, 0}, {0x02db, 0xffff, 1, 0}, {0x02dc, 0xffff, 1, 0}, {0x02dd, 0xffff, 1, 0},
  {0x02de, 0xffff, 1, 0}, {0x02df, 0xffff, 1, 0}, {0x02f0, 0xffff, 1, 0}, {0x02f1, 0xffff, 1, 0},
  {0x02f2, 0xffff, 1, 0}, {0x02f3, 0xffff, 1, 0}, {0x02f4, 0xffff, 1, 0}, {0x02f5, 0xffff, 1, 0},
  {0x02f6, 0xffff, 1, 0}, {0x02f7, 0xffff, 1, 0}, {0x02f8, 0xffff, 1, 0}, {0x02f9, 0xffff, 1, 0},
  {0x02fa, 0xffff, 1, 0}, {0x02fb, 0xffff, 1, 0}, {0x02fc, 0xffff, 1, 0}, {0x02fd, 0xffff, 1, 0},
  {0x02fe, 0xffff, 1, 0}, {0x02ff, 0xffff, 1, 0}, {0x02b0, 0xffff, 2, 0}, {0x02b1, 0xffff, 2, 0},
  {0x02b2, 0xffff, 2, 0}, {0x02b3, 0xffff, 2, 0}, {0x02b4, 0xffff, 2, 0}, {0x02b5, 0xffff, 2, 0},
  {0x02b6, 0xffff, 2, 0}, {0x02b7, 0xffff, 2, 0}, {0x02b8, 0xffff, 2, 0}, {0x02b9, 0xffff, 2, 0},
  {0x02ba, 0xffff, 2, 0}, {0x02bb, 0xffff, 2, 0}, {0x02bc, 0xffff, 2, 0}, {0x02bd, 0xffff, 2, 0},
  {0x02be, 0xffff, 2, 0}, {0x02bf, 0xffff, 2, 0}, {0x0270, 0xffff, 1, 0}, {0x0271, 0xffff, 1, 0},
  {0x0272, 0xffff, 1, 0}, {0x0273, 0xffff, 1, 0}, {0x0274, 0xffff, 1, 0}, {0x0275, 0xffff, 1, 0},
  {0x0276, 0xffff, 1, 0}, {0x0277, 0xffff, 1, 0}, {0x0278, 0xffff, 1, 0}, {0x0279, 0xffff, 1, 0},
  {0x027a, 0xffff, 1, 0}, {0x027b, 0xffff, 1, 0}, {0x027c, 0xffff, 1, 0}, {0x027d, 0xffff, 1, 0},
  {0x027e, 0xffff, 1, 0}, {0x027f, 0xffff, 1, 0}, {0x0290, 0xffff, 2, 0}, {0x0291, 0xffff, 2, 0},
  {0x0292, 0xffff, 2, 0}, {0x0293, 0xffff, 2, 0}, {0x0294, 0xffff, 2, 0}, {0x0295, 0xffff, 2, 0},
  {0x0296, 0xffff, 2, 0}, {0x0297, 0xffff, 2, 0}, {0x0298, 0xffff, 2, 0}, {0x0299, 0xffff, 2, 0},
  {0x029a, 0xffff, 2, 0}, {0x029b, 0xffff, 2, 0}, {0x029c, 0xffff, 2, 0}, {0x029d, 0xffff, 2, 0},
  {0x029e, 0xffff, 2, 0}, {0x029f, 0xffff, 2, 0}, {0x1700, 0xff1f, 1, 0}, {0x1701, 0xff1f, 1, 0},
  {0x1702, 0xff1f, 1, 0}, {0x1703, 0xff1f, 1, 0}, {0x1704, 0xff1f, 1, 0}, {0x1705, 0xff1f, 1, 0},
  {0x1706, 0xff1f, 1, 0}, {0x1707, 0xff1f, 1, 0}, {0x1708, 0xff1f, 1, 0}, {0x1709, 0xff1f, 1, 0},
  {0x170a, 0xff1f, 1, 0}, {0x170b, 0xff1f, 1, 0}, {0x170c, 0xff1f, 1, 0}, {0x170d, 0xff1f, 1, 0},
  {0x170e, 0xff1f, 1, 0}, {0x170f, 0xff1f, 1, 0}, {0x1710, 0xff1f, 1, 0}, {0x1711, 0xff1f, 1, 0},
  {0x1712, 0xff1f, 1, 0}, {0x1713, 0xff1f, 1, 0}, {0x1714, 0xff1f, 1, 0}, {0x1715, 0xff1f, 1, 0},
  {0x1716, 0xff1f, 1, 0}, {0x1717, 0xff1f, 1, 0}, {0x1718, 0xff1f, 1, 0}, {0x1719, 0xff1f, 1, 0},
  {0x171a, 0xff1f, 1, 0}, {0x171b, 0xff1f, 1, 0}, {0x171c, 0xff1f, 1, 0}, {0x171d, 0xff1f, 1, 0},
  {0x171e, 0xff1f, 1, 0}, {0x171f, 0xff1f, 1, 0}, {0x1200, 0xff00, 1, 0}, {0x1300, 0xff00, 1, 0},
  {0x1400, 0xfec0, 1, 0}, {0x1440, 0xfec0, 1, 0}, {0x1480, 0xfec0, 1, 0}, {0x14c0, 0xfec0, 1, 0},
  {0x02ca, 0xffff, 1, 0}, {0x02cb, 0xffff, 1, 0}, {0x0080, 0xffe0, 2, 0}, {0x00c0, 0xffe0, 2, 0},
  {0x00e0, 0xffe0, 2, 0}, {0x1c00, 0xfc00, 1, 0}, {0x1600, 0xff00, 2, 0}, {0x0400, 0xfe00, 1, 0},
  {0x0600, 0xfe00, 1, 0}, {0x0800, 0xf800, 1, 0}, {0x0200, 0xfeff, 2, 0}, {0x0220, 0xfeff, 2, 0},
  {0x0240, 0xfeff, 2, 0}, {0x0260, 0xfeff, 2, 0}, {0x0280, 0xfeff, 2, 0}, {0x02a0, 0xfeff, 2, 0},
  {0x02c0, 0xfeff, 2, 0}, {0x0210, 0xfefc, 1, 0}, {0x0214, 0xfefc, 1, 0}, {0x0218, 0xfefc, 1, 0},
  {0x021c, 0xfefc, 1, 0}, {0x0040, 0xffe0, 1, 0}, {0x0060, 0xffe0, 2, 0}, {0x1000, 0xff00, 1, 0},
  {0x1100, 0xff00, 2, 0}, {0x1800, 0xff80, 1, 0}, {0x1880, 0xff80, 1, 0}, {0x1900, 0xff80, 1, 0},
  {0x1980, 0xff80, 1, 0}, {0x1a00, 0xff80, 1, 0}, {0x1a80, 0xff80, 1, 0}, {0x1b00, 0xff80, 1, 0},
  {0x1b80, 0xff80, 1, 0}, {0x2000, 0xf800, 1, 0}, {0x2800, 0xfe00, 1, 0}, {0x2c00, 0xfc00, 1, 0},
  {0x3000, 0xfc80, 1, 1}, {0x3400, 0xfc80, 1, 1}, {0x3800, 0xfc80, 1, 1}, {0x3c00, 0xfe80, 1, 1},
  {0x3e00, 0xfe80, 1, 1}, {0x3080, 0xfe80, 1, 1}, {0x3280, 0xfe80, 1, 1}, {0x3480, 0xfc80, 1, 1},
  {0x3880, 0xfc80, 1, 1}, {0x3c80, 0xfe80, 1, 1}, {0x3e80, 0xfe80, 1, 1}, {0x4000, 0xf800, 1, 1},
  {0x4800, 0xfc00, 1, 1}, {0x4c00, 0xfe00, 1, 1}, {0x4e00, 0xfe00, 1, 1}, {0x5000, 0xf800, 1, 1},
  {0x5800, 0xfc00, 1, 1}, {0x5c00, 0xfe00, 1, 1}, {0x5e00, 0xfe00, 1, 1}, {0x6000, 0xf800, 1, 1},
  {0x6800, 0xfc00, 1, 1}, {0x6c00, 0xfe00, 1, 1}, {0x6e00, 0xfe00, 1, 1}, {0x7000, 0xfc00, 1, 1},
  {0x7400, 0xfe00, 1, 1}, {0x7600, 0xfe00, 1, 1}, {0x7800, 0xfe00, 1, 1}, {0x7a00, 0xfe00, 1, 1},
  {0x7c00, 0xfe00, 1, 1}, {0x7e00, 0xfe00, 1, 1}, {0x8000, 0xf700, 1, 1}, {0x8100, 0xf700, 1, 1},
  {0x8200, 0xff00, 1, 1}, {0x8300, 0xff00, 1, 1}, {0x8400, 0xff00, 1, 1}, {0x8500, 0xff00, 1, 1},
  {0x8600, 0xfe00, 1, 1}, {0x8a00, 0xff00, 1, 1}, {0x8b00, 0xff00, 1, 1}, {0x8c00, 0xff00, 1, 1},
  {0x8d00, 0xff00, 1, 1}, {0x8e00, 0xff00, 1, 1}, {0x8f00, 0xff00, 1, 1}, {0x9000, 0xf700, 1, 1},
  {0x9100, 0xf700, 1, 1}, {0x9200, 0xf600, 1, 1}, {0x9400, 0xf600, 1, 1}, {0x9600, 0xf600, 1, 1},
  {0xa000, 0xe700, 1, 1}, {0xa100, 0xf700, 1, 1}, {0xa200, 0xe600, 1, 1}, {0xa400, 0xe600, 1, 1},
  {0xa600, 0xe600, 1, 1}, {0xb100, 0xf700, 1, 1}, {0xc000, 0xe700, 1, 1}, {0xc100, 0xe700, 1, 1},
  {0xc200, 0xe600, 1, 1}, {0xc400, 0xe600, 1, 1}, {0xc600, 0xe600, 1, 1}, {0xe000, 0xfc00, 1, 1},
  {0xe400, 0xfc00, 1, 1}, {0xe800, 0xfc00, 1, 1}, {0xec00, 0xfc00, 1, 1}, {0xf000, 0xfe00, 1, 1},
  {0xf200, 0xfe00, 1, 1}, {0xf400, 0xfe00, 1, 1}, {0xf600, 0xfe00, 1, 1}, {0xf800, 0xfc00, 1, 1},
  {0xfc00, 0xfe00, 1, 1}, {0xfe00, 0xfe00, 1, 1},
};

// The interpreter's own table (DSPIntTables.cpp). Nothing matching runs op_nop.
static const struct { uint16_t opcode, mask; DspOp function; } ops[] = {
  {0x0000, 0xfffc, op_nop}, {0x0004, 0xfffc, op_dar}, {0x0008, 0xfffc, op_iar},
  {0x000c, 0xfffc, op_subarn}, {0x0010, 0xfff0, op_addarn}, {0x0021, 0xffff, op_halt},
  {0x02d0, 0xfff0, op_ret}, {0x02f0, 0xfff0, op_rti}, {0x02b0, 0xfff0, op_call},
  {0x0270, 0xfff0, op_ifcc}, {0x0290, 0xfff0, op_jcc}, {0x1700, 0xff10, op_jmprcc},
  {0x1710, 0xff10, op_callr}, {0x1200, 0xff00, op_sbclr}, {0x1300, 0xff00, op_sbset},
  {0x1400, 0xfec0, op_lsl}, {0x1440, 0xfec0, op_lsr}, {0x1480, 0xfec0, op_asl},
  {0x14c0, 0xfec0, op_asr}, {0x02ca, 0xffff, op_lsrn}, {0x02cb, 0xffff, op_asrn},
  {0x0080, 0xffe0, op_lri}, {0x00c0, 0xffe0, op_lr}, {0x00e0, 0xffe0, op_sr},
  {0x1c00, 0xfc00, op_mrr}, {0x1600, 0xff00, op_si}, {0x0400, 0xfe00, op_addis},
  {0x0600, 0xfe00, op_cmpis}, {0x0800, 0xf800, op_lris}, {0x0200, 0xfeff, op_addi},
  {0x0220, 0xfeff, op_xori}, {0x0240, 0xfeff, op_andi}, {0x0260, 0xfeff, op_ori},
  {0x0280, 0xfeff, op_cmpi}, {0x02a0, 0xfeff, op_andf}, {0x02c0, 0xfeff, op_andcf},
  {0x0210, 0xfefc, op_ilrr}, {0x0214, 0xfefc, op_ilrrd}, {0x0218, 0xfefc, op_ilrri},
  {0x021c, 0xfefc, op_ilrrn}, {0x0040, 0xffe0, op_loop}, {0x0060, 0xffe0, op_bloop},
  {0x1000, 0xff00, op_loopi}, {0x1100, 0xff00, op_bloopi}, {0x1800, 0xff80, op_lrr},
  {0x1880, 0xff80, op_lrrd}, {0x1900, 0xff80, op_lrri}, {0x1980, 0xff80, op_lrrn},
  {0x1a00, 0xff80, op_srr}, {0x1a80, 0xff80, op_srrd}, {0x1b00, 0xff80, op_srri},
  {0x1b80, 0xff80, op_srrn}, {0x2000, 0xf800, op_lrs}, {0x2800, 0xfe00, op_srsh},
  {0x2c00, 0xfc00, op_srs}, {0x3000, 0xfc80, op_xorr}, {0x3400, 0xfc80, op_andr},
  {0x3800, 0xfc80, op_orr}, {0x3c00, 0xfe80, op_andc}, {0x3e00, 0xfe80, op_orc},
  {0x3080, 0xfe80, op_xorc}, {0x3280, 0xfe80, op_notc}, {0x3480, 0xfc80, op_lsrnrx},
  {0x3880, 0xfc80, op_asrnrx}, {0x3c80, 0xfe80, op_lsrnr}, {0x3e80, 0xfe80, op_asrnr},
  {0x4000, 0xf800, op_addr}, {0x4800, 0xfc00, op_addax}, {0x4c00, 0xfe00, op_add},
  {0x4e00, 0xfe00, op_addp}, {0x5000, 0xf800, op_subr}, {0x5800, 0xfc00, op_subax},
  {0x5c00, 0xfe00, op_sub}, {0x5e00, 0xfe00, op_subp}, {0x6000, 0xf800, op_movr},
  {0x6800, 0xfc00, op_movax}, {0x6c00, 0xfe00, op_mov}, {0x6e00, 0xfe00, op_movp},
  {0x7000, 0xfc00, op_addaxl}, {0x7400, 0xfe00, op_incm}, {0x7600, 0xfe00, op_inc},
  {0x7800, 0xfe00, op_decm}, {0x7a00, 0xfe00, op_dec}, {0x7c00, 0xfe00, op_neg},
  {0x7e00, 0xfe00, op_movnp}, {0x8000, 0xf700, op_nx}, {0x8100, 0xf700, op_clr},
  {0x8200, 0xff00, op_cmp}, {0x8300, 0xff00, op_mulaxh}, {0x8400, 0xff00, op_clrp},
  {0x8500, 0xff00, op_tstprod}, {0x8600, 0xfe00, op_tstaxh}, {0x8a00, 0xff00, op_srbith},
  {0x8b00, 0xff00, op_srbith}, {0x8c00, 0xff00, op_srbith}, {0x8d00, 0xff00, op_srbith},
  {0x8e00, 0xff00, op_srbith}, {0x8f00, 0xff00, op_srbith}, {0x9000, 0xf700, op_mul},
  {0x9100, 0xf700, op_asr16}, {0x9200, 0xf600, op_mulmvz}, {0x9400, 0xf600, op_mulac},
  {0x9600, 0xf600, op_mulmv}, {0xa000, 0xe700, op_mulx}, {0xa100, 0xf700, op_abs},
  {0xa200, 0xe600, op_mulxmvz}, {0xa400, 0xe600, op_mulxac}, {0xa600, 0xe600, op_mulxmv},
  {0xb100, 0xf700, op_tst}, {0xc000, 0xe700, op_mulc}, {0xc100, 0xe700, op_cmpaxh},
  {0xc200, 0xe600, op_mulcmvz}, {0xc400, 0xe600, op_mulcac}, {0xc600, 0xe600, op_mulcmv},
  {0xe000, 0xfc00, op_maddx}, {0xe400, 0xfc00, op_msubx}, {0xe800, 0xfc00, op_maddc},
  {0xec00, 0xfc00, op_msubc}, {0xf000, 0xfe00, op_lsl16}, {0xf200, 0xfe00, op_madd},
  {0xf400, 0xfe00, op_lsr16}, {0xf600, 0xfe00, op_msub}, {0xf800, 0xfc00, op_addpaxz},
  {0xfc00, 0xfe00, op_clrl}, {0xfe00, 0xfe00, op_movpz},
};

static const struct { uint16_t opcode, mask; DspOp function; } ext_ops[] = {
  {0x0000, 0x00fc, ext_nop_ext}, {0x0004, 0x00fc, ext_dr}, {0x0008, 0x00fc, ext_ir},
  {0x000c, 0x00fc, ext_nr}, {0x0010, 0x00f0, ext_mv}, {0x0020, 0x00e4, ext_s},
  {0x0024, 0x00e4, ext_sn}, {0x0040, 0x00c4, ext_l}, {0x0044, 0x00c4, ext_ln},
  {0x0080, 0x00ce, ext_ls}, {0x0082, 0x00ce, ext_sl}, {0x0084, 0x00ce, ext_lsn},
  {0x0086, 0x00ce, ext_sln}, {0x0088, 0x00ce, ext_lsm}, {0x008a, 0x00ce, ext_slm},
  {0x008c, 0x00ce, ext_lsnm}, {0x008e, 0x00ce, ext_slnm}, {0x00c3, 0x00cf, ext_ldax},
  {0x00c7, 0x00cf, ext_ldaxn}, {0x00cb, 0x00cf, ext_ldaxm}, {0x00cf, 0x00cf, ext_ldaxnm},
  {0x00c0, 0x00cc, ext_ld}, {0x00c4, 0x00cc, ext_ldn}, {0x00c8, 0x00cc, ext_ldm},
  {0x00cc, 0x00cc, ext_ldnm},
};

#define TEMPLATE_COUNT (sizeof templates / sizeof templates[0])
#define NO_TEMPLATE 0xff
static uint8_t template_of[0x10000], op_of[0x10000];
static DspOp ext_table[0x100];
static int tables_ready;

void dsp_lle_tables_start(void) {
  if (tables_ready) return;
  for (uint32_t i = 0; i < 0x100; i++) {
    ext_table[i] = op_nop;
    for (uint32_t k = 0; k < sizeof ext_ops / sizeof ext_ops[0]; k++) {
      if ((i & ext_ops[k].mask) == ext_ops[k].opcode) {
        ext_table[i] = ext_ops[k].function;
        break;
      }
    }
  }
  for (uint32_t i = 0; i < 0x10000; i++) {
    template_of[i] = NO_TEMPLATE;
    for (uint32_t k = 0; k < TEMPLATE_COUNT; k++) {
      if ((i & templates[k].mask) == templates[k].opcode) {
        template_of[i] = (uint8_t)k;
        break;
      }
    }
    op_of[i] = 0xff;
    for (uint32_t k = 0; k < sizeof ops / sizeof ops[0]; k++) {
      if ((i & ops[k].mask) == ops[k].opcode) {
        op_of[i] = (uint8_t)k;
        break;
      }
    }
  }
  tables_ready = 1;
}

uint8_t dsp_lle_op_size(uint16_t opc) {
  uint8_t t = template_of[opc];
  return t == NO_TEMPLATE ? 1 : templates[t].size;
}

void dsp_lle_execute(uint16_t opc) {
  uint8_t t = template_of[opc];
  int extended = t != NO_TEMPLATE && templates[t].extended;
  if (extended) ext_table[(opc >> 12) == 3 ? opc & 0x7f : opc & 0xff](opc);  // 0x3xxx: 7 bits.
  uint8_t k = op_of[opc];
  if (k == 0xff) op_nop(opc);
  else ops[k].function(opc);
  if (extended) apply_write_back_log();
}
