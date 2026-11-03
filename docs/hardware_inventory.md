# Wii hardware inventory

Every part of the Wii, and how much of it `recomp/` models. Dolphin's source (`master`, read 2026-11-02) and WiiBrew were the map. The wording here is our own.

This came from reading the code. Nothing was built or run, so `done` means the code reads correctly, not that a game has proved it. Paths are under `recomp/`. Line numbers are from 2026-11-02, so they may drift.

Statuses:

- `done`: behaves as a game needs.
- `partial`: works in part. The note says what's missing.
- `missing`: no code, or a silent no-op.
- `not needed`: the note says why.
- `unknown`: couldn't be settled by reading.

## Summary

| Area | Done | Partial | Missing | Unknown | Not needed | Total |
|---|---|---|---|---|---|---|
| 1. CPU | 52 | 26 | 26 | 1 | 15 | 120 |
| 2. Graphics commands and registers | 55 | 15 | 30 | 0 | 10 | 110 |
| 3. TEV and pixel features | 33 | 10 | 33 | 0 | 0 | 76 |
| 4. Vertex and texture | 35 | 10 | 17 | 2 | 2 | 66 |
| 5. Hardware blocks | 9 | 9 | 32 | 0 | 15 | 65 |
| 6. Audio microcodes | 9 | 3 | 7 | 0 | 3 | 22 |
| 7. IOS | 17 | 11 | 31 | 0 | 6 | 65 |
| 8. Controllers | 23 | 15 | 25 | 0 | 2 | 65 |
| **All** | **233** | **99** | **201** | **3** | **53** | **589** |

## Biggest gaps

Ordered by how likely a real game is to hit them.

1. **The picture always comes from `R0`.** The chip shows the last stage's result, whichever register it wrote. We read `R0` (`gx_tev.c:378`), so a material ending in another register comes out as the stale `R0`, often black with alpha 0. The alpha test then discards it too.
2. **Only the DOL is translated.** `dispatch` stops on any address it has no code for (`main.c:90`), so `.rel` modules can't load. Many Wii games keep most of their code in them.
3. **Palette offsets use the wrong unit.** We use 32 bytes per step and the chip uses 512 (`gx_texture.c:22`, `:151`). Palettes in neighbouring regions overlap, which gives wrong colors, black or holes.
4. **No fog at all.** The BP registers are stored and nothing reads them. Most 3D games use fog.
5. **Real mipmaps and texture sampling settings are ignored.** Stored mip levels, LOD bias, LOD clamp and minify filter are all dropped (`gx_draw.c:66-71`, `gx_gl.c:253`).
6. **Lines and points never draw** (`gx_draw.c:151-154`).
7. **EFB copies never reach memory** (`gx_copy.c:53-64`), and a copy keeps shadowing its address after the game reuses that memory (`gx_draw.c:77`, `gx_copy.c:66-71`). The CPU reads black, and new textures show old pictures.
8. **Logic ops are ignored** (`gx_draw.c:103-126`).
9. **TEV compare modes are mostly missing.** Only the 8-bit modes exist (`gx_tev.c:315-323`).
10. **A stage with its texture off reads black.** The chip gives white when texture coordinates are enabled (`gx_tev.c:297-298`).
11. **Viewport depth range is ignored** (`gx_draw.c:135-140`). Anything relying on near and far outside 0 to 1 sorts wrongly.
12. **EFB peek and poke, bounding box and field rendering are missing.** Pokes also write into the shadow of real hardware registers (`hardware.c:43`).
13. **`/dev/net`, `/dev/aes`, `/dev/sha`, `/dev/sdio` answer "fine" and do nothing** (`devices.c:34-36`). Every other `/dev/usb` device never answers (`devices.c:30`), so a blocking call hangs.
14. **`ES` is two requests** (`es.c:9`). Downloaded content, title launch, return to menu and tickets all get zeros.
15. **Only one audio program exists.** AX in its newer command format is the only one (`ax.c:10-12`). Zelda-family games (Mario Galaxy, Twilight Princess) and older AX (Wii Sports PAL) are silent.
16. **GameCube controllers, memory cards and the EXI clock are absent.** SI has no code, and EXI only clears a busy bit (`hardware.c:163`).
17. **`lswi`, `lswx`, `stswi`, `stswx` stop the game** (`main.c:29-32`). Some compilers use them for small block copies.
18. **`setjmp`, C++ exceptions and fibers are at risk.** `blr` is a host `return` (`emit.py:465`), so jumping stacks leaves the host stack out of step.
19. **Wii Remote input is thin.** No IR, speaker, Nunchuk or MotionPlus, and a single remote.
20. **Rounding mode and FPSCR writes do nothing** (`emit.py:380-381`).

## 1. CPU

Dolphin lists 223 instruction handlers in `Interpreter_Tables.cpp`. A script compared those with `decode.py` and with what `emit()` returns. `decode.py` knows 220 names. It lacks `dcba`, `ecowx` and `lswx`. `emit()` has nothing for `eciwx`, `lswi`, `mfsrin`, `stswi` and `stswx`. All eight stop the game at that line if reached (`main.c:29-32`).

### Integer

| Item | Status | Our code | Notes |
|---|---|---|---|
| `addi addis addic addic. subfic mulli` | done | `translate/emit.py:200-211` | |
| `add subf neg mullw mulhw mulhwu` | done | `translate/emit.py:269-282` | Rc sets CR0. |
| Carry forms `addc adde addze addme subfc subfe subfze subfme` | done | `translate/emit.py:276-279`, `runtime/cpu.h:124` | |
| Overflow forms (`addo` and others, OE=1) | partial | `translate/decode.py:80` | Decoded as the plain form. OV is never set, and SO only changes through `mtxer`. |
| `divw` | partial | `translate/emit.py:274` | Divide by zero and overflow give 0. Dolphin gives all ones for a negative dividend. |
| `divwu` | done | `translate/emit.py:275` | |
| `cmp cmpi cmpl cmpli` | done | `translate/emit.py:212-219` | |
| `and or xor andc orc nor nand eqv`, immediates | done | `translate/emit.py:220-224`, `:283-293` | |
| `extsb extsh cntlzw` | done | `translate/emit.py:289-290` | |
| `rlwinm rlwnm rlwimi` | done | `translate/emit.py:225-234` | |
| `slw srw sraw srawi` | done | `translate/emit.py:287-302` | |
| `crand crandc creqv crnand crnor cror crorc crxor mcrf` | done | `translate/emit.py:332-339` | |
| `mfcr mtcrf` | done | `translate/emit.py:327-331` | |
| `mcrxr` | partial | `translate/emit.py:334` | OV always reads 0. |

### Branches and system

| Item | Status | Our code | Notes |
|---|---|---|---|
| `b bl bc bcl` | done | `translate/emit.py:448-459` | |
| `bclr` | partial | `translate/emit.py:460-465` | `blr` is a host `return`, assuming calls and returns pair up. Code that sets LR by hand or switches stacks goes wrong: `setjmp`, C++ unwinding, `OSSwitchFiber`. |
| `bcctr bcctrl` | done | `translate/emit.py:466-471` | Target needs a function entry. `functions.py:73-104` finds them. |
| Jump tables and vtables built at run time | partial | `translate/functions.py:122` | Needs `--all-code`. |
| `rfi` | partial | `translate/emit.py:474` | Copies SRR1 whole. Exception entry and return is handled by `threads.c`, not by this path. |
| `sc` | missing | `translate/emit.py:473`, `runtime/main.c:60` | No-op. |
| `tw twi` | missing | `translate/emit.py:102` | No-op, so traps never fire. |
| Code loaded at run time (`.rel`, `OSLink`) | missing | `runtime/main.c:90` | Stops with "no translated code". |
| Self-modifying code, `icbi` | missing | `translate/emit.py:102` | Static translation can't follow it. |
| Unknown instruction in reachable code | done | `runtime/main.c:29-32` | Stops with its name, which is the right failure. |

### Loads and stores

| Item | Status | Our code | Notes |
|---|---|---|---|
| `lbz lhz lha lwz` and `u x ux` forms | done | `translate/emit.py:235-246` | |
| `stb sth stw` and `u x ux` forms | done | `translate/emit.py:239-250` | |
| `lmw stmw` | done | `translate/emit.py:251-254` | |
| `lhbrx lwbrx sthbrx stwbrx` | done | `translate/emit.py:255-262` | |
| `lwarx stwcx.` | done | `translate/emit.py:263-266` | No reservation and always succeeds. Fine on one thread, as interrupts only land at polls. |
| `lswi` | missing | `translate/decode.py:43` | Decoded, not emitted. |
| `lswx` | missing | | Not decoded. |
| `stswi stswx` | missing | `translate/decode.py:44` | Decoded, not emitted. |
| `eciwx` | missing | `translate/decode.py:40` | Decoded, not emitted. |
| `ecowx` | missing | | Not decoded. |
| Misaligned access | done | `runtime/cpu.h:67-80` | `memcpy`, so works. No alignment exception. |

### Cache instructions

| Item | Status | Our code | Notes |
|---|---|---|---|
| `dcbf dcbst dcbt dcbtst` | not needed | `translate/emit.py:102` | Memory is flat, with no cache to manage. |
| `dcbi` | not needed | `translate/emit.py:102` | Real one drops unwritten data. No game should rely on that. |
| `dcbz` | done | `translate/emit.py:340` | |
| `dcbz_l` | done | `translate/emit.py:340` | Zeroes through normal writes, so also reaches the locked cache. |
| `dcba` | missing | | Not decoded. Hint only, so should be a no-op. |
| `icbi` | not needed | `translate/emit.py:102` | Only matters with generated code, listed above. |
| `sync isync eieio` | not needed | `translate/emit.py:102` | One host thread runs guest code. |

### Scalar floats

| Item | Status | Our code | Notes |
|---|---|---|---|
| `lfd lfdu lfdx lfdux` | done | `translate/emit.py:107-116` | |
| `stfd stfdu stfdx stfdux` | done | `translate/emit.py:119-123` | |
| `lfs lfsu lfsx lfsux` | partial | `translate/emit.py:108`, `runtime/cpu.h:100` | The C cast quiets signaling NaNs. Data moved with `lfs`/`stfs`, such as a color from `0xff800001` to `0xffbfffff`, comes out changed. Dolphin converts by hand. |
| `stfs stfsu stfsx stfsux` | partial | `translate/emit.py:123` | Same cast. |
| `stfiwx` | done | `translate/emit.py:359` | |
| `fadd fsub fmul fdiv` | partial | `translate/emit.py:126-141` | Results right. No flags and no Rc. |
| `fmadd fmsub fnmadd fnmsub` | partial | `translate/emit.py:135-136` | Not fused (`-ffp-contract=off`), so the last bit can differ. No flags. |
| `fadds fsubs fmuls fdivs` | partial | `translate/emit.py:129-140` | Double then cast to float. No 25-bit rounding of the multiplier (Dolphin's `Force25Bit`), so low bits can differ. Writes both lanes, as hardware does. |
| `fmadds fmsubs fnmadds fnmsubs` | partial | `translate/emit.py:129-140` | As above. |
| `fres` | partial | `translate/emit.py:365` | Exact, where hardware uses a table estimate. Doesn't fill ps1, where Dolphin fills both. |
| `frsqrte` | partial | `translate/emit.py:366` | Exact, not the estimate. Only ps0 is written, which matches Dolphin. |
| `frsp` | partial | `translate/emit.py:365` | Doesn't fill ps1, where Dolphin fills both. |
| `fsel` | done | `translate/emit.py:367` | Negative zero counts as not negative, as on hardware. No Rc. |
| `fmr fneg fabs fnabs` | done | `translate/emit.py:364` | No Rc. |
| `fctiw` | partial | `translate/emit.py:371-373` | Always rounds to nearest. Top word is 0 where hardware leaves a marker. Only `stfd` of the result would show that. |
| `fctiwz` | done | `translate/emit.py:372` | Saturates through `to_int`. Top word as above. |
| `fcmpu fcmpo` | partial | `translate/emit.py:374`, `runtime/cpu.h:111` | CR right. FPCC and invalid flags aren't set. |
| `fsqrt fsqrts` | not needed | | Not in Gekko or in Dolphin's table. |

### FPSCR and rounding

| Item | Status | Our code | Notes |
|---|---|---|---|
| `mffs` | partial | `translate/emit.py:376` | Returns the stored value. Status bits never change. |
| `mtfsf` | partial | `translate/emit.py:378` | Ignores the field mask and Rc. Nothing acts on the value. |
| `mtfsb0 mtfsb1 mtfsfi mcrfs` | missing | `translate/emit.py:380-381` | No-ops. `mcrfs` leaves the CR field stale. |
| Rounding mode (RN) | missing | | Host stays on round-to-nearest. |
| Non-IEEE mode (NI) | missing | | Dolphin flushes denormals. Could matter to code expecting that. |
| Status flags (FPRF, FR, FI, FX and others) | missing | | Never computed. |
| Rc on float instructions (CR1) | missing | `translate/emit.py:138` | The bit is ignored. Compilers rarely set it. |

### Paired singles

| Item | Status | Our code | Notes |
|---|---|---|---|
| `ps_add ps_sub ps_mul ps_div` | partial | `translate/emit.py:144-159` | Rounded to float per lane. No 25-bit rule, flags or Rc. |
| `ps_madd ps_msub ps_nmadd ps_nmsub` | partial | `translate/emit.py:144-159` | As above. |
| `ps_res ps_rsqrte` | partial | `translate/emit.py:148-149` | Exact, not the estimate. Both lanes written. |
| `ps_sum0 ps_sum1` | done | `translate/emit.py:169-170` | |
| `ps_muls0 ps_muls1 ps_madds0 ps_madds1` | done | `translate/emit.py:171-174` | Lanes checked against Dolphin. |
| `ps_merge00 01 10 11` | done | `translate/emit.py:165-168` | |
| `ps_mr ps_neg ps_abs ps_nabs` | done | `translate/emit.py:161-164` | |
| `ps_sel` | done | `translate/emit.py:175` | |
| `ps_cmpu0 ps_cmpo0 ps_cmpu1 ps_cmpo1` | done | `translate/emit.py:180-182` | CR only. |
| `psq_l psq_lu psq_lx psq_lux` | done | `translate/emit.py:186-193`, `:384-392`, `runtime/quantized.c:39` | u8, s8, u16, s16 and float, with scale. |
| `psq_st psq_stu psq_stx psq_stux` | done | `runtime/quantized.c:46` | Clamps. Truncates, as Dolphin does. |
| GQR registers | done | `runtime/quantized.c:40`, `:47` | Saved with thread contexts. |
| HID2 gates (paired singles enable, quantize enable) | not needed | | Always on. A game that relies on the fault wouldn't be normal. |

### Special registers

| Item | Status | Our code | Notes |
|---|---|---|---|
| LR, CTR | done | `translate/emit.py:305`, `:312-315` | |
| XER | partial | `translate/emit.py:305`, `:316` | SO and CA only. No OV or byte count, which `lswx` would need. |
| CR | done | `runtime/cpu.h:11` | |
| MSR | partial | `translate/emit.py:323-326` | Stored. Only EE has any effect (`interrupts.c:94`). |
| SRR0, SRR1 | done | `runtime/threads.c:116-117` | |
| SPRG0-3 | done | `runtime/cpu.h:13` | Plain storage. |
| DEC | done | `runtime/interrupts.c:71-76` | Runs on host time. |
| TBL, TBU reads, `mftb` | done | `translate/emit.py:306`, `:321` | |
| TBL, TBU writes (SPR 284, 285) | missing | `translate/emit.py:320` | Stored, with no effect, so `OSSetTime` wouldn't change the clock. |
| HID0, HID1, HID2, HID4 | partial | `translate/emit.py:320` | Stored, no effect. |
| WPAR | partial | `translate/emit.py:320` | Stored. The pipe works off its fixed address (`gx_fifo.c:430`). A pipe at another address isn't modelled. |
| DMAU, DMAL | done | `translate/emit.py:318`, `runtime/hardware.c:127` | Transfers finish at once. |
| PVR, ECID | unknown | `translate/emit.py:307` | Nothing sets them, so they read 0. Whether a game cares isn't known. |
| L2CR, THRM, ICTC | not needed | | Stored. No cache or thermal model. |
| PMC, MMCR, UMMCR | missing | | Stored. Counters stay still, which affects profiling code only. |
| IABR, DABR | not needed | | Debug only. |
| EAR | missing | | Only `eciwx`/`ecowx` use it. |
| DAR, DSISR | missing | | No faults raise them. |
| SDR1, BAT registers | not needed | `translate/emit.py:320` | Stored, ignored. See below. |

### MMU and exceptions

| Item | Status | Our code | Notes |
|---|---|---|---|
| Standard map (0x8, 0x9, 0xC, 0xD) | done | `runtime/cpu.h:56-61` | Top bits are masked, so cached and uncached views match. |
| Game-defined BAT maps | missing | `runtime/cpu.h:57` | Writes ignored. |
| Page tables, segment registers | missing | `translate/emit.py:103`, `:382` | `mtsr` no-op, `mfsr` reads 0, `mfsrin` stops. A few games use virtual memory. |
| `tlbie tlbsync` | not needed | `translate/emit.py:102` | No TLB. |
| Real-mode entry | not needed | `translate/system_functions.json` | Replaced by signature. |
| MEM1 24 MB, MEM2 64 MB | done | `runtime/cpu.h:53-54` | |
| External interrupt | done | `runtime/interrupts.c:95-97` | Polled every 2,000 checks. |
| Decrementer | done | `runtime/interrupts.c:98-102` | |
| System call | missing | `runtime/main.c:60` | |
| Program (trap, illegal) | missing | | |
| Floating-point unavailable | not needed | | Floats are always on. |
| Floating-point program | missing | | No flags to raise it. |
| DSI, ISI | missing | | Bad addresses go to `hw_read` and read shadow registers, silently. |
| Alignment | not needed | | Accesses just work. |
| Machine check, reset | missing | | |
| Performance monitor, thermal, breakpoint | not needed | | |
| Thread contexts | done | `runtime/threads.c` | Replaces `OSSaveContext` and `OSLoadContext`. At most 256 (`threads.c:18`). |

### Locked cache

| Item | Status | Our code | Notes |
|---|---|---|---|
| 16 KB at 0xE0000000 | done | `runtime/hardware.c:111-123` | Reached through `hw_read`, so slower than memory. |
| Enable bit (HID2 LCE) | not needed | | Always on. |
| DMA between memory and locked cache | done | `runtime/hardware.c:127-139` | |
| DMA queue length in HID2 | done | | Always reads 0, which is right as DMA finishes at once. |

## 2. Graphics commands and registers

### FIFO commands

| Item | Status | Our code | Notes |
|---|---|---|---|
| NOP 0x00 | done | `runtime/gx_fifo.c:138` | |
| Load CP 0x08 | done | `runtime/gx_fifo.c:144` | |
| Load XF 0x10 | done | `runtime/gx_fifo.c:149` | |
| Indexed XF A-D 0x20-0x38 | done | `runtime/gx_fifo.c:128`, `:156` | Read from memory at parse time. On Linux that's on another thread, so it can run later than the CPU expects. |
| Call display list 0x40 | done | `runtime/gx_fifo.c:161` | No depth limit. |
| Metrics 0x44 | missing | `runtime/gx_fifo.c:183` | Logged as unknown, one byte skipped. |
| Invalidate vertex cache 0x48 | not needed | `runtime/gx_fifo.c:138` | No cache. |
| Load BP 0x61 | done | `runtime/gx_fifo.c:139` | Masks handled at `:82-88`. |
| Draw quads, quads2, triangles, strip, fan | done | `runtime/gx_fifo.c:167`, `gx_draw.c:151` | |
| Draw lines, line strip | missing | `runtime/gx_draw.c:151-154` | Dropped. |
| Draw points | missing | `runtime/gx_draw.c:151-154` | Dropped. |
| Draw larger than the 4 MB stream | missing | `runtime/gx_fifo.c:171` | Would wait forever. Very unlikely. |

### CP registers

| Item | Status | Our code | Notes |
|---|---|---|---|
| 0x20 performance select | missing | `runtime/gx_fifo.c:146` | Stored only. |
| 0x30, 0x40 matrix index | done | `runtime/gx_vertex.c:145` | We use the CP copy. Dolphin warns when CP and XF differ, and we'd follow CP. |
| 0x50, 0x60 vertex descriptor | done | `runtime/gx_vertex.c:29` | |
| 0x70-0x77 format A | partial | `runtime/gx_vertex.c:40-48` | Byte dequant (bit 30) and normal index 3 (bit 31) are ignored. |
| 0x80-0x87 format B | done | `runtime/gx_vertex.c:49-59` | |
| 0x90-0x97 format C | done | `runtime/gx_vertex.c:49-59` | |
| 0xA0-0xAB array bases | done | `runtime/gx_vertex.c:137` | |
| 0xAC-0xAF matrix array bases | done | `runtime/gx_fifo.c:131` | |
| 0xB0-0xBF strides | done | `runtime/gx_vertex.c:137` | |

### XF memory and registers

| Item | Status | Our code | Notes |
|---|---|---|---|
| 0x000-0x0FF position and texture matrices | done | `runtime/gx_transform.c:132` | |
| 0x400-0x45F normal matrices | done | `runtime/gx_transform.c:110` | Index isn't masked to 31 as on Dolphin, so an index above 31 reads past the real memory. |
| 0x500-0x5FF post-transform matrices | done | `runtime/gx_transform.c:129` | |
| 0x600-0x67F lights | done | `runtime/gx_light.c:51` | |
| 0x1000 error, 0x1001 diagnostics | not needed | | |
| 0x1002-0x1004 state, clock | not needed | | |
| 0x1005 clip disable | missing | `runtime/gx_fifo.c:153` | Stored only. |
| 0x1006, 0x1007 performance | missing | | |
| 0x1008 vertex spec | not needed | | The CP descriptor decides. Dolphin also checks they agree. |
| 0x1009 color count | missing | `runtime/gx_transform.c:65` | Ignored. With 0 or 1, the unused channel should read 0 at TEV. Ours computes it anyway. |
| 0x100A-B ambient | done | `runtime/gx_light.c:46` | |
| 0x100C-D material | done | `runtime/gx_light.c:45` | |
| 0x100E-F color channel control | done | `runtime/gx_light.c:28-41` | |
| 0x1010-11 alpha channel control | done | `runtime/gx_light.c:40` | |
| 0x1012 post-transform enable | done | `runtime/gx_transform.c:58` | |
| 0x1018-19 matrix index | not needed | | Written beside the CP copy, which we read. |
| 0x101A-1C viewport size | done | `runtime/gx_draw.c:135` | |
| 0x101D-1F viewport origin and depth | partial | `runtime/gx_draw.c:137` | Assumes origin 342 rather than the scissor offset. Depth range and far are dropped. |
| 0x1020-26 projection | done | `runtime/gx_transform.c:72` | |
| 0x103F texture coordinate count | done | `runtime/gx_transform.c:58` | |
| 0x1040-47 texture matrix info | partial | `runtime/gx_transform.c:29-42` | Input form (bit 2) and emboss fields are ignored. |
| 0x1050-57 post matrix info | done | `runtime/gx_transform.c:41`, `:127` | |

### BP registers

| Item | Status | Our code | Notes |
|---|---|---|---|
| 0x00 gen mode | partial | `runtime/gx_tev.c:182-183`, `gx_draw.c:125` | Stages, indirect count, cull. Texture and color counts, multisample and z-freeze ignored. |
| 0x01-0x04 copy filter samples | missing | | |
| 0x06-0x0E indirect matrices | done | `runtime/gx_tev.c:140` | |
| 0x0F indirect texture mask | not needed | | Cache hint. Dolphin only stores it. |
| 0x10-0x1F indirect stage commands | partial | `runtime/gx_tev.c:66-85` | See area 3. |
| 0x20-0x21 scissor | done | `runtime/gx_draw.c:142` | |
| 0x22 line and point width | missing | | No lines or points. |
| 0x23-0x24 performance | missing | | |
| 0x25-0x26 indirect scale | done | `runtime/gx_tev.c:79` | |
| 0x27 indirect sources | done | `runtime/gx_tev.c:79` | |
| 0x28-0x2F stage order | done | `runtime/gx_tev.c:93` | |
| 0x30-0x3F texture coordinate scale and bias | missing | | Includes cylindrical wrap and line and point offsets. Normal texturing doesn't need the size part. |
| 0x40 z mode | done | `runtime/gx_draw.c:120-122` | |
| 0x41 blend mode | partial | `runtime/gx_draw.c:103-124` | Logic op and dither ignored. |
| 0x42 constant alpha | done | `runtime/gx_tev.c:199` | |
| 0x43 pixel format and z control | partial | `runtime/gx_fifo.c:123` | Only "is depth format". Early z, z format and format conversion ignored. |
| 0x44 field mask | missing | | |
| 0x45 draw done | done | `runtime/gx_fifo.c:93` | Delayed signal. |
| 0x46, 0x69 bus clock | not needed | | |
| 0x47 token | partial | `runtime/gx_fifo.c:97`, `:99` | We raise the signal here. Dolphin raises nothing on 0x47, and stores the token only. |
| 0x48 token with interrupt | partial | `runtime/gx_fifo.c:97` | Value stored, no signal. Dolphin raises here. Works when the SDK writes both, as it writes 0x47 second. A game writing only 0x48 gets no interrupt. |
| 0x49-0x4A copy rectangle | done | `runtime/gx_copy.c:45` | |
| 0x4B copy target | done | `runtime/gx_copy.c:60` | |
| 0x4D copy stride | missing | | Copies never reach memory. |
| 0x4E copy Y scale | missing | | Ignored. PAL and field copies get the wrong size. |
| 0x4F-0x51 clear color and depth | done | `runtime/gx_draw.c:202-209` | |
| 0x52 trigger copy | partial | `runtime/gx_fifo.c:106-125` | See area 3. |
| 0x53-0x54 vertical copy filter | missing | | |
| 0x55-0x56 bounding box | missing | | |
| 0x57 clear pixel performance | missing | | |
| 0x58 rev bits | not needed | | Dolphin ignores it too. |
| 0x59 scissor offset | done | `runtime/gx_draw.c:144` | |
| 0x60-0x63 texture preload | missing | | |
| 0x64 TLUT source | done | `runtime/gx_texture.c:21` | |
| 0x65 TLUT load | partial | `runtime/gx_texture.c:20-25` | Offset unit wrong: `<<5`, should be `<<9`. |
| 0x66 texture invalidate | not needed | | We hash textures instead. |
| 0x67 performance | missing | | |
| 0x68 field mode | missing | | |
| 0x80-0x83, 0xA0-0xA3 texture mode 0 | partial | `runtime/gx_draw.c:66-71` | Wrap and magnify only. |
| 0x84-0x87, 0xA4-0xA7 texture mode 1 | missing | | LOD limits. |
| 0x88-0x8B, 0xA8-0xAB image size and format | done | `runtime/gx_texture.c:143` | |
| 0x8C-0x93, 0xAC-0xB3 TMEM layout | not needed | | Only matters with preload. |
| 0x94-0x97, 0xB4-0xB7 image address | done | `runtime/gx_texture.c:144` | |
| 0x98-0x9B, 0xB8-0xBB TLUT setting | partial | `runtime/gx_texture.c:145`, `:151` | Same offset bug. |
| 0xC0-0xDF stage combiners | partial | `runtime/gx_tev.c:92-108` | See area 3. |
| 0xE0-0xE7 color and konst registers | done | `runtime/gx_fifo.c:89` | |
| 0xE8-0xED fog range | missing | | |
| 0xEE-0xF2 fog | missing | | |
| 0xF3 alpha test | done | `runtime/gx_tev.c:189` | |
| 0xF4 z texture bias | done | `runtime/gx_tev.c:175` | |
| 0xF5 z texture mode | done | `runtime/gx_tev.c:169` | |
| 0xF6-0xFD konst select and swap tables | done | `runtime/gx_tev.c:52`, `:93` | |
| 0xFE mask | done | `runtime/gx_fifo.c:82` | |

### CP and PE hardware registers

| Item | Status | Our code | Notes |
|---|---|---|---|
| CP status | partial | `runtime/gx_fifo.c:467` | Always idle, plus flags. |
| CP control | done | `runtime/gx_fifo.c:434-455` | |
| CP clear | done | `runtime/gx_fifo.c:435` | |
| CP FIFO base, end, read, write | done | `runtime/gx_fifo.c:317-358` | |
| CP high and low marks | done | `runtime/gx_fifo.c:303` | Only with `--fifo-breakpoints`. |
| CP breakpoint | done | `runtime/gx_fifo.c:294` | Only with `--fifo-breakpoints`, and paced for a Wii-like speed. |
| CP read-write distance | missing | | Plain register. |
| CP performance counters | missing | | |
| PE control and status | done | `runtime/gx_fifo.c:456`, `:468` | |
| PE token | done | `runtime/gx_fifo.c:469` | |
| PE z, alpha, destination alpha config | missing | `runtime/hardware.c:168` | Plain store. These change how CPU reads of the EFB behave. |
| PE alpha read mode | missing | | No peek. |
| PE bounding box 0x10-0x16 | missing | | |
| PE performance counters | missing | | |

## 3. TEV and pixel features

| Item | Status | Our code | Notes |
|---|---|---|---|
| Up to 16 stages | done | `runtime/gx_tev.c:182` | |
| Color inputs (all 16) | done | `runtime/gx_tev.c:224-229` | |
| Alpha inputs (all 8) | done | `runtime/gx_tev.c:231-235` | |
| Lerp with op, bias, scale | done | `runtime/gx_tev.c:319-326` | |
| 8-bit wrap of a, b, c inputs | partial | `runtime/gx_tev.c:319` | Floats, so a negative register in a, b or c doesn't wrap. No `c + (c >> 7)` weighting. |
| Clamp and 11-bit range | done | `runtime/gx_tev.c:328-329` | |
| Output register choice | done | `runtime/gx_tev.c:328` | |
| Final output from the last stage's register | missing | `runtime/gx_tev.c:378` | Always `R0`. Dolphin: "the results of the last texenv stage are put onto the screen, regardless of the used destination register". |
| Compare R8 greater, equal | done | `runtime/gx_tev.c:315-317` | |
| Compare GR16 | missing | `runtime/gx_tev.c:315` | Scale field ignored, so treated as R8. |
| Compare BGR24 | missing | `runtime/gx_tev.c:315` | |
| Compare RGB8 per channel | missing | `runtime/gx_tev.c:315` | |
| Alpha compare A8 | done | `runtime/gx_tev.c:322` | |
| Alpha compare R8, GR16, BGR24 | missing | `runtime/gx_tev.c:322` | Always compares alpha. |
| Konst colors and fractions | done | `runtime/gx_tev.c:241-252` | |
| Swap tables, raster and texture | done | `runtime/gx_tev.c:52`, `:117`, `:124` | |
| Stage with texture off | partial | `runtime/gx_tev.c:297-298` | Always black, alpha 1. Dolphin: white if the game has texture coordinates, all zero if it has none. |
| Stage texture with no coordinates enabled | missing | `runtime/gx_tev.c:293` | Dolphin reads zero. We sample. |
| Raster channel zero | done | `runtime/gx_tev.c:301` | |
| Bump alpha raster (5) | missing | `runtime/gx_tev.c:123`, `:301` | Reads zero. |
| Normalized bump alpha raster (6) | missing | `runtime/gx_tev.c:123`, `:301` | Reads zero. |
| Indirect stage selection | done | `runtime/gx_tev.c:66-72` | Only when the stage's texture is on, which may be too strict. |
| Indirect fixed matrices | done | `runtime/gx_tev.c:140-149` | |
| Indirect dynamic S matrices (5-7) | missing | `runtime/gx_tev.c:71` | Comment at `:153-154` says so. Water and heat effects often use them. |
| Indirect dynamic T matrices (9-11) | missing | `runtime/gx_tev.c:71` | |
| Indirect formats 8, 5, 4, 3 bits | done | `runtime/gx_tev.c:273` | |
| Indirect bias | done | `runtime/gx_tev.c:275-279` | |
| Indirect bump alpha select | missing | | The `bs` field is never read. |
| Indirect wrap | done | `runtime/gx_tev.c:259-264` | |
| Indirect add previous | done | `runtime/gx_tev.c:282` | |
| Indirect LOD from unmodified coordinates | missing | `runtime/gx_tev.c:290` | Reads level 0. |
| Indirect coordinate scale | done | `runtime/gx_tev.c:83`, `:276` | |
| Alpha test, 8 compares, 4 logic joins | done | `runtime/gx_tev.c:332-340`, `:380` | Compares floats, with a small tolerance for equal. |
| Fog: linear, exp, exp2, backward exp, backward exp2 | missing | | No code. |
| Fog range adjustment | missing | | |
| Z compare functions | done | `runtime/gx_draw.c:120-122` | |
| Z write enable | done | `runtime/gx_draw.c:122` | |
| Z compare location (early z) | missing | | A shader with `discard` tests depth late. Early z with alpha test should still write depth where alpha fails. |
| Z-freeze | missing | | Decals and shadows use it. |
| Z texture off, add, replace | done | `runtime/gx_tev.c:168-176`, `:343-354` | |
| Z texture Z8, Z16, Z24 | done | `runtime/gx_tev.c:347-350` | |
| Z16 precision formats | missing | | Host depth always. Rarely visible. |
| Depth range | missing | `runtime/gx_draw.c:135-140` | |
| Blend factors | done | `runtime/gx_draw.c:95-118` | Uses dual-source alpha. |
| Subtract blend | done | `runtime/gx_draw.c:109` | |
| Logic ops (16) | missing | `runtime/gx_draw.c:103` | Falls back to replace. |
| Dither | missing | | |
| Color and alpha write masks | done | `runtime/gx_draw.c:123-124` | |
| Destination alpha (constant alpha) | done | `runtime/gx_tev.c:389`, `gx_draw.c:114` | |
| Target alpha reads on RGB8 EFB | partial | `runtime/gx_draw.c:95-101` | Hardware has no alpha in that format and reads 1, and writes are off. We keep alpha. |
| EFB RGB8_Z24 | partial | `runtime/gx_fifo.c:123` | Host RGBA8 with no alpha rules. |
| EFB RGBA6_Z24 | partial | | No 6-bit rounding. Often visible only as banding. |
| EFB RGB565_Z16 | partial | | No 16-bit rounding. |
| EFB Z24 | done | `runtime/gx_fifo.c:123` | As a depth copy source. |
| EFB Y8, U8, V8, YUV420 | missing | | Multisampled YUV forms. Very rare. |
| Reinterpreting EFB after a format change | missing | | |
| Multisample and AA modes | missing | | Sample positions and copy filter ignored. |
| EFB peek (CPU reads color, depth) | missing | `runtime/hardware.c:143` | Reads the register shadow. |
| EFB poke (CPU writes) | missing | `runtime/hardware.c:168` | Writes the same shadow, so an EFB offset can alias a real register such as PI mask. |
| Bounding box | missing | | Paper Mario and others rely on it. |
| Copy to texture | partial | `runtime/gx_copy.c:53-64` | Stays on the GPU, never in memory. |
| Copy formats R4, R8, RA4, RA8, RGB565, RGB5A3, RGBA8, A8, G8, B8 | done | `runtime/gx_copy.c:33-43` | By channel swizzle. Bit depth isn't reduced. |
| Copy formats RG8, GB8 | missing | `runtime/gx_copy.c:36` | Fall through to plain RGBA. |
| Depth copy | partial | `runtime/gx_copy.c:62` | Any depth copy reads as one value. Format not applied. |
| Copy half size | done | `runtime/gx_copy.c:56` | |
| Copy Y scale | missing | | |
| Copy clear | done | `runtime/gx_fifo.c:124` | |
| Copy intensity (RGB to YUV) | missing | | Bit 15. |
| Copy clamp top and bottom | missing | | |
| Copy filter (AA and deflicker) | missing | | |
| Gamma on XFB copy (1.0, 1.7, 2.2) | missing | | |
| Copy to XFB | partial | `runtime/gx_fifo.c:118`, `gx_draw.c:216` | Present directly. XFB memory unused. |
| Field copy (frame to field) | missing | | |
| Scissor | done | `runtime/gx_draw.c:142-149` | |
| Cull none, front, back, all | done | `runtime/gx_draw.c:125`, `gx_gl.c:212` | |
| Vulkan draw shortcuts | partial | `runtime/gx_vulkan.c:117`, `:662`, `:689` | Tuned on Wii Fit. Anything landing at y >= 456 runs before the scene, which is wrong for a game drawing real picture there. Cover skipping needs `WII_RECOMP_COVER`. |

## 4. Vertex and texture

### Vertex formats

| Item | Status | Our code | Notes |
|---|---|---|---|
| Direct, index8, index16 | done | `runtime/gx_vertex.c:129-138` | |
| Null index on position (0xFF, 0xFFFF) | missing | `runtime/gx_vertex.c:135-137` | Dolphin skips the vertex. We read `base + 0xFF * stride`. |
| Null index on other attributes | missing | `runtime/gx_vertex.c:135-137` | Reads past the array, as above. |
| Position XY, XYZ | done | `runtime/gx_vertex.c:40` | |
| Position U8, S8, U16, S16, F32 | done | `runtime/gx_vertex.c:94-103` | |
| Position fraction | done | `runtime/gx_vertex.c:95` | |
| Byte dequant flag | missing | `runtime/gx_vertex.c:95` | Bit 30 ignored, so fraction always applies. Games normally set it. |
| Position matrix index | done | `runtime/gx_vertex.c:157` | |
| Texture matrix indexes 0-7 | done | `runtime/gx_vertex.c:159` | |
| Normal N (3 values) | done | `runtime/gx_vertex.c:169-175` | |
| Normal NBT (9 values) | partial | `runtime/gx_vertex.c:43`, `:172` | Bytes skipped correctly, binormal and tangent discarded. |
| Normal NBT with 3 indexes | missing | `runtime/gx_vertex.c:80`, `:171` | Hardware reads three indexes. We read one, so the vertex size is wrong and every following vertex and command is garbage. |
| Normal S8, S16 | done | `runtime/gx_vertex.c:140` | |
| Normal U8, U16 | partial | `runtime/gx_vertex.c:140` | Scale should be 2^-7 and 2^-15. We use 2^-14 for both. The SDK doesn't normally allow these. |
| Normal F32 | done | `runtime/gx_vertex.c:101` | |
| Missing normal | partial | `runtime/gx_vertex.c:168`, `gx_transform.c:236` | Fixed (0, 0, 1). Hardware keeps the last normal loaded. |
| Color RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8 | done | `runtime/gx_vertex.c:105-126` | |
| Missing color | done | `runtime/gx_vertex.c:177` | White, which is Dolphin's default. |
| Color 1 present without color 0 | missing | `runtime/gx_vertex.c:176-181` | Should feed channel 0. |
| Texture coordinates S, ST in all formats | done | `runtime/gx_vertex.c:182-189` | |
| Texture coordinate fraction, per coordinate | done | `runtime/gx_vertex.c:49-59` | |
| Vertex cache enhance flag | not needed | | No cache. |

### Texture coordinate generation

| Item | Status | Our code | Notes |
|---|---|---|---|
| Regular 2x4 | done | `runtime/gx_transform.c:200-205` | |
| Regular 3x4 (projected) | partial | `runtime/gx_transform.c:204`, `:212` | Divides per vertex, so perspective across a triangle is wrong. Should send three values and divide per pixel. |
| Emboss | missing | `runtime/gx_transform.c:29-42` | Treated as regular, with no light offset. |
| Color 0, color 1 (SRTG) | done | `runtime/gx_transform.c:34-37`, `:191` | |
| Source position | done | `runtime/gx_transform.c:196` | |
| Source normal | done | `runtime/gx_transform.c:197` | |
| Source binormal, tangent | missing | `runtime/gx_transform.c:39` | Become (0, 0, 1, 1). |
| Source colors row | not needed | | Only for SRTG types. |
| Source texture coordinates 0-7 | done | `runtime/gx_transform.c:198` | |
| Input form AB11 | missing | `runtime/gx_transform.c:196` | Position always keeps its z. |
| Post-transform matrix | done | `runtime/gx_transform.c:206-211` | |
| Post-transform normalize | done | `runtime/gx_transform.c:207` | |
| Identity shortcut for matrix 60 | done | `runtime/gx_transform.c:202` | |

### Lighting

| Item | Status | Our code | Notes |
|---|---|---|---|
| 2 color and 2 alpha channels | done | `runtime/gx_light.c:114-118` | |
| Material from register or vertex | done | `runtime/gx_light.c:97-98` | |
| Ambient from register or vertex | done | `runtime/gx_light.c:104-105` | |
| 8 lights, masks | done | `runtime/gx_light.c:33`, `:106` | |
| Diffuse none, sign, clamp | done | `runtime/gx_light.c:76` | |
| Attenuation off | done | `runtime/gx_light.c:77` | |
| Attenuation directional (2) | done | `runtime/gx_light.c:77` | Same as off, as in Dolphin. |
| Attenuation specular | partial | `runtime/gx_light.c:79-83` | Dolphin normalizes the distance terms when diffuse is on. We don't, so brightness differs. |
| Attenuation spot | done | `runtime/gx_light.c:84-86` | |
| Zero denominator | unknown | `runtime/gx_light.c:69` | We return black. Dolphin divides by zero. Hardware result isn't known. |
| Color count limiting channels | missing | `runtime/gx_transform.c:65` | See XF 0x1009. |
| Integer rounding of light results | partial | `runtime/gx_light.c:108` | Floats. Small banding differences only. |

### Textures

| Item | Status | Our code | Notes |
|---|---|---|---|
| I4, I8, IA4, IA8 | done | `runtime/gx_texture.c:98-105` | |
| RGB565, RGB5A3, RGBA8 | done | `runtime/gx_texture.c:106-110` | |
| C4, C8, C14X2 | partial | `runtime/gx_texture.c:111-117` | Decoders right, palette offset wrong. |
| CMPR | done | `runtime/gx_texture.c:52-75` | Blend weights differ slightly from hardware. |
| TLUT formats IA8, RGB565, RGB5A3 | done | `runtime/gx_texture.c:43-49` | |
| TLUT load | partial | `runtime/gx_texture.c:20-25` | Offset unit. |
| Wrap clamp, repeat, mirror | done | `runtime/gx_draw.c:67` | |
| Magnify near, linear | done | `runtime/gx_draw.c:69` | |
| Minify near | missing | `runtime/gx_gl.c:253`, `gx_vulkan_pipeline.c:245` | Always linear. |
| Mip filter none, near, linear | missing | `runtime/gx_gl.c:253` | Always trilinear on textures. |
| Stored mip levels | missing | `runtime/gx_texture.c:141-171`, `gx_gl.c:240` | Level 0 only, and the host builds the rest. Fading or tinted mips are lost. |
| LOD bias | missing | | |
| LOD min and max | missing | | |
| Anisotropy | missing | | |
| Edge LOD | missing | | |
| TMEM preload | missing | | |
| Update detection | partial | `runtime/gx_draw.c:86`, `gx_texture.c:135` | Checked once per frame, on a sample of bytes. A texture changed mid-frame, or a small change, can be missed. |
| Copies as textures | partial | `runtime/gx_draw.c:77`, `gx_copy.c:66` | Exact address only. Never expires. |
| Non-power-of-two wrap | unknown | | Hardware requires powers of two for repeat. Behaviour off-spec isn't known. |

## 5. Hardware blocks

Registers with no entry below are kept in a plain array (`hardware.c:39`), so reads return what was written.

### VI (video)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Retrace timing | partial | `runtime/interrupts.c:23`, `:84-89` | Fixed 60 Hz. PAL 50 Hz games run at the wrong rate. |
| Display interrupts 0-3 | partial | `runtime/hardware.c:75-85` | All enabled ones fire together at retrace, ignoring their line settings. |
| Interrupt clear | done | `runtime/hardware.c:87-92` | |
| Beam position (0x2C, 0x2E) | missing | | Reads 0. |
| Framebuffer addresses (0x1C-0x2A) | missing | | Picture comes from EFB copy (`gx_fifo.c:118`). A game writing XFB by CPU shows nothing. |
| Timing registers (0x00, 0x04-0x1A) | not needed | | Host window sets the output. |
| Interlace, progressive, field order | missing | | |
| Horizontal scaler, filter (0x48-0x6C) | missing | | Stored only. |
| Display latch (0x40-0x46) | missing | | Light gun only. |
| Clock select, DTV status | missing | | Plain register. Progressive check would read 0. |
| VI solid color (Hollywood 0x24) | missing | | |

### PI (processor interface)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Interrupt cause | done | `runtime/hardware.c:144`, `:157` | |
| Interrupt mask | done | `runtime/hardware.c:72` | |
| FIFO base, end, write | done | `runtime/gx_fifo.c:311`, `:363` | |
| FIFO reset (0x18) | missing | | `GXAbortFrame`. |
| Error cause, address | not needed | | No bus errors. |
| Reset code | missing | | |
| Hardware revision | missing | | Reads 0. Low memory has its own copy (`loader.c:92`). |
| Reset button interrupt | missing | | Never raised. |
| HSP, debug interrupts | not needed | | |

### MI (memory interface)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Protection regions | not needed | | Only used to catch bugs. |
| Memory interrupt | not needed | | |
| Timer and performance registers | not needed | | |

### AI (audio interface)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Sample counter | done | `runtime/hardware.c:145` | Host time at 48 kHz, regardless of control register. |
| Control | partial | | Stored. Rate bits don't change anything. |
| Volume | missing | | Stored. |
| Interrupt timing | missing | | `IRQ_AI` is never raised. |
| Disc audio streaming | not needed | | GameCube discs only. |

### DSP

| Item | Status | Our code | Notes |
|---|---|---|---|
| Control register | done | `runtime/hardware.c:164-169` | |
| Audio DMA interrupt | done | `runtime/hardware.c:104`, `audio.c:115` | Fires on a 3 ms clock. |
| Audio DMA start, length | partial | `runtime/hardware.c:101` | Only the enable bit is read. Samples are never fetched from the address. Output comes straight from `ax.c`. |
| Audio DMA blocks left | missing | | Plain register. |
| Mail to DSP | partial | `runtime/audio.c:128`, `translate/system_functions.json` | SDK functions replaced by signature. Only the AX list mail works. |
| Mail from DSP | missing | | Registers are plain. Code talking to the DSP without the SDK path gets nothing. |
| DSP boot | partial | `translate/system_functions.json` | Replaced, so it's assumed ready. |
| DSP accelerator interrupt | missing | `runtime/hardware.c:31` | Bit known, never set. |
| Instruction RAM, ROM, real DSP code | not needed | | Replaced in C. |
| ARAM | not needed | | Wii has none. A GameCube-style game would need it. |
| AR DMA registers | not needed | | Same. |

### SI (serial interface)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Channels 0-3, poll | missing | | No code, so no GameCube controller. |
| Communication control, status | missing | | Transfers never finish. A game starting SI could wait. |
| I/O buffer | missing | | |
| SI interrupt | missing | | Never raised. |
| GC controller, wheel, keyboard, dance mat | missing | | |
| GBA link | not needed | | Almost no Wii use. |

### EXI (expansion interface)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Control busy bit | partial | `runtime/hardware.c:163` | Cleared at once on channels A, B and C. |
| Status, DMA address, length, immediate data | missing | | Plain registers. |
| EXI interrupt | missing | | `IRQ_EXI` is never raised. |
| RTC and SRAM (channel 0) | missing | | Reads return stale values. Time comes from the time base instead (`interrupts.c:62`). |
| GC memory card | missing | | |
| Microphone | missing | | |
| Ethernet adapter | missing | | |
| USB Gecko, modem, AD16 | not needed | | Debug or rare. |

### DVD (disc interface)

| Item | Status | Our code | Notes |
|---|---|---|---|
| Registers 0xCD006000 | not needed | | Wii games go through `/dev/di`. |
| Reads through extracted files | done | `runtime/dvd.c:110-136` | See area 7. |
| Disc image files (ISO, WBFS, RVZ) | missing | `runtime/dvd.c:70` | Extracted folder only. |
| Second disc layer, disc swap | missing | | |

### IPC and Hollywood

| Item | Status | Our code | Notes |
|---|---|---|---|
| Message, control, reply | done | `runtime/ios.c:96-130` | Requests run at once. |
| IRQ flags | partial | `runtime/ios.c:102` | Reports the IPC flag if any interrupt is pending. |
| IRQ mask | partial | | Plain store. Line assumed enabled. |
| Other Hollywood interrupts (timer, GPIO) | missing | | |
| Relaunch bit | missing | `runtime/ios.c:16` | Named, not acted on. No IOS reload. |
| PPC speed, VI clock, resets | not needed | | Plain store. |
| GPIO (disc slot light, sensor bar) | missing | | Plain store. Visible only as a missing light. |
| AHB protection | not needed | | |

## 6. Audio microcodes

The AX work is rewritten in C. There's no DSP interpreter, so each program needs its own version.

| Item | Status | Our code | Notes |
|---|---|---|---|
| AXWii, newer command format | partial | `runtime/ax.c` | Everything below. |
| AXWii setup, add, subtract, add-subtract | done | `runtime/ax.c:220`, `:230` | |
| AXWii voices | done | `runtime/ax.c:212` | |
| AXWii ADPCM, PCM16, PCM8 | done | `runtime/ax.c:81`, `:103-108` | |
| AXWii looping, streams | done | `runtime/ax.c:68` | |
| AXWii sample rate conversion | partial | `runtime/ax.c:115` | Cubic stand-in for the ROM's filter tables. |
| AXWii envelope, mixer ramps | done | `runtime/ax.c:147`, `:168` | |
| AXWii low-pass filter | done | `runtime/ax.c:157` | |
| AXWii biquad, high-pass | missing | `runtime/ax.c:158` | Filter value 2 means biquad, but any nonzero value runs our low-pass. Dolphin marks Raving Rabbids and Bleach as using the newer layout, whose block offsets we don't handle. |
| AXWii aux A, B, C | done | `runtime/ax.c:244` | |
| AXWii aux surround uploads (commands 8, 9) | missing | `runtime/ax.c:313` | Skipped. |
| AXWii compressor | done | `runtime/ax.c:258` | |
| AXWii output stereo | done | `runtime/ax.c:282` | |
| AXWii Dolby surround output | partial | `runtime/ax.c:315` | Treated as stereo. |
| AXWii Wii Remote speaker output | missing | `runtime/ax.c:319` | Skipped. |
| AXWii older command format (`_OLD`) | missing | `runtime/ax.c:320` | Command numbers differ. Dolphin uses it for Wii Sports (PAL) and the Wii Startup Menu, chosen by checksum. An unknown command loses the frame. |
| AX (GameCube, Metroid Prime Trilogy) | missing | | |
| Zelda (Galaxy, Twilight Princess, Pikmin) | missing | | |
| ASnd, AESnd (homebrew) | not needed | | Commercial games don't use them. |
| CARD, GBA | not needed | | GameCube link helpers. |
| INIT, ROM | not needed | `translate/system_functions.json` | Boot is skipped. |
| Direct AI streaming without the DSP | missing | | Needed by anything that plays its own buffer. |

## 7. IOS

### Core

| Item | Status | Our code | Notes |
|---|---|---|---|
| Open, close | done | `runtime/ios.c:34`, `:55-76` | Device names match by prefix (`devices.c:19`). 32 handles. |
| Read, write, seek | done | `runtime/files.c:121-140` | Files only. Devices get "fine". |
| Ioctl, ioctlv dispatch | done | `runtime/devices.c:24` | |
| Unknown device | done | `runtime/ios.c:44` | Returns not found. |
| Asynchronous replies | partial | `runtime/ios.c:79-93` | Answers at once, one at a time. 64 queued, and more are dropped silently. |
| IOS version in low memory | partial | `runtime/loader.c:93-95` | Fixed at IOS36. Games wanting another version aren't told apart. |
| IOS reload | missing | | |

### `/dev/fs` and files

| Item | Status | Our code | Notes |
|---|---|---|---|
| FORMAT | not needed | | Only the system menu's reset. |
| GETSTATS | done | `runtime/fs.c:138` | Fixed free space. |
| CREATEDIR, CREATEFILE | done | `runtime/fs.c:26` | |
| READDIR | done | `runtime/fs.c:112` | |
| SETATTR | missing | `runtime/fs.c:164` | Says fine, does nothing. |
| GETATTR | partial | `runtime/fs.c:85` | Fixed owner and read-write. |
| DELETE | done | `runtime/fs.c:56` | |
| RENAME | done | `runtime/fs.c:74` | |
| SETFILEVERCTRL | missing | `runtime/fs.c:164` | Says fine. |
| GETFILESTATS | done | `runtime/files.c:141` | |
| GETUSAGE | partial | `runtime/fs.c:147` | Always 0 used. |
| SHUTDOWN | missing | `runtime/fs.c:164` | Says fine. |
| File open modes | missing | `runtime/ios.c:42` | Mode ignored. |
| Permissions, error codes | partial | `runtime/fs.c:15` | Only not found and exists. |

### `/dev/es`

| Item | Status | Our code | Notes |
|---|---|---|---|
| GETTITLEDIR | done | `runtime/es.c:19` | |
| GETTITLEID | done | `runtime/es.c:22` | |
| Identify (ticket, TMD, verify) | missing | `runtime/es.c:12-18` | Zeros and success. |
| GETDEVICEID, GETDEVICECERT, SIGN | missing | `runtime/es.c:12-18` | Zeros. |
| Title lists and counts | missing | `runtime/es.c:12-18` | Zeros. |
| Views, stored TMD | missing | `runtime/es.c:12-18` | Zeros. |
| Content open, read, seek, close | missing | `runtime/es.c:12-18` | No downloadable content or channels. |
| LAUNCH (return to menu, start title) | missing | `runtime/es.c:12-18` | Does nothing. |
| Title install, delete | not needed | | System tools only. |
| Encrypt, decrypt | missing | `runtime/es.c:12-18` | |
| Set UID | missing | `runtime/es.c:12-18` | Fine without permissions. |

### `/dev/di`

| Item | Status | Our code | Notes |
|---|---|---|---|
| ReadDiskID 0x70 | done | `runtime/dvd.c:143` | |
| Read 0x71 | done | `runtime/dvd.c:144` | Data partition, decrypted already. |
| OpenPartition 0x8B | partial | `runtime/dvd.c:140` | Says fine, and writes no ticket or TMD. |
| OpenPartition with TMD 0x93, 0x94 | missing | `runtime/dvd.c:140` | Says fine. |
| ClosePartition 0x8C | partial | `runtime/dvd.c:150` | Says fine. |
| GetCoverStatus 0x88, cover and status registers | partial | `runtime/dvd.c:150-152` | Zeros, read as a disc present. |
| Inquiry 0x12 | missing | `runtime/dvd.c:150-152` | Zeros. Drive date reads 0. |
| Reset, spin-up, cover interrupts | done | `runtime/dvd.c:150-152` | Nothing to do. |
| UnencryptedRead 0x8D | missing | `runtime/dvd.c:150-152` | Returns zeros with success. Game partition table and update partition read as empty. |
| GetLength, Seek, ReadDvd | missing | `runtime/dvd.c:150-152` | |
| Error reporting | missing | | Read errors aside, always OK. |

### Devices

| Item | Status | Our code | Notes |
|---|---|---|---|
| `/dev/stm/immediate` | partial | `runtime/devices.c:34-36` | Says fine. Shutdown, reset and idle do nothing. |
| `/dev/stm/eventhook` | partial | `runtime/devices.c:33` | Held open forever. Power and reset never sent. |
| `/dev/aes` | missing | `runtime/devices.c:34-36` | Says fine, output unchanged. |
| `/dev/sha` | missing | `runtime/devices.c:34-36` | Says fine, output unchanged. |
| `/dev/sdio/slot0` | missing | `runtime/devices.c:34-36` | Prefix match says fine. Should report no card, or model SD. Save-to-SD fails strangely. |
| `/dev/sdio/slot1` | not needed | | Wi-Fi. Dolphin uses a stub. |
| `/dev/usb/oh1/57e/305` | done | `runtime/bluetooth.c:253` | With `--virtual-bluetooth`. See area 8. |
| `/dev/usb/oh1` | partial | `runtime/devices.c:30` | Never replies. |
| `/dev/usb/oh0` | missing | `runtime/devices.c:30` | Never replies. USB devices, Wii Speak. |
| `/dev/usb/hid` | missing | `runtime/devices.c:30` | Never replies. Guitar Hero and Rock Band adapters, Skylanders portal. |
| `/dev/usb/ven` | missing | `runtime/devices.c:30` | Never replies. |
| `/dev/usb/kbd` | missing | `runtime/devices.c:30` | Never replies. |
| `/dev/usb/wfssrv`, `/dev/wfsi` | not needed | | Wii U mode. |
| `/dev/net/kd/request`, `/dev/net/kd/time` | missing | `runtime/devices.c:34-36` | Says fine. |
| `/dev/net/ncd/manage` | missing | `runtime/devices.c:34-36` | Says fine. Should report no connection. |
| `/dev/net/wd/command` | missing | `runtime/devices.c:34-36` | Says fine. |
| `/dev/net/ip/top` | missing | `runtime/devices.c:34-36` | Says fine. Sockets don't exist. |
| `/dev/net/ssl` | missing | `runtime/devices.c:34-36` | Says fine. |
| `/dev/wl0` | missing | `runtime/devices.c:14` | Says fine. |
| `/dev/dolphin` | not needed | | Dolphin's own. |
| MIOS (GameCube mode) | not needed | | |
| Save data in storage | done | `runtime/files.c` | In the save folder, over an optional Wii storage folder. |

## 8. Controllers

### Direct input (default)

| Item | Status | Our code | Notes |
|---|---|---|---|
| `KPADRead` replacement | partial | `runtime/input.c:159` | Remote on channel 0. Fixed motion. An older SDK layout reports no samples (`system_functions.json`). |
| `WPADProbe` replacement | done | `runtime/input.c:170` | |
| Balance Board | done | `runtime/input.c:131-150`, `:179-209` | Through signature-matched functions. Only for games with the board library. |
| Host input | partial | `runtime/input.c:222-236` | Control pipe commands and window. Sticks fixed (`input.c:99`). |

### Bluetooth stack

| Item | Status | Our code | Notes |
|---|---|---|---|
| USB control, bulk, interrupt endpoints | done | `runtime/bluetooth.c:253-281` | |
| HCI reset, buffer size, version, address, features | done | `runtime/bluetooth.c:170-182` | Reset falls to the default reply. |
| HCI stored link keys | done | `runtime/bluetooth.c:186` | |
| HCI accept connection | done | `runtime/bluetooth.c:183` | |
| HCI create connection | missing | `runtime/bluetooth.c:228` | Default reply with no connect event, so the console can't call a remote back. |
| HCI inquiry, inquiry cancel | missing | `runtime/bluetooth.c:228` | Default reply. No sync pairing. |
| HCI remote name, features, version, clock | done | `runtime/bluetooth.c:215-226` | |
| HCI authentication, link key reply | done | `runtime/bluetooth.c:193-196` | |
| HCI link key negative reply | missing | `runtime/bluetooth.c:228` | Default reply. |
| HCI delete stored link key | missing | `runtime/bluetooth.c:228` | Default reply. |
| HCI packet type, link policy, supervision timeout | done | `runtime/bluetooth.c:202-209` | |
| HCI sniff mode | missing | `runtime/bluetooth.c:228` | Default reply, no mode change event. |
| HCI write settings (scan, class, name, PIN, others) | done | `runtime/bluetooth.c:228-230` | Accepted. |
| HCI disconnect | partial | `runtime/bluetooth.c:210` | Link can't come back. `remote_called` stays set. |
| L2CAP connect, configure | done | `runtime/bluetooth_channels.c:42-80` | |
| L2CAP disconnect | missing | `runtime/bluetooth_channels.c:82` | Logged. |
| SDP queries | missing | `runtime/bluetooth_channels.c:66-91` | Dolphin answers them. Games on the SDK don't seem to need them here. |
| Paired remote record in SYSCONF | done | `runtime/bluetooth.c:237` | |

### Wii Remote reports

| Item | Status | Our code | Notes |
|---|---|---|---|
| 0x10 rumble | partial | `runtime/wii_remote.c:109` | Accepted, not felt. Rumble bit in other reports ignored. |
| 0x11 LEDs | partial | `runtime/wii_remote.c:109` | Accepted, not shown. |
| 0x12 data mode | done | `runtime/wii_remote.c:95` | |
| 0x13, 0x1A IR enable | missing | `runtime/wii_remote.c:112` | Bare ack. |
| 0x14 speaker enable | missing | `runtime/wii_remote.c:112` | Bare ack. |
| 0x15 status request | done | `runtime/wii_remote.c:101` | |
| 0x16 write memory | partial | `runtime/wii_remote.c:105` | Attachment space only. EEPROM, speaker and camera writes dropped. |
| 0x17 read memory | partial | `runtime/wii_remote.c:103`, `:53`, `:65` | Calibration and attachment. Mii block reads zero. |
| 0x18 speaker data | missing | `runtime/wii_remote.c:112` | Acks every packet. A real one doesn't unless asked. |
| 0x19 speaker mute | missing | `runtime/wii_remote.c:112` | Bare ack. |
| 0x20 status | partial | `runtime/wii_remote.c:36` | Battery full, attachment always in. No LED bits. |
| 0x21 read reply | done | `runtime/wii_remote.c:65` | |
| 0x22 ack | done | `runtime/wii_remote.c:44` | |
| 0x30 core | done | `runtime/wii_remote.c:22` | |
| 0x31 core, motion | partial | `runtime/wii_remote.c:125` | Motion fixed at rest. |
| 0x32 core, attachment 8 | done | `runtime/wii_remote.c:22` | |
| 0x33 core, motion, IR 12 | partial | `runtime/wii_remote.c:22` | IR bytes are zero, not "no object" (0xFF). |
| 0x34 core, attachment 19 | done | `runtime/wii_remote.c:22` | |
| 0x35 core, motion, attachment 16 | partial | `runtime/wii_remote.c:22` | Motion fixed. |
| 0x36 core, IR 10, attachment 9 | partial | `runtime/wii_remote.c:22` | IR zero. |
| 0x37 core, motion, IR 10, attachment 6 | partial | `runtime/wii_remote.c:22` | IR zero, motion fixed. |
| 0x3D attachment 21 | missing | `runtime/wii_remote.c:97` | Mode request ignored. |
| 0x3E, 0x3F interleaved | missing | `runtime/wii_remote.c:97` | Mode request ignored. |
| Report timing | done | `runtime/wii_remote.c:13`, `:117` | 100 a second. |

### Remote parts and attachments

| Item | Status | Our code | Notes |
|---|---|---|---|
| Buttons | done | `runtime/wii_remote.c:30` | |
| Accelerometer | missing | `runtime/wii_remote.c:125-129` | Fixed. Shakes and tilt aren't possible. |
| IR camera | missing | | Pointer only exists on the direct input path (`input.c:120`). |
| Speaker | missing | | |
| EEPROM | partial | `runtime/wii_remote.c:53` | Calibration only. Mii storage missing. |
| Several remotes | missing | `runtime/bluetooth.c:45` | One address, one handle. |
| Attachment wire protocol | done | `runtime/wii_attachment.c:58-72` | |
| Attachment encryption | partial | `runtime/wii_attachment.c:36-56` | Reads the game's own tables rather than working them out. Channel 0 only. Stops the game if they're empty. |
| Attachment hot plug | missing | | Always attached. |
| Classic Controller | done | `runtime/wii_attachment.c:76` | Sticks need a host source (`input.c:99`). |
| Nunchuk | missing | | Common. |
| Guitar, Drums, Turntable | missing | | |
| Taiko drum (TaTaCon) | missing | | |
| uDraw, Drawsome tablets | missing | | |
| Shinkansen controller | not needed | | One game in Japan. |
| Wii MotionPlus | missing | `runtime/wii_remote.c:76` | Reports no device. |
| Balance Board over Bluetooth | missing | | Direct input only. Dolphin's real path uses this. |
| Real Wii Remotes (`WiimoteReal`) | missing | | No host Bluetooth layer. |
| Passthrough Bluetooth adapter | not needed | | |
