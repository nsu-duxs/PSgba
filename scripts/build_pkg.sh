#!/usr/bin/env bash
set -euo pipefail

if [ -z "${OO_PS4_TOOLCHAIN:-}" ]; then
    echo "Erro: OO_PS4_TOOLCHAIN nao esta definida." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
EBOOT_BIN="${BUILD_DIR}/eboot.bin"
ASSETS_DIR="${PROJECT_ROOT}/assets"
PKG_ROOT="${BUILD_DIR}/pkg_root"
SCE_SYS_DIR="${PKG_ROOT}/sce_sys"
GP4_FILE="${PKG_ROOT}/PS4-GBA-4P.gp4"
OUTPUT_PKG="${BUILD_DIR}/PS4-GBA-4P.pkg"

PKGTOOL="${OO_PS4_TOOLCHAIN}/bin/linux/PkgTool.Core"

# Garante libssl.so.1.1 para o PkgTool.Core (.NET Core 3.1) em Ubuntu 22.04/24.04 WSL sem exigir sudo
if ! ldconfig -p 2>/dev/null | grep -q "libssl.so.1.1"; then
    LIBSSL_CACHE="${HOME}/.cache/openorbis-libssl"
    LIBSSL_LIBDIR="${LIBSSL_CACHE}/usr/lib/x86_64-linux-gnu"
    if [ ! -f "${LIBSSL_LIBDIR}/libssl.so.1.1" ]; then
        echo "Baixando libssl1.1 local para o PkgTool.Core..."
        mkdir -p "${LIBSSL_CACHE}"
        DEB_URL="http://security.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb"
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL "${DEB_URL}" -o "${LIBSSL_CACHE}/libssl1.1.deb"
        else
            wget -qO "${LIBSSL_CACHE}/libssl1.1.deb" "${DEB_URL}"
        fi
        dpkg-deb -x "${LIBSSL_CACHE}/libssl1.1.deb" "${LIBSSL_CACHE}"
    fi
    export LD_LIBRARY_PATH="${LIBSSL_LIBDIR}:${LD_LIBRARY_PATH:-}"
fi

TITLE_ID="MGBA00001"
CONTENT_ID="IV0000-${TITLE_ID}_00-MGBA4PSPLITSCR00"
APP_TITLE="mGBA 4-Players Split-Screen"
APP_CATEGORY="gd"
SYS_VER_DEC="0" # 0 = compatível com qualquer firmware PS4 Jailbroken (5.05 / 6.72 / 9.00 / 11.00)

if [ ! -f "${EBOOT_BIN}" ]; then
    echo "Erro: ${EBOOT_BIN} nao encontrado. Compile o projeto antes de gerar o PKG." >&2
    exit 1
fi

rm -rf "${PKG_ROOT}"
mkdir -p "${SCE_SYS_DIR}/about" "${PKG_ROOT}/sce_module" "${ASSETS_DIR}"
cp -f "${EBOOT_BIN}" "${PKG_ROOT}/eboot.bin"

# Função auxiliar para compilar PRX stub da OpenOrbis caso não esteja pré-compilado no toolchain
build_prx_stub() {
    local prx_name="$1"
    local dummy_fn="$2"
    local extra_libs="$3"
    local out_prx="$4"
    local tmp_dir="${BUILD_DIR}/prx_tmp_${prx_name}"
    mkdir -p "${tmp_dir}"
    printf "int %s(void) { return 0; }\n" "${dummy_fn}" > "${tmp_dir}/lib.c"
    clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables \
        -isysroot "${OO_PS4_TOOLCHAIN}" -isystem "${OO_PS4_TOOLCHAIN}/include" \
        -c -o "${tmp_dir}/lib.o" "${tmp_dir}/lib.c"
    ld.lld "${tmp_dir}/lib.o" -o "${tmp_dir}/${prx_name}.elf" \
        -m elf_x86_64 -pie --script "${OO_PS4_TOOLCHAIN}/link.x" --eh-frame-hdr \
        -L"${OO_PS4_TOOLCHAIN}/lib" ${extra_libs} "${OO_PS4_TOOLCHAIN}/lib/crtlib.o"
    "${OO_PS4_TOOLCHAIN}/bin/linux/create-fself" \
        -in="${tmp_dir}/${prx_name}.elf" \
        -out="${tmp_dir}/${prx_name}.oelf" \
        --lib="${out_prx}" \
        --paid 0x3800000000000011
}

# 1. right.sprx
RIGHT_SPRX_SRC="$(find "${OO_PS4_TOOLCHAIN}" -name "right.sprx" -size +0c 2>/dev/null | head -n 1 || true)"
GP4_FILES="eboot.bin sce_sys/param.sfo sce_sys/icon0.png"
if [ -n "${RIGHT_SPRX_SRC}" ] && [ -f "${RIGHT_SPRX_SRC}" ]; then
    cp -f "${RIGHT_SPRX_SRC}" "${SCE_SYS_DIR}/about/right.sprx"
    GP4_FILES="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png"
fi

# 2. sce_module/libc.prx (obrigatório para o loader do PS4 não acusar dados corrompidos)
LIBC_PRX_SRC="$(find "${OO_PS4_TOOLCHAIN}" -name "libc.prx" -size +0c 2>/dev/null | head -n 1 || true)"
if [ -n "${LIBC_PRX_SRC}" ] && [ -f "${LIBC_PRX_SRC}" ]; then
    cp -f "${LIBC_PRX_SRC}" "${PKG_ROOT}/sce_module/libc.prx"
else
    build_prx_stub "libc" "__dummy__libc_func" "-lSceLibcInternal -lkernel" "${PKG_ROOT}/sce_module/libc.prx"
fi
GP4_FILES="${GP4_FILES} sce_module/libc.prx"

# 3. sce_module/libSceFios2.prx (obrigatório para o loader do PS4 não acusar dados corrompidos)
FIOS2_PRX_SRC="$(find "${OO_PS4_TOOLCHAIN}" -name "libSceFios2.prx" -size +0c 2>/dev/null | head -n 1 || true)"
if [ -n "${FIOS2_PRX_SRC}" ] && [ -f "${FIOS2_PRX_SRC}" ]; then
    cp -f "${FIOS2_PRX_SRC}" "${PKG_ROOT}/sce_module/libSceFios2.prx"
else
    build_prx_stub "libSceFios2" "__dummy__sce_fios2_func" "-lkernel" "${PKG_ROOT}/sce_module/libSceFios2.prx"
fi
GP4_FILES="${GP4_FILES} sce_module/libSceFios2.prx"

# Gera icon0.png (512x512 RGB 24-bit) caso nao exista em assets/
generate_rgb24_png() {
    local out_file="$1"
    local r="$2"
    local g="$3"
    local b="$4"
    python3 - "$out_file" "$r" "$g" "$b" << 'EOF'
import sys, struct, zlib

out_path = sys.argv[1]
r, g, b = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
w, h = 512, 512

def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0) # 8-bit truecolor RGB (24-bit)
row = b"\x00" + bytes([r, g, b]) * w
raw = row * h
idat = zlib.compress(raw, 9)

with open(out_path, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n")
    f.write(chunk(b"IHDR", ihdr))
    f.write(chunk(b"IDAT", idat))
    f.write(chunk(b"IEND", b""))
EOF
}

if [ ! -f "${ASSETS_DIR}/icon0.png" ]; then
    generate_rgb24_png "${ASSETS_DIR}/icon0.png" 26 42 108
fi

cp -f "${ASSETS_DIR}/icon0.png" "${SCE_SYS_DIR}/icon0.png"

# Gera param.sfo via PkgTool.Core
PARAM_SFO="${SCE_SYS_DIR}/param.sfo"
rm -f "${PARAM_SFO}"

"${PKGTOOL}" sfo_new "${PARAM_SFO}"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" APP_TYPE --type Integer --maxsize 4 --value 1
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" APP_VER --type Utf8 --maxsize 8 --value "01.00"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" ATTRIBUTE --type Integer --maxsize 4 --value 0
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" CATEGORY --type Utf8 --maxsize 4 --value "${APP_CATEGORY}"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" CONTENT_ID --type Utf8 --maxsize 48 --value "${CONTENT_ID}"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" SYSTEM_VER --type Integer --maxsize 4 --value "${SYS_VER_DEC}"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" TITLE --type Utf8 --maxsize 128 --value "${APP_TITLE}"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" TITLE_ID --type Utf8 --maxsize 12 --value "${TITLE_ID}"
"${PKGTOOL}" sfo_setentry "${PARAM_SFO}" VERSION --type Utf8 --maxsize 8 --value "01.00"

# Constrói o arquivo .gp4 e compila o pacote final PS4-GBA-4P.pkg
rm -f "${GP4_FILE}" "${OUTPUT_PKG}"
cd "${PKG_ROOT}"
if [ -x "${OO_PS4_TOOLCHAIN}/bin/linux/create-gp4" ]; then
    "${OO_PS4_TOOLCHAIN}/bin/linux/create-gp4" -out "${GP4_FILE}" --content-id="${CONTENT_ID}" --files "${GP4_FILES}"
else
    cat > "${GP4_FILE}" <<EOF
<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<psproject fmt="gp4" version="1000">
  <volume>
    <volume_type>pkg_ps4_app</volume_type>
    <volume_id>PS4VOLUME</volume_id>
    <volume_ts>2026-10-15 00:00:00</volume_ts>
    <package content_id="${CONTENT_ID}" passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full" />
    <chunk_info chunk_count="1" scenario_count="1">
      <chunks>
        <chunk id="0" layer_no="0" label="Chunk #0" />
      </chunks>
      <scenarios default_id="0">
        <scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>
      </scenarios>
    </chunk_info>
  </volume>
  <files img_no="0">
    <file targ_path="sce_sys/param.sfo" orig_path="sce_sys/param.sfo" />
    <file targ_path="sce_sys/icon0.png" orig_path="sce_sys/icon0.png" />
    <file targ_path="eboot.bin" orig_path="eboot.bin" />
  </files>
  <rootdir>
    <dir targ_name="sce_sys" />
  </rootdir>
</psproject>
EOF
fi

"${PKGTOOL}" pkg_build "${GP4_FILE}" "${BUILD_DIR}"
if [ -f "${BUILD_DIR}/${CONTENT_ID}.pkg" ]; then
    mv -f "${BUILD_DIR}/${CONTENT_ID}.pkg" "${OUTPUT_PKG}"
fi

echo "Pacote Fake PKG gerado com sucesso: ${OUTPUT_PKG}"

