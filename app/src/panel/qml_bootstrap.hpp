// QML 环境装配:类型注册、单例注入、nbcore 通知接线(替代原 Sciter 桥的安装)。
//
// 装配内容:
// - ClipEntryModel → QML 名 "entries"(Main.qml 直接按名访问);
// - CoreFacade     → QML 名 "Core"(单例);
// - Theme.qml      → QML 单例(pragma Singleton + qmldir);
// - CoreFacade::toastMessage → 由 QML 侧 Connections 显示;
// - CoreFacade::clipChanged 的发射源是 PanelWindow::nativeEvent(见下 qmlAttach)。
#pragma once

#include <QJsonObject>

class QObject;
class QQuickWidget;
class QBoxLayout;

namespace nb {

class PanelWindow;
class ClipEntryModel;

/// QML 侧那个唯一的列表模型实例(由 setupQmlEnvironment 创建)。
/// **验收专用**:让 --note-flow 能走"面板编辑卡片的真实保存路径",
/// 而不是裸调 todo_update 绕开组装逻辑。setupQmlEnvironment 之前返回 nullptr。
ClipEntryModel* entryModelInstance();

/// 在 QApplication 之后、PanelWindow 创建前调用一次。
void setupQmlEnvironment(const QJsonObject& startupSettings = {});

/// 面板创建后接线:nbcore 通知 → CoreFacade 信号(线程安全,直连同线程)。
void qmlAttachPanel(PanelWindow* panel);

/// QML 加载失败时的兜底:用原生 QLabel 顶替内容区,把第一条错误直接显示出来。
///
/// 为什么需要:本项目"主面板整片空白"已反复出现多次(根因各不相同:模块未部署、
/// FINAL 属性覆盖、单例失效……),而空白窗口对用户零信息量、只能靠翻日志。
/// 这里至少把错误摆在界面上(完整信息仍在 nextboard.log)。
/// @return 是否处于加载失败状态
bool handleQmlLoadFailure(QQuickWidget* quick, QBoxLayout* layout);

} // namespace nb
