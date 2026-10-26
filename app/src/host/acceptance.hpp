// 自动化验收:待办 / 设置 / 备份恢复 / 更新检查交互链路的**真实点击/按键**驱动
// (仅 main.cpp 的 --todo-flow / --clear-done-flow / --clear-all-flow / --update-flow /
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

/// 更新检查验收(--update-flow):真实点击「设置 → 关于 → 检查更新」→ 读回状态行与
/// 下载链接 → 真实点击「打开下载页」。检查地址由 `--update-url` 指向本地环回 fixture
/// (`tools/update_fixture.py` 的 /new /same /prerelease /notfound /garbage 五种响应),
/// 全程不依赖外网;验收模式已把浏览器启动降级为日志(不会弹浏览器)。日志前缀 UPDATE STEP*。
/// ⚠️ 会发网络请求(仅环回)并读设置,必须配 --data-dir(与 --todo-flow 同一门禁)。
void runUpdateFlow(PanelWindow& panel);

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

/// 框选浮条 + 标注态验收(--capture-toolbar-flow):真实建一块遮罩窗口、真投鼠标事件,
/// 断言"标注画在哪儿"与"落在哪儿"一致 + 「完成」交出的合成图含标注且图幅正确
/// —— 回归两代"标注画不准"缺陷(都是逻辑/物理/局部三套坐标系换算出错)。
/// 纯本机几何与渲染断言,不写库、不联网,无需 --data-dir。
void runCaptureToolbarFlow();

/// 智能框选验收(--smart-capture-flow):A 段纯几何(四套坐标的 4→1 换算 + 命中边界),
/// B 段端到端(真实窗口 → 悬停预选 → 单击采纳 → 直通模式确认矩形),
/// C 段手框回归(拖拽不被"单击采纳"劫持),D 段自过滤(本进程窗口不进候选表)。
/// 不写库、不联网,无需 --data-dir。
void runSmartCaptureFlow();

/// UIA 元素级正向验收(--smart-uia-flow):对真实窗口探内部元素,断言"元素不超出窗口";
/// 探不到任何元素不判失败(无 UIA 的实现是合法的),只留日志。
void runSmartUiaFlow();

/// 智能框选**接线**验收(--smart-live-flow):真走 startCaptureSession、真枚举桌面,
/// 断言"枚举早于遮罩 show"这条硬约束没被改坏(改坏的典型症状是永远框住整屏)。
/// 不写库,不需要 --data-dir。
void runSmartLiveFlow();

/// 设置页「智能框选」开关验收(--smart-settings-flow):切到截图分类 → 真实点击开关
/// → 读回设置断言翻转并落盘 → 再点一次复原。会写 settings,必须配 --data-dir。
void runSmartSettingsFlow(PanelWindow& panel);

/// 编辑器裁剪/旋转验收(--editor-flow):A 段引擎级直测(标注图元跟随变换、
/// 撤销重做绝对还原、马赛克纹理随底图重建、compose 不含裁剪覆盖层),
/// B 段真窗口端到端(真点画笔按钮 + R/C/Enter/Esc 键位 + committed 收图)。
/// 不写库、不联网,无需 --data-dir。
void runEditorFlow();

/// 自启注册表自检(--autostart-flow):**真实读写** HKCU 的 Run 键,验证
/// "应用内开自启写的命令行与安装器同一形态(带 --hidden)"。
/// 步骤:保存原值 → 写启(读回断言含 --hidden 且以 exe 路径开头)→
///       伪造一条旧形态值(无 --hidden)→ 调 refreshCommandLineIfStale() 断言被改写
///       → 写停(断言值消失)→ 还原原值。
/// 日志前缀 AUTOSTART STEP*。⚠️ 会**临时改写用户的 Run 键**,结束前必定还原;
/// 它不碰剪贴板数据库,但为了"不干扰真实实例",仍要求配 --data-dir
/// (main.cpp 的门禁统一处理)。
void runAutoStartFlow();

/// 长截图设置与热键的**往返**验收(--stitch-settings-flow)。
///
/// 只验"能否存进去、能否读回来、非法值是否被拒",不验 QML 控件像素。
/// 值得单开一条:设置字段的典型故障是"QML 写了但 core 不认"(名字拼错/大小写不符),
/// 表现是**用户改了不生效且没有任何提示** —— save_settings 走字段级合并,拼错的
/// 名字会被静默忽略。
void runStitchSettingsFlow();

/// 滚动长截图验收(--stitch-flow):用**构造的帧序列**跑通 append → finish → abort,
/// 断言位移/总高/缺口/会话隔离/幂等/宽度变化。
///
/// 为什么不用真屏:这些断言只与像素内容有关,真屏采集要真的滚窗口、等渲染,
/// 不可重复也不可并行,失败时还分不清是算法错还是采集错。真机采集链路由
/// docs/19 §8 A4 的真机验收覆盖,不在这里假装。
///
/// 纯内存图像运算:不写库、不联网、不碰注册表,无需 --data-dir。
void runStitchFlow();

/// 长截图**真实采集**验收(--stitch-live-flow):真的造一个会滚的窗口,
/// 真的走「框选 → 开始 → 挖洞 → 采帧 → 滚动 → 结束」全链路。
///
/// 与 `--stitch-flow`(构造帧验算法)、`--capture-toolbar-flow`(验状态机与窗口
/// 区域)互补 —— 那两条都**没有验证真正的采集链路**。这条链路断了的表现是
/// "每帧都一样 → 核心判 duplicate → 长图只有一屏高",而日志里全是 ok,
/// 完全看不出错在哪。故必须有这一条。
void runStitchLiveFlow();

/// 多屏配对自检:并排打印 Qt 侧(`QScreen::name()/geometry/DPR`)与核心侧
/// (`list_monitors` 的 name/几何)的显示器清单,并逐屏核对**名字能否配上**、
/// 配上后取到的底图尺寸是否与该屏几何自洽。用于验证/回归"多屏位置与分辨率
/// 混乱"这一缺陷(根因是两端枚举顺序不同却按索引配对)。
void reportMonitors();

/// 待办编辑卡片的**字段顺序**验收(--todo-layout-flow):
/// 文档要求"第一行标题、第二行备注"。这两块曾整体写反(备注跑到了标题上面),
/// 而 QML 里两块控件看起来都能用 —— 只有肉眼才能发现,所以必须有断言把它钉住。
/// 断言方式:打开编辑卡片,按 objectName 取两个输入框,比较它们在场景中的 y。
/// 纯读回,不写库,无需 --data-dir。
void runTodoLayoutFlow(PanelWindow& panel);

/// 桌面便签交互验收(--note-flow,docs/20-todo-sticky-note.md §10.2):
/// 播种 → 建窗 → 拖到右边缘吸附 → 开自动收起 → 鼠标移开/移回 → 改正文 → 删条目。
/// 会**移动真实光标**(Enter/Leave 只能靠真光标触发),必须配 --data-dir。
/// 日志前缀 NOTE STEP*,失败行含 PASS/FAIL。
void runNoteAcceptanceFlow();

} // namespace nb
