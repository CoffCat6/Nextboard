// 复制条目后自动粘贴(设置 `copyAutoPaste`):向前台窗口投一次 Ctrl+V。
//
// 为什么在宿主做而不是 core:Rust 侧没有窗口焦点概念,而"面板收起后谁是前台"
// 只有 Win32 层知道;且这纯粹是输入模拟,与业务无关(业务仍全在 nbcore)。
//
// ⚠️ 三条硬约束(改实现前先读):
//  1. **必须等面板真正收起**再投键。面板还在前台时投出的 Ctrl+V 会落在面板自己
//     身上(搜索框里多出一段文本),用户看到的是"粘到搜索框了"。
//  2. **UIPI 阻挡**:前台窗口若以管理员权限运行,低权限进程的 SendInput 会被静默
//     丢弃(返回值甚至可能仍是 TRUE)。这不是 bug,无法在本进程内绕过 ——
//     所以本功能默认关闭,且失败只记日志、不弹窗(用户按一次 Ctrl+V 即可)。
//  3. **不抢焦点**:不调用 SetForegroundWindow。用户按下复制的那一刻前台窗口就是
//     目标,任何"先切焦点再粘贴"的做法都会把内容粘到错误的地方。
#pragma once

#include <QString>

namespace nb {

/// 面板收起后延迟投一次 Ctrl+V。
///
/// 调用点是"复制成功 + 面板已隐藏"之后(见 Main.qml 的 copyEntryAndAutoPaste)。
/// 内部**自行读取设置** `copyAutoPaste`:关闭时直接返回,调用方不必判断 ——
/// 这样新增的复制入口只要调一次即可,漏判不会造成"该粘的不粘"。
///
/// delayMs 是**给窗口管理器收起面板的时间**。取 60~90ms:实测面板 hide() 到
/// 前台窗口真正拿到焦点约 20~40ms,留一倍余量;太长会让用户感觉到"卡了一下"。
/// ⚠️ 名字**不叫** autoPasteIfEnabled:CoreFacade 有一个同名 Q_INVOKABLE(QML 调的
/// 那个)。同名时在 CoreFacade 的成员函数里写 `autoPasteIfEnabled()` 会先命中成员
/// 自己,编译器只报 C2653「不是类/命名空间」,不容易一眼看出。
/// ⚠️ 本函数直接住在 namespace nb 下(**没有** auto_paste 子命名空间),
/// 调用点在本命名空间内直接写名字即可。
void pasteToForegroundIfEnabled(int delayMs = 80);

} // namespace nb
