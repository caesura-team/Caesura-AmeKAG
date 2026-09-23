#!/usr/bin/env bash
# Explicit compatibility entry point; Python owns validation and all file work.
set -euo pipefail
if [[ $# -lt 2 || "$1" != "--python" ]]; then
    echo 'Usage: build_appimage.sh --python /absolute/python --tgz /explicit.tar.gz --sha256 HEX --requirements /build.json --requirements-sha256 HEX --appimagetool /absolute/tool --appimagetool-sha256 HEX --runtime-file /type2-runtime --runtime-sha256 HEX --work /new/external/work --output /explicit.AppImage' >&2
    exit 2
fi
PYTHON_EXECUTABLE="$2"
shift 2
case "$PYTHON_EXECUTABLE" in
    /*) ;;
    *) echo '--python must be an explicit absolute executable path' >&2; exit 2 ;;
esac
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
exec "$PYTHON_EXECUTABLE" "$SCRIPT_DIR/build_appimage.py" "$@"
