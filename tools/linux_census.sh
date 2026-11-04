#!/usr/bin/env bash
# tools/linux_census.sh — Linux 迁移编译普查(零代码改动)
#
# 目的(docs/29 §8.3 第一步 / §12 spike 0):在 Linux 上实测"编译阻塞清单",
# 把 docs/29 §4 的静态扫描结论替换为实测数据,支撑 M0 排期。
# 本脚本只读源码、只写 census/ 目录,不改任何源文件。
#
# 用法:
#   ./tools/linux_census.sh            # 全量(rust + cpp)
#   ./tools/linux_census.sh rust       # 只普查 Rust core(不需要 Qt)
#   ./tools/linux_census.sh cpp        # 只普查 C++ 宿主(需要 Qt >= 6.5)
#   QT6_DIR=/usr/lib64/cmake/Qt6 ./tools/linux_census.sh cpp
#
# 环境要求与推荐(docs/29 §8.3):
#   rust: cargo/rustc。推荐 ubuntu:22.04 容器 —— 顺带实测 AppImage 基线
#         glibc(glibc 2.35)下 libnbcore.so 能否编译。
#   cpp : cmake >= 3.21、ninja、Qt >= 6.5(app/CMakeLists.txt REQUIRES 6.5)。
#         注意:Ubuntu 22.04/24.04 系统仓 Qt 为 6.2/6.4,均低于 6.5,configure 必失败;
#         普查请用 Qt >= 6.6 的环境(Fedora/Arch 系统仓,或 aqtinstall 自装 6.9)。
#         Fedora: sudo dnf install cargo cmake ninja-build qt6-qtbase-devel qt6-qtdeclarative-devel
#         Arch  : sudo pacman -S rust cmake ninja qt6-base qt6-declarative
#
# 产出(均写入 census/,已入 .gitignore 不入库):
#   census-rust-check.txt / census-rust-test.txt / census-cpp-configure.txt / census-cpp-build.txt
#   census-summary.md   # 汇总:环境指纹 + 错误频次 + 首层文件清单
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/census"
mkdir -p "$OUT"

if [ "$(uname -s)" != "Linux" ]; then
    echo "!! 本脚本必须在 Linux 上运行(WSL2 / 容器 / 真机),当前是 $(uname -s)" >&2
    exit 1
fi

: > "$OUT/census-summary.md"

env_fingerprint() {
    {
        echo "## 环境"
        echo
        echo "- 时间: $(date '+%F %T %z')"
        echo "- 内核: $(uname -srm)"
        if [ -r /etc/os-release ]; then
            . /etc/os-release
            echo "- 发行版: ${PRETTY_NAME:-unknown}"
        fi
        echo "- glibc: $(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
        command -v rustc >/dev/null 2>&1 && echo "- rustc: $(rustc --version)"
        command -v cargo >/dev/null 2>&1 && echo "- cargo: $(cargo --version)"
        command -v cmake >/dev/null 2>&1 && echo "- cmake: $(cmake --version | head -1)"
        command -v ninja >/dev/null 2>&1 && echo "- ninja: $(ninja --version)"
        if [ -n "${QT6_DIR:-}" ] && [ -f "$QT6_DIR/Qt6Config.cmake" ]; then
            echo "- Qt6_DIR: $QT6_DIR"
        fi
        echo
    } >> "$OUT/census-summary.md"
}

census_rust() {
    if ! command -v cargo >/dev/null 2>&1; then
        echo "!! 未找到 cargo。安装: apt install cargo / dnf install cargo / rustup" >&2
        return 1
    fi
    echo "==> [rust] cargo check --workspace(工作区: core/)"
    (cd "$ROOT/core" && cargo check --workspace --message-format=short 2>&1) |
        tee "$OUT/census-rust-check.txt" | grep -E ': error' | head -25 || true
    echo "==> [rust] cargo test --workspace --no-run(连测试编译一起量)"
    (cd "$ROOT/core" && cargo test --workspace --no-run --message-format=short 2>&1) |
        tee "$OUT/census-rust-test.txt" | grep -E ': error' | head -25 || true
    {
        echo "## Rust 阻塞清单(nbcore)"
        echo
        echo "### 错误码频次"
        echo '```'
        grep -hoE 'error\[E[0-9]+\]' "$OUT"/census-rust-*.txt 2>/dev/null |
            sort | uniq -c | sort -rn | head -20
        echo '```'
        echo "### 涉及源文件(按报错提及次数排序,前 30)"
        echo '```'
        grep -hE ': error' "$OUT"/census-rust-*.txt 2>/dev/null |
            grep -oE '[A-Za-z0-9_/.-]+\.rs' | sort | uniq -c | sort -rn | head -30
        echo '```'
        echo
    } >> "$OUT/census-summary.md"
}

census_cpp() {
    # Qt6 定位:优先 QT6_DIR 环境变量,其次常见发行版路径
    if [ -z "${QT6_DIR:-}" ]; then
        for d in /usr/lib64/cmake/Qt6 /usr/lib/x86_64-linux-gnu/cmake/Qt6 /usr/lib/cmake/Qt6; do
            if [ -f "$d/Qt6Config.cmake" ]; then
                QT6_DIR="$d"
                break
            fi
        done
    fi
    if [ -z "${QT6_DIR:-}" ] || [ ! -f "$QT6_DIR/Qt6Config.cmake" ]; then
        echo "!! 未找到 Qt6Config.cmake。装 dev 包(dnf: qt6-qtbase-devel qt6-qtdeclarative-devel;" >&2
        echo "   apt: qt6-base-dev qt6-declarative-dev,需 Qt>=6.5)或 QT6_DIR=/path/to/cmake/Qt6 重跑" >&2
        return 1
    fi
    if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
        echo "!! 未找到 cmake/ninja。dnf install cmake ninja-build / apt install cmake ninja-build" >&2
        return 1
    fi
    echo "==> [cpp] cmake configure(Qt6_DIR=$QT6_DIR)"
    (cmake -S "$ROOT/app" -B "$OUT/build-cpp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DQt6_DIR="$QT6_DIR" 2>&1) |
        tee "$OUT/census-cpp-configure.txt" | grep -E 'CMake Error' | head -10 || true
    if [ ! -f "$OUT/build-cpp/build.ninja" ]; then
        echo "!! configure 失败,cpp 普查终止(详见 census/census-cpp-configure.txt)" >&2
        return 1
    fi
    echo "==> [cpp] ninja 编译(只统计不修;windows.h 级联报错属预期)"
    (cmake --build "$OUT/build-cpp" 2>&1) |
        tee "$OUT/census-cpp-build.txt" | grep -E 'fatal error|error:' | head -25 || true
    {
        echo "## C++ 阻塞清单(app)"
        echo
        echo "### 首爆缺失头文件(即平台耦合首层,预期 windows.h/dwmapi.h/UIAutomation.h 等)"
        echo '```'
        grep -hE 'fatal error:' "$OUT/census-cpp-build.txt" 2>/dev/null |
            sed -E 's/.*fatal error: //; s/:.*//' | sort | uniq -c | sort -rn
        echo '```'
        echo "### 报错涉及源文件(去重,仅含 error 行)"
        echo '```'
        grep -E 'fatal error|error:' "$OUT/census-cpp-build.txt" 2>/dev/null |
            grep -oE '[A-Za-z0-9_/.-]+\.(cpp|hpp|h)' | sort -u
        echo '```'
        echo
    } >> "$OUT/census-summary.md"
}

env_fingerprint

case "${1:-all}" in
    rust) census_rust ;;
    cpp)  census_cpp ;;
    all)  census_rust || true; census_cpp || true ;;
    *)    echo "用法: $0 [rust|cpp|all]" >&2; exit 2 ;;
esac

echo
echo "==> 普查完成。汇总: census/census-summary.md"
echo "    回填 docs/29 §4 时,把『静态结论』替换为『实测阻塞清单』并复核 A/B/C 三档工作量估计。"
