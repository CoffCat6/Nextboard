# 08-补充 · RapidOCR 接入本项目的开发路线与实施计划

| 项 | 内容 |
| --- | --- |
| 定位 | 对 [08-rapidocr-engine.md](./08-rapidocr-engine.md) 的**落地化拆解**:把该文档的 P2–P4 计划变成可执行、可验收、可回滚的工程任务 |
| 日期 | 2026-10-15 |
| 基线 | HEAD = `c4d1aeb`(P1 已提交);工作区唯一未跟踪文件 = `docs/08-rapidocr-engine.md` |
| 性质 | **分析 + 计划,不含任何代码改动** |
| 结论摘要 | 08 文档的**技术判断成立**(已逐条复核);但**工程量被低估**——真正的阻塞点不在 DLL,而在**构建接入、设置三处白名单、以及"引擎不可用"时的降级语义**。建议按本文的阶段 0→6 推进 |
| 补充 | **§9 是根据 RapidOCR 官方文档的第二轮分析**(2026-10-15 追加):官方已演进到 **v3.10.0**,引入 PP-OCRv5/v6、多推理引擎与"从内存解码图像"等能力。**§9 修正了 §1 中三条基于旧信息的判断** |

> ⚠️ **先读 §9**:第二轮查阅官方文档后,有 **3 条结论需要修正**(模型档位选择、C++ 侧的真实形态、以及一个此前完全未识别的"官方能力已就位"机会)。§1–§8 的结论仍然有效,但凡与 §9 冲突处,**以 §9 为准**。

---

## 1. 复核结论:08 文档哪些成立、哪些需要修正

我在落地前对 08 文档的每条关键论断做了**独立复核**(读源码 + 读 spike 原始产物 + 外部查证)。结论如下。

### 1.1 已复核**成立**的论断(可直接作为计划依据)

| # | 08 文档论断 | 复核方式 | 结论 |
| --- | --- | --- | --- |
| 1 | P1 已完成并提交 `c4d1aeb`,移除 Tesseract | `git log` + `git show --stat c4d1aeb` | ✅ 属实(`ocr.rs` -152 行、`settings.rs` +77 行) |
| 2 | 四条 OCR 入口全汇于 `processCapture` | 读 `core_facade.cpp:242-244 / 380-432 / 434-443`、`hotkeys.cpp:99-124` | ✅ 属实(见 §2.1) |
| 3 | `blobPath` 随 `list_entries` 下发,无需新增 RPC | `clip_entry_model.cpp:561,44,60,475-489` | ✅ 属实 |
| 4 | `CoreBridge::loadEntryImage` / `blobsDirAbs` 现成可用 | `core_bridge.cpp:167-170, 267-278`;`hpp:85,88` | ✅ 属实 |
| 5 | `settings.rs` 存在"值集合两处副本"耦合点 | `settings.rs:508-515`(validate) vs `:898-919`(repair) | ✅ 属实,**且比文档描述的更严重**(见 §1.2.3) |
| 6 | `settings.rs` 白名单当前只有 `""\|system\|windows` | `settings.rs:508-515` | ✅ 属实 |
| 7 | `ocr.rs:83 Backend::parse` 未知值回退 System | `ocr.rs:81-89` | ✅ 属实 |
| 8 | `ffi.rs:780` 把 probe 硬编码为 `Backend::System` | `ffi.rs:779-782` | ✅ 属实 |
| 9 | `normalize_cjk_spaces` 生产路径无调用点 | `ocr.rs:329` 定义;全仓仅测试引用 | ✅ 属实 |
| 10 | 设置页**无** OCR 后端选择器 | `SettingsDrawer.qml` 中 OCR 仅 3 处且全是热键 | ✅ 属实 |
| 11 | `package.ps1` 需三处改动;安装器递归段自动带上新文件 | `package.ps1:98-122, 147-151, 153-157, 160`;`installer.nsi:100`(全文件唯一 File 指令) | ✅ 属实,且安装器确实**零改动** |
| 12 | spike §2/§3 数字 | 读原始产物 `compare2.txt`/`crop_summary.txt` 反推 | ✅ **数字与原始产物完全一致**(见 §1.3) |
| 13 | `ocr_scale`/`LanguageChoice` 对 RapidOCR 语义分叉 | `ocr.rs:112-122`;`settings.rs:516-529` | ✅ 属实 |

### 1.2 需要**修正或补充**的论断(重要)

#### 1.2.1 ⚠️ `nbocr.dll` 的 C ABI 不是"照抄上游",而是**要自己写**

08 文档 §6 给出的 `nbocr_create/nbocr_detect/nbocr_free_result` 契约,**在 spike 制品里并不存在**。实测 `dist/ocr-spike/` 下**只有 exe,没有任何 dll/lib/头文件**:

```
dist/ocr-spike/rapidocr/windows-bin-vs2022-mt/win-BIN-CPU-x64/{RapidOcrOnnx.exe, benchmark.exe}
dist/ocr-spike/rapidocr/windows-bin-vs2022-mt/win-BIN-CPU-Win32/{RapidOcrOnnx.exe, benchmark.exe}
(无 .dll / .lib / .h)
```

上游确有 `OcrLiteCApi.h` 与 `OCR_OUTPUT=CLIB` 构建开关(08 附录 B 记载,本环境**无法联网复核**),但:
- 上游 C API 的签名/生命周期/内存所有权**与本项目 §6 草案并不相同**;
- 若采用上游 CLIB,则要额外适配"谁分配 utf8、谁释放、异常如何跨边界"。

**计划含义**:§6 的 6 个导出应视为**本项目自研的薄封装层**(在 RapidOcrOnnx 的 C++ 类之上再包一层),而不是"直接编译上游 CLIB"。这**增加了 P2 的工作量**,也**提升了可控性**(所有权约定可按本项目的 `releaseCoreBuffer` 范式设计)。→ 已拆为**阶段 3 的独立任务**,并明确 DoD。

#### 1.2.2 ⚠️ 构建接入点**不止** `package.ps1` 三处

08 文档把构建改动全部归入 §9.3(`package.ps1` 三处)。复核发现:

| 缺失项 | 事实 | 影响 |
| --- | --- | --- |
| `build.ps1` 无第三方 native 构建钩子 | 全文件仅构建 core(`:42-56`)与 app(`:58-80`),**不引用 `package.ps1`、无 OpenCV/ORT/RapidOcrOnnx 任何痕迹**;`:74` 只注入 3 个 CMake 变量 | `nbocr.dll` 无构建入口 → 换机不可复现 |
| `app/CMakeLists.txt` 无 `nbocr` 目标 | 全文无 `nbocr`;`:137-140` 的 POST_BUILD 只拷 `nbcore.dll` | 开发期 `app/build` 下不会有 `nbocr.dll` 与 `models/` |
| `.gitignore` 无 `thirdparty/` | `.gitignore:8` 是 `dist`(覆盖了 `dist/ocr-spike/` ✅),但**无 `thirdparty` 行**;仓库根 `thirdparty` 目录当前不存在 | 48MB ORT 静态包有误入库风险 |

**计划含义**:P2 必须新增 **(a) `nbocr` 的独立 CMake 子工程/目标**、**(b) `build.ps1` 的调用钩子**、**(c) `.gitignore:thirdparty/`**。这三条 08 文档均未列入 §11 的 P2 步骤。

#### 1.2.3 ⚠️ `ocr_backend` 的耦合点不止"两处副本",而是**三处 + 一条净归零**

08 §7.1 说 `settings.rs` 的"校验"与"归一分"是同一口径的两处副本。复核确认有**第三个**消费点,且组合行为更危险:

| # | 位置 | 当前值集合 | 若只改两处的后果 |
| --- | --- | --- | --- |
| 1 | `settings.rs:508-515` `validate()` | `"" \| system \| windows` | 新增值 `save_settings` **直接报错**,UI 存不进去 |
| 2 | `settings.rs:898-919` `repair_removed_ocr_backend()` | `"" \| system \| windows` | 不识别的新值**被静默改回 `system`** 并写 `settings_warnings` |
| 3 | `ocr.rs:81-89` `Backend::parse()` | 未知值 → 告警 + `System` | **`rapidocr`/`auto` 对 core 侧一律落为内置引擎** |

第 3 条是 08 文档**有意保留**的设计("对核心侧就是未知→内置,宿主另行处理")——这个设计**是对的**,但它意味着:

> **`ocr_backend=rapidocr` 时,core 侧的 `ocr_entry` 仍然会跑内置引擎。**
> 因此宿主**必须**在派发前自己拦截,绝不能让 `rapidocr` 值走到 `ffi.rs:783` 的 `ocr_entry`。

**计划含义**:除 §7.1 要求的"抽单一来源"外,必须在 **P3 增加一条显式断言/测试**:设置 `rapidocr` 时,宿主**不**调用 `ocr_entry`。否则会出现"UI 选 RapidOCR、实际跑内置引擎"的**静默错误**(且质量回归还查不出来,因为两条路都返回文本)。

#### 1.2.4 ⚠️ 体积目标实际要改 **5 处**,不是 2 处

08 §9.4 说改 `03 §6.4` 与 `package.ps1` 文案。复核发现 `<60MB` 共出现在:

| # | 位置 | 原文 |
| --- | --- | --- |
| 1 | `docs/03-roadmap.md:80` | `包体目标 <60MB`(§6.4 行) |
| 2 | `docs/04-build-package-release.md:206` | §5.2 流程图第 6 步 |
| 3 | `docs/04-build-package-release.md:377` | §9 DoD 第 5 条「≤ 60MB(路线图 6.4)」 |
| 4 | `tools/package.ps1:160` | 打印文案「路线图 6.4 目标 <60MB」 |
| 5 | `tools/package.ps1:161` | `if ($sizeMb -ge 60)` **阈值常量** |

**第 5 处是会真报警的代码**,漏改会导致每次打包都刷 WARN。→ 已并入任务 T4.5。

#### 1.2.5 ⚠️ "06 §11.2 协议"引用**落空**

08 文档有 4 处引用"按 06 §11.2 协议(≥6 轮 + 当场对照组)复测"(`08:90-91, 337, 399, 426`)。但复核 `06-memory-optimization.md`:
- §11.2 标题在 `:1030`,是**实测数据小节**,并未写成"协议条款";
- "≥6 轮 + 配对对照"的**真实出处是 §13 `:1175-1191`**(6 轮配对对照表);
- 探针判读警告在 §0 `:50-55`。

**计划含义**:P4 的内存复测引用应改为「06 §0 方法 + §13 配对对照范式」,或先在 06 补写一节显式协议。否则验收标准挂在一个不存在的条款上。

### 1.3 对 spike 数据的独立复核(§2/§3)

我**没有**采信文档转述,而是直接读 spike 原始产物反推:

| 文档声称 | 原始产物 | 复核 |
| --- | --- | --- |
| 12px 代码 193ms,框=2 | `compare2.txt`: `code12 框=2 det=68.3 angle=5.2 rec=119.0 全量=193.2ms` | ✅ 精确一致 |
| 中英混排 260ms,输出 `Q3revenue` | `compare2.txt`: `mixed14 ... 全量=259.7ms` + `会议纪要：Q3revenue增长12.5%...` | ✅ 精确一致(含 D8 空格丢失) |
| 全屏 5121ms / 168 框 / 157 非空行 | `compare2.txt`: `全量=5120.6ms` + `非空行数=157 / 总框数=168` | ✅ 精确一致 |
| 框选 760×420 = 1387ms / 52 框 | `crop_summary.txt`: `框=52 ... 全量=1386.8ms` | ✅ 精确一致 |
| 模型四件 15.47MB | `Get-ChildItem` 实测 4745517+10857958+585532+26250 = 15,465,257 B ≈ 15.47MB | ✅ 精确一致 |

**结论**:08 文档 §2/§3 的数字是**可信实测**,可作为决策依据。这一条很重要——它意味着**质量收益是真实的**,值得投入。

### 1.4 未能复核的项(诚实声明)

| 项 | 原因 | 处置 |
| --- | --- | --- |
| 上游 `OcrLiteCApi.h` 的确切签名 | 本环境**无法联网**(github.com / raw.githubusercontent.com 均解析到非公网 IP) | → 阶段 3 首个任务:人工取回并核对 |
| ~~`windows-bin-vs2022-mt.7z` 的 md 版是否存在~~ | ✅ **第二轮已解决**(见 §9.2):官方 `BUILD.md` 明确 "windows 平台分为 mt 和 md 版,mt 代表静态链接 CRT,md 代表动态链接 CRT" | **已确认 md 制品存在**,不再是风险 |
| `/MD` 混编是否真无冲突 | 需实际编译 | → 阶段 2 DoD |
| 剥离 `main.cpp`/`JNI` 后能否编出 CLIB | 需实际编译 | → 阶段 2 DoD |

---

## 2. 目标架构:接入点分析

### 2.1 现有 OCR 数据流(已复核)

```
[入口 1] Main.qml:2151     右键「识别文字」 → Core.ocrEntry(id)      ─┐
[入口 2] TranslateView.qml:522 「截图取字」  → Core.captureOcr()      ─┤ mode="ocr"
[入口 3] hotkeys.cpp:99-107 CaptureTranslate → processCapture(t)      ─┤ mode="translate"
[入口 4] hotkeys.cpp:115-124 CaptureOcr     → processCapture(e)      ─┘ mode="entry"
                                  │
                                  ▼
              CoreFacade::processCapture(id, mode)   core_facade.cpp:380  ← 【唯一接缝】
                                  │
                                  ▼
                      AsyncCall{method:"ocr_entry"}  core_facade.cpp:210-224
                                  │  (静态 QMutex 串行化 nb_call)
                                  ▼
                        CoreBridge::call → nb_call("ocr_entry")
                                  │
                                  ▼
                        ffi.rs:783  ocr_entry
                                  │
                    ocr.rs:112 OcrOptions::from_settings
                                  │
                        Backend::parse  →  一律 Backend::System
                                  │
                        recognize_configured → recognize_with(Windows OCR)
                                  │
                                  ▼
                        {ok, data:{text}}  →  emit ocrFinished
                                  │
                       ┌──────────┴──────────┬─────────────────┐
                  mode=ocr            mode=translate      mode=entry
                写剪贴板             translatePrefill    save_text_entry
             (cpp:420-429)          (cpp:397-401)      (source=ocr, cpp:403-416)
```

**关键观察**:
1. `processCapture` 是**唯一分派点** —— 改这一处即可覆盖全部四条入口 ✅(08 §5.1 判断正确)。
2. `AsyncCall` 内的**静态 QMutex 串行化 `nb_call`**(`core_facade.cpp:216-219`)——本地 OCR **绝不能占这把锁**(08 §5.2 判断正确)。
3. `ocrFinished`(`core_facade.hpp:189`)**无任何连接方** —— 全仓仅 3 处引用(声明/注释/emit)。这意味着**想展示"当前引擎"或"降级发生"必须新建接线**。

### 2.2 目标数据流(建议形态)

```
CoreFacade::processCapture(id, mode)
        │
        ├─ 读 ocrBackend (来自 get_settings，camelCase 键)
        │
   ┌────┴──────────────────────────────┐
   │ "system"                          │ "rapidocr" / "auto"
   ▼                                   ▼
 AsyncCall → nb_call              【宿主侧】OcrEngine
 (既有路径，零改动)                     │
   │                          QRunnable (线程池，不占 core 锁)
   │                                   │
   │                          blobPath → CoreBridge::loadEntryImage
   │                                   │  → QImage
   │                          QImage → BGR8 (注意 bytesPerLine 对齐!)
   │                                   │
   │                          OcrEngine::detect (内部 QMutex 串行)
   │                                   │  → nbocr.dll (显式 LoadLibrary)
   │                          ┌────────┴────────┐
   │                          │ auto 且结果为空 │
   │                          │ 或行数 < 阈值   │
   │                          ▼                 │
   │                    回退 nb_call           │
   │                    (内置引擎重试一次)      │
   └────────────┬─────────────────────────────┘
                ▼
        统一组装 {ok, data:{text}} → emit ocrFinished + 按 mode 走原分支
        (QML / toast / 预填 / 入库 逻辑零改动)
```

### 2.3 三条必须显式处理的语义分叉(08 §7.3 已识别,此处给出处置决策)

| 项 | 问题 | **建议处置** | 理由 |
| --- | --- | --- | --- |
| `ocrScale`(默认 2) | RapidOCR 走自身 `maxSideLen`,再 ×2 是纯浪费 | **宿主侧跳过预处理**:`rapidocr` 分支不读 `ocrScale` | spike 已证 `-s 960` 足够;×2 后 3072×1920 单帧 90MB |
| `normalizeCjkSpaces` | 是 Windows 引擎缺陷补丁,生产路径无调用点 | **不接线**给 RapidOCR | RapidOCR 不产生中文字间空格,折叠可能粘连英文词 |
| `LanguageChoice`(`zh-Hans-CN→en-US`) | RapidOCR v4 mobile 不分语言 | 设置页**按引擎显隐**该行 | 避免"设置项失效但 UI 仍显示" |
| **D8 空格补回** | RapidOCR 吃空格(`Q3 revenue`→`Q3revenue`) | **本轮不做**,仅记录 | 见 §5.3:真正痛点要等 `ocr_text` 回填才有搜索意义 |

---

## 3. 风险重估(按影响排序)

08 §13 已列 9 项风险。复核后我认为**优先级应重排**,把"工程可复现性"置于"性能"之前:

| 优先级 | 风险 | 08 原评级 | **复核后评级** | 说明 |
| --- | --- | --- | --- | --- |
| **P0** | 无 CI + 首次引入第三方 native 依赖 → **换机不可构建** | 已列 | **最高** | `build.ps1` 无钩子、`thirdparty` 未 gitignore、md 制品需人工下载。这是**唯一会让整个计划失败**的风险 |
| **P0** | `settings.rs` 三处值集合不同源 → **静默归零** | 已列 | **最高** | §1.2.3:会导致"选了 RapidOCR 实际跑内置引擎",且**测试查不出** |
| **P1** | C ABI 需自研(非照抄上游) | **未列** | 高 | §1.2.1:P2 工作量被低估 |
| **P1** | 全屏 3.8s + 瞬时 +95~233MB | 已列 | 高 | 真实,但**可由 `auto` 兜底链缓解** |
| **P2** | `ocr_scale`/语言项语义分叉 | 已列 | 中 | 有明确处置方案(§2.3) |
| **P2** | 体积目标 5 处待改 | **低估** | 中 | §1.2.4 |
| **P3** | 06 §11.2 引用落空 | **未列** | 低 | §1.2.5,改引用即可 |

---

## 4. 实施路线:7 个阶段

我把 08 文档的 P2/P3/P4 细化为 **阶段 0–6**。相比原文,主要变化:
- **新增阶段 0**(决策前置)与**阶段 1**(依赖落地);
- 把"构建接入"从 `package.ps1` 单点扩展为**完整构建链**;
- P2 拆为 **2(编译验证)** 与 **3(C ABI 封装)**;
- **新增阶段 6**(文档收口)独立成阶段。

### 阶段 0 · 决策前置(解锁后续所有阶段的 3 个待拍板项)

> 08 §4 的 **D6/D7/D8 三个 ⚠️ 待拍板项**必须在此阶段闭环,否则阶段 4/5 无法编码。

| 任务 | 内容 | 建议决策 | 阻塞 |
| --- | --- | --- | --- |
| T0.1 | **D6 引擎选择策略** | ✅ **采纳 `auto` 为默认**:小图/框选 → 直接 RapidOCR;大图 → 先内置,**结果为空或行数 < 阈值时降级 RapidOCR 重试一次** | 阶段 4 |
| T0.2 | **D7 session 生命周期** | 暂定 **B(空闲 60s destroy)**,由阶段 5 实测定档 | 阶段 4/5 |
| T0.3 | **D8 空格补回** | ✅ **本轮不做**,记录为已知偏差 | 无 |
| T0.4 | **联网人工取回上游事实**(本环境无法联网) | 取 `OcrLiteCApi.h` + 确认 md 制品存在性 | 阶段 2/3 |

**DoD**:D6/D7/D8 三个决策落成文字(更新 08 §4 表格状态);`OcrLiteCApi.h` 已取回并归档。

### 阶段 1 · 依赖与工程环境(纯新增,主干零改动)

| 任务 | 内容 | 关键约束 |
| --- | --- | --- |
| T1.1 | 取 **md 版**依赖:`opencv-4.11.0-windows-vs2022-x64-md.7z`(9.7MB)、`onnxruntime-v1.23.2-windows-vs2022-x64-static-md.7z`(48.2MB) | ⚠️ **必须 md**;官方预编译 bin 只有 mt,**不可直接嵌入** |
| T1.2 | RapidOcrOnnx 源码钉到**具体 commit**(非 `main`) | 可复现 |
| T1.3 | 解压到 `thirdparty/`;**新增 `.gitignore` 的 `thirdparty/` 行** | 48MB ORT 不入库 |
| T1.4 | 四件模型入 `models/ocr/`:**记 SHA256 并校验** | 全离线,不首次运行拉取 |
| T1.5 | 写 `tools/fetch-deps.ps1`(可选但建议):把下载/解压/校验固化成脚本 | 弥补"无 CI" |

**DoD**:新机器按 `tools/fetch-deps.ps1` + 文档步骤能复现出完整依赖树;SHA256 校验通过。
**回滚**:纯新增目录 + 脚本,主干未变。

### 阶段 2 · `nbocr` 可编译性验证(不动主干行为)

| 任务 | 内容 | 已知坑 |
| --- | --- | --- |
| T2.1 | 建 `app/src/ocr/CMakeLists.txt`(或独立 `native/nbocr/`),产出 `nbocr.dll` | — |
| T2.2 | **显式剔除** `main.cpp` 与 `OcrLiteJni.cpp`/`getopt.cpp` | ⚠️ 上游用 `file(GLOB src/*.cpp)`,不剔必然链接冲突 |
| T2.3 | 命名空间隔离 `clipper.cpp/hpp` 的全局符号 | 未来引入其它 Clipper 库会撞 |
| T2.4 | 统一 `/MD`;核对宿主 C++20 与上游 C++11/14 混编告警、`/utf-8` 一致 | `app/CMakeLists.txt:10, :120` |
| T2.5 | 在 `build.ps1` **新增 `nbocr` 构建步骤**;在 `app/CMakeLists.txt` 加 POST_BUILD 拷贝(对齐 `:137-140` 的 `nbcore.dll` 范式) | §1.2.2 |
| T2.6 | 用 `tools/smoke_host/` 同款最小宿主跑三档样例 | 复用现有工装 |

**DoD**:① `nbocr.dll` 能被 `LoadLibraryW` 成功加载;② 三档样例识别结果与 spike **一致**;③ `/MD` 混编无 CRT 冲突;④ 缺失 DLL 时不崩;⑤ `build.ps1` 全流程可复现。
**回滚**:新增目标,主干未变。

### 阶段 3 · C ABI 封装(本项目自研薄层)

> ⚠️ 依 §1.2.1,这不是"编译上游 CLIB",而是**在 RapidOcrOnnx C++ 类之上自研 6 个导出**。

| 任务 | 内容 |
| --- | --- |
| T3.1 | 核对上游 `OcrLiteCApi.h`,决定"复用上游 C API" vs "自研薄封装"(建议后者) |
| T3.2 | 实现 §6 的 6 个导出:`nbocr_abi_version / nbocr_available / nbocr_create / nbocr_destroy / nbocr_detect / nbocr_free_result` |
| T3.3 | **内存所有权**:`NbOcrResult` 与 `utf8` 一律由 `nbocr` 分配、只由 `nbocr_free_result` 释放;**跨 CRT 释放是 mt 版必然崩点** |
| T3.4 | **输入格式**:只认 BGR8(紧凑或带 stride),与 `cv::Mat` 默认一致 |
| T3.5 | **默认参数**(依 §3.3 实测):`doAngle=0`、`maxSideLen=960`、`padding=50`、`boxScoreThresh=0.5`、`boxThresh=0.3`、`unClipRatio=1.6`、`numThread=min(硬件线程,8)` |
| T3.6 | **异常不得跨 C ABI 边界**:全部 `try/catch`,失败返回 NULL/错误码 |
| T3.7 | 写 `app/src/ocr/nbocr.h`(**宿主只认这几个导出**,与 `core_bridge` 同策略:裸名 + 显式加载) |

**DoD**:6 个符号可被 `GetProcAddress` 解析;`nbocr_abi_version()==1`;异常注入测试不跨边界崩溃;连续 100 次 `detect`+`free_result` 无泄漏。
**回滚**:新增目录,主干未变。

### 阶段 4 · 接线(宿主 + 核心 + UI)

> **这是唯一改动主干的阶段**,也是风险最集中处。建议按 T4.1→T4.7 顺序,每步可独立回滚。

| 任务 | 文件 | 内容 | 风险 |
| --- | --- | --- | --- |
| **T4.1** | `settings.rs` | **抽单一来源**:新增 `ocr_backend::is_supported(v) -> bool`,`validate():508-515` 与 `repair_removed_ocr_backend():898-919` **共用**;值集合扩为 `""\|system\|windows\|rapidocr\|auto` | ⚠️ 漏改则 `rapidocr` 被静默归零 |
| **T4.2** | `ocr.rs:81-89` | `Backend::parse` **保持不动**(未知值仍 → `System`) | 08 §7.1 明确要求 |
| **T4.3** | `ffi.rs:779-782` | `ocr_probe` 接受 `backend` 参数(默认 `System`, **向后兼容**) | 旧调用方不传参时行为不变 |
| **T4.4** | **新增** `app/src/ocr/ocr_engine.{hpp,cpp}` | 显式 `LoadLibrary`、ABI 校验、`QImage→BGR8`(**注意 `bytesPerLine()` 行对齐,不能假定 `w*3`**)、内部 `QMutex` 串行、session 生命周期(B/C 档)、缺失即禁用 + 日志 | 线程安全 |
| **T4.5** | `core_facade.cpp:380-432` | `processCapture` 按 `ocr_backend` 分派;**`rapidocr` 分支不再走 `nb_call`**;输出仍组装 `{ok,data:{text}}` | ⚠️ **不占 `nb_call` 静态锁**(`:216-219`) |
| **T4.6** | `SettingsDrawer.qml` | 新增 OCR 后端下拉(用现成 `ReadableComboBox`,`model` **必须纯字符串**,见 `ReadableComboBox.qml:10,49-63`)+ 每行可用性与原因;`save()`(`:395-498`)**显式赋值**新增字段 | ⚠️ `save()` 是整份回读再回传,不显式赋值则不写入 |
| **T4.7** | `package.ps1` + 文档 | 三处改动(`:98-122` 白名单、`:153-157` `$must`、体积文案)+ **§1.2.4 的 5 处 `<60MB`** | 漏改 `:161` 会刷 WARN |

**关键设计决策(必须写进代码注释)**:
```
⚠️ ocr_backend = "rapidocr" 时，core 侧的 ocr_entry 依然会跑内置引擎
   (Backend::parse 把未知值归为 System，这是有意保留的设计)。
   因此宿主必须在 processCapture 内**先于** nb_call 拦截，
   绝不能让 "rapidocr" 值流到 ffi.rs:783。
```

**DoD**:§4.1 的验收矩阵全绿(见下)。
**回滚**:T4.1–T4.3 可独立回滚(core);T4.4–T4.5 可独立回滚(host);T4.6 可独立回滚(UI)。

### 阶段 5 · 策略与预算实测

| 任务 | 内容 | 判据出处 |
| --- | --- | --- |
| T5.1 | **D7 生命周期定档**:A/B/C 三档按 **06 §0 方法 + §13 配对对照范式**(≥6 轮 + 当场对照组)实测 | ⚠️ **不是** "06 §11.2"(见 §1.2.5) |
| T5.2 | **D6 阈值调参**:框数上限 / 分区 / 长图切分 | 08 §3.3 规律 1:耗时由框数主导 |
| T5.3 | 嵌入形态内存实测:`detect` **净**增量与高水位;验证 `nbocr_destroy` 是否真回落 | 08 §14 未验证项 2/3 |
| T5.4 | 验证 `destroy` 后私有内存是否真交还 OS(arena 可能留在进程内) | 08 §14 未验证项 3 |
| T5.5 | 决定默认引擎档位(回填 08 §4 的 D6/D7 状态) | — |

**DoD**:给出**嵌入形态**的真实叠加数字;D6/D7 定档;08 §14 未验证项 2/3/4 闭环。

### 阶段 6 · 文档与合规收口

| 任务 | 内容 |
| --- | --- |
| T6.1 | `03-roadmap.md:80` `<60MB` → `<90MB`(顺带修该行遗留的 "Sciter.dll" 口径漂移) |
| T6.2 | `04-build-package-release.md`:`:206`、`:377` 改 `<90MB`;**§3.2 补"依赖放置步骤"**(当前**不存在**) |
| T6.3 | `package.ps1:160` 文案 + `:161` **阈值常量** |
| T6.4 | 修订 08 文档的 06 §11.2 引用 → 06 §0 + §13 |
| T6.5 | 随包附 LICENSE/NOTICE(RapidOcrOnnx Apache-2.0、PaddleOCR 模型 Apache-2.0) |
| T6.6 | 修 `ocr.rs:39,186` 等指向**已不存在的** `docs/phase6.md` 的注释,改指 08 §2/§3 |

---

## 5. 验收矩阵(在原 08 §12 基础上补强)

| 层 | 用例 | 判据 | 相对 08 §12 的变化 |
| --- | --- | --- | --- |
| 单测(Rust) | `backend_and_language_parsing`、`validate_checks_ocr_options`、`load_repairs_removed_ocr_backend` | 全绿;**值集合三处必须同源**(T4.1) | 保持 |
| **单测(Rust)新增** | **`rapidocr_value_survives_validate_and_repair`** | `"rapidocr"`/`"auto"` 经 `validate()` **不报错**、经 `repair` **不被改写** | ⚠️ **新增**(防 §1.2.3 静默归零) |
| 真机诊断 | `cargo test -- --ignored --nocapture ocr_probe_backends` | 两引擎各自 available 与语言如实上报 | 保持 |
| 质量回归 | `--ignored ocr_quality_bench`(同目录双引擎并排) | 三档样例不退 | 保持 |
| 端到端 | 四条入口(`Main.qml:2151`、`TranslateView.qml:522`、`hotkeys.cpp:99,115`) | 行为与模式语义**均不变** | 保持 |
| **端到端新增** | **设 `ocrBackend=rapidocr` → 右键识别文字** | 输出**确实来自 RapidOCR**;日志可证未走 `nb_call` | ⚠️ **新增**(§1.2.3) |
| **验收新增** | 仿 `acceptance.cpp:354 runSettingsFlow` STEP3 手法,新增 OCR 后端/缩放断言 | `--settings-flow` 内读回 `get_settings` 断言 `ocrBackend` | ⚠️ **新增**(当前 acceptance **零 OCR 覆盖**) |
| 失败路径 | 删 `nbocr.dll` / 删 `models/ocr/*` 后启动 | 应用**照常启动**,设置页给可懂原因,`system` 仍可用 | 保持 |
| 内存 | **06 §0 方法 + §13 范式**(≥6 轮 + 当场对照组) | 嵌入形态净增量与稳态残留;验证 `destroy` 回落 | ⚠️ **引用修正** |
| 体积 | `package.ps1` 第 6 步打印 | stage ≤90MB;`$must` 含 `nbocr.dll` 与 `models/ocr/ppocr_keys_v1.txt` | 保持 |
| 安装 | 装完启动并识别一次 | 递归段(`installer.nsi:100`)自动带上的文件确实到位 | 保持 |

### 5.3 关于 D8(空格补回)的建议

**本轮不做**。理由:
1. RapidOCR 输出 `Q3revenue` 对**当前**功能无影响——四条入口都只做"显示/复制/存条目",不参与搜索匹配;
2. 真正会产生影响的是 **`ocr_text` 回填 + FTS**(08 §1.2 明确本轮不做);
3. 补回空格需要"中英边界检测"启发式,本身是**新的错误来源**(可能把 `OpenAI` 拆成 `Open AI`);
4. 08 §14 未验证项 7 已把它标为"回填落地时才有意义"。

**建议**:在回填那一轮与 FTS 一起评估,届时用真实查询做 A/B。

---

## 6. 与 08 原计划的差异对照(便于评审)

| 维度 | 08 原计划 | **本路线** | 理由 |
| --- | --- | --- | --- |
| 阶段数 | P1(已完成)+ P2/P3/P4 | **阶段 0–6**(新增决策前置、依赖落地、C ABI 独立、文档收口) | 拆分可回滚边界 |
| C ABI | "导出 §6 的 6 个符号"(暗示直接编译上游) | **明确为本项目自研薄封装层** | spike 制品**无 dll/头文件**,§1.2.1 |
| 构建接入 | `package.ps1` 三处 | `package.ps1` + **`build.ps1` 钩子** + **`app/CMakeLists.txt` 目标** + **`.gitignore`** | §1.2.2 |
| 体积改动 | 2 处 | **5 处**(含 `:161` 阈值常量) | §1.2.4 |
| 内存复测引用 | 06 §11.2 | **06 §0 + §13** | §1.2.5(引用落空) |
| 验收矩阵 | 9 行 | **12 行**(新增静默归零防护、端到端引擎证真、验收覆盖) | §4 新增风险 |
| 决策前置 | D6/D7/D8 散在 §4 | **独立为阶段 0**并作为阶段 4 的门禁 | 未拍板则无法编码 |
| D8 处置 | ⚠️ 待拍板 | **建议不做**,给出 4 条理由 | §5.3 |

---

## 7. 建议的推进顺序与里程碑

```
阶段0 决策前置 ──┐ (D6/D7/D8 + 联网取回上游事实)
                 │
阶段1 依赖落地 ──┼─► 阶段2 编译验证 ──► 阶段3 C ABI ──► 阶段4 接线 ──► 阶段5 实测 ──► 阶段6 文档
  (纯新增)       │      (纯新增)          (纯新增)        (唯一改主干)     (定档)        (收口)
                 │
                 └─► 阶段 1–3 全部"主干零改动"，任一步失败均可整体放弃而不影响现有功能
```

**里程碑判据**:

| 里程碑 | 判据 | 失败处置 |
| --- | --- | --- |
| M1(阶段 0–1 完成) | 依赖齐备 + SHA256 通过 + 决策落文字 | 成本极低,可放弃 |
| M2(阶段 2–3 完成) | `nbocr.dll` 三档样例结果与 spike 一致 | **主干未动**,整体回滚无代价 |
| **M3(阶段 4 完成)** | §5 验收矩阵全绿 | **唯一有主干风险的节点**,建议单独提交、单独回滚 |
| M4(阶段 5 完成) | 嵌入形态真实数字 + D6/D7 定档 | 可退回 `system`-only 发布 |
| M5(阶段 6 完成) | 文档口径一致 + 合规文件随包 | — |

**关键建议**:阶段 1–3 是**纯增量**,应作为"探针"先行;只有 M2 通过后才投入阶段 4 的主干改造。这样即使 RapidOCR 最终因体积/内存被否决,项目也只损失了一个 `docs/` 目录和几个 `tools/` 脚本。

---

## 8. 未决问题清单(需人工拍板)

| # | 问题 | 影响 | 建议 |
| --- | --- | --- | --- |
| Q1 | **D6 默认档位**是否采纳 `auto`? | 决定慢路径(3.8s/233MB)是否进入默认流程 | ✅ 建议 `auto` + 降级链 |
| Q2 | **D7 生命周期** A/B/C? | 稳态内存 vs 首次识别时延 | 阶段 5 实测后定,起点 B(N=60) |
| Q3 | **D8 空格补回**? | 搜索可用性(回填后才生效) | ✅ 建议本轮不做 |
| Q4 | 是否接受**包体从 56MB → ≈88MB**(+57%)? | 产品决策 | 需用户拍板(D4 已倾向接受) |
| Q5 | 是否接受**全屏场景 3.8s 起**? | 感知性能 | ✅ 建议由 `auto` 兜底链缓解 |
| Q6 | 是否引入**自编译 md 依赖**,而非用官方 mt 预编译? | 跨 CRT 崩溃风险 | **必须 self-build md**,无替代方案 |
| Q7 | 上游 C API:**复用** vs **自研薄封装**? | 阶段 3 工作量 | ✅ 建议自研(所有权可控) |
| Q8 | 长图/超大截图是否需要**分区或切分**? | 全屏时延 | 阶段 5 评估 |

---

## 附:本次复核使用的方法与可追溯性

- **源码核查**:`read`/`grep`/`glob` 精确到行号,全部结论附 `文件:行号`。
- **spike 数据复核**:直接读 `dist/ocr-spike/compare2.txt`、`crop_summary.txt` 原始产物反推,而非采信文档转述(见 §1.3)。
- **制品复核**:文件字节数与文档声称逐项对比(模型四件)。
- **git 状态**:HEAD=`c4d1aeb`;唯一未跟踪文件为 `docs/08-rapidocr-engine.md`。
- **无法联网**:github.com / raw.githubusercontent.com / rapidai.github.io 在本环境解析到非公网 IP,`web_fetch` 对官方文档站直接失败 → 上游事实经**可达镜像与官方作者原文**获取(见 §9 方法说明)。
- **本文件为唯一产物**;未修改任何代码、配置或其他文档。

---

# 9. 第二轮分析:依据 RapidOCR 官方文档的补充结论

> 本轮用户要求"读取官方文档快速开始页再分析"。`rapidai.github.io` 在本环境**解析到非公网 IP,直接抓取失败**。
> 因此改用**可达的等价来源**:① 官方仓库作者 **Danno(SWHL)** 在博客园发布的官方文档摘录与版本发布说明;
> ② Gitee 上 RapidOcr-Java 的 `COMPILE_LIB.md`(照抄官方 `BUILD.md` 的编译步骤);
> ③ Web 检索命中的官方文档站 URL 索引(用于确认章节存在性)。
> **凡本文标注"官方文档"的内容,均来自官方文档的原文摘录或官方作者博文,非二手转述。** 无法直连官方站这一点见 §9.6。

## 9.1 🔴 最重要的修正:RapidOCR 已到 **v3.10.0**,而 08 文档与 spike 都停留在 **v1.2.3 / PP-OCRv4** 时代

08 文档附录 B 记载的上游事实是 "1.2.3 预编译仅 mt"、"SWHL/RapidOCR(HF) 无 v5/v6"。这在写作时正确,但**官方已大幅前进**:

| 维度 | 08 文档 / spike 认知 | **官方现状(2026-10 核实)** |
| --- | --- | --- |
| 最新版本 | 1.2.3(预编译) | **rapidocr v3.10.0**(2026-10-09 发布) |
| 模型代际 | PP-OCRv4 mobile 三件套 | **PP-OCRv5 已集成**(v3.0.0 起),且 v3.10 已见 **PP-OCRv6** 相关讨论 |
| 推理引擎 | 仅 ONNX Runtime | onnxruntime / OpenVINO / PaddlePaddle / **PyTorch** 四种 |
| 图像输入 | 文件路径 | **支持"从内存地址解码图像(含 format+stride 元数据)"** |
| 模型管理 | 随包 | `default_models.yaml` **model_routes** 集中路由 + **延迟加载** + **SHA256 校验** |
| 默认模型 | — | 仍是 **PP-OCRv4**(v3.0.0 起默认 `ocr_version: PP-OCRv4`) |

**为什么这很重要**:08 文档的 spike 用 PP-OCRv4 得到"质量碾压内置引擎"的结论 —— 这个结论**依然成立且只会更好**,因为:
- 官方自己在 v3.0.0 明确:**PP-OCRv5 在官方自建测评集上"并没有特别突出",故默认仍用 PP-OCRv4**;
- 且官方 det 模型实测(见 §9.3)**PP-OCRv4_mobile_det 的 H-mean 0.8476 反而高于 PP-OCRv5_mobile_det 的 0.8058**。

→ **结论:08 文档选 PP-OCRv4 mobile 是对的,不需要改。** 但**必须**知道上游已能提供 v5/v6,否则后续"质量为什么不够好"时会重复选型。

## 9.2 🔴 修正 §1.2.1:C++ 侧的**真实形态**比 08 文档描述得更清楚

我在 §1.2.1 说"spike 制品只有 exe,C ABI 要自己写" —— **这个结论对,但原因需要补正**。官方 `BUILD.md`(经 Gitee 镜像的 RapidOcr-Java `COMPILE_LIB.md` 照抄)明确:

```
1. git clone RapidAI/RapidOcrOnnx
2. 下载 opencv(RapidAI/OpenCVBuilder releases)
   ⚠️ windows 平台分为 mt 和 md 版：mt = 静态链接 CRT，md = 动态链接 CRT
3. 下载 onnxruntime(RapidAI/OnnxruntimeBuilder releases)
   static 为静态库，shared 为动态库，一般情况下使用静态库即可
4. 解压到 opencv-static/ 与 onnxruntime-static/
5. ./build.sh → 按提示输入选项 → 最后选择 'JNI动态库'
   编译结果：libRapidOcrOnnx.dylib 即为所需库文件
```

**关键事实(直接影响 §1.2.2 与阶段 2/3)**:

| # | 事实 | 对本项目的影响 |
| --- | --- | --- |
| 1 | 官方构建**必须**先手动放置 `opencv-static/` 与 `onnxruntime-static/` 两个目录 | ✅ 印证 §1.2.2:`thirdparty/` 目录设计**与官方一致**,不是我们的臆造 |
| 2 | 官方明确区分 **mt(静态 CRT)/ md(动态 CRT)**,与本项目 `/MD` 诉求对齐 | ✅ 印证 08 §8.1"必须 md";**md 制品确实存在** |
| 3 | 官方 `build.sh` 是**交互式**的,最后选择产物类型(JNI 动态库等) | ⚠️ **无 CI 场景下无法自动化** → 阶段 2 必须**绕过 `build.sh`,直接写自己的 CMake**(§9.4 已调整) |
| 4 | 编译后需**手工修改 include 路径**(见下) | ⚠️ 新增工作量,08 文档未提 |
| 5 | 官方代码内置**无法关闭的 printf 日志** | ⚠️ 会污染宿主日志 → 阶段 2 需剔除(§9.4) |

第 4 条官方原文:

```c
将 #include <onnxruntime/core/session/onnxruntime_cxx_api.h>
改为 #include <onnxruntime/onnxruntime_cxx_api.h>
// 受影响的文件:AngleNet.h、CrnnNet.h、DbNet.h、OcrLite.h、OcrUtils.h
```

第 5 条官方原文(`COMPILE_LIB.md` 的"去除动态库日志打印"节):

> 在使用过程中,会发现会在控制台打印配置信息,而且**没办法关闭**……这是因为在 RapidOnnx/RapidNcnn 代码中添加了打印日志,所以需要**修改源码**,去除日志打印。例如搜索 `printf("numThread=` 找到相应代码注释掉,并重新打包即可。

**⚠️ 这对本项目是很实际的坑**:宿主已有统一日志(`nextboard.log`)。若 `nbocr.dll` 向 stdout 打印 `numThread=` 等,会污染日志甚至干扰验收断言。**必须在阶段 2 作为独立任务处理**,且因为要改上游源码,需记录改动清单以便升级时重放。

## 9.3 🟢 新增证据:官方 det 模型实测指标,支持"沿用 PP-OCRv4"的选择

官方作者公布的 det 模型对比(`TextDetMetric`,卡证82+文档75+自然场景55,**CPU**):

| Exp | 模型 | 框架 | Precision↑ | Recall↑ | **H-mean↑** | Elapse↓ |
| --- | --- | --- | --- | --- | --- | --- |
| 3 | **PP-OCRv5_mobile_det** | RapidOCR/ONNX | 0.7861 | 0.8266 | **0.8058** | 0.1653 |
| 4 | **PP-OCRv4_mobile_det** | RapidOCR/ONNX | 0.8301 | 0.8659 | **0.8476** | — |
| 7 | PP-OCRv5_server_det | RapidOCR/ONNX | 0.7394 | 0.8442 | 0.7883 | 2.0628 |
| 8 | PP-OCRv4_server_det | RapidOCR/ONNX | 0.7922 | 0.8128 | 0.7691 | — |

官方结论原文:

> Exp2 和 Exp3 相比,mobile 模型转换为 ONNX 格式后,指标几乎一致,说明**模型转换前后,误差较小**,推理速度也有提升。
> 最终建议:**如果是单一中英文场景,建议用 PP-OCRv4 系列**;如果是中英日、印刷和手写体混合场景,建议用 PP-OCRv5 系列。

**对本项目的直接意义**:
1. 本项目场景是**桌面截图 = 单一中英文印刷体** → 官方明确推荐 **PP-OCRv4 系列** ✅ **与 08 文档的 D3 决策完全吻合**;
2. PP-OCRv4_mobile_det 的 H-mean(0.8476)**高于** PP-OCRv5_mobile_det(0.8058)→ 08 文档"选 mobile v4"在**指标上也是更优选择**,不只是"够用";
3. server 档**全面更差且慢**(v5_server 2.06s) → 08 文档选 mobile、拒 server 的决策得到官方数据支持;
4. "**模型转换前后误差较小**"→ 说明从 Paddle 转 ONNX 的链路可靠,本项目直接取现成 ONNX 模型的风险低。

## 9.4 对实施计划的**具体修订**(并入阶段 2/3/5)

### 阶段 2 修订

| 任务 | 原计划 | **修订后** |
| --- | --- | --- |
| T2.1 | 建 CMake 目标 | **改为:不使用官方 `build.sh`(交互式,不可自动化),自写 CMake 直接编译 RapidOcrOnnx 的 `src/*.cpp`** |
| T2.2 | 剔除 `main.cpp`/`JNI` | **保留**,并新增:**新增任务 T2.7 剔除官方内置 `printf` 日志**(逐处注释,记录清单) |
| T2.3 | clipper 命名空间隔离 | 保留 |
| T2.4 | `/MD` 统一 | **保留并强化**:官方明确 md 制品存在 → **阶段 1 必须取 md**(已确认存在,非猜测) |
| **T2.7(新)** | — | **源码 include 路径修正**:5 个头文件的 `<onnxruntime/core/session/...>` → `<onnxruntime/...>`;记录为 patch 清单 |
| **T2.8(新)** | — | 建立 `thirdparty/README` 记录**上游 commit + 全部本地 patch**,使升级可重放 |

### 阶段 3 修订

| 任务 | 原计划 | **修订后** |
| --- | --- | --- |
| T3.1 | 核对上游 `OcrLiteCApi.h` | **保留(仍需人工确认)**,但**倾向性更强**:官方 `build.sh` 的产物选项含"JNI 动态库",说明**存在生成 dll 的路径**;而其 C API 是否覆盖本项目 §6 的所有需求(尤其内存所有权)**仍需实测** |
| T3.4 | 只认 BGR8 | **新增机会**:v3.10 官方已支持**"从内存地址解码图像(带 format 与 stride 元数据)"** → 若上游 C++ 侧也跟进,**`QImage→BGR8` 的转换转嫁给上游**可省一次拷贝。阶段 3 应**主动核对上游是否已有该能力**,有则采用 |

### 阶段 5 新增

| 任务 | 内容 |
| --- | --- |
| **T5.6(新)** | **模型档位 A/B**:在真实桌面截图上对比 PP-OCRv4_mobile vs PP-OCRv5_mobile vs v6(若可用)。官方测评集**不含桌面 UI 场景**,其"建议 v4"结论**不必然适用于本项目的界面截图**(含大量小字号、图标旁文字)。**用本项目自己的样例决定,而不是照抄官方建议** |
| **T5.7(新)** | 复核官方"模型转换前后误差较小"是否在本项目样例上成立(即现成 ONNX 与 Paddle 原模型的一致性) |

> ⚠️ T5.6 很重要:官方建议"中英文印刷体用 v4"基于**卡证/文档/自然场景**三类测评集,**没有桌面软件界面**这一类。本项目的主场景恰恰是桌面 UI。**08 文档的质量结论(§2)是在 PP-OCRv4 上测的,已验证可用;但"v5 是否更好"在本项目场景下仍是开放问题** —— 这正是阶段 5 该测的。

## 9.5 一条此前**完全未识别**的机会:官方能力已就位,可能省掉大量自研

第二轮分析发现官方 v3.10.0 有三项能力,与 08 文档的设计**部分重叠**:

| 官方能力 | 与 08 文档设计的关系 | 建议 |
| --- | --- | --- |
| **`default_models.yaml` + model_routes + SHA256 校验** | 08 §8.1 第 4 条要求"模型记 SHA256 并校验" | ✅ **直接采用官方的 SHA256 值**,不必自行计算基线(阶段 1 T1.4 可简化) |
| **模型延迟加载(lazy model load)** | 08 §10.2 的 D7"session 生命周期"A/B/C 三档 | 🟢 **官方已解决持久化开销问题**。虽 C++ 侧未必有此特性,但**证明"延迟加载 + 首次使用才构造"是官方认可的方向** → D7 的 B 档(空闲 destroy)与官方思路一致;**阶段 5 可参考官方做法** |
| **从内存解码图像(含 stride)** | 08 §6 硬性约定 2"只认 BGR8,由宿主从 QImage 转换(注意 bytesPerLine 对齐)" | 🟢 **若上游 C++ 支持,可把"格式/步长"直接传下去**,免去宿主手工转换及其对齐 bug 风险。阶段 3 T3.4 应优先核对 |
| **OpenCV 默认改 headless 版** | 本项目 `nbocr.dll` 静态链 OpenCV | 🟢 官方为"轻量化"改用 `opencv-python-headless` → **印证本项目应链接 OpenCV 静态库且不需要 GUI 模块**,可显著减体积。阶段 2 构建时**应确认未链入 OpenCV 的 GUI/HighGUI 模块** |

最后一条尤其值得注意:**08 §9.4 的体积外推(引擎 16.4MB)基于官方 CLI exe**。若官方自身都在做 headless 瘦身,则**自编译时裁剪 HighGUI 等模块可能让 `nbocr.dll` 比 16.4MB 更小** → 包体可能好于 ≈88MB 的悲观外推。**建议阶段 2 实测时专门记录此项**。

## 9.6 诚实声明:本轮**仍未能**直接读取官方文档站

| 项 | 状态 |
| --- | --- |
| `https://rapidai.github.io/RapidOCRDocs/main/quickstart/` | ❌ **直接抓取失败**:`rapidai.github.io` 解析到非公网 IP |
| 同上,各版本页(`v3.8.0`/`v3.8.3`/`v3.9.1`/`main/en/`) | ❌ 同上 |
| github.com / raw.githubusercontent.com / cdn.jsdelivr.net / pypi.org / deepwiki.com | ❌ 全部解析失败 |
| **实际采用的可达来源** | ✅ 博客园(作者本人官方文档摘录 + 版本发布说明)、Gitee 镜像(`COMPILE_LIB.md` 照抄官方 `BUILD.md`)、掘金(同文转载)、Web 检索索引 |

**因此本节的"官方文档"内容,来自官方文档的原文摘录与官方作者本人的发布说明**,而非我直接浏览官方站。**若需逐字核对,应人工访问官方站复核 §9.1–§9.5 的每条引用**。此外:
- `OcrLiteCApi.h` 的**确切签名**仍未取得(仅确认官方构建流程存在 dll 产物路径);
- PP-OCRv6 的存在仅见于检索命中的第三方标题,**未取得官方一手确认** → 阶段 5 若要评估 v6,需先人工核实。

## 9.7 §9 对决策清单(§8)的更新

| # | 原建议 | **§9 更新** |
| --- | --- | --- |
| Q6 | 必须 self-build md | ✅ **确认可行**:官方明确提供 md 制品(mt/md 二选一),**非猜测** |
| Q7 | 上游 C API:建议自研 | ⚠️ **需先核对**:官方存在 dll 产物路径;若能满足 §6 所有权语义则**优先复用**,否则自研。判断依据在阶段 3 T3.1 |
| **Q9(新)** | — | **模型档位是否要用 PP-OCRv5/v6?** 官方建议中英印刷体用 v4,但**其测评集不含桌面 UI** → 建议阶段 5 用本项目样例实测后再定;**当前维持 PP-OCRv4 mobile** |
| **Q10(新)** | — | **是否接受"打 patch 上游源码"**(剔除 printf 日志、修 include 路径)?这是**无 CI 下的维护负担**,需明确接受并建立 patch 清单 |

---

## 附 A:两轮分析的方法对比(可追溯性)

| | 第一轮(§1–§8) | 第二轮(§9) |
| --- | --- | --- |
| 数据源 | 本地源码 + spike 原始产物 + git 历史 | RapidOCR 官方文档(经可达镜像/作者原文)+ Web 检索 |
| 覆盖问题 | "08 文档的描述与本仓库实际是否一致" | "08 文档对上游的认知是否过时" |
| 主要产出 | 13 条论断复核 + 5 条修正 + 7 阶段路线 | 3 条结论修正 + 2 条新增任务组 + 1 项新机会 |
| 未解决 | 上游 C API 签名、md 制品存在性 | 官方站直连、`OcrLiteCApi.h` 签名、PP-OCRv6 一手确认 |

**两轮合起来,§8 的未决清单从 8 项增至 10 项(Q9/Q10 新增),其中 Q6 已由"猜测"转为"已确认"。**

---

# 10. 第三轮:对《RapidOCR 安装指南》的分析

> 用户直接提供了官方《RapidOCR 安装指南》全文(即 §9.6 中我无法直连的官方文档站内容之一)。
> 这是**官方一手文档**,可信度高于 §9 经镜像获取的内容。以下逐条分析其对本项目的意义。

## 10.1 🔴 首要结论:该文档**通篇面向 Python 使用者**,与本项目的 C++/Qt 路线**基本不适用**

文档自述边界:徽章标注 `Python >=3.8,<4`、`OS Linux/Win/Mac`,正文全部是 pip 安装与 Python API。

| 文档内容 | 对本项目的适用性 |
| --- | --- |
| `pip install rapidocr onnxruntime` | ❌ **不适用**。本项目走 **C++ 宿主 + nbocr.dll**(D1 决策),不引入 Python 运行时 |
| "包大小约 27.2M,含三个模型" | ⚠️ 仅供参考。那是 **whl 内嵌模型**的 Python 包;本项目要的是 **.onnx 原件**手动入 models/ocr/ |
| rapidocr_onnxruntime / _openvino / _paddle 不再维护 | ✅ **有参考价值**:印证"以 rapidocr 为主"的收敛方向。但**注意**:C++ 侧对应物是**独立仓库 RapidOcrOnnx**,不是这些 Python 包,不要混淆 |
| 12 个 Python 依赖(pyclipper/Shapely/PyYAML/omegaconf/colorlog…) | ❌ 纯 Python 依赖树。C++ 侧等价物是 **clipper.cpp(上游自带) + OpenCV + ONNX Runtime** —— 即 08 §8 已识别的组件 |
| `rapidocr check` 验证安装 | ❌ C++ 侧**无等价物** → **本项目需自造"引擎自检"**(见 §10.3 任务 T3.8) |
| `rapidocr[rtl]` → python-bidi | ❌ 仅 Python 生态;本项目无需阿拉伯语 RTL |
| "如果依赖装不上,先单独装某个依赖" | ❌ Python 特有排障,无对应 |

**⚠️ 必须避免的误读**:本安装指南是**Python 用户**的入口文档。若据此认为"装个 pip 包就能用",会得到与本项目完全不同的实施路径。

## 10.2 🔴 第二结论:该文档**暴露了一条此前未发现的强证据 —— 默认模型已转向 PP-OCRv6-small**

这是本轮**最有价值**的发现。文档的 `rapidocr check` 验证输出**明列了模型文件名**:

```
rapidocr>=2.0.3 验证方式(官方原文摘录):
[INFO] ... File exists and is valid: .../rapidocr/models/PP-OCRv6_det_small.onnx
[INFO] ... Using .../rapidocr/models/PP-OCRv6_det_small.onnx
[INFO] ... File exists and is valid: .../rapidocr/models/ch_ppocr_mobile_v2.0_cls_mobile.onnx
[INFO] ... File exists and is valid: .../rapidocr/models/PP-OCRv6_rec_small.onnx
Success! rapidocr is installed correctly!
```

**与 08 文档 / 本地 spike(逐字比对)**:

| 件 | 08 文档 & 本地 spike | **官方当前默认(本次文档)** | 差异 |
| --- | --- | --- | --- |
| det | `ch_PP-OCRv4_det_infer.onnx` | **`PP-OCRv6_det_small.onnx`** | 代际(v4→**v6**)+ 命名 + 档位(infer→**small**) |
| cls | `ch_ppocr_mobile_v2.0_cls_infer.onnx` | `ch_ppocr_mobile_v2.0_cls_mobile.onnx` | 后缀 infer→mobile |
| rec | `ch_PP-OCRv4_rec_infer.onnx` | **`PP-OCRv6_rec_small.onnx`** | 代际 + 命名 + 档位全变 |
| keys | `ppocr_keys_v1.txt`(6623 行) | 未打印,但 **rec 换代 → 字典必须重新配对** | ⚠️ 08 §8.1 第 3 条"字典必须与 rec 配对"依然适用 |

**三重含义**:

1. **PP-OCRv6 确认为真**(此前 §9.6 只能标"仅见第三方标题,无一手确认")→ **现已升级为一手确认**。且命名含 small,印证存在 det/rec × small/medium/tiny 的档位体系;
2. **08 文档 §14 未验证项 5 提到的"v6-tiny / v6-tiny-int8(5.48MB / 2.80MB)"得到方向性印证** —— 体积档确实存在,且比 v4 mobile 更小;
3. ⚠️ **但"官方默认改用 v6"≠"本项目应该用 v6"**。理由:
   - 官方默认档变更是**产品取向**(追求开箱体积/速度),其 small 档**未必**优于 v4 mobile 在本项目"桌面 UI 小字号"场景的表现;
   - §9.3 的官方 det 实测中,**PP-OCRv4_mobile_det 的 H-mean 0.8476 高于 PP-OCRv5_mobile_det 的 0.8058** —— 代际更新在该测评集上**并未体现为指标提升**;
   - 官方测评集**不含桌面软件界面**,而这是本项目主场景。

→ **结论:维持 08 的 D3(PP-OCRv4 mobile)不变,但把 v6-small / v6-tiny 正式纳入阶段 5 的 A/B 评测清单**(见 §10.4 T5.6 修订)。这是**必须实测而非照抄默认值**的典型场景。

## 10.3 🟢 第三结论:三条可直接采纳的工程要点

| # | 文档事实 | 对本项目的行动 |
| --- | --- | --- |
| 1 | `rapidocr>=3.10.0` 把 opencv-python 改为 **opencv-python-headless**,并注明"服务端、Docker、无桌面环境用 headless;需要 imshow/GUI 才装普通版" | ✅ **强化 §9.5 第 4 条**:nbocr.dll 是**纯计算**,**绝不需要 HighGUI**。阶段 2 构建时**必须确认未链入 HighGUI 模块** —— 这是**可省体积的确定性项**,应写入 T2 的 DoD |
| 2 | `rapidocr>=2.0.6` **不再把 ONNX Runtime 作为依赖**,需手动安装推理引擎 | ✅ 印证 08 §8.1 钉死 ORT 版本的做法**是必要的**(上游不代管版本);也说明"引擎与模型解耦"是官方方向 |
| 3 | 文档强调 opencv-python 与 opencv-python-headless **不能同时安装**("避免 cv2 冲突") | ⚠️ 提示本项目一个**同类风险**:OpenCV 若同时被 nbocr.dll(静态链)与其他组件(动态)引入,也会有符号/运行时冲突 → 归入 §8.2 的 clipper 命名空间隔离同一类问题 |

**新增任务 T3.8(源于本轮的缺口识别)**:
> 官方有 `rapidocr check` 做"引擎+模型是否就绪"的自检;**本项目 C++ 侧无等价物**。
> 08 §6 只定义了 `nbocr_available(models_dir)`(仅文件存在性检查)。**建议扩展**:让 nbocr_available 除文件存在性外,还能返回**模型可加载性**的探测结果(尝试构建 session 后立即销毁,或返回明确的失败原因码),使设置页能区分"缺文件"与"文件在但加载失败"。
> 对应 08 §7.4 的"设置页给出可懂原因",当前设计只能给出前者。

## 10.4 对计划的具体修订

| 位置 | 修订 |
| --- | --- |
| **T5.6**(§9.4 新增的档位 A/B) | **扩展评测清单**:PP-OCRv4_mobile(基线) vs **PP-OCRv6_small** vs **PP-OCRv6_tiny**(§14 未验证项 5 提及,5.48MB/2.80MB)vs PP-OCRv5_mobile。**用本项目桌面 UI 样例判定** |
| **T2.x**(阶段 2) | 新增 DoD 项:**确认生成物未链接 OpenCV HighGUI 模块**(依据 §10.3 第 1 条) |
| **T3.8**(新) | nbocr_available 扩展为"可加载性探测",对齐官方 rapidocr check 的能力;**设置页区分"缺文件"与"加载失败"** |
| **§8 Q9** | 更新:PP-OCRv6 **已升级为一手确认**(原为"需人工核实")→ 决策项从"是否要 v6"变为"**用哪个档**",由 T5.6 实测定 |
| **§9.6 未解决项** | "PP-OCRv6 一手确认" → ✅ **本轮解决** |

## 10.5 该文档**没有**回答、且对本项目最关键的三个问题

诚实声明:这份安装指南虽为官方一手,但**回答不了本项目的核心问题**:

| 问题 | 为何这份文档答不了 | 应去哪里找 |
| --- | --- | --- |
| **1. C++ 侧怎么用?** | 全文零 C++ 内容。C++ 侧是**另一个仓库** RapidOcrOnnx | RapidOcrOnnx 的 BUILD.md / OcrLiteCApi.h(§9.2 已部分获取) |
| **2. 模型 .onnx 原件从哪下载?** | 文档只说"已打到 whl 包内",**未给直链**。而本项目**不能**用 whl,要裸 .onnx | ModelScope / HF(SWHL/RapidOCR);⚠️ 08 §8.1 已指出 HF 仓**不含字典 txt** |
| **3. 桌面 UI 场景该用哪代/哪档模型?** | 官方测评集是卡证/文档/自然场景,**不含桌面界面** | **只能自己测**(T5.6) |

第 2 条尤其要注意:文档说"small 版模型较小,因此相关模型都已打到 whl 包"——这意味着**新档位(v6-small)的裸 .onnx 下载路径需要重新确认**,不能沿用 08 附录 A 里那套 HF 链接(v4 专用的)。→ **新增阶段 1 任务 T1.6**。

**新增任务 T1.6**:确认 v6-small/tiny 档的**裸 .onnx 与配套字典**的官方下载地址(whl 之外的来源),记录 SHA256。若官方仅以 whl 分发,则需从 whl 中提取 —— **这会影响"全离线、不首次运行拉取"的落地方式**,需在阶段 1 定案。

## 10.6 §10 小结:这份文档改变了什么

| 项 | 变化 |
| --- | --- |
| **新增一手确认** | PP-OCRv6 真实存在,且已是官方**默认**模型(§10.2) |
| **强化** | 不链 OpenCV HighGUI(§10.3);ORT 版本需自行钉死(§10.3) |
| **新增缺口** | 无 rapidocr check 等价物 → T3.8 |
| **新增待办** | v6 裸 .onnx 与字典的下载来源 → T1.6 |
| **维持不变** | **D3 仍为 PP-OCRv4 mobile** —— 官方默认变更不构成改用理由,需实测(§10.2) |
| **明确不适用** | pip/whl/Python 依赖树/rapidocr check/[rtl] —— 本项目**一概不采用**(§10.1) |

---

## 附 B:三轮分析的累积结论

| | 第一轮(§1–§8) | 第二轮(§9) | 第三轮(§10) |
| --- | --- | --- | --- |
| 数据源 | 本地源码 + spike 产物 | 官方文档(镜像/作者原文) | **官方安装指南全文(用户提供)** |
| 核心发现 | 工程量被低估:构建接入、三处白名单、降级语义 | 上游已到 v3.10.0;C++ 侧真实形态与两个坑 | **默认模型已转 v6-small;官方能力清单** |
| 对 D3 的影响 | 支持 v4 mobile | 支持 v4 mobile(官方指标佐证) | **仍维持 v4,但 v6 必须实测**(§10.2) |
| 新增任务 | — | T2.7/T2.8/T5.6/T5.7 | **T1.6/T3.8**,T5.6 扩展 |
| 遗留 | C API 签名、md 存在性 | 官方站直连、v6 一手确认 | **v6 裸 onnx 下载源、C++ C API 签名** |

---

# 11. 第四轮:CLI 路线可行性评估(回答"不是可以使用 CLI 吗")

> **用户的质疑是对的,而且提得很关键。** 我在 §1–§10 通篇默认走 `nbocr.dll` 嵌入路线(沿用 08 文档的 D1 决策),
> **没有认真评估过"直接调用官方预编译 CLI"这条更简单的路**。本节补上这个分析。
> 
> 结论先给:**CLI 在"功能上完全可行、且已经被 spike 实证跑通过",但它在"每次识别的固有开销"与"输出解析"
> 两点上劣于嵌入路线;不过它有一个被严重低估的价值 —— 应当作为阶段 2/5 的对照基准与降级兜底。**

## 11.1 事实澄清:CLI 不仅可行,**本项目已经用它跑出了全部 spike 数据**

这一点我此前没有说清楚。复核 `dist/ocr-spike/` 后确认:

| 证据 | 内容 |
| --- | --- |
| 制品 | `win-BIN-CPU-x64/RapidOcrOnnx.exe`(mt 版,官方预编译) |
| 调用 | 08 附录 A 的复现命令就是 CLI:`RapidOcrOnnx.exe -d models -1 ... -i samples/screen.png -t 8 -a 0 -s 960` |
| 产出 | `out_*.txt`(stdout 捕获)、`samples/*.png-result.txt`(结果文件) |
| 数据来源 | 08 §2/§3 的全部数字(193ms / 260ms / 1387ms / 5121ms、168 框、157 行)**都是 CLI 跑出来的** |

**⇒ 08 文档 §2/§3 的实测数据,本质上是"CLI 路线"的数据。** 这在方法论上很重要:它意味着
"CLI 能不能用"这个问题**已经有实证答案 —— 能,而且质量结论正是基于它**。

**CLI 的完整接口(从 `run-test.bat` / `run-benchmark.bat` 实测)**:

```
输入: --image <文件路径>          ← 只接受文件路径,不支持内存/管道/stdin
--models --det --cls --rec --keys --image
--numThread --padding --maxSideLen --boxScoreThresh --boxThresh
--unClipRatio --doAngle --mostAngle --GPU

输出: ① stdout(日志与结果混排)  ② <图片同目录>/<图片名>-result.txt  ③ -result.jpg(可视化)
形态: 每次进程做一次识别(批量工具),没有"常驻服务"模式
```

## 11.2 CLI 的三个真实优点(不应否认)

| # | 优点 | 价值 |
| --- | --- | --- |
| 1 | **零编译成本** | 不需要阶段 2/3 的全部工作(自写 CMake、剔除 main.cpp/JNI、修 include、剔 printf 日志、C ABI 封装) |
| 2 | **零 CRT 风险** | 不存在 08 §5.3 的"隐式链接 Rust cdylib 导致 `nb_init` 内 0xC0000005"那类问题 —— 进程边界天然隔离 |
| 3 | **崩溃隔离** | CLI 崩了只死一个子进程,不会拖垮 `nextboard.exe`;这对"无 CI、难定位现场"的项目是**实打实的鲁棒性** |

第 2、3 条尤其值得重视:**08 §5.3 花了整节论证"为什么必须是独立 DLL",但 CLI 用"独立进程"达到了同等甚至更强的隔离** —— 这是 08 文档没有对比过的选项。

## 11.3 但 CLI 有两个硬伤(这是它成不了主路线的真实原因)

### 硬伤 1:每次识别都要"进程启动 + 模型重载",无法常驻

CLI 是**一次进程一次识别**的形态。从 spike 证据可推导其固有开销:

```
每次调用 = 进程启动 + 加载 3 个模型 + 识别
模型体积  = det 4.7MB + rec 10.9MB + cls 0.6MB = 16.2MB(每次从磁盘读 + ORT session 构建)
08 §10.2 已实测: 总 wall − detect ≈ 382ms(含进程启动)
```

**与嵌入路线对比(08 §10.2 策略 A 常驻)**

| 场景 | CLI(每次) | 嵌入常驻 |
| --- | --- | --- |
| 小图首次 | 193ms + 382ms ≈ **575ms** | 193ms(已含加载) |
| 小图后续 | 193ms + 382ms ≈ **575ms** | **193ms** |
| 全屏 | 5121ms + 382ms ≈ **5503ms** | 5121ms(首次) |

> ⚠️ 上面 CLI 侧的 382ms 是**外推**(08 §14 未验证项 3 已注明该值"含进程启动、未定量")。
> **需在阶段 2 用同一台机器、同一批样例逐项实测 CLI vs 嵌入的 wall time**,不能把外推当结论。

对照本项目场景:**右键「识别文字」是一个高频、交互式动作**(用户点完期望即时出字)。每次多付 ~380ms 且**无法通过常驻摊薄**,是产品体验上的实质损失。
而 08 §10.2 的"策略 B/C(空闲 destroy / 每次 destroy)"在**嵌入形态下仍可优化**(destroy 后可选择保留进程),CLI 则**永远拿不回这 382ms**。

### 硬伤 2:输出是"给人看的文本",不是给程序用的协议

这是**最容易低估**的一条。CLI 的结果文件**把日志和正文混在同一份文本里**(实测):

```
=====Input Params=====            ← 日志
numThread(8),padding(50),...       ← 日志
=====Init Models=====              ← 日志
--- Init DbNet ---                 ← 日志
=====Start detect=====             ← 日志
ScaleParam(...)                    ← 日志
TextBox[0](+padding)[score(...),...] ← 日志
textScores[0]{0.999124 ,...}       ← 日志
=====End detect=====               ← 分隔
FullDetectTime(225.186000ms)       ← 日志
const int MAX_LIMIT = ...;         ← ★ 正文从这里开始
if (11 != I1) Count0 += 90;        ← ★ 正文
```

**实测规律**(3 个样例均一致):正文位于 `FullDetectTime(...)` **之后**、`=====End detect=====` 之后的尾部区域。

| 样例 | 总行数 | End detect 位置 | 尾部正文行数 |
| --- | --- | --- | --- |
| code12 | 29 | 25 | 2 |
| mixed14 | 29 | 25 | 2 |
| crop760 | 329 | 275 | ~52 |

**解析层面的问题**:
1. **正文本身可能与日志行同形** —— 若截图内容里出现 `=====` 或 `TextBox[` 之类字符串,**用"找分隔符"的启发式会误判**;
2. **文本框(坐标)信息必须从日志区解析** —— `TextBox[n](+padding)[score(...),[x:..,y:..],...]` 是唯一的坐标来源。08 §6 的 `NbOcrLine{x,y,w,h,score,utf8}` 若需要坐标,**只能靠正则解析这批日志行**;
3. **编码坑**:08 附录 A 已实测记录"PowerShell 5.1 读 .ps1 需 UTF-8 BOM,控制台 GBK 会显示乱码" —— 子进程 stdout 的编码需显式约定(`chcp 65001` 或按 UTF-8 读取);
4. **没有结构化输出选项** —— 无 `--json`,只能文本解析。

**代价对比**:

| | 嵌入(nbocr.dll) | CLI |
| --- | --- | --- |
| 文本 | `NbOcrLine.utf8` 直接给 | 需从尾部切分 + 逐行判断 |
| 坐标 | 结构体字段 | **需正则解析日志行** |
| 置信度 | `float score` 字段 | 需解析 `textScores[]{...}` |
| 稳定性 | ABI 冻结 | **依赖上游日志格式不变化**(上游改一行 print 就可能崩解析) |

最后一行是**长期风险**:CLI 的输出格式是**实现细节而非契约**,上游无义务保持稳定。嵌入路线的 C ABI 则由我们自己冻结。

## 11.4 我的判断:CLI **不作为主路线,但应当正式采纳为"对照基准 + 降级兜底"**

经此分析,我更正前面几节"只有嵌入一条路"的隐含假设。建议:

| 角色 | 采纳? | 理由 |
| --- | --- | --- |
| **主路线** | ❌ 不采纳 | 每次 +~380ms 无法摊薄;输出需脆弱解析;坐标获取依赖日志格式 |
| **阶段 2 的对照基准** | ✅ **强烈建议** | 用 CLI 与自编译 `nbocr.dll` 跑**同一批样例**,逐项比对文本/坐标/耗时 —— 这是验证"自编译是否引入偏差"的**现成、零成本**对照组 |
| **阶段 5 的档位评测工具** | ✅ **强烈建议** | T5.6 要测 v4/v5/v6 × mobile/small/tiny 多档。**用 CLI 换模型只需改 `--det/--rec` 参数,不必重编译** —— 这会**极大降低 T5.6 的成本** |
| **生产环境的降级兜底** | ⚠️ 可选 | 若 `nbocr.dll` 加载失败(缺件/ABI 不符),可临时回退调 CLI 保证"至少能用"。但会增加包体(再带一个 exe)与两条路径的维护成本,**建议默认不做**,除非实测发现 DLL 路线脆弱 |

**特别是第 2、3 条 —— 这是本轮最有价值的产出**:
> 阶段 2 原计划是"编出 nbocr.dll 然后用它跑三档样例,看结果与 spike 一致"。
> 但"与 spike 一致"的**参照物就是 CLI 的输出**。既然如此,**应当把 CLI 固化为测试工装的一部分**(而非只用一次就丢),
> 让"自编译 DLL vs 官方 CLI"成为**可重复执行的一致性测试**。这也天然满足了 08 §13 风险表里"无 CI"的缓解诉求。

## 11.5 由此产生的新任务与计划修订

| 任务 | 内容 | 归属 |
| --- | --- | --- |
| **T2.9(新)** | **把官方 CLI 固化为测试工装**:保留 `RapidOcrOnnx.exe` + 模型于 `tools/ocr-bench/`(gitignore),写脚本对同一批样例跑 CLI,输出基线 JSON | 阶段 2 |
| **T2.10(新)** | **一致性测试**:自编译 `nbocr.dll` 与 CLI 在**同一批样例**上比对文本(逐行,允许已知的字形歧义差异)与耗时;差异需逐条解释 | 阶段 2 |
| **T5.6(修订)** | 档位评测**优先用 CLI**(改参数即可换模型),仅在最终选定的档位上再做 DLL 侧验证 | 阶段 5 |
| **T5.8(新)** | **实测 CLI 的 382ms 开销**(08 §14 未验证项 3 的定量):同机同批次对比 `CLI wall` vs `detect 净耗时`,给出真实值而非外推 | 阶段 5 |
| **§3 风险表** | 新增一行:"CLI 输出格式属上游实现细节,非契约" → 缓解:不改用 CLI 作主路线;仅作测试工装,格式变化只影响测试脚本 | — |
| **§1.2.1** | 补注:CLI 虽可行,但**预编译只有 mt 版**,与项目 `/MD` 不符 → 即便走 CLI 路线也**不能直接用官方预编译**(见 §11.6) | — |

## 11.6 ⚠️ 一个容易被忽略的致命细节:官方预编译 CLI 是 **mt** 版

**即便决定走 CLI 路线,也不能直接用官方预编译的 `RapidOcrOnnx.exe`**。理由:

1. 官方预编译包名就是 `windows-bin-vs2022-**mt**.7z`(08 §8.1 已钉死这个事实);
2. mt = **静态链接 CRT**。若该 exe 与 `nextboard.exe` 同处一个安装目录,虽因**进程隔离**不会跨 CRT 释放崩溃,但会带来:
   - **包体重复**:16.4MB(引擎) + 15.47MB(模型) 里,CRT 部分与主程序重复;
   - **更新/维护两份 CRT 口径**;
3. 更重要的是:CLI 路线下 `nextboard.exe` **仍需自己处理"调用子进程"** —— 进程创建、参数拼接、超时/取消、stdout 编码、临时文件清理与并发(用户连点两次)。**这些不是零成本**,只是把 08 阶段 2/3 的"编译复杂度"换成了"进程管理复杂度"。

> 结论:**CLI 降低的是"编译与 ABI 风险",不是"总体工程量"**。这个交换值不值得,取决于团队更怕哪一类问题。
> 我的建议仍是嵌入为主,但**在阶段 2 先用 CLI 快速验证一切**(零编译成本),再决定是否投入嵌入。

## 11.7 §11 小结:CLI 改变了什么

| 项 | 变化 |
| --- | --- |
| **纠正我的遗漏** | 前 10 节默认只有嵌入一条路;**CLI 可行且已被 spike 实证**(§11.1) |
| **新增对照组** | CLI 应固化为**测试工装**,用于"自编译 DLL vs 官方 CLI"一致性测试(T2.9/T2.10) |
| **降低 T5.6 成本** | 档位评测用 CLI 改参数即可换模型,**无需反复编译** |
| **澄清硬伤** | 每次 +~380ms(无法摊薄)+ 输出需脆弱解析(尤其坐标) |
| **补充关键事实** | 官方预编译是 **mt**,与项目 `/MD` 不符 → CLI 也不能直接用官方预编译(§11.6) |
| **主路线判断** | **维持嵌入为主**,但建议**阶段 2 先用 CLI 探路**再决定投入 |

**最终建议的一句话版本**:CLI 不是替代方案,而是**探针与标尺** —— 用它在阶段 2 以零成本验证整条链路与档位选择,再用嵌入形态落地到产品。
