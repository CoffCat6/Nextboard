#!/usr/bin/env bash
# build.sh — Linux 构建脚本(build.ps1 的 Linux 对应物,docs/29 §7.2)
#
# 用法:
#   ./build.sh            # core(cargo)+ app(cmake+ninja)全量构建
#   ./build.sh core       # 只构建 nbcore(libnbcore.so)
#   ./build.sh app        # 只构建 nextboard(需 Qt6≥6.5 开发包)
#
# 环境要求(docs/29 §4.3 的 Linux 环境清单,Fedora 系示例):
#   sudo dnf install cargo cmake ninja-build qt6-qtbase-devel qt6-qtdeclarative-devel \
#       openssl-devel wayland-devel libxkbcommon-devel libX11-devel pipewire-devel \
#       clang-devel mesa-libgbm-devel mesa-libEGL-devel
#   Ubuntu 系对应:libwayland-dev libpipewire-0.3-dev libclang-dev libssl-dev
#   qt6-base-dev qt6-declarative-dev(注意:22.04/24.04 系统仓 Qt < 6.5,不可用)
#
# 与 Windows 的差异:
#   - 无 vcvars64/cmake/Ninja 路径注入(系统工具链);
#   - 无 icacls 完整性修复(Windows 沙箱特有问题);
#   - OCR 资产:模型照常部署;libonnxruntime.so 待 M4 入仓(thirdparty 现为 win-x64);
#   - Qt 插件由发行版包管理器提供,无 windeployqt 对应步骤(发布期用 linuxdeploy,M4)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

build_core() {
    echo "==> [1/2] cargo build --release (libnbcore.so)"
    (cd "$ROOT/core" && cargo build --release)
    ls -la "$ROOT/core/target/release/libnbcore.so"
}

build_app() {
    echo "==> [2/2] cmake + ninja (app/)"
    # Qt6 定位:优先 QT6_DIR 环境变量,其次常见发行版路径
    if [ -z "${QT6_DIR:-}" ]; then
        for d in /usr/lib64/cmake/Qt6 /usr/lib/x86_64-linux-gnu/cmake/Qt6 /usr/lib/cmake/Qt6; do
            if [ -f "$d/Qt6Config.cmake" ]; then QT6_DIR="$d"; break; fi
        done
    fi
    [ -n "${QT6_DIR:-}" ] || { echo "!! 未找到 Qt6(需 ≥6.5):装 dev 包或 QT6_DIR=... 重跑"; exit 1; }
    echo "    Qt6_DIR = $QT6_DIR"
    cmake -S "$ROOT/app" -B "$ROOT/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DQt6_DIR="$QT6_DIR"
    ninja -C "$ROOT/build-linux"
    echo "==> 完成:$ROOT/build-linux/nextboard(libnbcore.so 同目录,可直接运行)"
}

case "${1:-all}" in
    core) build_core ;;
    app)  build_app ;;
    all)  build_core; build_app ;;
    *)    echo "用法: $0 [core|app|all]" >&2; exit 2 ;;
esac
