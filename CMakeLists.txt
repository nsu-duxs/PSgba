cmake_minimum_required(VERSION 3.16)

# Gera automaticamente o compile_commands.json para o Clangd e CodeGraph (Zero Token Waste!)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Verifica e valida o caminho do OpenOrbis Toolchain
set(OO_PS4_TOOLCHAIN "$ENV{OO_PS4_TOOLCHAIN}")
if(NOT EXISTS "${OO_PS4_TOOLCHAIN}/include/assert.h")
    foreach(CANDIDATE_PATH
        "/opt/OpenOrbis/PS4Toolchain"
        "/opt/OpenOrbis-PS4-Toolchain"
        "$ENV{HOME}/OpenOrbis/PS4Toolchain"
        "$ENV{HOME}/OpenOrbis-PS4-Toolchain"
        "/usr/local/OpenOrbis/PS4Toolchain"
    )
        if(EXISTS "${CANDIDATE_PATH}/include/assert.h")
            set(OO_PS4_TOOLCHAIN "${CANDIDATE_PATH}")
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${OO_PS4_TOOLCHAIN}/include/assert.h")
    message(FATAL_ERROR "Headers da OpenOrbis nao encontrados em OO_PS4_TOOLCHAIN='${OO_PS4_TOOLCHAIN}'. Verifique onde o OpenOrbis PS4 Toolchain esta instalado no seu WSL.")
endif()
message(STATUS "Usando OpenOrbis Toolchain em: ${OO_PS4_TOOLCHAIN}")

set(CMAKE_SYSTEM_NAME FreeBSD)
# Usa clang apontando para o target x86_64-pc-freebsd12-elf do PS4
set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")

project(ps4_gba_4p C CXX)

# Flags de compilação da OpenOrbis (PS4 - AMD Jaguar btver2 otimizado)
set(ORBIS_CFLAGS
    "-target" "x86_64-pc-freebsd12-elf"
    "-O3"
    "-march=btver2"
    "-mtune=btver2"
    "-ffast-math"
    "-funroll-loops"
    "-fomit-frame-pointer"
    "-fPIC"
    "-funwind-tables"
    "-isysroot" "${OO_PS4_TOOLCHAIN}"
    "-isystem" "${OO_PS4_TOOLCHAIN}/include"
    "-D__ORBIS__"
    "-D__PS4__"
    "-DNDEBUG"
)
add_compile_options(${ORBIS_CFLAGS})

# ==============================================================================
# 1. GERAÇÃO DE HEADERS INTERNOS DO mGBA (version.h / flags)
# ==============================================================================
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/mgba_gen/mgba/core")
file(WRITE "${CMAKE_BINARY_DIR}/mgba_gen/mgba/core/version.h" "
#ifndef VERSION_H
#define VERSION_H
#define GIT_COMMIT \"ps4-split\"
#define GIT_BRANCH \"main\"
#define GIT_REV 1
#define BINARY_NAME \"mgba-ps4\"
#define PROJECT_NAME \"mGBA PS4 4P\"
#define PROJECT_VERSION \"1.06\"
#endif
")

# Evita conflito de typedef locale_t com alltypes.h sem puxar TLS (.tbss) da libc
# e desativa mmap em file descriptors (_POSIX_MAPPED_FILES) no Orbis OS
file(WRITE "${CMAKE_BINARY_DIR}/mgba_gen/ps4_compat.h" "
#ifndef PS4_COMPAT_H
#define PS4_COMPAT_H
#include <stdlib.h>
#include <unistd.h>
#undef _POSIX_MAPPED_FILES
#include <locale.h>
#define locale_t mgba_locale_t
#endif
")

# ==============================================================================
# 2. CORE C PURO DO mGBA (Sem dependências externas, apenas GBA + ARM7TDMI)
# ==============================================================================
set(MGBA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mgba")

file(GLOB MGBA_CORE_SRCS
    "${MGBA_DIR}/src/arm/*.c"
    "${MGBA_DIR}/src/core/*.c"
    "${MGBA_DIR}/src/gba/*.c"
    "${MGBA_DIR}/src/gba/cart/*.c"
    "${MGBA_DIR}/src/gba/cheats/*.c"
    "${MGBA_DIR}/src/gba/renderers/*.c"
    "${MGBA_DIR}/src/gba/sio/*.c"
    "${MGBA_DIR}/src/gb/audio.c"
    "${MGBA_DIR}/src/util/audio-buffer.c"
    "${MGBA_DIR}/src/util/circle-buffer.c"
    "${MGBA_DIR}/src/util/configuration.c"
    "${MGBA_DIR}/src/util/crc32.c"
    "${MGBA_DIR}/src/util/formatting.c"
    "${MGBA_DIR}/src/util/gbk-table.c"
    "${MGBA_DIR}/src/util/hash.c"
    "${MGBA_DIR}/src/util/math.c"
    "${MGBA_DIR}/src/util/md5.c"
    "${MGBA_DIR}/src/util/memory.c"
    "${MGBA_DIR}/src/util/patch.c"
    "${MGBA_DIR}/src/util/patch-fast.c"
    "${MGBA_DIR}/src/util/patch-ips.c"
    "${MGBA_DIR}/src/util/patch-ups.c"
    "${MGBA_DIR}/src/util/ring-fifo.c"
    "${MGBA_DIR}/src/util/sha1.c"
    "${MGBA_DIR}/src/util/string.c"
    "${MGBA_DIR}/src/util/table.c"
    "${MGBA_DIR}/src/util/text-codec.c"
    "${MGBA_DIR}/src/util/vector.c"
    "${MGBA_DIR}/src/util/vfs.c"
    "${MGBA_DIR}/src/util/vfs/vfs-mem.c"
    "${MGBA_DIR}/src/util/vfs/vfs-fd.c"
    "${MGBA_DIR}/src/third-party/inih/ini.c"
)

# Remove arquivos que exigem sockets POSIX completos ou bibliotecas desktop
list(REMOVE_ITEM MGBA_CORE_SRCS
    "${MGBA_DIR}/src/gba/sio/lockstep.c" # Substituiremos pelo nosso lockstep em memória de 4 instâncias!
    "${MGBA_DIR}/src/gba/sio/dolphin.c"
    "${MGBA_DIR}/src/core/scripting.c"
    "${MGBA_DIR}/src/gba/renderers/gl.c"
    "${MGBA_DIR}/src/gba/renderers/video-software.c" # Mantido na lista via software.c padrão
)
# Re-adiciona o renderizador software puro (16-bit/32-bit buffer em RAM)
list(APPEND MGBA_CORE_SRCS "${MGBA_DIR}/src/gba/renderers/software-bg.c"
                           "${MGBA_DIR}/src/gba/renderers/software-mode0.c"
                           "${MGBA_DIR}/src/gba/renderers/software-obj.c"
                           "${MGBA_DIR}/src/gba/renderers/video-software.c")

add_library(mgba_core STATIC ${MGBA_CORE_SRCS})

target_include_directories(mgba_core PUBLIC
    "${CMAKE_BINARY_DIR}/mgba_gen"
    "${MGBA_DIR}/include"
    "${MGBA_DIR}/src"
)

target_compile_options(mgba_core PRIVATE "-include" "${CMAKE_BINARY_DIR}/mgba_gen/ps4_compat.h")

# Flags críticas para enxugar o mGBA ao osso (apenas GBA, 32-bit color, sem threads extras)
# IMPORTANTE: NAO definir COLOR_16_BIT (image.h usa #ifdef COLOR_16_BIT; defini-lo ativa 16-bit!)
target_compile_definitions(mgba_core PUBLIC
    M_CORE_GBA
    MINIMAL_CORE=2
    DISABLE_THREADING
    ENABLE_VFS
    ENABLE_VFS_FD
    HAVE_STRNDUP
    HAVE_STRLCPY
    HAVE_LOCALTIME_R
    binaryName="mgba-ps4"
    projectName="mGBA PS4 4P"
    projectVersion="1.06"
    gitCommit="ps4-split"
    gitCommitShort="ps4"
    gitBranch="main"
    gitRevision=1
)

# ==============================================================================
# 3. EXECUTÁVEL DO EMULADOR PS4 (4-Way Split Screen + Link Cable Hub)
# ==============================================================================
file(GLOB PS4_APP_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libjbc/*.c"
)

# Invoca ld.lld diretamente (padrão oficial OpenOrbis) para não injetar PT_INTERP/PT_NOTE de Linux
set(CMAKE_C_LINK_EXECUTABLE "ld.lld <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

add_executable(${PROJECT_NAME}.elf ${PS4_APP_SRCS})

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/libjbc"
)

# Linkagem com as bibliotecas do sistema Orbis OS e libc da OpenOrbis
target_link_options(${PROJECT_NAME}.elf PRIVATE
    "-m" "elf_x86_64"
    "-pie"
    "--script" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/link.x"
    "--eh-frame-hdr"
    "-L${OO_PS4_TOOLCHAIN}/lib"
)

target_link_libraries(${PROJECT_NAME}.elf
    mgba_core
    "${OO_PS4_TOOLCHAIN}/lib/crt1.o"
    c
    c++
    kernel
    SceVideoOut
    ScePad
    SceUserService
    SceAudioOut
    SceSysmodule
    SceSystemService
)

# Pós-build: Converte o .elf em eboot.bin assinado (FSELF) para PS4 Jailbroken
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND "${OO_PS4_TOOLCHAIN}/bin/linux/create-fself"
            -in="${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf"
            -out="${CMAKE_BINARY_DIR}/${PROJECT_NAME}.oelf"
            --eboot="${CMAKE_BINARY_DIR}/eboot.bin"
            --paid 0x3800000000000011
            -sdkver=0x1000051
    COMMENT "Gerando eboot.bin (FSELF) para PS4..."
)

# ==============================================================================
# 4. TARGET PKG: Empacota o eboot.bin em PS4-GBA-4P.pkg (Fake PKG / GoldHEN)
# ==============================================================================
add_custom_target(pkg
    COMMAND ${CMAKE_COMMAND} -E env "OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_pkg.sh" "${CMAKE_BINARY_DIR}"
    DEPENDS ${PROJECT_NAME}.elf
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Empacotando PS4-GBA-4P.pkg via PkgTool.Core..."
)