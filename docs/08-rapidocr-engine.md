# 08 · RapidOCR 引擎接入（宿主侧 `nbocr.dll`）

| 项 | 内容 |
| --- | --- |
| 版本 | v0.1（设计与实施计划） |
| 日期 | 2026-10-15 |
| 状态 | **P1 已实施并提交**（`c4d1aeb` 移除 Tesseract 后端）；**P2–P4 未实施**。§3 全部数字来自 2026-10-15 spike **实测**（官方预编译 CLI、独立进程口径，非嵌入形态）；§14 明确列出未验证项 |
| 关联 | [03-roadmap.md](./03-roadmap.md) §6.4（包体目标 `<60MB` 需改为 `<90MB`）· [04-build-package-release.md](./04-build-package-release.md) §3 构建 / §4 运行期依赖与体积预算 / §5 打包 / §6 安装 / §10 实测记录 · [06-memory-optimization.md](./06-memory-optimization.md) §11.2 复测协议 / §12 分阶段拆解 · [07-auto-update.md](./07-auto-update.md)（发布渠道仍空置） |

---

## 1. 目标与非目标

### 1.1 做

给 OCR 增加一个**自带模型、不依赖系统语言包**的本地引擎（RapidOCR / PP-OCRv4），
以解决内置 Windows 引擎在**小字号、中英混排、全屏截图**三类场景下的质量缺陷（§2 有实测证据）。
接入位置在 **Qt/C++ 宿主侧**，独立 DLL + C ABI，不进 `nbcore`。

### 1.2 明确不做

- **不做 `ocr_text` 回填**（图片条目内容可搜索）：DB 与 FTS 管道早已就位
  （`store/migrations/v4.sql:29,35,47`、`v6.sql:56`、`store/entry.rs:347,366`），
  但 `store/entry.rs:856 set_ocr_text` **至今无生产调用点**（全仓仅 `entry.rs:1681` 测试）。
  本轮只加引擎，回填另开一轮。
- 不接云 OCR、不用 GPU（`-G -1` CPU 推理）、不联网下载模型（保持全离线；出网只属于 translate）。
- 不扩 `nbcore` 方法表（ABI v1 冻结，见 03 §29）；不改 FFI JSON 协议的既有字段语义。
- 不做 Rust 侧推理（`ort` 至今仍是 `2.0.0-rc.14`，且 RapidAI **没有官方 Rust 绑定**：
  crates.io API 返回 `crate rapidocr does not exist`，官方 README 只列 Python/C++/Java/C#）。

---

## 2. 为什么值得做（spike 实测质量对比）

同一批样例图，两个引擎（内置引擎走现成测试 `ocr.rs` 的 `ocr_quality_bench`，默认放大 2x）：

| 样例 | RapidOCR PP-OCRv4 mobile | Windows 内置引擎 |
| --- | --- | --- |
| 12px 等宽代码<br>`const int MAX_LIMIT = build_entry("lI1O0");`<br>`if (1l != Il) Count0 += 9O;` | `const int MAX_LIMIT = build_entry("1I100");`<br>`if (11 != I1) Count0 += 90;` | 原图（48ms）：`build 已 nt 冂 脲 “`（整行崩）<br>放大 2x（38ms）：`const int MAX_LIt•11T = bui1d—entry("1110Ø");` |
| 14px 中英混排<br>`会议纪要：Q3 revenue 增长 12.5%，请在 8 月 5 日前提交 review。` | `会议纪要：Q3revenue增长12.5%，请在8月5日前提交review。` | 放大 2x（31ms）：`会 议 纪 要 ， Q3 revenue 增 长 125 ％ ， 请 在 8 月 5 日 前 提 交 revi ew.` |
| 真实全屏截图 1920×1080 | **168 框 / 157 个非空行**，界面文字基本可读 | 放大 2x（0.98s）：`享 文 件 到 辑 0` —— **实测等于不可用** |

要点（全部为实测）：

1. 内置引擎把 `build_entry` 的下划线读成长破折号 `—`，正是 `ocr.rs:39-41` 记载并已靠"放大 2x"缓解的缺陷；RapidOCR 保住下划线。
2. 内置引擎中文夹字间空格（`会 议 纪 要`），且把 `12.5%` 读成 `125 ％`（丢小数点）。
   `ocr.rs:329 normalize_cjk_spaces` 是为前者写的补丁，但**生产路径无调用点**（全仓仅测试引用）。
3. **全屏截图这一主场景，内置引擎实测不出可用结果**，而截图入库正是本产品的核心动作。
4. RapidOCR 的残余错误集中在字形歧义（`l/I/1`、`O/0`），不是整行崩塌 —— 属可接受质量档。

> ⚠️ **出处提示**：`ocr.rs:39`、`ocr.rs:186` 等注释把实测依据指向 `docs/phase6.md §3.6`，
> 而该文件在现行 `docs/` 下**已不存在**（阶段文档被收敛，属既有文档漂移，非本次引入）。
> 本文 §2/§3 的数字是 2026-10-15 在本机重新实测的，不依赖那份缺失文档；
> 落地时建议把代码注释的依据改指本文 §2/§3（属 P3 的顺带清理）。

---

## 3. 实测代价（spike 数据）

机器：14 逻辑核；参数 `-t 8`，官方预编译 `windows-bin-vs2022-mt`，PP-OCRv4 mobile，
`maxSideLen=1024`（默认）/ `doAngle=1`（默认）。

### 3.1 分阶段耗时

| 场景 | 检出框 | det | angle | rec | 全量 detect |
| --- | --- | --- | --- | --- | --- |
| 12px 代码 760×80 | 2 | 68ms | 5ms | 119ms | **193ms** |
| 中英混排 720×70 | 2 | 66ms | 7ms | 187ms | **260ms** |
| 框选区域 760×420（真实桌面） | 52 | 104ms | 103ms | 1149ms | **1387ms** |
| 全屏 1920×1080 | 168 | 240ms | 336ms | 4025ms | **5121ms** |

### 3.2 内存（进程私有工作集，单位 MB）

| 场景 | 平台值 p05 | 单次峰值 max | 循环反复识别 |
| --- | --- | --- | --- |
| 12px 代码 | 17.5 | 95.4 | — |
| 框选 760×420 | 18.5 | 200.7 | max 204.2 |
| 全屏 1920×1080 | 42.2 | 232.5 | **max 400.5（p50 358.8）** |

### 3.3 由数据得出的三条规律

1. **耗时由检出框数主导，单框 rec ≈ 16~24ms，与像素数关系弱**。
   → 有效杠杆是"限框数/分区"，**不是**降分辨率：`-s 640` 把全屏压到 2115ms，但非空行 157→102（丢 35%）；
   `-a 0 -s 960` 得 3796ms / 145 行（几乎无质量代价）。
2. **`doAngle` 对截图是纯浪费**：全屏白耗 336ms、框选 103ms（截图不会倒置）。
3. **内存分两层**：模型加载后的平台只 ~18–42MB，但**单次识别瞬时抬升 +95 / +200 / +233MB**；
   反复识别全屏时涨到 400MB **不回落**（arena/缓冲不归还）。

> 口径声明：以上为**独立进程、单次采样**。判数量级够用，判 ±5MB 不够；
> 且未含"嵌入 `nextboard.exe` 后与面板/遮罩叠加"的真实形态 —— 转正前必须按 06 §11.2 协议
> （≥6 轮 + 当场对照组）复测。

---

## 4. 决策记录

| # | 决策 | 状态 |
| --- | --- | --- |
| D1 | 走 **C++ 宿主 + RapidOcrOnnx（RapidAI 官方 C++ 实现）**，不进 `nbcore` | ✅ 已拍板（用户："UI 是 Qt/C++，不走 rust core"；且上游无官方 Rust 绑定） |
| D2 | **移除 Tesseract** 后端 | ✅ 已实施并提交 `c4d1aeb` |
| D3 | 模型档 = **PP-OCRv4 mobile 三件套**（det+cls+rec+keys，实测 15.47MB） | ✅ 已拍板 |
| D4 | 包体积目标由 `<60MB` 放宽到 **`<90MB`**（03 §6.4） | ✅ 已拍板（**待改文档**，见 §9.4） |
| D5 | 本轮**不做** `ocr_text` 回填 | ✅ 已拍板 |
| D6 | 引擎选择策略：`system` / `rapidocr` / `auto`（兜底链） | ⚠️ **待拍板**（§7.2 给出建议与理由） |
| D7 | `nbocr` session 生命周期默认档 | ⚠️ **待拍板**（§10.2，需 P4 实测支撑） |
| D8 | RapidOCR 输出中英文间空格被吃掉（`Q3 revenue`→`Q3revenue`），是否在输出侧补回 | ⚠️ 待拍板（影响搜索与可读性，不影响本轮可行性） |

---

## 5. 架构与数据流

### 5.1 单一接缝：四条入口全部汇于 `processCapture`

| 入口 | 位置 | 模式 | 结果去向 |
| --- | --- | --- | --- |
| 图片条目右键「识别文字」 | `Main.qml:2151` → `Core.ocrEntry` → `core_facade.cpp:243` | `ocr` | 写系统剪贴板（`core_facade.cpp:418-429`） |
| 翻译窗「截图取字」 | `TranslateView.qml:522` → `Core.captureOcr` → `core_facade.cpp:434-443` | `ocr` | 同上 |
| 热键 截图翻译 | `hotkeys.cpp:99-107` | `translate` | 翻译页预填（`core_facade.cpp:397-401`） |
| 热键 截图取字入列表 | `hotkeys.cpp:115-122` | `entry` | `save_text_entry` 存为**新文本条目** `source=ocr`（`:403-416`） |

```
QML ──► CoreFacade::processCapture(id, mode)        ← 唯一分派点(core_facade.cpp:380)
                 │
                 ├─ 读 ocr_backend(整份设置来自 get_settings,键为 camelCase)
                 │
        ┌────────┴─────────┐
   "system"              "rapidocr"
        │                    │
  AsyncCall → nb_call     QRunnable → OcrEngine::detect(本进程线程池)
  (ocr_entry RPC)             │
        │              blobPath → CoreBridge::loadEntryImage → QImage
        └────────┬─────┘
                 ▼
        统一组装 {ok, data:{text}} → emit ocrFinished + 按 mode 走原有分支
              （QML、toast、预填、入库逻辑**零改动**）
```

### 5.2 宿主侧已具备的能力（因此**不需要新增 Rust RPC**）

| 能力 | 出处 |
| --- | --- |
| `blobPath` 随 `list_entries` 一起下发（注释明确"曾在每行发同步 `get_entry` 是性能陷阱"） | `clip_entry_model.cpp:561`、`:44`、`:475-482` |
| 相对→绝对 blob 路径（含自定义数据目录生效逻辑） | `core_bridge.cpp:167-170 blobsDirAbs()` |
| 直接取 `QImage`（贴图功能已在用） | `core_bridge.cpp:267-278 loadEntryImage()`，声明 `:85` |
| 线程池异步 + 投回 UI 线程的既有载体 | `core_facade.cpp:210-224 struct AsyncCall`（内含串行 `nb_call` 的静态互斥） |

> `nb_call` 由一把静态互斥串行化（`core_facade.cpp:216-219`）。本地 OCR **不应**占这把锁：
> 本轮 OCR 不写库，与核心无共享状态；若 D5 回填日后落地，必须改为"识别在池内、写库回投串行 RPC"。

### 5.3 为什么必须是独立 DLL

| 依据 | 出处 |
| --- | --- |
| 隐式链接 Rust cdylib 时 CRT/TLS 静态初始化顺序与 Qt 冲突，**实测 `nb_init` 内 `0xC0000005`**；改显式加载后正常 | `app/CMakeLists.txt:114-116`（注释原文） |
| 显式加载 + 裸名解析 + ABI 校验的现成写法 | `core_bridge.cpp:41-72`（`LoadLibraryW(L"nbcore.dll")`） |
| 可选依赖降级先例：非致命导出取不到时回退，不阻断启动 | `core_bridge.cpp:60-63` |
| 跨边界内存所有权范式：被调方提供 `free` 指针，宿主只调它 | `core_bridge.cpp:27-32 releaseCoreBuffer` |

OpenCV + ONNX Runtime 这类重型 native 库的风险面与 Rust cdylib 同类，故同构处理：
**独立 `nbocr.dll` + 显式 `LoadLibrary` + C ABI**，不链进 `nextboard.exe`。

---

## 6. `nbocr.dll` 边界契约（C ABI 草案）

```c
// app/src/ocr/nbocr.h —— 宿主只认这几个导出(与 nbcore 同一策略:裸名 + 显式加载)
typedef struct { int padding; int maxSideLen; float boxScoreThresh; float boxThresh;
                 float unClipRatio; int doAngle; int mostAngle; int numThread; } NbOcrParam;
typedef struct { int x, y, w, h; float score; char* utf8; } NbOcrLine;   // utf8 由 nbocr 分配
typedef struct { NbOcrLine* lines; int count; } NbOcrResult;

uint32_t   nbocr_abi_version(void);                       // 期望 1;不等则宿主禁用该引擎
int        nbocr_available(const char* models_dir_utf8);  // 只做文件存在性检查,不建 session
NbOcrHandle nbocr_create (const char* models_dir_utf8, const NbOcrParam* p); // NULL→默认参数
void        nbocr_destroy(NbOcrHandle);                   // 释放 session(内存高水位唯一出口)
NbOcrResult* nbocr_detect(NbOcrHandle, const uint8_t* bgr8, int w, int h, int stride);
void        nbocr_free_result(NbOcrResult*);
```

硬性约定：

1. **内存所有权**：`NbOcrResult` 与其内 `utf8` 一律由 `nbocr` 分配、只由 `nbocr_free_result` 释放；
   宿主在释放前拷入 `QString`。**跨 CRT 释放是 mt 版二进制的必然崩点**，此约定把它挡在边界外。
2. **输入格式**：只认一种 —— BGR8 紧凑/带 stride 行（与 `cv::Mat` 默认一致），由宿主从 `QImage`
   转换（注意 `QImage::bytesPerLine()` 的行对齐，不能假定 `w*3`）。
3. **默认参数**（依 §3.3 实测）：`doAngle=0`、`maxSideLen=960`、`padding=50`、
   `boxScoreThresh=0.5`、`boxThresh=0.3`、`unClipRatio=1.6`、`numThread` 取 `min(硬件线程数, 8)`。
4. **线程安全**：session 非线程安全 → `OcrEngine` 内部一把 `QMutex` 串行；识别在池线程，绝不碰 UI 线程。
5. **缺失即禁用**：`LoadLibrary` 失败、ABI 不符、`nbocr_available()==0` → 引擎标记不可用并写日志，
   应用照常启动（沿用 §5.3 的可选导出降级口径）。

---

## 7. 引擎选择与设置项语义

### 7.1 `ocr_backend` 取值与校验必须同步

| 位置 | 当前 | 目标 |
| --- | --- | --- |
| `settings.rs:108` 文档 | 仅 `system`（P1 已收窄） | `system` / `rapidocr` / `auto` |
| `settings.rs` 校验白名单（`:507-514`） | `"" \| "system" \| "windows"` | 同步加入 `rapidocr` / `auto` |
| `ocr.rs:83 Backend::parse` | 未知值告警回退 `System` | **保持不变**：`rapidocr`/`auto` 对核心侧就是"未知→内置"，宿主另行处理 |
| 加载期归一 `repair_removed_ocr_backend` | 把不支持的值改回 `system` | **必须同步扩展白名单**，否则会把 `rapidocr` 静默改回 `system`（P1 已加的回归测试会立刻红） |

> ⚠️ 这条是 P1 留下的**已知耦合点**：`settings.rs` 校验与 `repair_removed_ocr_backend` 的
> 值集合是同一份口径的两处副本。落地时应抽一个共同常量/函数（如 `ocr_backend::is_supported`），
> 避免下次再加引擎时只改一处。

### 7.2 分派策略（D6，待拍板）

建议 **`auto` 为默认**：

- 小图与框选区域 → 直接 RapidOCR（193ms / 1387ms，质量碾压）；
- 大图全屏 → 先内置引擎；**当内置返回空文本或行数 < 阈值时自动降级 RapidOCR 重试一次**（实测全屏内置引擎只出 1 行垃圾，等于必然触发降级 —— 这条链正是"增强备份"的字面含义）。
- 理由：把 3.8~5.1s + 233MB 的成本留给"内置引擎已失败"的低频路径，而不是默认路径；
  同时不放弃全屏这个内置引擎实测不可用的主场景。
- 备选：`rapidocr` 全量默认（简单，但每次全屏识别都要 3.8s 起）；或纯手选无 `auto`。

### 7.3 预处理语义分叉（必须处理，否则会静默变慢/变错）

| 项 | 归属 | 处置 |
| --- | --- | --- |
| `ocr_scale`（默认 2x，`ocr.rs:193 MAX_OCR_EDGE`、`ocr.rs:206 apply_preprocess`） | **只对 `system` 有意义** | RapidOCR 走自身 `maxSideLen`；对它再 2x 放大是纯浪费（3072×1920 放大后单帧 RGBA 90MB，`ocr.rs:186` 原注释） |
| `normalize_cjk_spaces`（`ocr.rs:329`） | Windows 引擎专用缺陷补丁，**生产路径无调用点** | **不要接给 RapidOCR**：它不产生中文字间空格，折叠反而可能粘连英文词 |
| `LanguageChoice` 自动链 `zh-Hans-CN → en-US` | 仅内置引擎 | RapidOCR v4 mobile 不分语言（中英混排一体），语言项对其无效，设置页需按引擎显隐 |

### 7.4 探测与 UI

- `ffi.rs:780` 现把 probe **硬编码**为 `Backend::System`（P1 有意未动）。目标：`ocr_probe` 接受 `backend` 参数，
  并由宿主聚合两路 → `{engines:[{id, available, languages, reason}]}`，响应体加字段属向后兼容。
- 设置页**目前没有任何 OCR 后端选择器**（`SettingsDrawer.qml` 只有热键 `:353`/`:431`/`:1123`）。
  需新增：后端下拉（用现成 `ReadableComboBox`）+ 每行可用性与不可用原因（缺模型/缺语言包/缺 DLL）。

---

## 8. 依赖与构建

### 8.1 版本钉死（全部为核实过的具体制品）

| 制品 | 钉定版本 | 实测体积 |
| --- | --- | --- |
| RapidOcrOnnx 源码 | 固定到某 commit（当前 `main`，最近推送 2026-10-04，Apache-2.0） | — |
| OpenCV（RapidAI 预编译） | `opencv-4.11.0-windows-vs2022-x64-**md**.7z` | 9.7MB |
| ONNX Runtime（RapidAI 预编译） | `onnxruntime-v1.23.2-windows-vs2022-x64-static-**md**.7z` | 48.2MB |
| det | `PP-OCRv4/ch_PP-OCRv4_det_infer.onnx` | 4,745,517 B |
| rec | `PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx` | 10,857,958 B |
| cls | `PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx` | 585,532 B |
| keys | `ppocr_keys_v1.txt`（PaddleOCR main，Apache-2.0） | 26,250 B / **6,623 行** |

要点：

1. **必须 md，不能用 mt**。项目走 `/MD` + app-local CRT（`tools/package.ps1:31 $CrtDlls` 四个 DLL），
   而官方预编译 bin/clib **只有 mt**（`windows-bin-vs2022-mt.7z`）。本次 spike 用 mt 版只够验证效果，
   **不能**直接嵌进入宿主。
2. RapidOcrOnnx 的 `BUILD.md` 写"从 OnnxRuntime 1.7.0 起只支持 vs2019"——**已过期**：
   RapidAI 的 OnnxruntimeBuilder 实测提供 `vs2022` x64 的 static/shared × md/mt 全套，与项目 VS2022 工具链一致。
3. 模型字典必须与 rec 配对（`ppocr_keys_v1.txt` 6623 行 ↔ `ch_PP-OCRv4_rec_infer`）。
   HF `SWHL/RapidOCR` 仓库**只含 .onnx 不含 txt**，字典须单独取。
4. 四个模型文件入库或入发布包时**记 SHA256 并校验**（07 文档与 `dist/` 已有 SHA256 习惯）。
   不联网下载、不做首次运行拉取 —— 破坏全离线语义。

### 8.2 CMake 侧已知坑

- RapidOcrOnnx 的 `CMakeLists.txt` 用 `file(GLOB src/*.cpp)`，会把 `main.cpp`、`OcrLiteJni.cpp`、
  `getopt.cpp` 一并编入。作 CLIB/自有 DLL 用时**必须显式剔除** `main.cpp` 与 JNI，否则链接冲突。
- 其自带 `clipper.cpp`/`clipper.hpp` 是全局符号，若未来引入其它含 Clipper 的库需改名或加命名空间。
- `thirdparty/`（opencv-static / onnxruntime-static 解压目录）**加入 .gitignore**：ORT 静态包 48MB
  不该入库；项目**没有 CI**（`.github`/`.gitlab-ci*`/`circle*` 均无），所以手工下载与放置步骤
  必须写进 04 §3 的构建前置清单，否则换机不可复现。
- 宿主是 C++20（`app/CMakeLists.txt:10`），RapidOcrOnnx 是 C++11/14 风格；两者混编需实测警告与
  `/utf-8`（`:120`）一致性。

---

## 9. 打包与安装

### 9.1 落盘布局

```
<安装目录>/
  nextboard.exe
  nbcore.dll
  nbocr.dll                 ← 新增(app-local,靠裸名 LoadLibrary 命中)
  models/ocr/
    ch_PP-OCRv4_det_infer.onnx
    ch_PP-OCRv4_rec_infer.onnx
    ch_ppocr_mobile_v2.0_cls_infer.onnx
    ppocr_keys_v1.txt
```

### 9.2 安装器无需改动

`tools/installer.nsi:100` 是递归段 `File /r "${SRC_DIR}\*"`（注释已说明用 `*` 而非 `*.*` 以带上
无扩展名的 `qmldir`）→ 放进 stage 的文件自动随包装。

### 9.3 `tools/package.ps1` 需要三处改动

1. 第 4 步白名单拷贝：新增 `nbocr.dll` 与 `models\ocr\*`（脚本现有口径是逐条显式拷贝，见 `:100-137`）。
2. 第 6.2 步 `$must` 数组：加 `nbocr.dll` 与 `models\ocr\ppocr_keys_v1.txt`，缺件直接 `Die`
   （与现有 `platforms\qwindows.dll` 同级 —— 静默缺件是最难查的发布事故）。
3. 第 6.1 步"禁止混入"扩展名（`.lib/.exp/.pdb/.log`）**不受影响**：`.onnx` 不在列，
   而该规则反而能挡住 OpenCV/ORT 静态库误入 stage。

### 9.4 体积账（03 §6.4 目标改 `<90MB`）

| 项 | 体积 | 口径 |
| --- | --- | --- |
| 当前 stage（0.2.0） | **56MB** | 实测 `du -sm dist/Nextboard-0.2.0` |
| 引擎（含 OpenCV+ORT 静态链接） | 16.4MB | 实测官方预编译 CLI exe 体积；自编译 `nbocr.dll` **未测** |
| 模型四件 | **15.47MB** | 实测文件字节 |
| 合计 | ≈ **88MB** | 外推（依赖自编译 DLL 的真实体积） |

→ 现目标 `<60MB` 数学上不可能守住；D4 已拍板改为 `<90MB`，需同步改 03 §6.4 与
`tools/package.ps1` 第 6 步打印的文案（"路线图 6.4 目标 <60MB"）。

---

## 10. 内存与性能预算

### 10.1 转正门槛（写死，避免事后放宽）

| 判据 | 门槛 | spike 实测 | 判定 |
| --- | --- | --- | --- |
| 质量（小字号/中英混排/全屏） | 明显优于内置引擎 | 全面优于，全屏场景内置引擎不可用 | ✅ 过 |
| 单次识别时延 | 小图 ≤300ms | 193ms | ✅ 过 |
| 单次识别时延 | 全屏 ≤300ms | 3796ms（`-a 0 -s 960`） | ❌ 不过 |
| 常驻增量 | ≤40MB | 平台值 18~42MB | ⚠️ 勉强 |
| 单次瞬时峰值 | ≤40MB | +95 / +200 / +233MB | ❌ 不过 |
| 包体 | ≤90MB | ≈88MB | ✅ 过 |

结论：**质量收益真实且大，成本集中在全屏**。故 D6 的"分场景 + 兜底链"不是优化偏好，而是让
这套数字可接受的必要设计。

### 10.2 session 生命周期（D7）

实测的 400MB 不回落（§3.2）说明必须显式处置。三个候选，P4 用 06 §11.2 协议实测后定档：

| 策略 | 代价 | 预期 |
| --- | --- | --- |
| A 常驻（`create` 一次直到退出） | 无时延代价 | 高水位累积，最坏 |
| B 空闲 N 秒后 `destroy` | N 秒内首次识别多一次加载 | 推荐起点（N=60） |
| C 每次识别后 `destroy` | 每次 +模型加载 | 最省内存；spike 中"总 wall − detect ≈ 382ms"**含进程启动**，纯加载代价应为 250~300ms 量级（外推，未验证） |

### 10.3 与既有内存结论的关系

06 §12 实测遮罩会话 +77.5MB、进程峰值 ~270MB，该阶段已由用户判定收尾。
本引擎会在**识别时刻**抬 +95~233MB —— 这是结构性新增开销，不属于"内存优化"议题的复活，
但**必须在 P4 给出嵌入形态的真实叠加数字**，再决定是否默认启用。

---

## 11. 实施计划（分阶段，每阶段可独立回滚）

### P1 · 核心侧减法与配置兼容 —— ✅ 已完成（`c4d1aeb`）

删 `mod tesseract`（112 行）与 `Backend::Tesseract` 及其 4 处分派/测试；校验收窄为
`system|windows`；新增加载期 `repair_removed_ocr_backend` —— 因为设置抽屉保存回传**整份**
已加载对象（`SettingsDrawer.qml:402 const s = cur.data`），留着旧值会让 `validate()` 拒绝
之后**任何一次**保存。237 测试全绿（含新增 1 例），`nbcore.dll` cdylib 重建正常，
真机 `ocr_probe_backends` 报 `system 可用=true 语言=[en-US, zh-Hans-CN]`。

### P2 · 构建可行性验证（不动主干行为）

1. 按 §8.1 取 md 依赖，`thirdparty/` + gitignore；
2. 编出 `nbocr.dll`（剔 `main.cpp`/JNI），导出 §6 的 6 个符号；
3. 用 `tools/` 下一个最小 C++ 测试宿主（或 `tools/smoke_host` 同款形态）对 §3 的三档样例跑通，
   确认：识别结果与 spike 一致、`/MD` 混编无冲突、`nbocr_destroy` 后私有内存确实回落。

**DoD**：`nbocr.dll` 能在 `nextboard.exe` 同目录被 `LoadLibrary` 成功、跑通三档样例；缺失时不崩。
**回滚**：纯新增目录 + 新脚本，主干未变。

### P3 · 接线（宿主 + 核心 + UI + 打包）

- `app/src/ocr/ocr_engine.{hpp,cpp}`（加载、`QImage→BGR8`、串行、session 生命周期）；
- `processCapture`（`core_facade.cpp:380`）按 `ocr_backend` 分派，输出仍组装成 `{ok,data:{text}}`；
- `ocr_probe` 聚合两路（含修 `ffi.rs:780` 硬编码）；
- `SettingsDrawer.qml` 后端选择器 + 可用性与原因；`settings.rs` 校验与 §7.1 耦合点收口；
- `package.ps1` 三处（§9.3）；03 §6.4 与脚本文案改 `<90MB`；04 §3 补依赖放置步骤。

**DoD**：§12 全表通过。

### P4 · 策略与预算实测

D6 阈值调参（框数上限/分区/长图切分）、D7 生命周期定档（≥6 轮 + 当场对照组）、
D8 空格补回与否、决定默认引擎档位。

---

## 12. 验收标准（DoD）与测试矩阵

| 层 | 用例 | 判据 |
| --- | --- | --- |
| 单测（Rust） | `backend_and_language_parsing`、`validate_checks_ocr_options`、`load_repairs_removed_ocr_backend` | 全绿；值集合三处（parse / validate / repair）**必须同源** |
| 真机诊断 | `cargo test -- --ignored --nocapture ocr_probe_backends` | 两引擎各自 available 与语言如实上报 |
| 质量回归 | `--ignored ocr_quality_bench`（同一目录双引擎并排） | 三档样例结果不退 |
| 端到端 | `--acceptance`/手工：识别文字→剪贴板；截图翻译预填；取字入列表 | 四条入口（§5.1）行为与模式语义均不变 |
| 失败路径 | 删 `nbocr.dll` / 删 `models/ocr/*` 后启动 | 应用**照常启动**，设置页给出可懂原因，`system` 引擎仍可用 |
| 内存 | 06 §11.2 协议：≥6 轮、±5MB 噪声、当场对照组 | 给出嵌入形态的识别瞬时增量与稳态残留；验证 `destroy` 回落 |
| 体积 | `package.ps1` 第 6 步打印 | stage ≤90MB；`$must` 含 `nbocr.dll` 与模型 |
| 安装 | 装完启动并识别一次 | 递归段自动带上的文件确实到位（04 §6/§10） |

---

## 13. 风险与待办

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 首次引入 C++ 第三方二进制依赖，且**无 CI** | 换机不可构建 | §8.1 版本钉死 + SHA256 + 04 §3 前置清单；`thirdparty` 不入库 |
| 官方预编译只有 **mt**，与项目 `/MD` 不符 | 跨边界释放崩溃 / CRT 混用 | §6 所有权约定 + §8.1 用 md 依赖自编；spike 数据只当效果验证 |
| 全屏识别时延 3.8s 起 | 用户感知卡顿 | `auto` 兜底链（§7.2）把成本留给低频路径；识别本就在池线程，UI 不冻 |
| 单次 +95~233MB、循环 400MB 不回落 | 叠在 ~270MB 遮罩峰值上翻新峰值 | session 生命周期（§10.2）+ P4 实测后才定默认档 |
| `ocr_scale`/语言项语义分叉 | 对 RapidOCR 白耗内存、或设置项失效但 UI 仍显示 | §7.3 按引擎显隐；不接 `normalize_cjk_spaces` |
| `settings.rs` 三处值集合副本 | 新增引擎时漏改 → 静默把 `rapidocr` 归一回 `system` | §7.1 收口为单一来源（P3 必做） |
| RapidOcrOnnx 的 `file(GLOB)` + 自带 clipper/getopt 全局符号 | 构建失败或符号冲突 | §8.2 剔除与命名空间隔离 |
| `windeployqt` 不会处理 `nbocr.dll` 与模型 | 发布包缺件 | §9.3 手工白名单 + `$must` 断言 |
| 第三方模型/引擎许可 | 合规 | RapidOcrOnnx Apache-2.0、PaddleOCR 模型 Apache-2.0；随包附 LICENSE/NOTICE（04 §5） |

---

## 14. 未验证清单（不得当作结论引用）

1. 自编译 **md 版** `nbocr.dll` 的真实体积（16.4MB 是官方 mt CLI 口径）。
2. 嵌入 `nextboard.exe` 后 `detect` 的**净**内存增量与高水位行为（现数据为独立进程单次采样）。
3. `nbocr_destroy` 是否真能把私有内存交还 OS（arena 可能仍留在进程内）。
4. 每轮识别之间的时延噪声（spike 见同参重跑 193→225ms，未做 ≥6 轮，未定量）。
5. v6-tiny / v6-tiny-int8（5.48MB / 2.80MB）与 v5-mobile（24.69MB）档的质量-耗时曲线。
6. 中文长段落的检出稳定性（spike 中英混排只有一行 48 字）。
7. PP-OCR 吃空格（`Q3revenue`）对 FTS/LIKE 搜索的实际影响（回填落地时才有意义）。
8. 多屏/DPR>1 下 `QImage→BGR8` 与 `maxSideLen` 的综合效果（spike 用 1920×1080 单屏）。

---

## 附录 A：spike 复现步骤（产物在 `dist/ocr-spike/`，已被 gitignore）

```bash
# 1) 官方预编译 CLI(仅用于验证效果;不可直接嵌入)
curl -L -o bin.7z https://github.com/RapidAI/RapidOcrOnnx/releases/download/1.2.3/windows-bin-vs2022-mt.7z
"/c/Program Files/7-Zip/7z.exe" x -y -orapidocr bin.7z

# 2) 模型(HF 只有 .onnx;字典单独取)
curl -sL -o models/ch_PP-OCRv4_det_infer.onnx \
  https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx
curl -sL -o models/ch_PP-OCRv4_rec_infer.onnx \
  https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx
curl -sL -o models/ch_ppocr_mobile_v2.0_cls_infer.onnx \
  https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx
curl -sL -o models/ppocr_keys_v1.txt \
  https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/ppocr_keys_v1.txt

# 3) 识别(注意输出是 UTF-8;PowerShell 5.1 读 .ps1 需 UTF-8 BOM,控制台 GBK 会显示乱码)
RapidOcrOnnx.exe -d models -1 ch_PP-OCRv4_det_infer.onnx -2 ch_ppocr_mobile_v2.0_cls_infer.onnx \
  -3 ch_PP-OCRv4_rec_infer.onnx -4 ppocr_keys_v1.txt -i samples/screen.png -t 8 -a 0 -s 960
```

对照基线（现成测试，零代码改动）：

```bash
cd core && OCR_BENCH_DIR="<样例图目录>" cargo test --release -p nbcore --lib -- \
  --ignored --nocapture ocr_quality_bench
```

> ⚠️ `dist/ocr-spike/samples/screen.png`、`crop760.png` 与 `out_screen*.txt` 含当次桌面的可见文字。
> 需要干净的复现请重跑 `make_samples.ps1` 自行截图，不要共享这些文件。

## 附录 B：上游仓库事实（2026-10-15 核实）

| 仓库 | 说明 | 关键事实 |
| --- | --- | --- |
| `RapidAI/RapidOCR` | Python 主仓，8147★，Apache-2.0 | README 只列 Python/C++/Java/C#/Android；**无 Rust 绑定**；releases 不含模型 |
| `RapidAI/RapidOcrOnnx` | 官方 C++（DbNet/AngleNet/CrnnNet + clipper），376★，Apache-2.0 | 1.2.3 预编译仅 mt；`OCR_OUTPUT` 支持 BIN/JNI/**CLIB**；有 `OcrLiteCApi.h` |
| `RapidAI/OpenCVBuilder` | OpenCV 静态库（含 vs2022 x64 md/mt） | 4.11.0 md x64 = 9.7MB |
| `RapidAI/OnnxruntimeBuilder` | ORT 静态/共享（vs2022 x64 md/mt） | 1.23.2 static-md x64 = 48.2MB；shared-md = 3.2MB |
| `SWHL/RapidOCR`（HF） | PP-OCRv1~v4 的 .onnx 仓库 | **不含字典 txt**；无 v5/v6 |
| Rust 侧候选（`rusto-rs`/`rapidocr-core`/`paddle-ocr-rs`） | 第三方移植，非官方 | 两个依赖 `ort` **rc 版**；本方案不采用（D1） |
