---
trigger: glob
globs: "src/link_cable*.c, src/link_cable*.h, src/core*.c"
description: Regras de sincronização SIO/Link Cable entre as 4 instâncias do mGBA
---
# Regras de Emulação mGBA Multi-Instância
- Nunca bloqueie a thread principal aguardando I/O de rede; o Link Cable entre os 4 jogadores é local em memória compartilhada (ring-buffer ou mutex lockstep por ciclo de frame).
- Consulte apenas `mgba/include/mgba/internal/gba/sio.h` e `mgba/include/mgba/core/core.h` via `@[sio.h](mgba/include/mgba/internal/gba/sio.h)` quando precisar verificar structs de transferência.