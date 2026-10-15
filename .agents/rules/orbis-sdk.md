---
trigger: glob
globs: "src/main.c, src/video*.c, src/input*.c, Makefile, CMakeLists.txt"
description: Restrições da toolchain OpenOrbis para PS4
---
# Regras OpenOrbis PS4
- Use exclusivamente headers do `$OO_PS4_TOOLCHAIN/include` (`orbis/libkernel.h`, `orbis/Pad.h`, `orbis/UserService.h`, `orbis/VideoOut.h`).
- Ao inicializar 4 controles DualShock 4, obtenha os `userId` via `sceUserServiceGetLoginUserIdList` antes de chamar `scePadOpen` para cada slot.
- Nunca sugira APIs POSIX bloqueantes ou bibliotecas ausentes no FreeBSD 9 / Orbis OS.