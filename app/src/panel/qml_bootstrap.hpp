// QML 环境装配:类型注册、单例注入、nbcore 通知接线(替代原 Sciter 桥的安装)。
//
// 装配内容:
// - ClipEntryModel → QML 名 "entries"(Main.qml 直接按名访问);
// - CoreFacade     → QML 名 "Core"(单例);
// - Theme.qml      → QML 单例(pragma Singleton + qmldir);
// - CoreFacade::toastMessage → 由 QML 侧 Connections 显示;
// - CoreFacade::clipChanged 的发射源是 PanelWindow::nativeEvent(见下 qmlAttach)。
#pragma once

class QObject;

namespace nb {

class PanelWindow;

/// 在 QApplication 之后、PanelWindow 创建前调用一次。
void setupQmlEnvironment();

/// 面板创建后接线:nbcore 通知 → CoreFacade 信号(线程安全,直连同线程)。
void qmlAttachPanel(PanelWindow* panel);

} // namespace nb
