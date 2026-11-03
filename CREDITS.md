# Credits

This project is licensed under the GNU General Public License, version 2 or later. See `LICENSE`.

- [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later) is the reference for how the Wii's hardware behaves, through `docs/hardware_inventory.md`. Any code or tables taken from it must be listed here with the file they came from.
  - Texture sampling (`recomp/runtime/gx_draw.c`, `gx_texture.c`): the rules for which stored mip levels are read, the level of detail clamp where the largest level wins, and wrap mode 3 clamping follow `Source/Core/VideoCommon/RenderState.cpp` (`SamplerState::Generate`) and `TextureInfo.cpp`.
  - Fog (`recomp/runtime/gx_tev.c`, `read_fog` and `add_fog`): the depth to distance formulas, the four curve lines, the range adjustment and the whole-number color mix are taken from `Source/Core/VideoCommon/PixelShaderGen.cpp` (`WriteFog`) and `PixelShaderManager.cpp`; the register layout and the infinite A and C case from `BPMemory.h` and `BPMemory.cpp`.
  - Logic operations (`recomp/runtime/gx_draw.c`, `approximate_logic`, and `read_logic` in `gx_tev.c`): the table of blending that stands in for each operation, the shader's part, and treating a no-op as no writing are taken from `Source/Core/VideoCommon/RenderState.cpp` (`BlendingState::Generate` and `ApproximateLogicOpWithBlending`) and `PixelShaderGen.cpp` (`WriteLogicOpBlend`).
  - Lines and points (`recomp/runtime/gx_draw.c`, `gx_draw_expanded`): widening a line up and down or sideways by its slope in pixels, the sizes in sixths of a pixel, and no culling follow `Source/Core/VideoCommon/ShaderGenCommon.cpp` (`GenerateLineOffset`), `GeometryShaderGen.cpp`, `GeometryShaderManager.cpp` and `RenderState.cpp`.
  - Viewport depth range (`recomp/runtime/gx_draw.c`, `apply_viewport`): the near and far ends worked out from the viewport's z range and far value follow `Source/Core/VideoCommon/BPFunctions.cpp` (`SetScissorAndViewport`).
  - Destination alpha (`recomp/runtime/gx_draw.c`, `gx_copy.c`, `gx_tev.c`): when the constant output alpha applies, how it blends when subtracting or with a no-op, and a pixel format without alpha storing none, blending its alpha as one and copying it as full, follow `Source/Core/VideoCommon/RenderState.cpp` (`BlendingState::Generate`), `PixelShaderGen.cpp`, `BPFunctions.cpp` (`ClearScreen`) and `TextureConverterShaderGen.cpp`.
- [Hector Martin (marcan)](https://github.com/marcan) and others documented the Wii Remote and its attachments.
- [WiiBrew](https://wiibrew.org) and YAGCD document the hardware.

Nintendo's discs, system software and saves are not part of this project and stay out of the repository.
