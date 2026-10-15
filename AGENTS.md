# GBA 4-Way Split-Screen Emulator for PS4 (OpenOrbis)

## Architecture & Goals
- Target: PS4 Jailbroken via OpenOrbis Toolchain (OO-Toolchain).
- Core: Embedded mGBA core (using `mgba/include/mgba/core/core.h` and C core APIs).
- Display: 4-way split-screen viewport rendered via Piglet/GLES2, SDL2 or Orbis libkernel video buffers (1080p/4K).
- Input: Map 4 DualShock 4 controllers via `scePad` / `sceUserService` to each of the 4 mGBA instances.
- Link Cable: In-memory software packet ring-buffer connecting `GBALink` ports between instances 1..4.

## AI Behavior Rules
- Be concise. Return only modified code blocks or unified diffs.
- Never output compilation logs or full subproject source trees.
- Never write platform-specific code for Windows/Linux/POSIX desktop when interacting with controllers or memory; always target OpenOrbis/PS4 APIs.