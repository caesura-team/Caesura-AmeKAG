#!/usr/bin/env bash
# =============================================================================
# build_appimage.sh — Linux AppImage 发行适配（035 D 块 t210）
#
# 输入：CPack TGZ（build/CaesuraAmeKAG-*-Linux-*.tar.gz，与 verify_release_package.sh
#       的 Linux lane 同一产物；CPACK_GENERATOR=TGZ，见 CMakeLists.txt L559-566）
# 输出：<TGZ 同名>.AppImage（+ .sha256 侧车）——放在 TGZ 同目录或 --out 目录
# 阶段：AppDir 布局产物留在 build/appimage/AppDir（dry-run 可检视）
#
# AppDir 布局（AppImage 规范）：
#   AppDir/
#   ├── AppRun                        # 入口包装：cd 到 AppDir 根 → exec usr/bin/CaesuraAmeKAG
#   ├── caesura-amekag.desktop        # 根副本（appimagetool 要求）+ usr/share/applications/
#   ├── caesura-amekag.png            # 根副本（appimagetool 要求）+ hicolor 主题路径
#   ├── usr/bin/CaesuraAmeKAG
#   ├── usr/share/applications/caesura-amekag.desktop
#   ├── usr/share/icons/hicolor/256x256/apps/caesura-amekag.png
#   └── scripts/ assets/ web-editor/ editor/ tools/ external/ README.md LICENSE
#       （与 TGZ 同 install 语义：引擎自身二进制 + lua 解释器 + 资源 + 编辑器）
#
# 运行时依赖由宿主提供（与 TGZ 同约定）：SDL3/OpenGL/libEGL.so.1（平台矩阵注明的
# Linux 运行时依赖），AppImage 不捆绑它们。
#
# 参数/环境：
#   --tgz <path>          显式 CPack TGZ（缺省自动取 build/ 下最新 *-Linux-*.tar.gz）
#   --out <dir>           .AppImage 输出目录（缺省 = TGZ 所在目录）
#   --appimagetool <path> 显式 appimagetool 路径（或设 APPIMAGETOOL 环境变量）
#
# 退出码：0 = 打包完成；2 = appimagetool 缺失（AppDir 布局已生成——dry-run 布局验证；
#          指引见输出）；1 = 其他错误（TGZ 缺失/布局校验失败）。
# =============================================================================

set -euo pipefail

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
TOOLS_DIR="$REPO_ROOT/tools/appimage"
WORK_ROOT="${TMPDIR:-/tmp}/appimage-work.$$"
APP_DIR="$REPO_ROOT/build/appimage/AppDir"
APPIME_TOOL=""
OUT_DIR=""
TGZ=""
ARCH="$(uname -m)"

usage() {
    cat <<'EOF'
Usage: scripts/build_appimage.sh [--tgz <path>] [--out <dir>] [--appimagetool <path>]

  --tgz <path>          Linux CPack TGZ（缺省自动取 build/ 下最新 *-Linux-*.tar.gz）
  --out <dir>           .AppImage 输出目录（缺省 = TGZ 所在目录）
  --appimagetool <path> appimagetool 路径（缺省 PATH / 环境变量 APPIMAGETOOL）
EOF
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --tgz) TGZ="${2:?--tgz needs a path}"; shift 2 ;;
            --out) OUT_DIR="${2:?--out needs a dir}"; shift 2 ;;
            --appimagetool) APPIME_TOOL="${2:?--appimagetool needs a path}"; shift 2 ;;
            -h|--help) usage; exit 0 ;;
            *) echo "unknown argument: $1" >&2; usage; exit 1 ;;
        esac
    done
}

# ----------------------------------------------------------------------------
locate_tgz() {
    if [ -n "$TGZ" ]; then
        [ -f "$TGZ" ] || { echo "ERROR: --tgz file not found: $TGZ" >&2; exit 1; }
        return
    fi
    local newest=""
    for f in "$REPO_ROOT"/build/CaesuraAmeKAG-*-Linux-*.tar.gz; do
        [ -f "$f" ] || continue
        if [ -z "$newest" ] || [ "$f" -nt "$newest" ]; then newest="$f"; fi
    done
    if [ -z "$newest" ]; then
        cat >&2 <<'EOF'
ERROR: no Linux CPack TGZ found under build/CaesuraAmeKAG-*-Linux-*.tar.gz.
Build one on a Linux host first:
  cmake -B build -DCAESURA_LIVE2D=OFF -DCAESURA_ENABLE_FFMPEG=OFF   # (per docs/team/development-guide.md)
  cmake --build build -j$(nproc)
  cd build && cpack -C Release -G TGZ
or pass --tgz <path>.
EOF
        exit 1
    fi
    TGZ="$newest"
    echo "[appimage] using TGZ: $TGZ"
}

# ----------------------------------------------------------------------------
extract_and_find_root() {
    mkdir -p "$WORK_ROOT"
    tar -xzf "$TGZ" -C "$WORK_ROOT" || { echo "ERROR: tar -xzf failed" >&2; exit 1; }
    local d="" root=""
    for d in "$WORK_ROOT"/*/; do
        d="${d%/}"
        if [ -f "$d/CaesuraAmeKAG" ]; then root="$d"; break; fi
    done
    if [ -z "$root" ] && [ -f "$WORK_ROOT/CaesuraAmeKAG" ]; then
        root="$WORK_ROOT"
    fi
    if [ -z "$root" ]; then
        echo "ERROR: no CaesuraAmeKAG executable under the TGZ top-level entries:" >&2
        ls -1 "$WORK_ROOT" | sed 's/^/   /' >&2
        exit 1
    fi
    ROOT="$root"
    echo "[appimage] TGZ root: $ROOT"
}

# ----------------------------------------------------------------------------
validate_payload() {
    [ -f "$ROOT/CaesuraAmeKAG" ] || { echo "ERROR: $ROOT/CaesuraAmeKAG missing" >&2; exit 1; }
    for d in scripts assets; do
        [ -d "$ROOT/$d" ] || { echo "ERROR: $ROOT/$d/ missing (same-install-semantics payload)" >&2; exit 1; }
    done
    [ -f "$ROOT/external/lua/lua" ] || { echo "ERROR: $ROOT/external/lua/lua missing (packaged lua interpreter)" >&2; exit 1; }
    [ -d "$ROOT/tools/project_templates" ] || { echo "ERROR: $ROOT/tools/project_templates/ missing" >&2; exit 1; }
}

# ----------------------------------------------------------------------------
stage_appdir() {
    rm -rf "$APP_DIR"
    mkdir -p "$APP_DIR/usr/bin" \
             "$APP_DIR/usr/share/applications" \
             "$APP_DIR/usr/share/icons/hicolor/256x256/apps"
    chmod +x "$ROOT/CaesuraAmeKAG"

    # AppRun：cd 到 AppDir 根再 exec —— 引擎的 CWD 资源解析与 TGZ lane 同构
    cat > "$APP_DIR/AppRun" <<'APPRUN'
#!/bin/bash
# AppRun — AppImage entry (generated by scripts/build_appimage.sh)
# The engine resolves assets/relative paths from the CWD (same semantics as
# the TGZ lane), so launch from the AppDir root.
APPDIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
cd "$APPDIR"
exec "$APPDIR/usr/bin/CaesuraAmeKAG" "$@"
APPRUN
    chmod +x "$APP_DIR/AppRun"

    cp "$ROOT/CaesuraAmeKAG" "$APP_DIR/usr/bin/CaesuraAmeKAG"
    chmod 755 "$APP_DIR/usr/bin/CaesuraAmeKAG"

    cp "$TOOLS_DIR/caesura-amekag.desktop" "$APP_DIR/caesura-amekag.desktop"
    cp "$TOOLS_DIR/caesura-amekag.desktop" "$APP_DIR/usr/share/applications/caesura-amekag.desktop"
    cp "$TOOLS_DIR/caesura-amekag.png" "$APP_DIR/caesura-amekag.png"
    cp "$TOOLS_DIR/caesura-amekag.png" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/caesura-amekag.png"

    # Payload：与 TGZ 同 install 语义（二进制入 usr/bin；resources/lua/editor 原样进 AppDir）
    for item in scripts assets web-editor editor tools external README.md LICENSE; do
        if [ -e "$ROOT/$item" ]; then cp -a "$ROOT/$item" "$APP_DIR/"; fi
    done
    echo "[appimage] AppDir staged at: $APP_DIR"
}

# ----------------------------------------------------------------------------
find_appimagetool() {
    if [ -n "$APPIME_TOOL" ]; then
        [ -x "$APPIME_TOOL" ] || { echo "ERROR: --appimagetool not executable: $APPIME_TOOL" >&2; exit 1; }
        return
    fi
    APPIME_TOOL="${APPIMAGETOOL:-}"
    if [ -z "$APPIME_TOOL" ]; then
        APPIME_TOOL="$(command -v appimagetool || true)"
    fi
}

# ----------------------------------------------------------------------------
print_install_hint() {
    cat <<'EOF'

===============================================================================
ERROR: appimagetool not found — Linux AppImage 打包无法执行。
（AppDir 布局已生成：build/appimage/AppDir —— dry-run 布局验证可检视。）

安装指引（任一）：
  1) 下载官方 appimagetool AppImage 并放到 PATH：
       wget -O /usr/local/bin/appimagetool \
         https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
       chmod +x /usr/local/bin/appimagetool
     （本机架构非 x86_64 时改用对应产物名，如 aarch64。）
  2) 或下载到任意路径后以环境变量提供：
       export APPIMAGETOOL=/path/to/appimagetool
       bash scripts/build_appimage.sh --tgz <路径>
  3) Ubuntu host 可用官方 PPA：
       sudo add-apt-repository ppa:appimagelauncher/stable && sudo apt install appimagelauncher
      （launcher 不含 appimagetool CLI；CLI 必须以 1) 或 2) 提供。）

重跑同一命令即可完成打包（AppDir 会重建）。CI（Ubuntu runner）接线后由
ci.yml 下载 appimagetool 后调用本脚本输出 .AppImage 产物。
===============================================================================
EOF
}

# ----------------------------------------------------------------------------
main() {
    parse_args "$@"
    command -v tar >/dev/null || { echo "ERROR: tar required" >&2; exit 1; }

    locate_tgz
    extract_and_find_root
    validate_payload
    stage_appdir

    find_appimagetool
    if [ -z "$APPIME_TOOL" ]; then
        print_install_hint
        exit 2
    fi

    [ -n "$OUT_DIR" ] || OUT_DIR="$(dirname "$TGZ")"
    mkdir -p "$OUT_DIR"
    local out_name
    out_name="$(basename "$TGZ" .tar.gz)"
    local out_file="$OUT_DIR/$out_name.AppImage"

    echo "[appimage] appimagetool: $APPIME_TOOL"
    # APPIMAGE_EXTRACT_AND_RUN：appimagetool 本身是 AppImage——无 FUSE 环境（WSL/容器）下
    # 以 extract-and-run 运行，等效于挂载运行时。
    APPIMAGE_EXTRACT_AND_RUN=1 ARCH="${ARCH:-x86_64}" \
        "$APPIME_TOOL" "$APP_DIR" "$out_file"
    chmod +x "$out_file"
    ( cd "$OUT_DIR" && sha256sum "$(basename "$out_file")" > "$(basename "$out_file").sha256" )
    echo "[appimage] done: $out_file"
    echo "[appimage] checksum: $out_file.sha256"
}

main "$@"
