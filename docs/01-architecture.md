# Nextboard 总体架构

| 项 | 内容 |
| --- | --- |
| 版本 | v1.0 |
| 日期 | 2026-10-05 |
| 状态 | 已定稿,进入实施 |
| 前代项目 | clipboard-plus(D:/3project/rust/clipboard-plus) |
| 关联文档 | [nbcore 改写路线](./02-migration-core.md) · [开发路线图](./03-roadmap.md) |

---

## 1. 产品定位

Windows 桌面**一体化效率板**:剪贴板历史、截图(框选/全屏/编辑)、翻译三大能力集成为一块
"board"。第一代(clipboard-plus)验证了功能设计,本项目的首要工程目标是:

1. **根治内存**:第一代 WebView2 多窗口模型(每窗口一个渲染进程)实测常驻 300–500MB,
   本项目必须收敛到 ~100–140MB(主面板 + 10 张贴图);
2. **继承资产**:第一代 Rust 核心(存储/去重/翻译/OCR)以 DLL 形态直接复用;
3. **观感升级**:主面板采用 HTML/CSS 渲染,延续第一代的界面设计语言。

**非目标**(当前阶段不做):移动端、云同步、跨设备、Linux/macOS(全部决策按 Windows 优先,
核心层保持平台中立,GUI 层接受 Windows 绑定)。

## 2. 技术栈定案

| 层 | 技术 | 版本/工具链 | 选型理由 |
| --- | --- | --- | --- |
| 业务核心 | **Rust → nbcore.dll** | `x86_64-pc-windows-msvc` | 复用 clipboard-plus 的 store/clip/translate/ocr 全部业务代码与 31 个单测;FFI 面用 JSON 门面收窄 |
| 宿主/UI 框架 | **Qt 6(Widgets)** | MSVC 套件(**禁用 MinGW 套件**,跨 ABI 风险) | 拥有事件循环与进程壳;遮罩/贴图/编辑器用 QPainter/QGraphicsScene,开发量约为手写 Win32/D2D 的一半 |
| 主面板渲染 | **Sciter**(嵌入 Qt 窗口) | 单 DLL ~5MB | 历史/搜索/设置/翻译抽屉用 HTML/CSS 编写,观感与迭代速度对标 Web;非 Chromium,单窗口 ~10–20MB |
| 存储 | SQLite(rusqlite bundled,含 FTS5) | 随 nbcore | 与第一代同构,schema 直接继承并扩展 |
| OCR | Windows.Media.Ocr(经 nbcore) | 随 nbcore | 离线零依赖;后处理参数(2x 放大)沿用第一代实测结论 |
| 翻译 | WinHTTP + ureq(经 nbcore) | 随 nbcore | 三类 Provider 与代理模型原样继承 |

**许可要点**:Qt 6 走 LGPL 动态链接(合规要求:允许用户替换 Qt DLL);Sciter 为闭源引擎,
**采用前须到官网确认最新商用条款**(见 [03-roadmap.md](./03-roadmap.md) S0)。

## 3. 进程与窗口模型

**单进程**,Qt 拥有事件循环与进程壳;Sciter 作为原生子窗口嵌入 Qt 主面板容器。
禁止双进程方案——贴图窗口长驻,进程隔离省不了内存,反而把图片传递变回跨进程 IPC
(正是本项目要逃离的 WebView2 模型)。

### 3.1 窗口清单与内存预算

| 窗口 | 技术 | 生命周期 | 预算 |
| --- | --- | --- | --- |
| 主面板(历史/搜索/设置/翻译抽屉) | Qt 无边框窗口 + 嵌入 Sciter | 常驻;失焦隐藏(沿用第一代 200ms 延迟复查方案) | ~40–60MB(常驻上限) |
| 截图遮罩(每屏一个) | Qt 无边框置顶窗口 | 会话期创建,收起即释放底图,空闲超时销毁 | ~5MB/屏 |
| 贴图 | Qt 无边框置顶窗口 | 手动关闭;**数量软上限 10 张,超限淘汰最旧** | 3–10MB/张 |
| 编辑器 | Qt 窗口,QGraphicsScene | 会话期创建,提交/取消即销毁 | 会话期 ~80MB(大图),会话结束释放 |
| 翻译 | **并入主面板抽屉**(不再独立窗口) | 随主面板 | 0(取代第一代 D12"关闭即销毁"的取舍) |

对照:第一代主窗口 + N 遮罩 + N 贴图 + 编辑器 + 翻译全部是 WebView2 实例;本项目
**Sciter 实例只有 1 个**,其余全为轻量原生窗口。整体常驻预期 **~100–140MB**。

### 3.2 失焦隐藏

主面板沿用第一代方案:窗口失焦后延迟 200ms 复查,仍无焦点才隐藏(规避 Windows 拖动
标题栏触发的虚假 `Focused(false)`)。实现位置移到 C++ 宿主(第一代在 Tauri lib.rs)。

## 4. 分层与数据流

```
┌──────────────────────────────────────────────────────┐
│                  单进程(Qt 宿主)                      │
│  ┌─────────────────┐   ┌───────────────────────────┐ │
│  │ Sciter 主面板    │   │ Qt 窗口层                  │ │
│  │ 历史/搜索/设置/   │   │ 遮罩×N(会话期)             │ │
│  │ 翻译抽屉         │   │ 贴图×N(软上限 10)           │ │
│  │ (HTML/CSS)      │   │ 编辑器(QGraphicsScene)      │ │
│  └────────┬────────┘   └─────────────┬─────────────┘ │
│           │  C++ 薄封装 nbcore.h(全部 UI 共用)       │
│  ┌────────┴──────────────────────────┴─────────────┐ │
│  │ nbcore.dll(Rust cdylib)                         │ │
│  │  JSON 门面:nb_call(method, req_json) → resp_json│ │
│  │  RGBA 通道:nb_capture_monitor → 裸像素(截图专用) │ │
│  │  通知:PostMessage(通知 HWND) → 前端拉取          │ │
│  │  内部:store/FTS5 · clip 监听(事件驱动化)         │ │
│  │       去重淘汰 · 缩略图 · translate · ocr         │ │
│  └──────────────────────────────────────────────────┘ │
│  C++ 宿主保留:托盘 · 全局热键 · 窗口生命周期 · DnD      │
└────────────────────────────────────────────────────────┘
```

职责一句话:**"有窗口的归 C++,有逻辑的归 Rust,有界面的归 Sciter/Qt"**。

### 4.1 核心事件流

```
剪贴板变化(系统) → nbcore 监听线程(AddClipboardFormatListener,事件驱动)
  → 读取/过滤/去重 → 入库(文本直存;图片 PNG 落盘 blobs/ + DB 记路径)
  → PostMessage(通知 HWND, WM_APP_CLIP_CHANGED)
  → Qt 收到 → ① 调 nb_call("list_entries") 增量刷新主面板
             → ② 广播给贴图/编辑器等关心的窗口
```

截图会话流(对照第一代 phase5,去掉跨进程底图传输):

```
热键(C++) → nb_call("capture_begin") → 各屏 RGBA 直取(Rust 内部 mss/GDI 等价物)
  → C++ 逐屏创建遮罩窗口,QImage::fromPixels 直绘冻结底图(无 IPC、无协议头、无 CORS)
  → 框选确认 → 裁剪(Rust 侧 nb_capture_crop,入 RGBA 通道)→ 入库/写剪贴板
  → (可选)编辑器:QGraphicsScene 以同一份 QImage 为底
```

## 5. nbcore.dll FFI 契约

**门面形态**:少量类型化导出 + JSON 门面,不按命令逐个导出(第一代 38 个 command,
逐个导出会把 FFI 面撑大且契约脆)。

```rust
// nbcore.dll 导出全集(签名示意,完整表见 02-migration-core.md §4)
nb_version() -> u32
nb_init(config: *const NbConfig) -> i32        // 数据目录、通知 HWND、日志级别
nb_call(method: *const c_char, req: *const c_char, out: *mut *mut c_char) -> i32
nb_capture_monitor(index: u32, out_rgba: *mut *mut u8, w: *mut u32, h: *mut u32) -> i32
nb_free(ptr: *mut c_void)
nb_shutdown()
```

- `nb_call` 的 `method` 即第一代 command 名(list_entries / copy_entry / translate_text /
  ocr_entry / …),请求/响应 JSON 结构沿用第一代 `src/api/types.ts` 的 camelCase 契约,
  前端代码可近乎原样搬运;
- **panic 边界(最高优先级坑)**:核心 crate 的 release profile 必须从第一代的
  `panic = "abort"` 改回 unwind,且每个导出函数体包 `catch_unwind` 转错误码——
  否则任何 panic 直接杀宿主进程;
- 内存:谁分配谁释放,所有出参指针配对 `nb_free`;字符串 ABI 层统一 UTF-8 + 长度。

## 6. 数据模型(第一代继承 + 一体化扩展)

第一代 v4 schema 原样继承(条目表/FTS5/触发器/迁移机制),新增两列,避免走它
v1→v3 重建表的老路:

```sql
ALTER TABLE clip_entries ADD COLUMN source TEXT NOT NULL DEFAULT 'clipboard';
  -- 'clipboard' | 'capture' | 'translate'(board 化的统一素材库基础)
ALTER TABLE clip_entries ADD COLUMN tags TEXT;   -- JSON 数组,预留分组/收藏
```

FTS5 同步触发器相应更新。第一代踩坑结论(全部继续生效):

- FTS5 用普通 FTS 表(TEXT 主键不能用外部内容表);
- 中文搜索 = FTS 通道 + LIKE 兜底(含 `kind='image'` 时查 `ocr_text`);
- 图片写入系统剪贴板必须单次 `SetClipboardData`(CF_DIBV5)且带打开重试;
- 图片先入库后写剪贴板,防监听自触发重复;
- 缩略图 320px 上限、入库时预生成、原子写、删除连带清理。

## 7. 目录结构(目标)

```
Nextboard/
├── docs/                    # 本文档体系
├── core/                    # Rust 工作区
│   ├── nbcore/              # 库 crate:从 clipboard-plus 剥离的业务核心
│   │   ├── src/
│   │   │   ├── store/ clip/ translate/ ocr/ settings/   # 平移(见 02 §3)
│   │   │   ├── ffi/         # JSON 门面、RGBA 通道、通知
│   │   │   └── lib.rs
│   │   └── Cargo.toml       # crate-type=["cdylib"],panic 改 unwind
│   └── nbcore-tests/        # 集成测试(真机行为:剪贴板重试、FTS 中文等)
├── app/                     # C++/Qt 宿主
│   ├── CMakeLists.txt
│   ├── host/                # 进程壳:托盘、热键、单实例、自启、设置桥接
│   ├── panel/               # 主面板容器 + Sciter 嵌入 + HTML/JS 桥
│   │   └── sciter/          # 主面板 HTML/CSS/JS(从第一代 src/ 移植)
│   ├── capture/             # 遮罩窗口、会话状态机
│   ├── pin/                 # 贴图窗口
│   ├── editor/              # QGraphicsScene 编辑器
│   └── 3rdparty/            # Sciter SDK、nbcore.h
└── tools/                   # 打包脚本、图标
```

## 8. 风险登记

| # | 风险 | 等级 | 缓解 |
| --- | --- | --- | --- |
| R1 | Rust panic 穿 FFI 杀宿主 | 高 | release unwind + 全导出 catch_unwind + nbcore-tests 压测 |
| R2 | Sciter 商用条款变化 | 中 | S0 里程碑先确认条款,不达标则主面板回退 Qt Widgets(窗口层不受影响) |
| R3 | Qt + Sciter 同线程消息循环冲突 | 中 | Sciter 官方有 Qt 集成先例;S2 做嵌入 PoC,失败回退方案同 R2 |
| R4 | MSVC/MinGW 工具链混用 | 中 | 规范:全链 MSVC(Rust msvc 目标 + Qt MSVC 套件) |
| R5 | 剥离期间双项目并行漂移 | 低 | 02-migration-core.md 逐文件映射表;clipboard-plus 只读不改 |
| R6 | 内存预算不达标 | 中 | 03-roadmap.md S6 设量化验收(见 §3.1 预算),超限则主面板降级单窗口 |

## 9. 已裁决事项(与第一代决策的对照)

| 本项目 | 取代第一代 | 理由 |
| --- | --- | --- |
| 单 Sciter 主面板,翻译/编辑器并入或原生窗口 | D12 翻译窗口"关闭即销毁" | 不再需要为一个 WebView 的内存省建窗成本 |
| 遮罩 Qt 直绘冻结底图 | captureimg raw 协议 + CORS 头 | 同进程内无跨源,协议/污染问题整体消失 |
| 贴图软上限 10 张(硬性) | Phase 11 软上限(建议性) | 原生窗口便宜,但仍需上限兜底内存预算 |
| 剪贴板监听 AddClipboardFormatListener | 200ms 轮询 + 序号 | 零轮询 CPU,事件延迟从 ~100ms 降至消息级 |
| JSON 门面 FFI | (第一代无此层) | 契约沿用 types.ts,前端可搬运,FFI 面最小 |
