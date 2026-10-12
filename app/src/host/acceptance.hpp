// 自动化验收:待办交互链路的**真实点击**驱动(仅 --todo-flow 使用)。
//
// 为什么单独成文件:这段逻辑有 ~100 行、只在验收时跑,留在 main.cpp 里会
// 把产品入口(参数解析/窗口装配/信号接线)淹没;抽出来也让"哪些代码是验收
// 专用"一目了然。
//
// ⚠️ 本文件只被 main.cpp 的 --todo-flow 分支调用,不进产品交互路径。
//    移除方式:删本对文件 + main.cpp 的 --todo-flow 分支 + Main.qml 的
//    acceptance* 成员 + PanelWindow 的验收钩子(见 panel_window.hpp 注释)。
#pragma once

namespace nb {

class PanelWindow;

/// 驱动一次完整的待办交互验收:
///   打开卡片 → 点「截止」开浮层 → 拨轮 → **真实点击**确定 → 输入正文
///   → **真实点击**保存 → 读库比对。
/// 每步把状态打到 qInfo(前缀 STEP*),供人工或脚本断言。
/// 结束后调用 QApplication::quit()。
void runTodoAcceptanceFlow(PanelWindow& panel);

} // namespace nb
