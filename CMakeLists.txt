cmake_minimum_required(VERSION 3.16)

# Gera automaticamente o compile_commands.json para o Clangd e CodeGraph (Zero Token Waste!)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Verifica a variável de ambiente do OpenOrbis Toolchain
if(NOT DEFINED ENV{OO_PS4_TOOLCHAIN})
    message(FATAL_ERROR "A variável de ambiente OO_PS4_TOOLCHAIN não está definida!")
endif()

set(OO_PS4_TOOLCHAIN $ENV{OO_PS4_TOOLCHAIN})
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_C_COMPILER "${OO_PS4_TOOLCHAIN}/bin/linux/create-fself" CACHE FILEPATH "" FORCE)
# Usa clang apontando para o target x86_64-pc-freebsd12-elf do PS4
set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")

project(ps4_gba_4p C CXX)

# Flags de compilação da OpenOrbis (PS4)
set(ORBIS_CFLAGS
    "-target" "x86_64-pc-freebsd12-elf"
    "-fPIC"
    "-funwind-tables"
    "-isysroot" "${OO_PS4_TOOLCHAIN}"
    "-isystem" "${OO_PS4_TOOLCHAIN}/include"
    "-D__ORBIS__"
    "-D__PS4__"
)
add_compile_options(${ORBIS_CFLAGS})

# ==============================================================================
# 1. GERAÇÃO DE HEADERS INTERNOS DO mGBA (version.h / flags)
# ==============================================================================
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/mgba_gen/mgba-util")
file(WRITE "${CMAKE_BINARY_DIR}/mgba_gen/mgba/core/version.h" "
#ifndef VERSION_H
#define VERSION_H
#define GIT_COMMIT \"ps4-split\"
#define GIT_BRANCH \"main\"
#define GIT_REV 1
#define BINARY_NAME \"mgba-ps4\"
#define PROJECT_NAME \"mGBA PS4 4P\"
#define PROJECT_VERSION \"0.10.0\"
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
    "${MGBA_DIR}/src/gba/renderers/*.c"
    "${MGBA_DIR}/src/gba/sio/*.c"
    "${MGBA_DIR}/src/util/circle-buffer.c"
    "${MGBA_DIR}/src/util/configuration.c"
    "${MGBA_DIR}/src/util/crc32.c"
    "${MGBA_DIR}/src/util/formatting.c"
    "${MGBA_DIR}/src/util/gbk-table.c"
    "${MGBA_DIR}/src/util/hash.c"
    "${MGBA_DIR}/src/util/math.c"
    "${MGBA_DIR}/src/util/memory.c"
    "${MGBA_DIR}/src/util/patch.c"
    "${MGBA_DIR}/src/util/patch-fast.c"
    "${MGBA_DIR}/src/util/patch-ips.c"
    "${MGBA_DIR}/src/util/patch-ups.c"
    "${MGBA_DIR}/src/util/ring-fifo.c"
    "${MGBA_DIR}/src/util/string.c"
    "${MGBA_DIR}/src/util/table.c"
    "${MGBA_DIR}/src/util/text-codec.c"
    "${MGBA_DIR}/src/util/vfs.c"
    "${MGBA_DIR}/src/util/vfs/vfs-mem.c"
    "${MGBA_DIR}/src/util/vfs/vfs-fd.c"
)

# Remove arquivos que exigem sockets POSIX completos ou bibliotecas desktop
list(REMOVE_ITEM MGBA_CORE_SRCS
    "${MGBA_DIR}/src/gba/sio/lockstep.c" # Substituiremos pelo nosso lockstep em memória de 4 instâncias!
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

# Flags críticas para enxugar o mGBA ao osso (apenas GBA, 32-bit color, sem threads extras)
target_compile_definitions(mgba_core PUBLIC
    M_CORE_GBA
    MINIMAL_CORE=2
    COLOR_16_BIT=0
    DISABLE_THREADING
    HAVE_STRNDUP
    HAVE_STRLCPY
)

# ==============================================================================
# 3. EXECUTÁVEL DO EMULADOR PS4 (4-Way Split Screen + Link Cable Hub)
# ==============================================================================
file(GLOB PS4_APP_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(${PROJECT_NAME}.elf ${PS4_APP_SRCS})

target_include_directories(${PROJECT_NAME}.elf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Linkagem com as bibliotecas do sistema Orbis OS e libc da OpenOrbis
target_link_options(${PROJECT_NAME}.elf PRIVATE
    "-target" "x86_64-pc-freebsd12-elf"
    "-Wl,-m,elf_x86_64_fbsd"
    "-Wl,--script,${OO_PS4_TOOLCHAIN}/link.x"
    "-Wl,--eh-frame-hdr"
    "-nostdlib"
    "-L${OO_PS4_TOOLCHAIN}/lib"
)

target_link_libraries(${PROJECT_NAME}.elf
    mgba_core
    "${OO_PS4_TOOLCHAIN}/lib/crtlib.o"
    c
    c++
    kernel
    SceVideoOut
    ScePad
    SceUserService
    SceAudioOut
    SceSysmodule
)

# Pós-build: Converte o .elf em eboot.bin assinado (FSELF) para PS4 Jailbroken
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND "${OO_PS4_TOOLCHAIN}/bin/linux/create-fself"
            -in="${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf"
            -out="${CMAKE_BINARY_DIR}/eboot.bin"
            --paid 0x3800000000000011
    COMMENT "Gerando eboot.bin (FSELF) para PS4..."
)