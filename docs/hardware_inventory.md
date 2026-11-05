# Wii hardware inventory

Every part of the Wii, and how much of it `recomp/` models. Dolphin's source (`master`, read 2026-11-02) and WiiBrew were the map. The wording here is our own.

This came from reading the code. `done` means the code reads correctly, not that a game has proved it. Paths are under `recomp/`.

First written 2026-11-02. Checked again against `main` at b76cc05 (2026-11-03). Of the 201 rows that were `missing`, 69 turned out to be done or partly done, and 20 are now `on demand`. Rows that were `partial` were only checked where later work touched them. Line numbers drift.

Every row has an ID, so the runtime can name the exact row when a game asks for something we don't do.

Statuses:

- `done`: behaves as a game needs.
- `partial`: works in part. The note says what's missing.
- `missing`: no code, a silent no-op, or an error answer where a Wii would act.
- `on demand`: real hardware few or no titles use. Built when a tested title needs it.
- `not needed`: the note says why.
- `unknown`: couldn't be settled by reading.

## Summary

| Area | Done | Partial | Missing | Unknown | On demand | Not needed | Total |
|---|---|---|---|---|---|---|---|
| 1. CPU | 73 | 21 | 5 | 1 | 4 | 16 | 120 |
| 2. Graphics commands and registers | 65 | 22 | 4 | 0 | 9 | 10 | 110 |
| 3. TEV and pixel features | 51 | 17 | 3 | 0 | 4 | 1 | 76 |
| 4. Vertex and texture | 50 | 10 | 0 | 3 | 1 | 2 | 66 |
| 5. Hardware blocks | 25 | 12 | 6 | 0 | 6 | 16 | 65 |
| 6. Audio microcodes | 9 | 3 | 7 | 0 | 0 | 3 | 22 |
| 7. IOS | 33 | 16 | 3 | 0 | 7 | 6 | 65 |
| 8. Controllers | 34 | 16 | 7 | 0 | 6 | 2 | 65 |
| **All** | **340** | **117** | **35** | **4** | **37** | **56** | **589** |

## Biggest gaps

Ordered by how likely a real game is to hit them.

1. **`.rel` modules aren't translated ahead of time** (`cpu.code_loaded_at_run_time_rel_oslink`). Only `.rso` modules are. A `.rel` runs under the interpreter until the next build translates what it reached. Many commercial games keep most of their code in `.rel` modules.
2. **Disc images are read only in this project's own format** (`hw.disc_image_files_iso_wbfs_rvz`). ISO, WBFS and RVZ need extracting first.
3. **ES and DI answer errors to much of what a Wii does** (`ios.identify_ticket_tmd_verify`, `ios.getlength_seek_readdvd`, `ios.openpartition_with_tmd_0x93_0x94`, `ios.encrypt_decrypt`, `ios.launch_return_to_menu_start_title`). Launching works only between the Wii Menu and a disc.
4. **Only one audio program exists** (`audio.*`). AX in its newer command format. Zelda-family games, older AX and GameCube AX are silent.
5. **`setjmp`, C++ exceptions and fibers are at risk** (`cpu.bclr`). `blr` is a host `return`, so code that switches stacks leaves the host stack out of step.
6. **Self-modifying code** (`cpu.self_modifying_code_icbi`). `icbi` is a no-op, so translated code is never replaced.
7. **Video interface registers are only stored** (`hw.*` in VI). No beam position, interlace, PAL or 480p modes.
8. **No GameCube controllers** (`hw.gc_controller_wheel_keyboard_dance_mat`). Every port answers that nothing is plugged in.
9. **Wii Remote motion is fixed at rest, and there's no Nunchuk, MotionPlus, speaker or second remote** (`ctl.*`). Covered by the controllers stage of `ROADMAP.md`.
10. **Floating-point status flags aren't worked out from arithmetic** (`cpu.status_flags_fprf_fr_fi_fx_and_others`). Only the game's own writes change them.
11. **No guest exceptions or memory mapping** (`cpu.program_trap_illegal`, `cpu.dsi_isi`, `cpu.game_defined_bat_maps`, `cpu.page_tables_segment_registers`).
12. **Copy filters are partly followed** (`pixel.copy_*`, `pixel.gamma_on_xfb_copy_1_0_1_7_2_2`). Copies in memory follow the vertical filter, its clamps, gamma and YUV, but the card's copy drawn as a texture has none of them, and Y scale, anti-aliasing and field copies are missing.
13. **The bounding box needs Vulkan** (`pixel.bounding_box`). OpenGL has no shaders that write memory, so it keeps the values the game set.

## Regressions

Since 7691d88 ("stop faking success"), STM shutdown, reset and idle answer an error (`ios.dev_stm_immediate`). The other two found then, DI's cover status and the stop past 64 queued replies, are fixed.

## 1. CPU

Dolphin lists 223 instruction handlers in `Interpreter_Tables.cpp`. Every one is decoded and translated now, and the interpreter (`runtime/interp.c`) has the same set. `test/test_recomp_differential.py` runs each instruction both ways and compares. An unknown opcode stops the program (`runtime/dispatch.c:14`).

### Integer

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.addi_addis_addic_addic_subfic_mulli` | `addi addis addic addic. subfic mulli` | done | `translate/emit.py:200-211` |  |
| `cpu.add_subf_neg_mullw_mulhw_mulhwu` | `add subf neg mullw mulhw mulhwu` | done | `translate/emit.py:269-282` | Rc sets CR0. |
| `cpu.carry_forms_addc_adde_addze_addme_subfc` | Carry forms `addc adde addze addme subfc subfe subfze subfme` | done | `translate/emit.py:276-279`, `runtime/cpu.h:124` |  |
| `cpu.overflow_forms_addo_and_others_oe_1` | Overflow forms (`addo` and others, OE=1) | done | `translate/emit.py:215`, `runtime/cpu.h:151` | OV and SO are set. |
| `cpu.divw` | `divw` | done | `runtime/cpu.h:172` | Dolphin's results for divide by zero and overflow. |
| `cpu.divwu` | `divwu` | done | `translate/emit.py:275` |  |
| `cpu.cmp_cmpi_cmpl_cmpli` | `cmp cmpi cmpl cmpli` | done | `translate/emit.py:212-219` |  |
| `cpu.and_or_xor_andc_orc_nor_nand_eqv_immedia` | `and or xor andc orc nor nand eqv`, immediates | done | `translate/emit.py:220-224`, `:283-293` |  |
| `cpu.extsb_extsh_cntlzw` | `extsb extsh cntlzw` | done | `translate/emit.py:289-290` |  |
| `cpu.rlwinm_rlwnm_rlwimi` | `rlwinm rlwnm rlwimi` | done | `translate/emit.py:225-234` |  |
| `cpu.slw_srw_sraw_srawi` | `slw srw sraw srawi` | done | `translate/emit.py:287-302` |  |
| `cpu.crand_crandc_creqv_crnand_crnor_cror_cro` | `crand crandc creqv crnand crnor cror crorc crxor mcrf` | done | `translate/emit.py:332-339` |  |
| `cpu.mfcr_mtcrf` | `mfcr mtcrf` | done | `translate/emit.py:327-331` |  |
| `cpu.mcrxr` | `mcrxr` | done | `translate/emit.py:334` | OV is kept in XER. |

### Branches and system

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.b_bl_bc_bcl` | `b bl bc bcl` | done | `translate/emit.py:448-459` |  |
| `cpu.bclr` | `bclr` | partial | `translate/emit.py:460-465` | `blr` is a host `return`, assuming calls and returns pair up. Code that sets LR by hand or switches stacks goes wrong: `setjmp`, C++ unwinding, `OSSwitchFiber`. |
| `cpu.bcctr_bcctrl` | `bcctr bcctrl` | done | `translate/emit.py:466-471` | Target needs a function entry. `functions.py:73-104` finds them. |
| `cpu.jump_tables_and_vtables_built_at_run_tim` | Jump tables and vtables built at run time | partial | `translate/functions.py:122` | Needs `--all-code`. |
| `cpu.rfi` | `rfi` | partial | `translate/emit.py:474` | Copies SRR1 whole. Exception entry and return is handled by `threads.c`, not by this path. |
| `cpu.sc` | `sc` | done | `runtime/processor.c` | A handler that only syncs, as the SDK installs at 0xC00, has nothing to do here, which is checked on every call. Any other handler is reported. |
| `cpu.tw_twi` | `tw twi` | done | `translate/emit.py:228`, `runtime/interp.c:57`, `runtime/processor.c:43` | Conditions are evaluated and a trap that fires is reported. It isn't delivered, as a retail handler only reports an assert and halts. |
| `cpu.code_loaded_at_run_time_rel_oslink` | Code loaded at run time (`.rel`, `OSLink`) | partial | `translate/modules.py`, `runtime/modules.c:79`, `runtime/dispatch.c:81-97` | `.rso` modules are translated ahead of time and bound after the game links them. `.rel` runs under the interpreter, and the addresses it reaches are translated on the next build. |
| `cpu.self_modifying_code_icbi` | Self-modifying code, `icbi` | missing | `translate/emit.py:111` | `icbi` is a no-op, so translated code is never invalidated. The interpreter reads memory fresh. |
| `cpu.unknown_instruction_in_reachable_code` | Unknown instruction in reachable code | done | `runtime/dispatch.c:14` | Stops with its name, which is the right failure. |

### Loads and stores

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.lbz_lhz_lha_lwz_and_u_x_ux_forms` | `lbz lhz lha lwz` and `u x ux` forms | done | `translate/emit.py:235-246` |  |
| `cpu.stb_sth_stw_and_u_x_ux_forms` | `stb sth stw` and `u x ux` forms | done | `translate/emit.py:239-250` |  |
| `cpu.lmw_stmw` | `lmw stmw` | done | `translate/emit.py:251-254` |  |
| `cpu.lhbrx_lwbrx_sthbrx_stwbrx` | `lhbrx lwbrx sthbrx stwbrx` | done | `translate/emit.py:255-262` |  |
| `cpu.lwarx_stwcx` | `lwarx stwcx.` | done | `translate/emit.py:263-266` | No reservation and always succeeds. Fine on one thread, as interrupts only land at polls. |
| `cpu.lswi` | `lswi` | done | `translate/emit.py:205`, `:288` | Decoded, not emitted. |
| `cpu.lswx` | `lswx` | done | `translate/decode.py:42`, `translate/emit.py:292` | Count from XER. |
| `cpu.stswi_stswx` | `stswi stswx` | done | `translate/emit.py:290`, `:294` | Decoded, not emitted. |
| `cpu.eciwx` | `eciwx` | done | `translate/emit.py:296` | Plain word load. EAR isn't checked. |
| `cpu.ecowx` | `ecowx` | done | `translate/decode.py:41`, `translate/emit.py:298` | Plain word store. EAR isn't checked. |
| `cpu.misaligned_access` | Misaligned access | done | `runtime/cpu.h:67-80` | `memcpy`, so works. No alignment exception. |

### Cache instructions

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.dcbf_dcbst_dcbt_dcbtst` | `dcbf dcbst dcbt dcbtst` | not needed | `translate/emit.py:102` | Memory is flat, with no cache to manage. |
| `cpu.dcbi` | `dcbi` | not needed | `translate/emit.py:102` | Real one drops unwritten data. No game should rely on that. |
| `cpu.dcbz` | `dcbz` | done | `translate/emit.py:340` |  |
| `cpu.dcbz_l` | `dcbz_l` | done | `translate/emit.py:340` | Zeroes through normal writes, so also reaches the locked cache. |
| `cpu.dcba` | `dcba` | done | `translate/decode.py:44`, `translate/emit.py:111` | A no-op, which no program can tell apart. |
| `cpu.icbi` | `icbi` | not needed | `translate/emit.py:102` | Only matters with generated code, listed above. |
| `cpu.sync_isync_eieio` | `sync isync eieio` | not needed | `translate/emit.py:102` | One host thread runs guest code. |

### Scalar floats

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.lfd_lfdu_lfdx_lfdux` | `lfd lfdu lfdx lfdux` | done | `translate/emit.py:107-116` |  |
| `cpu.stfd_stfdu_stfdx_stfdux` | `stfd stfdu stfdx stfdux` | done | `translate/emit.py:119-123` |  |
| `cpu.lfs_lfsu_lfsx_lfsux` | `lfs lfsu lfsx lfsux` | done | `runtime/cpu.h:246-253` | Signaling NaNs keep their bits, as Dolphin converts them. |
| `cpu.stfs_stfsu_stfsx_stfsux` | `stfs stfsu stfsx stfsux` | done | `runtime/cpu.h:255-263` | NaNs rearranged by hand; other values through a cast. |
| `cpu.stfiwx` | `stfiwx` | done | `translate/emit.py:359` |  |
| `cpu.fadd_fsub_fmul_fdiv` | `fadd fsub fmul fdiv` | partial | `translate/emit.py:126-141` | Results right. No flags and no Rc. |
| `cpu.fmadd_fmsub_fnmadd_fnmsub` | `fmadd fmsub fnmadd fnmsub` | partial | `translate/emit.py:135-136` | Not fused (`-ffp-contract=off`), so the last bit can differ. No flags. |
| `cpu.fadds_fsubs_fmuls_fdivs` | `fadds fsubs fmuls fdivs` | partial | `translate/emit.py:129-140` | Double then cast to float. No 25-bit rounding of the multiplier (Dolphin's `Force25Bit`), so low bits can differ. Writes both lanes, as hardware does. |
| `cpu.fmadds_fmsubs_fnmadds_fnmsubs` | `fmadds fmsubs fnmadds fnmsubs` | partial | `translate/emit.py:129-140` | As above. |
| `cpu.fres` | `fres` | partial | `translate/emit.py:365` | Exact, where hardware uses a table estimate. Doesn't fill ps1, where Dolphin fills both. |
| `cpu.frsqrte` | `frsqrte` | partial | `translate/emit.py:366` | Exact, not the estimate. Only ps0 is written, which matches Dolphin. |
| `cpu.frsp` | `frsp` | partial | `translate/emit.py:365` | Doesn't fill ps1, where Dolphin fills both. |
| `cpu.fsel` | `fsel` | done | `translate/emit.py:367` | Negative zero counts as not negative, as on hardware. No Rc. |
| `cpu.fmr_fneg_fabs_fnabs` | `fmr fneg fabs fnabs` | done | `translate/emit.py:364` | No Rc. |
| `cpu.fctiw` | `fctiw` | done | `translate/emit.py:422-423` | Honours RN, and writes the 0xfff8 marker in the top word. |
| `cpu.fctiwz` | `fctiwz` | done | `translate/emit.py:372` | Saturates through `to_int`. Top word as above. |
| `cpu.fcmpu_fcmpo` | `fcmpu fcmpo` | partial | `translate/emit.py:374`, `runtime/cpu.h:111` | CR right. FPCC and invalid flags aren't set. |
| `cpu.fsqrt_fsqrts` | `fsqrt fsqrts` | not needed |  | Not in Gekko or in Dolphin's table. |

### FPSCR and rounding

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.mffs` | `mffs` | partial | `translate/emit.py:376` | Returns the stored value. Arithmetic never updates the status bits. |
| `cpu.mtfsf` | `mtfsf` | done | `translate/emit.py:378` | Field mask and Rc respected. FX, VX and FEX follow Dolphin. |
| `cpu.mtfsb0_mtfsb1_mtfsfi_mcrfs` | `mtfsb0 mtfsb1 mtfsfi mcrfs` | done | `translate/emit.py:431-438`, `runtime/cpu.h:202-222` | FX, VX and FEX follow Dolphin. `mcrfs` clears the exception bits it reads. |
| `cpu.rounding_mode_rn` | Rounding mode (RN) | done | `runtime/processor.c`, `runtime/cpu.h` (`fpscr_write`) | Applied to the host's floating point whenever it changes, as Dolphin does (SetSIMDMode). |
| `cpu.non_ieee_mode_ni` | Non-IEEE mode (NI) | done | `runtime/processor.c` | Flush to zero on the host, as Dolphin does: FTZ on x86, FPCR.FZ on arm64. |
| `cpu.status_flags_fprf_fr_fi_fx_and_others` | Status flags (FPRF, FR, FI, FX and others) | missing | `runtime/cpu.h:185` | Not worked out from arithmetic. Only the game's own writes change them. |
| `cpu.rc_on_float_instructions_cr1` | Rc on float instructions (CR1) | partial | `translate/emit.py:85`, `runtime/interp.c:64` | Copies FPSCR's top 4 bits, which arithmetic never updates. |

### Paired singles

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.ps_add_ps_sub_ps_mul_ps_div` | `ps_add ps_sub ps_mul ps_div` | partial | `translate/emit.py:144-159` | Rounded to float per lane. No 25-bit rule, flags or Rc. |
| `cpu.ps_madd_ps_msub_ps_nmadd_ps_nmsub` | `ps_madd ps_msub ps_nmadd ps_nmsub` | partial | `translate/emit.py:144-159` | As above. |
| `cpu.ps_res_ps_rsqrte` | `ps_res ps_rsqrte` | partial | `translate/emit.py:148-149` | Exact, not the estimate. Both lanes written. |
| `cpu.ps_sum0_ps_sum1` | `ps_sum0 ps_sum1` | done | `translate/emit.py:169-170` |  |
| `cpu.ps_muls0_ps_muls1_ps_madds0_ps_madds1` | `ps_muls0 ps_muls1 ps_madds0 ps_madds1` | done | `translate/emit.py:171-174` | Lanes checked against Dolphin. |
| `cpu.ps_merge00_01_10_11` | `ps_merge00 01 10 11` | done | `translate/emit.py:165-168` |  |
| `cpu.ps_mr_ps_neg_ps_abs_ps_nabs` | `ps_mr ps_neg ps_abs ps_nabs` | done | `translate/emit.py:161-164` |  |
| `cpu.ps_sel` | `ps_sel` | done | `translate/emit.py:175` |  |
| `cpu.ps_cmpu0_ps_cmpo0_ps_cmpu1_ps_cmpo1` | `ps_cmpu0 ps_cmpo0 ps_cmpu1 ps_cmpo1` | done | `translate/emit.py:180-182` | CR only. |
| `cpu.psq_l_psq_lu_psq_lx_psq_lux` | `psq_l psq_lu psq_lx psq_lux` | done | `translate/emit.py:186-193`, `:384-392`, `runtime/quantized.c:39` | u8, s8, u16, s16 and float, with scale. |
| `cpu.psq_st_psq_stu_psq_stx_psq_stux` | `psq_st psq_stu psq_stx psq_stux` | done | `runtime/quantized.c:46` | Clamps. Truncates, as Dolphin does. |
| `cpu.gqr_registers` | GQR registers | done | `runtime/quantized.c:40`, `:47` | Saved with thread contexts. |
| `cpu.hid2_gates_paired_singles_enable_quantiz` | HID2 gates (paired singles enable, quantize enable) | not needed |  | Always on. A game that relies on the fault wouldn't be normal. |

### Special registers

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.lr_ctr` | LR, CTR | done | `translate/emit.py:305`, `:312-315` |  |
| `cpu.xer` | XER | done | `translate/emit.py:305`, `:316` | SO, OV, CA and the byte count. |
| `cpu.cr` | CR | done | `runtime/cpu.h:11` |  |
| `cpu.msr` | MSR | partial | `translate/emit.py:323-326` | Stored. Only EE has any effect (`interrupts.c:94`). |
| `cpu.srr0_srr1` | SRR0, SRR1 | done | `runtime/threads.c:116-117` |  |
| `cpu.sprg0_3` | SPRG0-3 | done | `runtime/cpu.h:13` | Plain storage. |
| `cpu.dec` | DEC | done | `runtime/interrupts.c:71-76` | Runs on host time. |
| `cpu.tbl_tbu_reads_mftb` | TBL, TBU reads, `mftb` | done | `translate/emit.py:306`, `:321` |  |
| `cpu.tbl_tbu_writes_spr_284_285` | TBL, TBU writes (SPR 284, 285) | done | `translate/emit.py:366`, `runtime/interrupts.c:76` | Sets the guest clock. |
| `cpu.hid0_hid1_hid2_hid4` | HID0, HID1, HID2, HID4 | partial | `translate/emit.py:320` | Stored, no effect. |
| `cpu.wpar` | WPAR | partial | `translate/emit.py:320` | Stored. The pipe works off its fixed address (`gx_fifo.c:430`). A pipe at another address isn't modelled. |
| `cpu.dmau_dmal` | DMAU, DMAL | done | `translate/emit.py:318`, `runtime/hardware.c:127` | Transfers finish at once. |
| `cpu.pvr_ecid` | PVR, ECID | unknown | `translate/emit.py:307` | Nothing sets them, so they read 0. Whether a game cares isn't known. |
| `cpu.l2cr_thrm_ictc` | L2CR, THRM, ICTC | not needed |  | Stored. No cache or thermal model. |
| `cpu.pmc_mmcr_ummcr` | PMC, MMCR, UMMCR | on demand | `translate/emit.py:368` | Stored only. Performance monitors, used by profilers rather than game logic. |
| `cpu.iabr_dabr` | IABR, DABR | not needed |  | Debug only. |
| `cpu.ear` | EAR | on demand |  | Stored only. Only `eciwx`/`ecowx` use it. |
| `cpu.dar_dsisr` | DAR, DSISR | not needed | `translate/emit.py:320` | Only a DSI sets them, which is `cpu.dsi_isi`. Game writes keep their values. |
| `cpu.sdr1_bat_registers` | SDR1, BAT registers | not needed | `translate/emit.py:320` | Stored, ignored. See below. |

### MMU and exceptions

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.standard_map_0x8_0x9_0xc_0xd` | Standard map (0x8, 0x9, 0xC, 0xD) | done | `runtime/guest_memory.h`, `runtime/cpu.h` | Memory is one window at a fixed host address, indexed by the address with its top two bits cleared, so cached and uncached views are the same bytes. One compare sends the hardware block to `hw_read`. |
| `cpu.game_defined_bat_maps` | Game-defined BAT maps | missing | `runtime/cpu.h:61` | Memory is the fixed retail map. |
| `cpu.page_tables_segment_registers` | Page tables, segment registers | partial | `translate/emit.py:439-446`, `runtime/cpu.h:15` | `mtsr`, `mfsr`, `mtsrin`, `mfsrin` keep their values. No address goes through them. |
| `cpu.tlbie_tlbsync` | `tlbie tlbsync` | not needed | `translate/emit.py:102` | No TLB. |
| `cpu.real_mode_entry` | Real-mode entry | not needed | `translate/system_functions.json` | Replaced by signature. |
| `cpu.mem1_24_mb_mem2_64_mb` | MEM1 24 MB, MEM2 64 MB | done | `runtime/guest_memory.h` | At 0 and 0x10000000 in the window. |
| `cpu.external_interrupt` | External interrupt | done | `runtime/interrupts.c:95-97` | Polled every 2,000 checks. |
| `cpu.decrementer` | Decrementer | done | `runtime/interrupts.c:98-102` |  |
| `cpu.system_call` | System call | done | `runtime/processor.c` | See `cpu.sc`. |
| `cpu.program_trap_illegal` | Program (trap, illegal) | missing | `runtime/interp.c:56` | An unknown opcode stops the program. |
| `cpu.floating_point_unavailable` | Floating-point unavailable | not needed |  | Floats are always on. |
| `cpu.floating_point_program` | Floating-point program | on demand | `runtime/processor.c:48`, `runtime/interrupts.c:106` | Arithmetic never sets FPSCR's exception bits. Reported once if a game turns exceptions on in both MSR and FPSCR. |
| `cpu.dsi_isi` | DSI, ISI | missing | `runtime/guest_memory.c`, `runtime/interp.c:536` | No exception is raised. A data access where a Wii has no memory opens a zero page there and is named with this ID and its address. Bad code addresses stop with exit 4. |
| `cpu.alignment` | Alignment | not needed |  | Accesses just work. |
| `cpu.machine_check_reset` | Machine check, reset | on demand |  | Not raised during normal play. |
| `cpu.performance_monitor_thermal_breakpoint` | Performance monitor, thermal, breakpoint | not needed |  |  |
| `cpu.thread_contexts` | Thread contexts | done | `runtime/threads.c` | Replaces `OSSaveContext` and `OSLoadContext`. At most 256 (`threads.c:18`). |

### Locked cache

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `cpu.16_kb_at_0xe0000000` | 16 KB at 0xE0000000 | done | `runtime/guest_memory.h`, `runtime/hardware.c:182` | In the memory window, so as quick as memory. |
| `cpu.enable_bit_hid2_lce` | Enable bit (HID2 LCE) | not needed |  | Always on. |
| `cpu.dma_between_memory_and_locked_cache` | DMA between memory and locked cache | done | `runtime/hardware.c:127-139` |  |
| `cpu.dma_queue_length_in_hid2` | DMA queue length in HID2 | done |  | Always reads 0, which is right as DMA finishes at once. |

## 2. Graphics commands and registers

### FIFO commands

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `gx.nop_0x00` | NOP 0x00 | done | `runtime/gx_fifo.c:138` |  |
| `gx.load_cp_0x08` | Load CP 0x08 | done | `runtime/gx_fifo.c:144` |  |
| `gx.load_xf_0x10` | Load XF 0x10 | done | `runtime/gx_fifo.c:149` |  |
| `gx.indexed_xf_a_d_0x20_0x38` | Indexed XF A-D 0x20-0x38 | done | `runtime/gx_fifo.c:128`, `:156` | Read from memory at parse time. On Linux that's on another thread, so it can run later than the CPU expects. |
| `gx.call_display_list_0x40` | Call display list 0x40 | done | `runtime/gx_fifo.c:161` | No depth limit. |
| `gx.metrics_0x44` | Metrics 0x44 | done | `runtime/gx_fifo.c:176` | Read as a one-byte command, as Dolphin reads it. The profiler counters it starts are `gx.cp_performance_counters`. |
| `gx.invalidate_vertex_cache_0x48` | Invalidate vertex cache 0x48 | not needed | `runtime/gx_fifo.c:138` | No cache. |
| `gx.load_bp_0x61` | Load BP 0x61 | done | `runtime/gx_fifo.c:139` | Masks handled at `:82-88`. |
| `gx.draw_quads_quads2_triangles_strip_fan` | Draw quads, quads2, triangles, strip, fan | done | `runtime/gx_fifo.c:167`, `gx_draw.c:151` |  |
| `gx.draw_lines_line_strip` | Draw lines, line strip | partial | `runtime/gx_draw.c:377-423`, `runtime/gx_fifo.c:176-178` | Drawn as quads with the set width. Texture offsets and aspect adjustment are only reported (`gx_draw.c:367-375`). |
| `gx.draw_points` | Draw points | partial | `runtime/gx_draw.c:377-423` | Drawn as quads with the set size. Texture offsets only reported. |
| `gx.draw_larger_than_the_4_mb_stream` | Draw larger than the 4 MB stream | done | `runtime/gx_fifo.c:48-53` | The stream holds 9 MiB, enough for the largest possible draw, and a static assert keeps it so. |

### CP registers

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `gx.0x20_performance_select` | 0x20 performance select | on demand | `runtime/gx_fifo.c:146` | Performance counters, read by profiling tools rather than game logic. |
| `gx.0x30_0x40_matrix_index` | 0x30, 0x40 matrix index | done | `runtime/gx_vertex.c:145` | We use the CP copy. Dolphin warns when CP and XF differ, and we'd follow CP. |
| `gx.0x50_0x60_vertex_descriptor` | 0x50, 0x60 vertex descriptor | done | `runtime/gx_vertex.c:29` |  |
| `gx.0x70_0x77_format_a` | 0x70-0x77 format A | partial | `runtime/gx_vertex.c:40-48` | Byte dequant (bit 30) and normal index 3 (bit 31) are ignored. |
| `gx.0x80_0x87_format_b` | 0x80-0x87 format B | done | `runtime/gx_vertex.c:49-59` |  |
| `gx.0x90_0x97_format_c` | 0x90-0x97 format C | done | `runtime/gx_vertex.c:49-59` |  |
| `gx.0xa0_0xab_array_bases` | 0xA0-0xAB array bases | done | `runtime/gx_vertex.c:137` |  |
| `gx.0xac_0xaf_matrix_array_bases` | 0xAC-0xAF matrix array bases | done | `runtime/gx_fifo.c:131` |  |
| `gx.0xb0_0xbf_strides` | 0xB0-0xBF strides | done | `runtime/gx_vertex.c:137` |  |

### XF memory and registers

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `gx.0x000_0x0ff_position_and_texture_matrice` | 0x000-0x0FF position and texture matrices | done | `runtime/gx_transform.c:132` |  |
| `gx.0x400_0x45f_normal_matrices` | 0x400-0x45F normal matrices | done | `runtime/gx_transform.c:110` | Index isn't masked to 31 as on Dolphin, so an index above 31 reads past the real memory. |
| `gx.0x500_0x5ff_post_transform_matrices` | 0x500-0x5FF post-transform matrices | done | `runtime/gx_transform.c:129` |  |
| `gx.0x600_0x67f_lights` | 0x600-0x67F lights | done | `runtime/gx_light.c:51` |  |
| `gx.0x1000_error_0x1001_diagnostics` | 0x1000 error, 0x1001 diagnostics | not needed |  |  |
| `gx.0x1002_0x1004_state_clock` | 0x1002-0x1004 state, clock | not needed |  |  |
| `gx.0x1005_clip_disable` | 0x1005 clip disable | on demand | `runtime/gx_fifo.c:193` | Only changes geometry outside the view, and Dolphin doesn't follow it either. Reported when set. |
| `gx.0x1006_0x1007_performance` | 0x1006, 0x1007 performance | on demand |  | Performance counters, read by profiling tools rather than game logic. |
| `gx.0x1008_vertex_spec` | 0x1008 vertex spec | not needed |  | The CP descriptor decides. Dolphin also checks they agree. |
| `gx.0x1009_color_count` | 0x1009 color count | done | `runtime/gx_transform.c:98-113` | Channels past the count reach the stages as 0, as in Dolphin, but can still make texture coordinates. |
| `gx.0x100a_b_ambient` | 0x100A-B ambient | done | `runtime/gx_light.c:46` |  |
| `gx.0x100c_d_material` | 0x100C-D material | done | `runtime/gx_light.c:45` |  |
| `gx.0x100e_f_color_channel_control` | 0x100E-F color channel control | done | `runtime/gx_light.c:28-41` |  |
| `gx.0x1010_11_alpha_channel_control` | 0x1010-11 alpha channel control | done | `runtime/gx_light.c:40` |  |
| `gx.0x1012_post_transform_enable` | 0x1012 post-transform enable | done | `runtime/gx_transform.c:58` |  |
| `gx.0x1018_19_matrix_index` | 0x1018-19 matrix index | not needed |  | Written beside the CP copy, which we read. |
| `gx.0x101a_1c_viewport_size` | 0x101A-1C viewport size | done | `runtime/gx_draw.c:135` |  |
| `gx.0x101d_1f_viewport_origin_and_depth` | 0x101D-1F viewport origin and depth | partial | `runtime/gx_draw.c:137` | Assumes origin 342 rather than the scissor offset. Depth range and far are dropped. |
| `gx.0x1020_26_projection` | 0x1020-26 projection | done | `runtime/gx_transform.c:72` |  |
| `gx.0x103f_texture_coordinate_count` | 0x103F texture coordinate count | done | `runtime/gx_transform.c:58` |  |
| `gx.0x1040_47_texture_matrix_info` | 0x1040-47 texture matrix info | partial | `runtime/gx_transform.c:29-42` | Input form (bit 2) and emboss fields are ignored. |
| `gx.0x1050_57_post_matrix_info` | 0x1050-57 post matrix info | done | `runtime/gx_transform.c:41`, `:127` |  |

### BP registers

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `gx.0x00_gen_mode` | 0x00 gen mode | partial | `runtime/gx_tev.c:182-183`, `gx_draw.c:125` | Stages, indirect count, cull. Texture and color counts, multisample and z-freeze ignored. |
| `gx.0x01_0x04_copy_filter_samples` | 0x01-0x04 copy filter samples | missing |  |  |
| `gx.0x06_0x0e_indirect_matrices` | 0x06-0x0E indirect matrices | done | `runtime/gx_tev.c:140` |  |
| `gx.0x0f_indirect_texture_mask` | 0x0F indirect texture mask | not needed |  | Cache hint. Dolphin only stores it. |
| `gx.0x10_0x1f_indirect_stage_commands` | 0x10-0x1F indirect stage commands | partial | `runtime/gx_tev.c:66-85` | See area 3. |
| `gx.0x20_0x21_scissor` | 0x20-0x21 scissor | done | `runtime/gx_draw.c:142` |  |
| `gx.0x22_line_and_point_width` | 0x22 line and point width | done | `runtime/gx_draw.c:387-393` | No lines or points. |
| `gx.0x23_0x24_performance` | 0x23-0x24 performance | on demand |  | Performance counters, read by profiling tools rather than game logic. |
| `gx.0x25_0x26_indirect_scale` | 0x25-0x26 indirect scale | done | `runtime/gx_tev.c:79` |  |
| `gx.0x27_indirect_sources` | 0x27 indirect sources | done | `runtime/gx_tev.c:79` |  |
| `gx.0x28_0x2f_stage_order` | 0x28-0x2F stage order | done | `runtime/gx_tev.c:93` |  |
| `gx.0x30_0x3f_texture_coordinate_scale_and_b` | 0x30-0x3F texture coordinate scale and bias | partial | `runtime/gx_tev.c:297-299` | Used for indirect stages. Cylindrical wrap and line offsets aren't. |
| `gx.0x40_z_mode` | 0x40 z mode | done | `runtime/gx_draw.c:120-122` |  |
| `gx.0x41_blend_mode` | 0x41 blend mode | partial | `runtime/gx_draw.c:103-124` | Logic op and dither ignored. |
| `gx.0x42_constant_alpha` | 0x42 constant alpha | done | `runtime/gx_tev.c:199` |  |
| `gx.0x43_pixel_format_and_z_control` | 0x43 pixel format and z control | partial | `runtime/gx_fifo.c:123` | Only "is depth format". Early z, z format and format conversion ignored. |
| `gx.0x44_field_mask` | 0x44 field mask | missing |  |  |
| `gx.0x45_draw_done` | 0x45 draw done | done | `runtime/gx_fifo.c:93` | Delayed signal. |
| `gx.0x46_0x69_bus_clock` | 0x46, 0x69 bus clock | not needed |  |  |
| `gx.0x47_token` | 0x47 token | partial | `runtime/gx_fifo.c:97`, `:99` | We raise the signal here. Dolphin raises nothing on 0x47, and stores the token only. |
| `gx.0x48_token_with_interrupt` | 0x48 token with interrupt | partial | `runtime/gx_fifo.c:97` | Value stored, no signal. Dolphin raises here. Works when the SDK writes both, as it writes 0x47 second. A game writing only 0x48 gets no interrupt. |
| `gx.0x49_0x4a_copy_rectangle` | 0x49-0x4A copy rectangle | done | `runtime/gx_copy.c:45` |  |
| `gx.0x4b_copy_target` | 0x4B copy target | done | `runtime/gx_copy.c:60` |  |
| `gx.0x4d_copy_stride` | 0x4D copy stride | done | `runtime/gx_copy.c:316`, `:106` | Rows of tiles land this far apart in memory, and the bytes between them are left alone. |
| `gx.0x4e_copy_y_scale` | 0x4E copy Y scale | missing |  | Ignored. PAL and field copies get the wrong size. |
| `gx.0x4f_0x51_clear_color_and_depth` | 0x4F-0x51 clear color and depth | done | `runtime/gx_draw.c:202-209` |  |
| `gx.0x52_trigger_copy` | 0x52 trigger copy | partial | `runtime/gx_fifo.c:106-125` | See area 3. |
| `gx.0x53_0x54_vertical_copy_filter` | 0x53-0x54 vertical copy filter | partial | `runtime/gx_copy.c:274`, `gx_encode.c:72` | Applied to copies as they land in memory, colour only, with the wrap past 128 Dolphin found. The card's own copy, drawn as a texture, has none, and says so (`pixel.copy_filter_aa_and_deflicker`). |
| `gx.0x55_0x56_bounding_box` | 0x55-0x56 bounding box | partial | `runtime/gx_efb.c:132` | Starts the bounding box widening, on Vulkan. OpenGL has none and names `pixel.bounding_box`. |
| `gx.0x57_clear_pixel_performance` | 0x57 clear pixel performance | on demand |  | Performance counters, read by profiling tools rather than game logic. |
| `gx.0x58_rev_bits` | 0x58 rev bits | not needed |  | Dolphin ignores it too. |
| `gx.0x59_scissor_offset` | 0x59 scissor offset | done | `runtime/gx_draw.c:144` |  |
| `gx.0x60_0x63_texture_preload` | 0x60-0x63 texture preload | on demand | `runtime/gx_fifo.c:111` | Textures are read from main memory. Modelling texture memory's banks is large, and Dolphin calls its own version uncertain. Reported when used. |
| `gx.0x64_tlut_source` | 0x64 TLUT source | done | `runtime/gx_texture.c:21` |  |
| `gx.0x65_tlut_load` | 0x65 TLUT load | partial | `runtime/gx_texture.c:20-25` | Offset unit wrong: `<<5`, should be `<<9`. |
| `gx.0x66_texture_invalidate` | 0x66 texture invalidate | not needed |  | We hash textures instead. |
| `gx.0x67_performance` | 0x67 performance | on demand |  | Performance counters, read by profiling tools rather than game logic. |
| `gx.0x68_field_mode` | 0x68 field mode | missing |  |  |
| `gx.0x80_0x83_0xa0_0xa3_texture_mode_0` | 0x80-0x83, 0xA0-0xA3 texture mode 0 | partial | `runtime/gx_draw.c:66-71` | Wrap and magnify only. |
| `gx.0x84_0x87_0xa4_0xa7_texture_mode_1` | 0x84-0x87, 0xA4-0xA7 texture mode 1 | done | `runtime/gx_draw.c:97-102`, `runtime/gx_gl.c:280-281` | LOD minimum and maximum. The LOD clamp bit is only reported. |
| `gx.0x88_0x8b_0xa8_0xab_image_size_and_forma` | 0x88-0x8B, 0xA8-0xAB image size and format | done | `runtime/gx_texture.c:143` |  |
| `gx.0x8c_0x93_0xac_0xb3_tmem_layout` | 0x8C-0x93, 0xAC-0xB3 TMEM layout | not needed |  | Only matters with preload. |
| `gx.0x94_0x97_0xb4_0xb7_image_address` | 0x94-0x97, 0xB4-0xB7 image address | done | `runtime/gx_texture.c:144` |  |
| `gx.0x98_0x9b_0xb8_0xbb_tlut_setting` | 0x98-0x9B, 0xB8-0xBB TLUT setting | partial | `runtime/gx_texture.c:145`, `:151` | Same offset bug. |
| `gx.0xc0_0xdf_stage_combiners` | 0xC0-0xDF stage combiners | partial | `runtime/gx_tev.c:92-108` | See area 3. |
| `gx.0xe0_0xe7_color_and_konst_registers` | 0xE0-0xE7 color and konst registers | done | `runtime/gx_fifo.c:89` |  |
| `gx.0xe8_0xed_fog_range` | 0xE8-0xED fog range | done | `runtime/gx_tev.c:250-260` | Follows Dolphin's own reading of the layout, which Dolphin marks as a guess. |
| `gx.0xee_0xf2_fog` | 0xEE-0xF2 fog | done | `runtime/gx_tev.c:228-261` |  |
| `gx.0xf3_alpha_test` | 0xF3 alpha test | done | `runtime/gx_tev.c:189` |  |
| `gx.0xf4_z_texture_bias` | 0xF4 z texture bias | done | `runtime/gx_tev.c:175` |  |
| `gx.0xf5_z_texture_mode` | 0xF5 z texture mode | done | `runtime/gx_tev.c:169` |  |
| `gx.0xf6_0xfd_konst_select_and_swap_tables` | 0xF6-0xFD konst select and swap tables | done | `runtime/gx_tev.c:52`, `:93` |  |
| `gx.0xfe_mask` | 0xFE mask | done | `runtime/gx_fifo.c:82` |  |

### CP and PE hardware registers

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `gx.cp_status` | CP status | partial | `runtime/gx_fifo.c:467` | Always idle, plus flags. |
| `gx.cp_control` | CP control | done | `runtime/gx_fifo.c:434-455` |  |
| `gx.cp_clear` | CP clear | done | `runtime/gx_fifo.c:435` |  |
| `gx.cp_fifo_base_end_read_write` | CP FIFO base, end, read, write | done | `runtime/gx_fifo.c:317-358` |  |
| `gx.cp_high_and_low_marks` | CP high and low marks | done | `runtime/gx_fifo.c:303` | Only with `--fifo-breakpoints`. |
| `gx.cp_breakpoint` | CP breakpoint | done | `runtime/gx_fifo.c:294` | Only with `--fifo-breakpoints`, and paced for a Wii-like speed. |
| `gx.cp_read_write_distance` | CP read-write distance | done | `runtime/gx_fifo.c:512` | From the read and write positions, across the wrap, as Dolphin works it out. |
| `gx.cp_performance_counters` | CP performance counters | on demand |  | Performance counters, read by profiling tools rather than game logic. |
| `gx.pe_control_and_status` | PE control and status | done | `runtime/gx_fifo.c:456`, `:468` |  |
| `gx.pe_token` | PE token | done | `runtime/gx_fifo.c:469` |  |
| `gx.pe_z_alpha_destination_alpha_config` | PE z, alpha, destination alpha config | partial | `runtime/gx_efb.c:58` | Only pokes use it. They go straight in, as in Dolphin, and a poke set to test depth, blend, use a logic operation or leave channels alone names this row. |
| `gx.pe_alpha_read_mode` | PE alpha read mode | done | `runtime/gx_efb.c:30`, `gx_encode.c:185` | Peeks read alpha as 0, 0xff or as stored. |
| `gx.pe_bounding_box_0x10_0x16` | PE bounding box 0x10-0x16 | partial | `runtime/gx_efb.c:159`, `gx_fifo.c` | Waits for the card and reads the four values, which turns the box off, as Dolphin's PixelEngine.cpp does. Vulkan only. |
| `gx.pe_performance_counters` | PE performance counters | on demand |  | Performance counters, read by profiling tools rather than game logic. |

## 3. TEV and pixel features

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `pixel.up_to_16_stages` | Up to 16 stages | done | `runtime/gx_tev.c:182` |  |
| `pixel.color_inputs_all_16` | Color inputs (all 16) | done | `runtime/gx_tev.c:224-229` |  |
| `pixel.alpha_inputs_all_8` | Alpha inputs (all 8) | done | `runtime/gx_tev.c:231-235` |  |
| `pixel.lerp_with_op_bias_scale` | Lerp with op, bias, scale | done | `runtime/gx_tev.c:319-326` |  |
| `pixel.8_bit_wrap_of_a_b_c_inputs` | 8-bit wrap of a, b, c inputs | partial | `runtime/gx_tev.c:319` | Floats, so a negative register in a, b or c doesn't wrap. No `c + (c >> 7)` weighting. |
| `pixel.clamp_and_11_bit_range` | Clamp and 11-bit range | done | `runtime/gx_tev.c:328-329` |  |
| `pixel.output_register_choice` | Output register choice | done | `runtime/gx_tev.c:328` |  |
| `pixel.final_output_from_the_last_stage_s_regis` | Final output from the last stage's register | done | `runtime/gx_tev.c:649-650` | Always `R0`. Dolphin: "the results of the last texenv stage are put onto the screen, regardless of the used destination register". |
| `pixel.compare_r8_greater_equal` | Compare R8 greater, equal | done | `runtime/gx_tev.c:315-317` |  |
| `pixel.compare_gr16` | Compare GR16 | done | `runtime/gx_tev.c:464-494`, `:532-550` | Scale field ignored, so treated as R8. |
| `pixel.compare_bgr24` | Compare BGR24 | done | `runtime/gx_tev.c:464-494`, `:532-550` |  |
| `pixel.compare_rgb8_per_channel` | Compare RGB8 per channel | done | `runtime/gx_tev.c:464-494`, `:532-550` |  |
| `pixel.alpha_compare_a8` | Alpha compare A8 | done | `runtime/gx_tev.c:322` |  |
| `pixel.alpha_compare_r8_gr16_bgr24` | Alpha compare R8, GR16, BGR24 | done | `runtime/gx_tev.c:464-494`, `:532-550` | Always compares alpha. |
| `pixel.konst_colors_and_fractions` | Konst colors and fractions | done | `runtime/gx_tev.c:241-252` |  |
| `pixel.swap_tables_raster_and_texture` | Swap tables, raster and texture | done | `runtime/gx_tev.c:52`, `:117`, `:124` |  |
| `pixel.stage_with_texture_off` | Stage with texture off | done | `runtime/gx_tev.c:510-514` | White, or zero when no coordinates are enabled, as Dolphin. |
| `pixel.stage_texture_with_no_coordinates_enable` | Stage texture with no coordinates enabled | done | `runtime/gx_tev.c:142`, `:388`, `:640` | Dolphin reads zero. We sample. |
| `pixel.raster_channel_zero` | Raster channel zero | done | `runtime/gx_tev.c:301` |  |
| `pixel.bump_alpha_raster_5` | Bump alpha raster (5) | done | `runtime/gx_tev.c:153-157`, `:444`, `:517-518` | Reads zero. |
| `pixel.normalized_bump_alpha_raster_6` | Normalized bump alpha raster (6) | done | `runtime/gx_tev.c:153-157`, `:444`, `:517-518` | Reads zero. |
| `pixel.indirect_stage_selection` | Indirect stage selection | done | `runtime/gx_tev.c:66-72` | Only when the stage's texture is on, which may be too strict. |
| `pixel.indirect_fixed_matrices` | Indirect fixed matrices | done | `runtime/gx_tev.c:140-149` |  |
| `pixel.indirect_dynamic_s_matrices_5_7` | Indirect dynamic S matrices (5-7) | done | `runtime/gx_tev.c:448-452` | Comment at `:153-154` says so. Water and heat effects often use them. |
| `pixel.indirect_dynamic_t_matrices_9_11` | Indirect dynamic T matrices (9-11) | done | `runtime/gx_tev.c:448-452` |  |
| `pixel.indirect_formats_8_5_4_3_bits` | Indirect formats 8, 5, 4, 3 bits | done | `runtime/gx_tev.c:273` |  |
| `pixel.indirect_bias` | Indirect bias | done | `runtime/gx_tev.c:275-279` |  |
| `pixel.indirect_bump_alpha_select` | Indirect bump alpha select | done | `runtime/gx_tev.c:153-157`, `:444`, `:517-518` | The `bs` field is never read. |
| `pixel.indirect_wrap` | Indirect wrap | done | `runtime/gx_tev.c:259-264` |  |
| `pixel.indirect_add_previous` | Indirect add previous | done | `runtime/gx_tev.c:282` |  |
| `pixel.indirect_lod_from_unmodified_coordinates` | Indirect LOD from unmodified coordinates | partial | `runtime/gx_tev.c:501` | Always reads level 0. |
| `pixel.indirect_coordinate_scale` | Indirect coordinate scale | done | `runtime/gx_tev.c:83`, `:276` |  |
| `pixel.alpha_test_8_compares_4_logic_joins` | Alpha test, 8 compares, 4 logic joins | done | `runtime/gx_tev.c:332-340`, `:380` | Compares floats, with a small tolerance for equal. |
| `pixel.fog_linear_exp_exp2_backward_exp_backwar` | Fog: linear, exp, exp2, backward exp, backward exp2 | done | `runtime/gx_tev.c:228-261`, `:581-609` | Types 1 and 3 are drawn as linear. Left out when the draw's vertex depths prove it adds nothing (`gx_draw.c:235-245`). |
| `pixel.fog_range_adjustment` | Fog range adjustment | done | `runtime/gx_tev.c:250-260`, `:591-599` |  |
| `pixel.z_compare_functions` | Z compare functions | done | `runtime/gx_draw.c:120-122` |  |
| `pixel.z_write_enable` | Z write enable | done | `runtime/gx_draw.c:122` |  |
| `pixel.z_compare_location_early_z` | Z compare location (early z) | done | `runtime/gx_tev.c:325-375`, `runtime/gx_vulkan_pipeline.c:30` | With early z, depth is stored where the alpha test fails, and depth textures are ignored. OpenGL can't, and reports it. |
| `pixel.z_freeze` | Z-freeze | on demand | `runtime/gx_fifo.c:102-106` | Needs the last triangle's depth worked out on the CPU and a per-pixel depth path. Reported when turned on. |
| `pixel.z_texture_off_add_replace` | Z texture off, add, replace | done | `runtime/gx_tev.c:168-176`, `:343-354` |  |
| `pixel.z_texture_z8_z16_z24` | Z texture Z8, Z16, Z24 | done | `runtime/gx_tev.c:347-350` |  |
| `pixel.z16_precision_formats` | Z16 precision formats | on demand | `runtime/gx_fifo.c:108` | Depth is kept at the host's full precision, as Dolphin keeps it. Reported when chosen. |
| `pixel.depth_range` | Depth range | partial | `runtime/gx_draw.c:256-267`, `runtime/gx_gl.c:225`, `runtime/gx_vulkan.c:474` | Near and far within 0 to 1 are exact. Values outside are clamped and logged. |
| `pixel.blend_factors` | Blend factors | done | `runtime/gx_draw.c:95-118` | Uses dual-source alpha. |
| `pixel.subtract_blend` | Subtract blend | done | `runtime/gx_draw.c:109` |  |
| `pixel.logic_ops_16` | Logic ops (16) | partial | `runtime/gx_draw.c:126-155`, `runtime/gx_tev.c:263-271` | Clear, copy, no-op, invert, copy-inverted and set are exact. The rest are approximated with blending and logged. |
| `pixel.dither` | Dither | not needed |  | Dither only spreads the error of rounding to 6 bits, which isn't done (`pixel.efb_rgba6_z24`), as in Dolphin's default. Needed again if that rounding is added. |
| `pixel.color_and_alpha_write_masks` | Color and alpha write masks | done | `runtime/gx_draw.c:123-124` |  |
| `pixel.destination_alpha_constant_alpha` | Destination alpha (constant alpha) | done | `runtime/gx_tev.c:389`, `gx_draw.c:114` |  |
| `pixel.target_alpha_reads_on_rgb8_efb` | Target alpha reads on RGB8 EFB | partial | `runtime/gx_draw.c:95-101` | Hardware has no alpha in that format and reads 1, and writes are off. We keep alpha. |
| `pixel.efb_rgb8_z24` | EFB RGB8_Z24 | partial | `runtime/gx_fifo.c:123` | Host RGBA8 with no alpha rules. |
| `pixel.efb_rgba6_z24` | EFB RGBA6_Z24 | partial |  | No 6-bit rounding. Often visible only as banding. |
| `pixel.efb_rgb565_z16` | EFB RGB565_Z16 | partial |  | No 16-bit rounding. |
| `pixel.efb_z24` | EFB Z24 | done | `runtime/gx_fifo.c:123` | As a depth copy source. |
| `pixel.efb_y8_u8_v8_yuv420` | EFB Y8, U8, V8, YUV420 | on demand | `runtime/gx_fifo.c:109` | Picture formats for anti-aliased YUV drawing, very rarely used. Reported when chosen. |
| `pixel.reinterpreting_efb_after_a_format_change` | Reinterpreting EFB after a format change | missing | `runtime/gx_draw.c:339-345` | Logged only. |
| `pixel.multisample_and_aa_modes` | Multisample and AA modes | on demand | `runtime/gx_fifo.c:104` | Pictures are drawn without anti-aliasing. Reported when turned on. |
| `pixel.efb_peek_cpu_reads_color_depth` | EFB peek (CPU reads color, depth) | done | `runtime/gx_efb.c:30`, `:79`, `gx_encode.c:185` | Color as 0xAARRGGBB through the pixel format and alpha read mode, depth as 24 bits, or 16 in a 16-bit depth format, as Dolphin's EFBInterface.cpp and CompressZ16. The first peek after anything changed reads the whole picture back at the Wii's size; later ones read that. Both renderers. The color and depth form (bit 23), which nothing has found the meaning of, and accesses narrower than a word, name this row. |
| `pixel.efb_poke_cpu_writes` | EFB poke (CPU writes) | partial | `runtime/gx_efb.c:58`, `gx_vulkan.c:1986`, `gx_gl.c` | Sets every picture pixel the game's pixel covers, color or depth, on both renderers. The PE's poke settings aren't followed (`gx.pe_z_alpha_destination_alpha_config`), as in Dolphin, which marks them as unknown. |
| `pixel.bounding_box` | Bounding box | partial | `runtime/gx_efb.c:132`, `gx_tev.c:705`, `gx_vulkan.c:2016` | On Vulkan, shaders drawn while it's on widen it with each of the game's pixels they reach after the alpha test, in squares of 2 by 2, as Dolphin's PixelShaderGen.cpp. A draw a Vulkan shortcut leaves out (`pixel.vulkan_draw_shortcuts`) doesn't count. OpenGL has no shaders that write memory, and names this row. |
| `pixel.copy_to_texture` | Copy to texture | done | `runtime/gx_copy.c:316`, `:106`, `memory_watch.c:125` | The copy stays on the card for drawing, and its memory is held: the first read or write of it from anywhere faults, the copy is read back, encoded as the chip does (`gx_encode.c`) and written, and the access goes on. A place read once is read back as each new copy is made, without waiting, and lands as the next picture is shown. Reads are recorded as the frame's commands end, so they never split a pass. A touch of something beside a copy on its first or last page doesn't wait: that page lands with the next picture, taking only bytes nothing has written since. A copy the card left out, as a held-back layer the screen never showed, names this row instead. |
| `pixel.copy_formats_r4_r8_ra4_ra8_rgb565_rgb5a3` | Copy formats R4, R8, RA4, RA8, RGB565, RGB5A3, RGBA8, A8, G8, B8 | done | `runtime/gx_encode.c:130`, `gx_copy.c:204` | Exact bytes in memory, through the picture's pixel format. Drawn from the card by channel swizzle, at full bit depth. |
| `pixel.copy_formats_rg8_gb8` | Copy formats RG8, GB8 | done | `runtime/gx_encode.c:130`, `gx_copy.c:204` | RG8 is green then red, GB8 blue then green, in memory and in the card's copy. |
| `pixel.depth_copy` | Depth copy | partial | `runtime/gx_encode.c:36`, `gx_copy.c:274` | In memory, depth's three bytes read as red, green and blue, for Z4, Z8, Z8M, Z8L, Z16, Z16L and Z24X8. RA4, RA8, RGB565, RGB5A3 and A8 from depth, which Dolphin's software encoder has no answer for, name this row. Drawn from the card, depth reads as one value. |
| `pixel.copy_half_size` | Copy half size | done | `runtime/gx_copy.c:316`, `gx_encode.c:52` | In memory each texel is the mean of its 2 by 2 box. |
| `pixel.copy_y_scale` | Copy Y scale | missing |  |  |
| `pixel.copy_clear` | Copy clear | done | `runtime/gx_fifo.c:124` |  |
| `pixel.copy_intensity_rgb_to_yuv` | Copy intensity (RGB to YUV) | partial | `runtime/gx_copy.c:274`, `gx_encode.c:72` | Bits 15 and 16. In memory, Y, U and V from Dolphin's constants, which it found by testing hardware, rounding a half up, for the intensity formats. The card's copy, drawn as a texture, reads red and says so. Other formats asking for it name this row, as Dolphin's two encoders disagree there. |
| `pixel.copy_clamp_top_and_bottom` | Copy clamp top and bottom | partial | `runtime/gx_encode.c:52` | In memory the filter's rows above and below stop at the copy's edge when asked. A copy read back from the card has no rows beyond its own, so both edges stop there. |
| `pixel.copy_filter_aa_and_deflicker` | Copy filter (AA and deflicker) | partial | `runtime/gx_encode.c:72` | The vertical filter applies in memory (`gx.0x53_0x54_vertical_copy_filter`). Anti-aliasing doesn't (`pixel.multisample_and_aa_modes`). |
| `pixel.gamma_on_xfb_copy_1_0_1_7_2_2` | Gamma on XFB copy (1.0, 1.7, 2.2) | partial | `runtime/gx_encode.c:72` | Texture copies in memory follow it, as Dolphin's encoder does. Copies to the screen and the card's copy drawn as a texture don't, and say so. |
| `pixel.copy_to_xfb` | Copy to XFB | partial | `runtime/gx_fifo.c:118`, `gx_draw.c:216` | Present directly. XFB memory unused. |
| `pixel.field_copy_frame_to_field` | Field copy (frame to field) | missing |  |  |
| `pixel.scissor` | Scissor | done | `runtime/gx_draw.c:142-149` |  |
| `pixel.cull_none_front_back_all` | Cull none, front, back, all | done | `runtime/gx_draw.c:125`, `gx_gl.c:212` |  |
| `pixel.vulkan_draw_shortcuts` | Vulkan draw shortcuts | partial | `runtime/gx_vulkan.c:117`, `:662`, `:689` | Tuned on Wii Fit. Anything landing at y >= 456 runs before the scene, which is wrong for a game drawing real picture there. Cover skipping needs `WII_RECOMP_COVER`. |

## 4. Vertex and texture

### Vertex formats

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `vtx.direct_index8_index16` | Direct, index8, index16 | done | `runtime/gx_vertex.c:129-138` |  |
| `vtx.null_index_on_position_0xff_0xffff` | Null index on position (0xFF, 0xFFFF) | done | `runtime/gx_vertex.c:86-90`, `:155`, `:178` | Dolphin skips the vertex. We read `base + 0xFF * stride`. |
| `vtx.null_index_on_other_attributes` | Null index on other attributes | unknown | `runtime/gx_vertex.c:52-61` | Unclear whether hardware does anything different from what we do. |
| `vtx.position_xy_xyz` | Position XY, XYZ | done | `runtime/gx_vertex.c:40` |  |
| `vtx.position_u8_s8_u16_s16_f32` | Position U8, S8, U16, S16, F32 | done | `runtime/gx_vertex.c:94-103` |  |
| `vtx.position_fraction` | Position fraction | done | `runtime/gx_vertex.c:95` |  |
| `vtx.byte_dequant_flag` | Byte dequant flag | done | `runtime/gx_vertex.c:68` | When clear, whole-number positions and coordinates ignore their shift, as Dolphin's compiled vertex loaders do. Its reference loader disagrees, so this is worth a check on hardware. |
| `vtx.position_matrix_index` | Position matrix index | done | `runtime/gx_vertex.c:157` |  |
| `vtx.texture_matrix_indexes_0_7` | Texture matrix indexes 0-7 | done | `runtime/gx_vertex.c:159` |  |
| `vtx.normal_n_3_values` | Normal N (3 values) | done | `runtime/gx_vertex.c:169-175` |  |
| `vtx.normal_nbt_9_values` | Normal NBT (9 values) | partial | `runtime/gx_vertex.c:43`, `:172` | Bytes skipped correctly, binormal and tangent discarded. |
| `vtx.normal_nbt_with_3_indexes` | Normal NBT with 3 indexes | done | `runtime/gx_vertex.c:50`, `:74-84` | Hardware reads three indexes. We read one, so the vertex size is wrong and every following vertex and command is garbage. |
| `vtx.normal_s8_s16` | Normal S8, S16 | done | `runtime/gx_vertex.c:140` |  |
| `vtx.normal_u8_u16` | Normal U8, U16 | partial | `runtime/gx_vertex.c:140` | Scale should be 2^-7 and 2^-15. We use 2^-14 for both. The SDK doesn't normally allow these. |
| `vtx.normal_f32` | Normal F32 | done | `runtime/gx_vertex.c:101` |  |
| `vtx.missing_normal` | Missing normal | partial | `runtime/gx_vertex.c:168`, `gx_transform.c:236` | Fixed (0, 0, 1). Hardware keeps the last normal loaded. |
| `vtx.color_rgb565_rgb8_rgbx8_rgba4_rgba6_rgba` | Color RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8 | done | `runtime/gx_vertex.c:105-126` |  |
| `vtx.missing_color` | Missing color | done | `runtime/gx_vertex.c:177` | White, which is Dolphin's default. |
| `vtx.color_1_present_without_color_0` | Color 1 present without color 0 | done | `runtime/gx_vertex.c:212` | Color 1 feeds channel 0, and channel 1 reads as missing, as in Dolphin. |
| `vtx.texture_coordinates_s_st_in_all_formats` | Texture coordinates S, ST in all formats | done | `runtime/gx_vertex.c:182-189` |  |
| `vtx.texture_coordinate_fraction_per_coordina` | Texture coordinate fraction, per coordinate | done | `runtime/gx_vertex.c:49-59` |  |
| `vtx.vertex_cache_enhance_flag` | Vertex cache enhance flag | not needed |  | No cache. |

### Texture coordinate generation

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `vtx.regular_2x4` | Regular 2x4 | done | `runtime/gx_transform.c:200-205` |  |
| `vtx.regular_3x4_projected` | Regular 3x4 (projected) | partial | `runtime/gx_transform.c:204`, `:212` | Divides per vertex, so perspective across a triangle is wrong. Should send three values and divide per pixel. |
| `vtx.emboss` | Emboss | done | `runtime/gx_transform.c:60-65`, `:304-309` | Treated as regular, with no light offset. |
| `vtx.color_0_color_1_srtg` | Color 0, color 1 (SRTG) | done | `runtime/gx_transform.c:34-37`, `:191` |  |
| `vtx.source_position` | Source position | done | `runtime/gx_transform.c:196` |  |
| `vtx.source_normal` | Source normal | done | `runtime/gx_transform.c:197` |  |
| `vtx.source_binormal_tangent` | Source binormal, tangent | done | `runtime/gx_transform.c:45`, `:297-298` | Become (0, 0, 1, 1). |
| `vtx.source_colors_row` | Source colors row | not needed |  | Only for SRTG types. |
| `vtx.source_texture_coordinates_0_7` | Source texture coordinates 0-7 | done | `runtime/gx_transform.c:198` |  |
| `vtx.input_form_ab11` | Input form AB11 | done | `runtime/gx_transform.c:70`, `:301` | Position always keeps its z. |
| `vtx.post_transform_matrix` | Post-transform matrix | done | `runtime/gx_transform.c:206-211` |  |
| `vtx.post_transform_normalize` | Post-transform normalize | done | `runtime/gx_transform.c:207` |  |
| `vtx.identity_shortcut_for_matrix_60` | Identity shortcut for matrix 60 | done | `runtime/gx_transform.c:202` |  |

### Lighting

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `vtx.2_color_and_2_alpha_channels` | 2 color and 2 alpha channels | done | `runtime/gx_light.c:114-118` |  |
| `vtx.material_from_register_or_vertex` | Material from register or vertex | done | `runtime/gx_light.c:97-98` |  |
| `vtx.ambient_from_register_or_vertex` | Ambient from register or vertex | done | `runtime/gx_light.c:104-105` |  |
| `vtx.8_lights_masks` | 8 lights, masks | done | `runtime/gx_light.c:33`, `:106` |  |
| `vtx.diffuse_none_sign_clamp` | Diffuse none, sign, clamp | done | `runtime/gx_light.c:76` |  |
| `vtx.attenuation_off` | Attenuation off | done | `runtime/gx_light.c:77` |  |
| `vtx.attenuation_directional_2` | Attenuation directional (2) | done | `runtime/gx_light.c:77` | Same as off, as in Dolphin. |
| `vtx.attenuation_specular` | Attenuation specular | partial | `runtime/gx_light.c:79-83` | Dolphin normalizes the distance terms when diffuse is on. We don't, so brightness differs. |
| `vtx.attenuation_spot` | Attenuation spot | done | `runtime/gx_light.c:84-86` |  |
| `vtx.zero_denominator` | Zero denominator | unknown | `runtime/gx_light.c:69` | We return black. Dolphin divides by zero. Hardware result isn't known. |
| `vtx.color_count_limiting_channels` | Color count limiting channels | done | `runtime/gx_transform.c:98-113`, `runtime/gx_light.c:46` | See `gx.0x1009_color_count`. |
| `vtx.integer_rounding_of_light_results` | Integer rounding of light results | partial | `runtime/gx_light.c:108` | Floats. Small banding differences only. |

### Textures

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `vtx.i4_i8_ia4_ia8` | I4, I8, IA4, IA8 | done | `runtime/gx_texture.c:98-105` |  |
| `vtx.rgb565_rgb5a3_rgba8` | RGB565, RGB5A3, RGBA8 | done | `runtime/gx_texture.c:106-110` |  |
| `vtx.c4_c8_c14x2` | C4, C8, C14X2 | partial | `runtime/gx_texture.c:111-117` | Decoders right, palette offset wrong. |
| `vtx.cmpr` | CMPR | done | `runtime/gx_texture.c:52-75` | Blend weights differ slightly from hardware. |
| `vtx.tlut_formats_ia8_rgb565_rgb5a3` | TLUT formats IA8, RGB565, RGB5A3 | done | `runtime/gx_texture.c:43-49` |  |
| `vtx.tlut_load` | TLUT load | partial | `runtime/gx_texture.c:20-25` | Offset unit. |
| `vtx.wrap_clamp_repeat_mirror` | Wrap clamp, repeat, mirror | done | `runtime/gx_draw.c:67` |  |
| `vtx.magnify_near_linear` | Magnify near, linear | done | `runtime/gx_draw.c:69` |  |
| `vtx.minify_near` | Minify near | done | `runtime/gx_draw.c:85-104`, `runtime/gx_vulkan_pipeline.c:481-486` | Always linear. |
| `vtx.mip_filter_none_near_linear` | Mip filter none, near, linear | done | `runtime/gx_draw.c:85-104`, `runtime/gx_vulkan_pipeline.c:481-486` | Always trilinear on textures. |
| `vtx.stored_mip_levels` | Stored mip levels | done | `runtime/gx_texture.c:147-154`, `:171-202` | Every stored level decoded and uploaded. |
| `vtx.lod_bias` | LOD bias | done | `runtime/gx_tev.c:171-180`, `:506` | Applied in the shader, as Metal samplers have no bias. |
| `vtx.lod_min_and_max` | LOD min and max | done | `runtime/gx_draw.c:97-102` |  |
| `vtx.anisotropy` | Anisotropy | done | `runtime/gx_draw.c`, `runtime/gx_vulkan_pipeline.c`, `runtime/gx_gl.c` | Up to 2 or 4 samples along a slope, in the sampler, as Dolphin does. Off on a device without it, which Vulkan says at start-up. |
| `vtx.edge_lod` | Edge LOD | partial | `runtime/gx_tev.c` (`add_stage`) | Diagonal mode is the chip's: each coordinate's change across and down a pixel added, given to `textureGrad`. Edge mode is the GPU's own, which can differ from the chip's by up to half a level at an angle. |
| `vtx.tmem_preload` | TMEM preload | on demand | `runtime/gx_draw.c:78` | See `gx.0x60_0x63_texture_preload`. |
| `vtx.update_detection` | Update detection | partial | `runtime/gx_draw.c:86`, `gx_texture.c:135` | Checked once per frame, on a sample of bytes. A texture changed mid-frame, or a small change, can be missed. |
| `vtx.copies_as_textures` | Copies as textures | done | `runtime/gx_draw.c:130`, `gx_copy.c:387` | The card's copy, while memory still holds it and the texture has its shape and format. Read any other way, at another size, format, stride or through a palette, the texture comes from the copy's real bytes in memory. |
| `vtx.non_power_of_two_wrap` | Non-power-of-two wrap | unknown |  | Hardware requires powers of two for repeat. Behaviour off-spec isn't known. |

## 5. Hardware blocks

Registers with no entry below are kept in a plain array (`hardware.c:39`), so reads return what was written.

### VI (video)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.retrace_timing` | Retrace timing | partial | `runtime/interrupts.c:23`, `:84-89` | Fixed 60 Hz. PAL 50 Hz games run at the wrong rate. |
| `hw.display_interrupts_0_3` | Display interrupts 0-3 | partial | `runtime/hardware.c:75-85` | All enabled ones fire together at retrace, ignoring their line settings. |
| `hw.interrupt_clear` | Interrupt clear | done | `runtime/hardware.c:87-92` |  |
| `hw.beam_position_0x2c_0x2e` | Beam position (0x2C, 0x2E) | missing |  | Reads 0. |
| `hw.framebuffer_addresses_0x1c_0x2a` | Framebuffer addresses (0x1C-0x2A) | missing |  | Picture comes from EFB copy (`gx_fifo.c:118`). A game writing XFB by CPU shows nothing. |
| `hw.timing_registers_0x00_0x04_0x1a` | Timing registers (0x00, 0x04-0x1A) | not needed |  | Host window sets the output. |
| `hw.interlace_progressive_field_order` | Interlace, progressive, field order | missing |  |  |
| `hw.horizontal_scaler_filter_0x48_0x6c` | Horizontal scaler, filter (0x48-0x6C) | missing |  | Stored only. |
| `hw.display_latch_0x40_0x46` | Display latch (0x40-0x46) | missing |  | Light gun only. |
| `hw.clock_select_dtv_status` | Clock select, DTV status | done | `runtime/hardware.c:295`, `:312` | No component cable, and the NTSC-J bit on a Japanese console, as Dolphin answers without progressive scan. Clock select only times a signal the window replaces. |
| `hw.vi_solid_color_hollywood_0x24` | VI solid color (Hollywood 0x24) | on demand | `runtime/ios.c:189` | Only system software blanks the screen this way, and Dolphin ignores it. Reported when turned on. |

### PI (processor interface)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.interrupt_cause` | Interrupt cause | done | `runtime/hardware.c:144`, `:157` |  |
| `hw.interrupt_mask` | Interrupt mask | done | `runtime/hardware.c:72` |  |
| `hw.fifo_base_end_write` | FIFO base, end, write | done | `runtime/gx_fifo.c:311`, `:363` |  |
| `hw.fifo_reset_0x18` | FIFO reset (0x18) | done | `runtime/gx_fifo.c:452`, `:465` | Drops the half-filled burst and the commands not yet run, and resets CP control and marks, as Dolphin does. |
| `hw.error_cause_address` | Error cause, address | not needed |  | No bus errors. |
| `hw.reset_code` | Reset code | done | `runtime/hardware.c` | Reads 0, a cold boot, as Dolphin answers. |
| `hw.hardware_revision` | Hardware revision | done | `runtime/hardware.c` | 0x246500B1, Flipper revision C, as Dolphin answers. |
| `hw.reset_button_interrupt` | Reset button interrupt | done | `runtime/hardware.c:282-293`, `runtime/stm.c:45`, `runtime/input.c:249` | The control pipe's `reset` raises the interrupt, holds the button for 0.5 s and answers STM's event hook, as Dolphin sends both. |
| `hw.hsp_debug_interrupts` | HSP, debug interrupts | not needed |  |  |

### MI (memory interface)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.protection_regions` | Protection regions | not needed |  | Only used to catch bugs. |
| `hw.memory_interrupt` | Memory interrupt | not needed |  |  |
| `hw.timer_and_performance_registers` | Timer and performance registers | not needed |  |  |

### AI (audio interface)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.sample_counter` | Sample counter | done | `runtime/hardware.c:145` | Host time at 48 kHz, regardless of control register. |
| `hw.control` | Control | partial |  | Stored. Rate bits don't change anything. |
| `hw.volume` | Volume | not needed |  | Only scales streamed disc audio, which Wii discs don't use (`hw.disc_audio_streaming`). |
| `hw.interrupt_timing` | Interrupt timing | done | `runtime/hardware.c:259-280`, `runtime/interrupts.c:105` | The AI interrupt fires as the sample counter passes the timing register. The counter can be reset and written. |
| `hw.disc_audio_streaming` | Disc audio streaming | not needed |  | GameCube discs only. |

### DSP

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.control_register` | Control register | done | `runtime/hardware.c:164-169` |  |
| `hw.audio_dma_interrupt` | Audio DMA interrupt | done | `runtime/hardware.c:104`, `audio.c:115` | Fires on a 3 ms clock. |
| `hw.audio_dma_start_length` | Audio DMA start, length | partial | `runtime/hardware.c:101` | Only the enable bit is read. Samples are never fetched from the address. Output comes straight from `ax.c`. |
| `hw.audio_dma_blocks_left` | Audio DMA blocks left | done | `runtime/hardware.c` | Worked out from the time left in the current audio frame, a block every 0.25 ms, zero-based as Dolphin reads it. |
| `hw.mail_to_dsp` | Mail to DSP | partial | `runtime/audio.c:128`, `translate/system_functions.json` | SDK functions replaced by signature. Only the AX list mail works. |
| `hw.mail_from_dsp` | Mail from DSP | on demand | `runtime/hardware.c:114` | No DSP runs, so frame-done mail is a call to the AX frame-done function. A game reading the mailbox itself is reported. |
| `hw.dsp_boot` | DSP boot | partial | `translate/system_functions.json` | Replaced, so it's assumed ready. |
| `hw.dsp_accelerator_interrupt` | DSP accelerator interrupt | done | `runtime/hardware.c:228`, `:345` | Accelerator DMA copies between main memory and MEM2 at once, as Dolphin's Wii mode does, then raises its interrupt. |
| `hw.instruction_ram_rom_real_dsp_code` | Instruction RAM, ROM, real DSP code | not needed |  | Replaced in C. |
| `hw.aram` | ARAM | not needed |  | Wii has none. A GameCube-style game would need it. |
| `hw.ar_dma_registers` | AR DMA registers | done | `runtime/hardware.c:228` | See `hw.dsp_accelerator_interrupt`. A transfer outside memory is reported. |

### SI (serial interface)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.channels_0_3_poll` | Channels 0-3, poll | partial | `runtime/si.c` | Every port answers "no response", as empty ports do. |
| `hw.communication_control_status` | Communication control, status | partial | `runtime/si.c:35-51` | Transfers finish at once. Error bits can be cleared. |
| `hw.i_o_buffer` | I/O buffer | done | `runtime/hardware.c:312` | Plain memory the game fills and reads. With nothing plugged in, no transfer writes a reply. |
| `hw.si_interrupt` | SI interrupt | done | `runtime/si.c:21-25` | Transfer done and read status. |
| `hw.gc_controller_wheel_keyboard_dance_mat` | GC controller, wheel, keyboard, dance mat | missing |  |  |
| `hw.gba_link` | GBA link | not needed |  | Almost no Wii use. |

### EXI (expansion interface)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.control_busy_bit` | Control busy bit | done | `runtime/exi.c` | Transfers finish at once. |
| `hw.status_dma_address_length_immediate_data` | Status, DMA address, length, immediate data | done | `runtime/exi.c` | Immediate and DMA transfers, both directions. Devices other than the clock chip are empty. |
| `hw.exi_interrupt` | EXI interrupt | done | `runtime/exi.c` | Transfer done, EXI and insert interrupts, each behind its mask. |
| `hw.rtc_and_sram_channel_0` | RTC and SRAM (channel 0) | done | `runtime/exi.c` | The clock is local time since 2000, as the time base, and the game can move it. SRAM holds a new Wii's settings with correct checksums. |
| `hw.gc_memory_card` | GC memory card | on demand |  | GameCube titles only. |
| `hw.microphone` | Microphone | on demand |  | A handful of titles. |
| `hw.ethernet_adapter` | Ethernet adapter | on demand |  | Online services the adapter reached are gone. |
| `hw.usb_gecko_modem_ad16` | USB Gecko, modem, AD16 | not needed |  | Debug or rare. |

### DVD (disc interface)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.registers_0xcd006000` | Registers 0xCD006000 | not needed |  | Wii games go through `/dev/di`. |
| `hw.reads_through_extracted_files` | Reads through extracted files | done | `runtime/dvd.c:110-136` | See area 7. |
| `hw.disc_image_files_iso_wbfs_rvz` | Disc image files (ISO, WBFS, RVZ) | partial | `runtime/drive.c:1-40`, `tools/make_disc.py` | Only this project's own `.wrd` image or an extracted folder. |
| `hw.second_disc_layer_disc_swap` | Second disc layer, disc swap | partial | `runtime/drive.c:280`, `runtime/dvd.c:105`, `runtime/ios.c:193` | The second layer works, as offsets are 64-bit. Swapping discs isn't done, and ejecting is reported. |

### IPC and Hollywood

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `hw.message_control_reply` | Message, control, reply | done | `runtime/ios.c:96-130` | Requests run at once. |
| `hw.irq_flags` | IRQ flags | partial | `runtime/ios.c:102` | Reports the IPC flag if any interrupt is pending. |
| `hw.irq_mask` | IRQ mask | partial |  | Plain store. Line assumed enabled. |
| `hw.other_hollywood_interrupts_timer_gpio` | Other Hollywood interrupts (timer, GPIO) | on demand | `runtime/ios.c:183` | Only IPC replies reach the game; the timer and GPIO lines belong to IOS. Reported if a game unmasks another line. |
| `hw.relaunch_bit` | Relaunch bit | not needed | `runtime/ios.c:166` | The SDK sets it after taking every reply, so it's part of the handshake, and games reload IOS through ES. |
| `hw.ppc_speed_vi_clock_resets` | PPC speed, VI clock, resets | not needed |  | Plain store. |
| `hw.gpio_disc_slot_light_sensor_bar` | GPIO (disc slot light, sensor bar) | done | `runtime/ios.c:138-198` | Both mirrors, keeping only the pins the game owns. Disc-in reads the drive. The slot light and sensor bar have nothing to drive, as in Dolphin. |
| `hw.ahb_protection` | AHB protection | not needed |  |  |

## 6. Audio microcodes

The AX work is rewritten in C. There's no DSP interpreter, so each program needs its own version.

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `audio.axwii_newer_command_format` | AXWii, newer command format | partial | `runtime/ax.c` | Everything below. |
| `audio.axwii_setup_add_subtract_add_subtract` | AXWii setup, add, subtract, add-subtract | done | `runtime/ax.c:220`, `:230` |  |
| `audio.axwii_voices` | AXWii voices | done | `runtime/ax.c:212` |  |
| `audio.axwii_adpcm_pcm16_pcm8` | AXWii ADPCM, PCM16, PCM8 | done | `runtime/ax.c:81`, `:103-108` |  |
| `audio.axwii_looping_streams` | AXWii looping, streams | done | `runtime/ax.c:68` |  |
| `audio.axwii_sample_rate_conversion` | AXWii sample rate conversion | partial | `runtime/ax.c:115` | Cubic stand-in for the ROM's filter tables. |
| `audio.axwii_envelope_mixer_ramps` | AXWii envelope, mixer ramps | done | `runtime/ax.c:147`, `:168` |  |
| `audio.axwii_low_pass_filter` | AXWii low-pass filter | done | `runtime/ax.c:157` |  |
| `audio.axwii_biquad_high_pass` | AXWii biquad, high-pass | missing | `runtime/ax.c:158` | Filter value 2 means biquad, but any nonzero value runs our low-pass. Dolphin marks Raving Rabbids and Bleach as using the newer layout, whose block offsets we don't handle. |
| `audio.axwii_aux_a_b_c` | AXWii aux A, B, C | done | `runtime/ax.c:244` |  |
| `audio.axwii_aux_surround_uploads_commands_8_9` | AXWii aux surround uploads (commands 8, 9) | missing | `runtime/ax.c:313` | Skipped. |
| `audio.axwii_compressor` | AXWii compressor | done | `runtime/ax.c:258` |  |
| `audio.axwii_output_stereo` | AXWii output stereo | done | `runtime/ax.c:282` |  |
| `audio.axwii_dolby_surround_output` | AXWii Dolby surround output | partial | `runtime/ax.c:315` | Treated as stereo. |
| `audio.axwii_wii_remote_speaker_output` | AXWii Wii Remote speaker output | missing | `runtime/ax.c:319` | Skipped. |
| `audio.axwii_older_command_format_old` | AXWii older command format (`_OLD`) | missing | `runtime/ax.c:320` | Command numbers differ. Dolphin uses it for Wii Sports (PAL) and the Wii Startup Menu, chosen by checksum. An unknown command loses the frame. |
| `audio.ax_gamecube_metroid_prime_trilogy` | AX (GameCube, Metroid Prime Trilogy) | missing |  |  |
| `audio.zelda_galaxy_twilight_princess_pikmin` | Zelda (Galaxy, Twilight Princess, Pikmin) | missing |  |  |
| `audio.asnd_aesnd_homebrew` | ASnd, AESnd (homebrew) | not needed |  | Commercial games don't use them. |
| `audio.card_gba` | CARD, GBA | not needed |  | GameCube link helpers. |
| `audio.init_rom` | INIT, ROM | not needed | `translate/system_functions.json` | Boot is skipped. |
| `audio.direct_ai_streaming_without_the_dsp` | Direct AI streaming without the DSP | missing |  | Needed by anything that plays its own buffer. |

## 7. IOS

### Core

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ios.open_close` | Open, close | done | `runtime/ios.c:34`, `:55-76` | Device names match by prefix (`devices.c:19`). 32 handles. |
| `ios.read_write_seek` | Read, write, seek | done | `runtime/files.c:121-140` | Files only. Devices get "fine". |
| `ios.ioctl_ioctlv_dispatch` | Ioctl, ioctlv dispatch | done | `runtime/devices.c:24` |  |
| `ios.unknown_device` | Unknown device | done | `runtime/ios.c:44` | Returns not found. |
| `ios.asynchronous_replies` | Asynchronous replies | done | `runtime/ios.c:101` | The reply queue grows as needed, so there's no limit. |
| `ios.ios_version_in_low_memory` | IOS version in low memory | partial | `runtime/loader.c:93-95` | Fixed at IOS36. Games wanting another version aren't told apart. |
| `ios.ios_reload` | IOS reload | partial | `runtime/es.c:65-78`, `runtime/console.c:108` | Launching between the Wii Menu and a disc restarts the console. No general reload. |

### `/dev/fs` and files

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ios.format` | FORMAT | not needed |  | Only the system menu's reset. |
| `ios.getstats` | GETSTATS | done | `runtime/fs.c:138` | Fixed free space. |
| `ios.createdir_createfile` | CREATEDIR, CREATEFILE | done | `runtime/fs.c:26` |  |
| `ios.readdir` | READDIR | done | `runtime/fs.c:112` |  |
| `ios.setattr` | SETATTR | done | `runtime/fs.c:157`, `:267` | Owner, group, permissions and flags kept in `saves/.attributes`, used by create, GETATTR, rename and delete. The caller always has full rights. |
| `ios.getattr` | GETATTR | partial | `runtime/fs.c:85` | Fixed owner and read-write. |
| `ios.delete` | DELETE | done | `runtime/fs.c:56` |  |
| `ios.rename` | RENAME | done | `runtime/fs.c:74` |  |
| `ios.setfileverctrl` | SETFILEVERCTRL | on demand | `runtime/ios_trace.c:44` | No source says what it changes, so it answers an error and is reported. |
| `ios.getfilestats` | GETFILESTATS | done | `runtime/files.c:141` |  |
| `ios.getusage` | GETUSAGE | partial | `runtime/fs.c:147` | Always 0 used. |
| `ios.shutdown` | SHUTDOWN | done | `runtime/fs.c:268`, `runtime/files.c` | Every changed open file is written out, then 0 is answered. |
| `ios.file_open_modes` | File open modes | done | `runtime/files.c:85`, `:165`, `:173`, `runtime/ios.c` | A forbidden read or write answers -102, a folder -101, a missing file -106, and no free handle -109. |
| `ios.permissions_error_codes` | Permissions, error codes | partial | `runtime/fs.c:15` | Only not found and exists. |

### `/dev/es`

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ios.gettitledir` | GETTITLEDIR | done | `runtime/es.c:19` |  |
| `ios.gettitleid` | GETTITLEID | done | `runtime/es.c:22` |  |
| `ios.identify_ticket_tmd_verify` | Identify (ticket, TMD, verify) | missing | `runtime/es.c:104` | Answered with an error. |
| `ios.getdeviceid_getdevicecert_sign` | GETDEVICEID, GETDEVICECERT, SIGN | partial | `runtime/es_titles.c:159`, `:165` | Device ID done. Certificate and signing answered with an error. |
| `ios.title_lists_and_counts` | Title lists and counts | done | `runtime/es_titles.c:166-169` | From the console's storage (`titles.c`). |
| `ios.views_stored_tmd` | Views, stored TMD | done | `runtime/es_titles.c:172-181`, `runtime/es.c:44` | Ticket and TMD views. |
| `ios.content_open_read_seek_close` | Content open, read, seek, close | done | `runtime/es_content.c:91-96` | No downloadable content or channels. |
| `ios.launch_return_to_menu_start_title` | LAUNCH (return to menu, start title) | partial | `runtime/es.c:70-78` | Wii Menu to disc and back only. Other titles get an error. |
| `ios.title_install_delete` | Title install, delete | not needed |  | System tools only. |
| `ios.encrypt_decrypt` | Encrypt, decrypt | on demand | `runtime/ios_trace.c:47` | Needs the console's own secret keys, which can't be shipped. Answers an error and is reported. |
| `ios.set_uid` | Set UID | done | `runtime/es_titles.c:162` | Fine without permissions. |

### `/dev/di`

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ios.readdiskid_0x70` | ReadDiskID 0x70 | done | `runtime/dvd.c:143` |  |
| `ios.read_0x71` | Read 0x71 | done | `runtime/dvd.c:144` | Data partition, decrypted already. |
| `ios.openpartition_0x8b` | OpenPartition 0x8B | partial | `runtime/dvd.c:140` | Says fine, and writes no ticket or TMD. |
| `ios.openpartition_with_tmd_0x93_0x94` | OpenPartition with TMD 0x93, 0x94 | missing | `runtime/dvd.c:100` | Answered with an error. |
| `ios.closepartition_0x8c` | ClosePartition 0x8C | partial | `runtime/dvd.c:150` | Says fine. |
| `ios.getcoverstatus_0x88_cover_and_status_reg` | GetCoverStatus 0x88, cover and status registers | done | `runtime/dvd.c:110` | 1 with no disc and 2 with one, as Dolphin answers. |
| `ios.inquiry_0x12` | Inquiry 0x12 | partial | `runtime/dvd.c:119` | Zeros with success. |
| `ios.reset_spin_up_cover_interrupts` | Reset, spin-up, cover interrupts | done | `runtime/dvd.c:150-152` | Nothing to do. |
| `ios.unencryptedread_0x8d` | UnencryptedRead 0x8D | done | `runtime/dvd.c:80`, `:110` | Reads the raw area, made region-free. |
| `ios.getlength_seek_readdvd` | GetLength, Seek, ReadDvd | missing | `runtime/dvd.c:123` | Answered with an error. |
| `ios.error_reporting` | Error reporting | partial | `runtime/dvd.c:13`, `:106` | No medium when the drive is empty, otherwise 0. |

### Devices

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ios.dev_stm_immediate` | `/dev/stm/immediate` | partial | `runtime/stm.c:8-15` | Dimming and the disc light only. **Regression:** shutdown, reset and idle answer an error since 7691d88. |
| `ios.dev_stm_eventhook` | `/dev/stm/eventhook` | partial | `runtime/devices.c:33` | Held open forever. Power and reset never sent. |
| `ios.dev_aes` | `/dev/aes` | done | `runtime/crypto.c:145` | Copy, and AES-128-CBC both ways with the game's key; the IV carries on, so a message can come in pieces. Checked against the FIPS-197 and SP 800-38A vectors. |
| `ios.dev_sha` | `/dev/sha` | partial | `runtime/crypto.c:227` | SHA-1 start, add and finish. The HMAC requests answer an error and are reported. |
| `ios.dev_sdio_slot0` | `/dev/sdio/slot0` | done | `runtime/sdio.c` | No card: status says empty, and insert waits are held. |
| `ios.dev_sdio_slot1` | `/dev/sdio/slot1` | not needed |  | Wi-Fi. Dolphin uses a stub. |
| `ios.dev_usb_oh1_57e_305` | `/dev/usb/oh1/57e/305` | done | `runtime/bluetooth.c:253` | With `--virtual-bluetooth`. See area 8. |
| `ios.dev_usb_oh1` | `/dev/usb/oh1` | partial | `runtime/devices.c:30` | Never replies. |
| `ios.dev_usb_oh0` | `/dev/usb/oh0` | done | `runtime/usb.c:75` | Empty ports: no devices, insertion hooks wait until cancelled, and a device opened by its IDs isn't found. |
| `ios.dev_usb_hid` | `/dev/usb/hid` | done | `runtime/usb.c:108` | Version 0x40001, an empty device list, later list requests wait until shutdown, and transfers answer not found. |
| `ios.dev_usb_ven` | `/dev/usb/ven` | on demand | `runtime/ios.c:53` | Only IOS 57 to 59 have it; open answers not found and is reported. |
| `ios.dev_usb_kbd` | `/dev/usb/kbd` | done | `runtime/usb.c:145` | Event requests wait for a keyboard that never comes; writes answer 0, as Dolphin does. |
| `ios.dev_usb_wfssrv_dev_wfsi` | `/dev/usb/wfssrv`, `/dev/wfsi` | not needed |  | Wii U mode. |
| `ios.dev_net_kd_request_dev_net_kd_time` | `/dev/net/kd/request`, `/dev/net/kd/time` | partial | `runtime/net.c:30-58`, `:127`, `runtime/wc24.c:112` | Scheduler, user ID, UTC and the real-time clock. |
| `ios.dev_net_ncd_manage` | `/dev/net/ncd/manage` | done | `runtime/net.c:87-124` | Not connected, the MAC address, and settings load and save. |
| `ios.dev_net_wd_command` | `/dev/net/wd/command` | on demand | `runtime/devices.c:40-45` | Answered with an error, so the game sees no network. |
| `ios.dev_net_ip_top` | `/dev/net/ip/top` | on demand | `runtime/devices.c:40-45` | Answered with an error, so the game sees no network. |
| `ios.dev_net_ssl` | `/dev/net/ssl` | on demand | `runtime/devices.c:40-45` | Answered with an error, so the game sees no network. |
| `ios.dev_wl0` | `/dev/wl0` | on demand | `runtime/devices.c:40-45` | Answered with an error, so the game sees no network. |
| `ios.dev_dolphin` | `/dev/dolphin` | not needed |  | Dolphin's own. |
| `ios.mios_gamecube_mode` | MIOS (GameCube mode) | not needed |  |  |
| `ios.save_data_in_storage` | Save data in storage | done | `runtime/files.c` | In the save folder, over an optional Wii storage folder. |

## 8. Controllers

### Direct input (default)

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ctl.kpadread_replacement` | `KPADRead` replacement | partial | `runtime/input.c:159` | Remote on channel 0. Fixed motion. An older SDK layout reports no samples (`system_functions.json`). |
| `ctl.wpadprobe_replacement` | `WPADProbe` replacement | done | `runtime/input.c:170` |  |
| `ctl.balance_board` | Balance Board | done | `runtime/input.c:131-150`, `:179-209` | Through signature-matched functions. Only for games with the board library. |
| `ctl.host_input` | Host input | partial | `runtime/input.c:222-236` | Control pipe commands and window. Sticks fixed (`input.c:99`). |

### Bluetooth stack

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ctl.usb_control_bulk_interrupt_endpoints` | USB control, bulk, interrupt endpoints | done | `runtime/bluetooth.c:253-281` |  |
| `ctl.hci_reset_buffer_size_version_address_fe` | HCI reset, buffer size, version, address, features | done | `runtime/bluetooth.c:170-182` | Reset falls to the default reply. |
| `ctl.hci_stored_link_keys` | HCI stored link keys | done | `runtime/bluetooth.c:186` |  |
| `ctl.hci_accept_connection` | HCI accept connection | done | `runtime/bluetooth.c:183` |  |
| `ctl.hci_create_connection` | HCI create connection | done | `runtime/bluetooth.c:285` | One of ours connects; an unknown address gets a page timeout, and one already connected 0x0B. |
| `ctl.hci_inquiry_inquiry_cancel` | HCI inquiry, inquiry cancel | done | `runtime/bluetooth.c:276` | No remote is discoverable, so a search ends at once; cancel completes. |
| `ctl.hci_remote_name_features_version_clock` | HCI remote name, features, version, clock | done | `runtime/bluetooth.c:215-226` |  |
| `ctl.hci_authentication_link_key_reply` | HCI authentication, link key reply | done | `runtime/bluetooth.c:193-196` |  |
| `ctl.hci_link_key_negative_reply` | HCI link key negative reply | done | `runtime/bluetooth.c:298` | Leads to a PIN request, whose reply gives a new key and finishes authentication. |
| `ctl.hci_delete_stored_link_key` | HCI delete stored link key | done | `runtime/bluetooth.c` | Deletes one address's key or all, answers how many, and keeps them out of the stored list until the game writes them again (0x0C11). |
| `ctl.hci_packet_type_link_policy_supervision` | HCI packet type, link policy, supervision timeout | done | `runtime/bluetooth.c:202-209` |  |
| `ctl.hci_sniff_mode` | HCI sniff mode | done | `runtime/bluetooth.c:319` | Status, then a change to sniff mode at the longest interval allowed. |
| `ctl.hci_write_settings_scan_class_name_pin_o` | HCI write settings (scan, class, name, PIN, others) | done | `runtime/bluetooth.c:228-230` | Accepted. |
| `ctl.hci_disconnect` | HCI disconnect | partial | `runtime/bluetooth.c:210` | Link can't come back. `remote_called` stays set. |
| `ctl.l2cap_connect_configure` | L2CAP connect, configure | done | `runtime/bluetooth_channels.c:42-80` |  |
| `ctl.l2cap_disconnect` | L2CAP disconnect | done | `runtime/bluetooth_channels.c:102` | Answered and the channel closed. Reports stop when the interrupt channel closes. |
| `ctl.sdp_queries` | SDP queries | on demand | `runtime/bluetooth_channels.c:84` | SDK games don't query a paired remote, so a discovery channel is refused and reported. |
| `ctl.paired_remote_record_in_sysconf` | Paired remote record in SYSCONF | done | `runtime/bluetooth.c:237` |  |

### Wii Remote reports

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ctl.0x10_rumble` | 0x10 rumble | partial | `runtime/wii_remote.c:109` | Accepted, not felt. Rumble bit in other reports ignored. |
| `ctl.0x11_leds` | 0x11 LEDs | partial | `runtime/wii_remote.c:109` | Accepted, not shown. |
| `ctl.0x12_data_mode` | 0x12 data mode | done | `runtime/wii_remote.c:95` |  |
| `ctl.0x13_0x1a_ir_enable` | 0x13, 0x1A IR enable | partial | `runtime/wii_remote.c:152` | 0x13 switches the camera flag in status reports, which WPAD checks before keeping the pointer on. 0x1A is a bare acknowledgement. The camera works from its register writes. |
| `ctl.0x14_speaker_enable` | 0x14 speaker enable | missing | `runtime/wii_remote.c:154` | Status reports carry the speaker flag, but no sound is played. Reported when switched on. |
| `ctl.0x15_status_request` | 0x15 status request | done | `runtime/wii_remote.c:101` |  |
| `ctl.0x16_write_memory` | 0x16 write memory | partial | `runtime/wii_remote.c:105` | Attachment space only. EEPROM, speaker and camera writes dropped. |
| `ctl.0x17_read_memory` | 0x17 read memory | partial | `runtime/wii_remote.c:103`, `:53`, `:65` | Calibration and attachment. Mii block reads zero. |
| `ctl.0x18_speaker_data` | 0x18 speaker data | missing | `runtime/wii_remote.c:112` | Acks every packet. A real one doesn't unless asked. |
| `ctl.0x19_speaker_mute` | 0x19 speaker mute | missing | `runtime/wii_remote.c:112` | Bare ack. |
| `ctl.0x20_status` | 0x20 status | partial | `runtime/wii_remote.c:36` | Battery full, attachment always in. No LED bits. |
| `ctl.0x21_read_reply` | 0x21 read reply | done | `runtime/wii_remote.c:65` |  |
| `ctl.0x22_ack` | 0x22 ack | done | `runtime/wii_remote.c:44` |  |
| `ctl.0x30_core` | 0x30 core | done | `runtime/wii_remote.c:22` |  |
| `ctl.0x31_core_motion` | 0x31 core, motion | partial | `runtime/wii_remote.c:125` | Motion fixed at rest. |
| `ctl.0x32_core_attachment_8` | 0x32 core, attachment 8 | done | `runtime/wii_remote.c:22` |  |
| `ctl.0x33_core_motion_ir_12` | 0x33 core, motion, IR 12 | partial | `runtime/wii_remote.c:22`, `runtime/wii_camera.c` | IR from the camera. Motion fixed at rest. |
| `ctl.0x34_core_attachment_19` | 0x34 core, attachment 19 | done | `runtime/wii_remote.c:22` |  |
| `ctl.0x35_core_motion_attachment_16` | 0x35 core, motion, attachment 16 | partial | `runtime/wii_remote.c:22` | Motion fixed. |
| `ctl.0x36_core_ir_10_attachment_9` | 0x36 core, IR 10, attachment 9 | done | `runtime/wii_remote.c:22`, `runtime/wii_camera.c` | IR zero. |
| `ctl.0x37_core_motion_ir_10_attachment_6` | 0x37 core, motion, IR 10, attachment 6 | partial | `runtime/wii_remote.c:22`, `runtime/wii_camera.c` | IR from the camera. Motion fixed at rest. |
| `ctl.0x3d_attachment_21` | 0x3D attachment 21 | done | `runtime/wii_remote.c:31` | 21 attachment bytes and no buttons. |
| `ctl.0x3e_0x3f_interleaved` | 0x3E, 0x3F interleaved | on demand | `runtime/wii_remote.c:121` | Needs the camera's full mode. Reported when asked for. |
| `ctl.report_timing` | Report timing | done | `runtime/wii_remote.c:13`, `:117` | 100 a second. |

### Remote parts and attachments

| ID | Item | Status | Our code | Notes |
|---|---|---|---|---|
| `ctl.buttons` | Buttons | done | `runtime/wii_remote.c:30` |  |
| `ctl.accelerometer` | Accelerometer | missing | `runtime/wii_remote.c:125-129` | Fixed. Shakes and tilt aren't possible. |
| `ctl.ir_camera` | IR camera | done | `runtime/wii_camera.c`, `runtime/wii_remote.c:151-156` | Basic and extended modes from the host pointer. 0xFF when off. Full mode is the 0x3E/0x3F row. |
| `ctl.speaker` | Speaker | missing |  |  |
| `ctl.eeprom` | EEPROM | partial | `runtime/wii_remote.c:53` | Calibration only. Mii storage missing. |
| `ctl.several_remotes` | Several remotes | partial | `runtime/bluetooth.h:8`, `runtime/bluetooth.c:66-77` | Five device slots. Only remote 0 and the board are present. |
| `ctl.attachment_wire_protocol` | Attachment wire protocol | done | `runtime/wii_attachment.c:58-72` |  |
| `ctl.attachment_encryption` | Attachment encryption | done | `runtime/wii_attachment_key.c` | Tables made from the key the game writes. |
| `ctl.attachment_hot_plug` | Attachment hot plug | on demand |  | Plugging in is the player's doing, not a game's request, so it needs a host control from the controllers stage. |
| `ctl.classic_controller` | Classic Controller | done | `runtime/wii_attachment.c:76` | Sticks need a host source (`input.c:99`). |
| `ctl.nunchuk` | Nunchuk | missing |  | Common. |
| `ctl.guitar_drums_turntable` | Guitar, Drums, Turntable | on demand |  | Few titles. Built when one is tested. |
| `ctl.taiko_drum_tatacon` | Taiko drum (TaTaCon) | on demand |  | Few titles. Built when one is tested. |
| `ctl.udraw_drawsome_tablets` | uDraw, Drawsome tablets | on demand |  | Few titles. Built when one is tested. |
| `ctl.shinkansen_controller` | Shinkansen controller | not needed |  | One game in Japan. |
| `ctl.wii_motionplus` | Wii MotionPlus | missing | `runtime/wii_remote.c:76` | Reports no device. |
| `ctl.balance_board_over_bluetooth` | Balance Board over Bluetooth | done | `runtime/wii_board.c`, `runtime/bluetooth.c`, `runtime/remote_link.c` | A real board takes slot 5, found by name or by its attachment ID `04 02`. Otherwise the simulated board, in games with a calibration flag. |
| `ctl.real_wii_remotes_wiimotereal` | Real Wii Remotes (`WiimoteReal`) | partial | `runtime/remote_link.c`, `remote_link_linux.c`, `remote_link_mac.c` | Remotes and the board pass their reports straight to the game's own code. Linux uses raw L2CAP sockets, the Mac IOKit's HID. Tested with a loopback stand-in and fake sockets; not yet with a real remote. |
| `ctl.passthrough_bluetooth_adapter` | Passthrough Bluetooth adapter | not needed |  |  |
