// 自动化验收:待办 / 设置 / 备份恢复交互链路的**真实点击/按键**驱动
// (仅 main.cpp 的 --todo-flow / --clear-done-flow / --clear-all-flow /
//  --settings-flow / --restore-flow / --todo-groups 分支使用)。
//
// 为什么单独成文件:这段逻辑有 ~100 行、只在验收时跑,留在 main.cpp 里会
// 把产品入口(参数解析/窗口装配/信号接线)淹没;抽出来也让"哪些代码是验收
// 专用"一目了然。
//
// ⚠️ 本文件只被 main.cpp 的 --*-flow 分支调用,不进产品交互路径。
//    移除方式:删本对文件 + main.cpp 的对应分支 + Main.qml 的
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

/// 清空已完成待办验收(--clear-done-flow):播种 2 条已完成 + 1 条未完成 →
/// 切待办视图 → **真实点击**组头「清空已完成」→ **真实点击**确认「清空」→
/// 读库断言"归档清空 + 未完成不受影响"。日志前缀 CLEAR-DONE STEP*。
/// ⚠️ 会写库(创建/删除待办),必须配 --data-dir(与 --todo-flow 同一门禁)。
void runClearDoneFlow(PanelWindow& panel);

/// 清空全部数据验收(--clear-all-flow;2026-10-15 语义调整后的常驻回归):
///   播种 1 条剪贴板 + 2 条待办(1 未完成 1 已完成)→ 真实点击设置按钮开抽屉 →
///   「清空全部数据」滚进可视区 → **真实点击**按钮 → **真实点击**「确认清空」→
///   读库断言"条目/待办全清、counts 全口径归零"。日志前缀 CLEAR-ALL STEP*。
/// ⚠️ 会写库并删除隔离目录内的全部数据,必须配 --data-dir(与 --todo-flow 同一门禁)。
void runClearAllFlow(PanelWindow& panel);

/// 设置抽屉交互验收(--settings-flow),回归三类用户实测缺陷:
///   ① 手输数值不保存 —— 点 SpinBox → 键入 "999" + 回车 → get_settings 读回;
///   ② 抽屉不挡主界面 / 只能滑到顶关闭 —— 滚到底后点 ✕(固定头部)、
///      点抽屉外的列表卡片(应被吞掉:抽屉关、面板不隐藏、剪贴板不变)、Esc;
///   ③ 零待办时「待办」页签被藏 —— 读回页签可见性 + 真实点击切换视图。
/// 日志前缀 SETTINGS STEP*。会写库(save_text_entry 播种)与 settings(historyLimit),
/// 必须配 --data-dir(与 --todo-flow 同一门禁)。
void runSettingsFlow(PanelWindow& panel);

/// 恢复/导入验收(--restore-flow):确认卡真实点击驱动两条链路 ——
///   ① 恢复(替换式):备份 → 变更(加条目/删条目/删 blob 文件)→ UI 确认恢复
///      → 断言数据与图片文件回到备份时刻 + 险备份 zip 存在;
///   ② 导入(合并式):再备份 → 删掉备份独有条目 → UI 确认导入 → 断言条目回来
///      (重复跳过)+ 再导一次幂等。
/// 日志前缀 RESTORE STEP*。⚠️ 唯一跳过的是原生文件对话框(路径直写),
/// 会写库/替换数据目录,必须配 --data-dir(与 --todo-flow 同一门禁)。
void runRestoreFlow(PanelWindow& panel);

/// 只做一件事:切到待办视图并把各行分组键打出来(STEP5 的自检独立入口)。
/// 供"先用脚本播种各种分组的待办,再校验组标题分界点"使用 ——
/// 不建数据、不改状态,纯读回,故可反复运行。
void reportTodoGroups(PanelWindow& panel);

/// 多屏配对自检:并排打印 Qt 侧(`QScreen::name()/geometry/DPR`)与核心侧
/// (`list_monitors` 的 name/几何)的显示器清单,并逐屏核对**名字能否配上**、
/// 配上后取到的底图尺寸是否与该屏几何自洽。用于验证/回归"多屏位置与分辨率
/// 混乱"这一缺陷(根因是两端枚举顺序不同却按索引配对)。
void reportMonitors();

} // namespace nb
