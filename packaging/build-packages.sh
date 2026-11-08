#!/usr/bin/env bash
# packaging/build-packages.sh — M4 分发:出 AppImage + deb(在 Linux 打包机上运行)
#
# 布局契约(docs/29 M4):
#   * exe 同级资产契约:libonnxruntime.so* 与 models/ocr 必须紧邻二进制
#     (nbcore 的 rapid.rs 以 current_exe() 解析安装目录)——两种包都放 bin/。
#   * 数据目录:app_paths.cpp 的 Linux 解析器在 exe 同级 data/ 不可写时
#     回落 ~/.local/share/Nextboard —— /opt 安装与 AppImage 只读挂载都靠它。
#   * Qt 运行库由 linuxdeploy-plugin-qt 拉齐(与宿主发行版 Qt 无关,免版本地狱);
#     插件/QML 位次见 qt.conf(../plugins、../qml);库本体靠 INSTALL_RPATH。
#
# 用法:仓库根目录下  ./packaging/build-packages.sh
# 产物:packaging/dist/Nextboard-<ver>-x86_64.AppImage + nextboard_<ver>_amd64.deb
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP=nextboard
VERSION="$(grep -oP 'project\(Nextboard VERSION \K[0-9.]+' "$REPO_ROOT/app/CMakeLists.txt" | head -1)"
BUILD="${BUILD_DIR:-$REPO_ROOT/build-linux}"
PKG="$REPO_ROOT/packaging"
WORK="$PKG/.work"
STAGE="$WORK/AppDir"
QT_ROOT="${QT_ROOT:-$HOME/Qt/6.9.0/gcc_64}"
DIST="$PKG/dist"

if [ -z "$VERSION" ]; then echo "ERROR: 版本号解析失败(app/CMakeLists.txt)"; exit 1; fi
[ -x "$BUILD/$APP" ] || { echo "ERROR: 先构建 $BUILD/$APP(./build.sh all)"; exit 1; }

echo "== [0/5] 打包工具(linuxdeploy + qt 插件) =="
CACHE="$HOME/.cache/nextboard-pkg"; mkdir -p "$CACHE"
LD="$CACHE/linuxdeploy-x86_64.AppImage"
LDQT="$CACHE/linuxdeploy-plugin-qt-x86_64.AppImage"
RUNTIME="$CACHE/runtime-x86_64"
[ -f "$LD" ]   || curl -fL --retry 3 -o "$LD"   https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
[ -f "$LDQT" ] || curl -fL --retry 3 -o "$LDQT" https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
# type2 runtime 手动落盘:appimage 插件构建时要从 GitHub 拉,504 间歇性炸包;
# 插件认 LDAI_RUNTIME_FILE 环境变量(等价 --runtime-file)。
[ -f "$RUNTIME" ] || curl -fL --retry 5 --retry-delay 3 -o "$RUNTIME" \
    https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
export LDAI_RUNTIME_FILE="$RUNTIME"
chmod +x "$LD" "$LDQT"

echo "== [1/5] AppDir 骨架(exe 同级资产 + 图标 + desktop) =="
rm -rf "$STAGE" "$WORK/debroot"
mkdir -p "$STAGE/usr/bin" "$STAGE/usr/lib" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/icons/hicolor/256x256/apps" \
         "$STAGE/usr/share/icons/hicolor/128x128/apps" \
         "$STAGE/usr/share/icons/hicolor/64x64/apps" \
         "$STAGE/usr/share/icons/hicolor/48x48/apps"
install -m755 "$BUILD/$APP"        "$STAGE/usr/bin/"
install -m755 "$BUILD/libnbcore.so" "$STAGE/usr/lib/"
cp -P "$BUILD"/libonnxruntime.so*  "$STAGE/usr/bin/"
cp -r "$BUILD/models"              "$STAGE/usr/bin/models"
# ico 是多帧序列(9 个尺寸),直接 convert 会写成 -0..-8 一串;取最大帧(-1)
# 作源再逐尺寸缩放。
ICON_SRC="$WORK/icon-256.png"
convert "$REPO_ROOT/app/resources/$APP.ico[-1]" "$ICON_SRC"
for s in 256x256 128x128 64x64 48x48; do
    convert "$ICON_SRC" -resize "${s/x*/}x${s/x*/}" \
        "$STAGE/usr/share/icons/hicolor/$s/apps/$APP.png"
done
sed -e "s|@BINDIR@|$APP|g" -e "s|@VERSION@|$VERSION|g" "$PKG/nextboard.desktop.in" \
    > "$STAGE/usr/share/applications/$APP.desktop"
cp "$PKG/qt.conf" "$STAGE/usr/bin/qt.conf"
# ⚠️ 应用 QML 源文件放进 AppDir:plugin-qt 的 qmlimportscanner 只扫**磁盘上的
# .qml 文件**,而本应用的 QML 全部内嵌 qrc —— 不放一份源进去,Qt 的 QML 模块
# (QtQuick/Controls/Basic 等)不会被部署,运行期面板加载失败(2026-11-08
# AppImage 冒烟实测:libQt6QuickControls2Basic.so.6 缺失)。扫描产物含依赖
# 解析,比手动 cp Qt 目录更完整。
mkdir -p "$STAGE/usr/share/$APP/qml-src"
cp "$REPO_ROOT/app/panel/qml/"*.qml "$STAGE/usr/share/$APP/qml-src/"

echo "== [2/5] linuxdeploy + qt 插件(拉齐 Qt 运行库) =="
export QMAKE="$QT_ROOT/bin/qmake"
export QML2_IMPORT_PATH="$QT_ROOT/qml"
export QML_IMPORT_PATH="$QT_ROOT/qml"
"$LD" --appdir "$STAGE" --plugin qt \
      --desktop-file "$STAGE/usr/share/applications/$APP.desktop" 2>&1 | tail -6

echo "== [2.5/5] QML 插件依赖兜底 =="
# ⚠️ plugin-qt 部署了 QML 插件本体,但**不解析插件自己的私有实现库**——
# Basic 样式的 libQt6QuickControls2Basic.so.6 留在构建机 Qt 目录,运行期
# dlopen 失败(2026-11-08 AppImage 冒烟:面板加载失败对话框)。逐插件 ldd,
# 把落在 $QT_ROOT/lib 里的缺失依赖连符号链接目标一起搬进 usr/lib;
# 再 patchelf 统一多深度 RPATH(不同深度的 qml 子目录到 usr/lib 的
# 相对层数不同,多入口按序尝试)。
sudo apt-get install -y -qq patchelf >/dev/null 2>&1
for so in $(find "$STAGE/usr/qml" "$STAGE/usr/plugins" -name "*.so*" -type f 2>/dev/null); do
    for dep in $(ldd "$so" 2>/dev/null | awk '/=> \/.*\// {print $3}'); do
        case "$dep" in
            "$QT_ROOT"/lib/*)
                name="$(basename "$dep")"
                [ -e "$STAGE/usr/lib/$name" ] || cp -P "$dep" "$STAGE/usr/lib/$name"
                real="$(readlink -f "$dep")"
                rname="$(basename "$real")"
                [ -e "$STAGE/usr/lib/$rname" ] || cp -P "$real" "$STAGE/usr/lib/$rname"
                ;;
        esac
    done
done
find "$STAGE/usr/qml" "$STAGE/usr/plugins" -name "*.so*" -type f \
    -exec patchelf --set-rpath '$ORIGIN/../../lib:$ORIGIN/../lib:$ORIGIN/../../../lib:$ORIGIN/../../../../lib' {} \;

echo "== [3/5] AppImage =="
mkdir -p "$DIST"
rm -f "$DIST/Nextboard-$VERSION-x86_64.AppImage"
# ⚠️ --output=appimage 必须等号形式(空格形式被参数解析拒绝);
# appimage 插件把产物写在 CWD,故 cd 进 dist 再调用。文件名取
# desktop 的 Name + X-AppImage-Version(= Nextboard-$VERSION)。
( cd "$DIST" && "$LD" --appdir "$STAGE" --output=appimage 2>&1 | tail -3 )

echo "== [5/5] deb(/opt/nextboard + 系统位 desktop/icon) =="
DEBROOT="$WORK/debroot"
mkdir -p "$DEBROOT/DEBIAN" \
         "$DEBROOT/opt/$APP" \
         "$DEBROOT/usr/bin" \
         "$DEBROOT/usr/share/applications" \
         "$DEBROOT/usr/share/icons/hicolor/256x256/apps" \
         "$DEBROOT/usr/share/icons/hicolor/128x128/apps" \
         "$DEBROOT/usr/share/icons/hicolor/64x64/apps" \
         "$DEBROOT/usr/share/icons/hicolor/48x48/apps"
# /opt/nextboard = 部署完的 usr 树(bin+lib+plugins+qml+share 资产原样并入)
cp -r "$STAGE/usr/." "$DEBROOT/opt/$APP/"
sed -e "s|@BINDIR@|/opt/$APP/bin/$APP|g" -e "s|@VERSION@|$VERSION|g" "$PKG/nextboard.desktop.in" \
    > "$DEBROOT/usr/share/applications/$APP.desktop"
for s in 256x256 128x128 64x64 48x48; do
    cp "$STAGE/usr/share/icons/hicolor/$s/apps/$APP.png" \
       "$DEBROOT/usr/share/icons/hicolor/$s/apps/$APP.png"
done
ln -sf "/opt/$APP/bin/$APP" "$DEBROOT/usr/bin/$APP"
# 依赖:仅系统基础库(Qt 全捆绑);以部署后 ldd 的缺失面为准取常用集
cat > "$DEBROOT/DEBIAN/control" <<EOF
Package: $APP
Version: $VERSION-1
Section: utils
Priority: optional
Architecture: amd64
Installed-Size: $(du -sk "$DEBROOT/opt" | cut -f1)
Maintainer: Nextboard <dev@nextboard.local>
Depends: libc6 (>= 2.31), libgcc-s1, libstdc++6, libx11-6, libxcb1, libfontconfig1, libxkbcommon0, libxkbcommon-x11-0, libxcb-cursor0, libgl1, libegl1
Description: 剪贴板历史 / 截图标注 / 贴图 / OCR / 翻译 / 待办 工具箱
 Nextboard 是跨平台效率工具箱:剪贴板历史、截图与标注、贴图、
 OCR 取字、翻译、待办与桌面便签。Qt 运行库随包捆绑于
 /opt/$APP,不依赖发行版 Qt 版本。
EOF
cat > "$DEBROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
update-desktop-database /usr/share/applications 2>/dev/null || true
gtk-update-icon-cache -f /usr/share/icons/hicolor 2>/dev/null || true
exit 0
EOF
chmod 755 "$DEBROOT/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$DEBROOT" "$DIST/${APP}_${VERSION}-1_amd64.deb"

echo "== 产物 =="
ls -la "$DIST"
du -sh "$STAGE" | sed 's/^/AppDir 体积: /'
