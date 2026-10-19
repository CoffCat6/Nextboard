# 10 · 方案 B 实施计划(RapidOCR Rust 引擎嵌入 nbcore)

| 项 | 内容 |
| --- | --- |
| 前提 | 方案 B 已选定(见 [09-ocr-optimal-plan.md](./09-ocr-optimal-plan.md));**按用户要求,本计划不考虑内存消耗问题** |
| 基线 | HEAD `c4d1aeb`;工作区干净(仅 docs 未跟踪) |
| 核心思路 | OCR 推理**下沉到 `nbcore` 进程内**,复用既有 `nb_call` JSON 门面;**宿主 C++ 侧几乎零改动** |
| 性质 | **实施计划,未修改任何代码** |

---

## 1. 方案 B 的架构落点(已核实的接缝)

```
QML 四条入口(不变)
   |
   v
CoreFacade::processCapture(id, mode)      core_facade.cpp:380   <-- 唯一分派点
   |  AsyncCall{method:"ocr_entry"}        core_facade.cpp:210-224
   v
CoreBridge::call -> nb_call("ocr_entry")   <-- FFI 边界,签名不变
   |
   v
ffi.rs:783  "ocr_entry"                   <-- 读图 + 取设置 + 调识别
   |    decode_png_to_rgba(bytes)          ffi.rs:796(已有)
   v
ocr.rs:129  recognize_configured(...)      <-- 【本计划的主战场】
   |
   +-- Backend::System   -> system::recognize()   (现有 Windows OCR,原样迁入)
   +-- Backend::RapidOcr -> rapid::recognize()    (新增)
   +-- Backend::Auto     -> dispatch::recognize() (新增:兜底链)
```

**关键收益**:上图中 **C++ 侧那一整段(宿主)完全不动** —— 因为 `ocr_entry` 的入参出参都不变。08a 阶段 4 里宿主侧最易错的三件事(`QImage→BGR8` 行对齐、不占 `nb_call` 锁的线程编排、DLL 生命周期)**全部不需要做**。

## 2. 分阶段实施

### 阶段 B0 · 前置决策与合规(阻塞后续)

| # | 任务 | 产出 |
| --- | --- | --- |
| B0.1 | **澄清 `rapid-ocr-rs` 的来源与许可**:独立重写还是移植自上游?`COPYRIGHT` 只声明 Contributors,需确认与 RapidAI 的关系 | 合规结论(决定是"依赖它"还是"借鉴自研") |
| B0.2 | **定依赖形态**:直接 `path`/`git` 依赖,还是把 OCR 模块**移植进 `nbcore/src/ocr/`** | 技术选型 |
| B0.3 | **确认 `ort` 版本策略**:钉 `=2.0.0-rc.13`(与其一致)还是评估更新版 | `Cargo.toml` 决策 |
| B0.4 | **D6 定档**:确认 `auto` 为默认,并定义降级阈值(行数 < N) | 决策记录 |

> ⚠️ B0.1 是**硬阻塞**:若来源不明,合规上不能直接依赖,只能"借鉴思路自研"(工作量上升,但路线不变)。

### 阶段 B1 · 依赖落地与模型准备

| # | 任务 | 细节 |
| --- | --- | --- |
| B1.1 | 在 `core/Cargo.toml` 加 `ort` 与 OCR 依赖 | 注意 `workspace` 是 `edition 2021`,而 `rapid-ocr-rs` 是 `edition 2024` —— **不同 crate 可各自不同 edition,无冲突** |
| B1.2 | **裁剪特性**:只要 CPU provider | 关掉 `cuda-provider` / `directml-provider` / `remote-input` / `cli`;保留最小集 |
| B1.3 | **ONNX Runtime 链接方式** | 三选一(见 §3),建议先 `download-binaries` 便于本地跑通 |
| B1.4 | 模型入 `models/ocr/` | 采用 [08b §12.3](./08b-rapid-ocr-rs-analysis.md) 给出的 **ModelScope 直链 + SHA256 + `dict_url`**;记录 SHA256 |
| B1.5 | **模型路径策略** | ⚠️ 本项目是**便携式布局**(exe 同级),而 `rapid-ocr-rs` 默认去 `%LOCALAPPDATA%`。必须**显式传路径**(见 §4.1) |
| B1.6 | 复核 ModelScope `v3.6.0`/`v3.9.1` 路径当前有效性 | 下载并校验 SHA256 |

**DoD**:`cargo build --release` 通过;模型文件与 SHA256 校验通过;最小 demo 能识别一张图。

### 阶段 B2 · 核心内引擎封装(主战场)

建议的模块结构(把现有 `ocr.rs` 拆成目录):

```
core/nbcore/src/ocr/
  mod.rs        # 公开契约:Backend / OcrOptions / recognize_configured / probe / languages
  preprocess.rs # 从 ocr.rs 原样迁出:MAX_OCR_EDGE / effective_scale / apply_preprocess / otsu_binarize
  system.rs     # 现有 mod platform(ocr.rs:355-492)原样迁入 + recognize_with 的 Windows 分支
  rapid.rs      # 【新增】RapidOCR 引擎:加载、识别、生命周期
  dispatch.rs   # 【新增】auto 兜底链
```

| # | 任务 | 细节 |
| --- | --- | --- |
| B2.1 | **纯搬迁重构**(零行为变更) | 把 `ocr.rs` 按上表拆开,**先保证 237 个测试全绿** —— 这一步不改任何逻辑 |
| B2.2 | `rapid.rs`:引擎持有 | `struct RapidEngine { engine: Option<Inner> }`,懒加载 + 可释放(生命周期见 B2.5) |
| B2.3 | `rapid.rs`:RGBA → 引擎输入 | `nbcore` 现有路径给的是 **RGBA**(`decode_png_to_rgba`);需转成引擎要的格式。**注意 `RecImage::from_bgr_buffer` 这类入口在 `rapid-ocr-rs` 已存在** —— 若可直传则省一次转换 |
| B2.4 | `rapid.rs`:模型路径解析 | **必须显式指定**,不能依赖其默认 `%LOCALAPPDATA%` 行为(见 §4.1) |
| B2.5 | **session 生命周期** | 照抄 `snow-ocr-process/src/session.rs` 语义:**先 drop 再 load**、**失败不保留旧引擎**、**带等价单测**(见 §4.2) |
| B2.6 | **失败回退范式** | 参考 `worker.rs`:**真实推理失败时持久化负结果并当场回退**,而非只做一次性可用性探测(见 §4.3) |
| B2.7 | `dispatch.rs`:auto 兜底链 | 先内置;**空文本或行数 < 阈值 → 降级 RapidOCR 重试一次**;结果统一为 `String` |

**DoD**:三档样例(小字代码 / 中英混排 / 全屏截图)在本项目真实图片上跑通,结果与 08 §2 的 spike 结论一致。

### 阶段 B3 · 接线(核心侧 + 设置 + UI)

| # | 文件 | 任务 | 风险 |
| --- | --- | --- | --- |
| **B3.1** | `settings.rs` | ⚠️ **必做**:抽 `ocr_backend::is_supported()` 单一来源,`validate():508-515` 与 `repair_removed_ocr_backend():898-919` **共用**;值集合扩为 `system/windows/rapidocr/auto` | **P0**:漏改则"UI 选 RapidOCR 实际跑内置" |
| B3.2 | `ocr.rs:81-89` | `Backend::parse` 扩为识别新值(与 B3.1 同源) | 见 §4.4,不能照搬 08 §7.1 |
| B3.3 | `ffi.rs:779-782` | `ocr_probe` 接受 `backend` 参数(默认 `System`,**向后兼容**) | 旧调用方不传参行为不变 |
| B3.4 | `ffi.rs:798` | `recognize_configured` **签名不变**,内部自动走新分派 —— **此处零改动** | — |
| B3.5 | `SettingsDrawer.qml` | 后端下拉(用现成 `ReadableComboBox`,**model 必须纯字符串**)+ 可用性与原因;`save()` 显式赋值新字段 | `save()` 整份回读再回传,不显式赋值则不写入 |
| B3.6 | `SettingsDrawer.qml` | `ocrScale`/语言项**按引擎显隐** | 避免"设置项失效但 UI 仍显示" |
| B3.7 | — | 后端切换时**释放/重建**引擎(复用 B2.5 的 release 语义) | 与 `set_settings` 挂钩 |

**DoD**:设置页可切换三档;切换后重启仍生效(验证 B3.1 未被 repair 归零)。

### 阶段 B4 · 打包与发布

| # | 任务 |
| --- | --- |
| B4.1 | `package.ps1`:白名单加 `models\ocr\*`;`$must` 加模型关键件(缺件直接 `Die`) |
| B4.2 | **体积文案 5 处**改 `<90MB`:含 `package.ps1:161` 的**阈值常量**(漏改会每次刷 WARN) |
| B4.3 | `installer.nsi`:**无需改动**(递归段 `File /r` 自动带上 `models/ocr/`) |
| B4.4 | 若用 `download-binaries`:确认 `onnxruntime.dll` 随包部署;若静态链接则无此问题 |
| B4.5 | 随包附 LICENSE/NOTICE(rapid-ocr-rs + ONNX Runtime + PaddleOCR 模型) |

### 阶段 B5 · 测试与验收

沿用 08a §5 的矩阵,按方案 B 调整:

| 层 | 用例 | 判据 |
| --- | --- | --- |
| 单测 | 现有 **237 项全绿** | B2.1 纯搬迁后必须保持 |
| **单测(新)** | `rapidocr_value_survives_validate_and_repair` | `rapidocr`/`auto` 经 `validate()` 不报错、经 `repair` 不被改写 |
| **单测(新)** | session 生命周期(照抄 `session.rs` 的两个测试语义) | 先 drop 再 load;失败不留旧引擎 |
| **单测(新)** | `dispatch` 兜底链 | 内置返回空/行数不足时**确实调用**了 RapidOCR;充足时不调用 |
| 真机诊断 | `ocr_probe_backends` | 两引擎各自 available/语言如实上报 |
| 质量回归 | `ocr_quality_bench`(双引擎并排) | 三档样例不退 |
| **端到端(新)** | 设 `ocrBackend=rapidocr` → 右键识别文字 | 输出**确实来自 RapidOCR**(日志可证) |
| 端到端 | 四条入口行为与模式语义不变 | 剪贴板/预填/入库均正常 |
| 验收 | 仿 `runSettingsFlow` 加 OCR 断言(当前 acceptance **零 OCR 覆盖**) | `--settings-flow` 内读回 settings 断言 |
| 失败路径 | 删 `models/ocr/*` 后启动 | 应用**照常启动**,`system` 仍可用,设置页给可懂原因 |
| 体积 | `package.ps1` 第 6 步 | stage ≤90MB |

## 3. ONNX Runtime 的链接方式(需在 B1.3 定)

| 方式 | 特性 | 取舍 |
| --- | --- | --- |
| **动态加载**(推荐先用) | `ort/load-dynamic` + `download-binaries` | 最易跑通;`onnxruntime.dll` 随包;可分开发布 |
| **静态链接** | `ort-sys/disable-linking` + 自建 link manifest | 包体最优(无独立 DLL);但**需要 CMake 侧生成 link manifest**(参照 `snow-ocr-process/build.rs`) |
| 混合 | 开发期动态、发布期静态 | 成本略高但最实用 |

> 💡 `snow-ocr-process` 的做法可**直接照抄**:它用 `build.rs` 读环境变量 `SNOW_ONNXRUNTIME_LINK_FILE` 指向的 link manifest,逐行转成 `cargo:rustc-link-arg`。

## 4. 四个必须提前想清楚的技术点

### 4.1 ⚠️ 模型路径:不能用其默认行为

`rapid-ocr-rs` 的 `default_model_store_dir()` 默认去 `%LOCALAPPDATA%\rapid-ocr-rs\models`。**本项目是便携式布局**(exe 同级 `data/`),与之冲突。

**必须**:通过 `PipelineSources` 显式传入本地 `ModelSource::File` 与 `DictionarySource::File`(其 API 已支持,见 `model_source.rs`),或直接关掉 `model-download` 特性 —— **本项目要求全离线,不首次运行拉取**。

### 4.2 session 生命周期的正确语义(照抄已验证的实现)

```rust
pub struct Session<E> { engine: Option<E> }

pub fn prepare(&mut self, create: impl FnOnce() -> Option<E>) -> bool {
    self.release();          // 先释放旧引擎
    self.engine = create();
    self.engine.is_some()
}
```

**两个必须写单测锁定的语义**:
- **替换时先 drop 再 load** —— 避免新旧引擎同时驻留;
- **准备失败不得保留旧引擎** —— 否则会出现"以为换了模型其实没换"的静默错误。

### 4.3 失败回退:不要只做一次性可用性探测

`rapid-ocr-rs` 的 `worker.rs` 有一条重要教训:

> A provider can pass the inexpensive availability check and still fail while creating or executing a session.
> **Persist the negative result and retry this request on CPU** so one bad driver does not fail the OCR operation.

对本项目的等价应用:**模型加载/推理失败时,持久化"RapidOCR 不可用"并当场回退内置引擎**,而不是让这次识别直接失败。这恰好也是 `dispatch.rs` 兜底链要处理的情形之一。

### 4.4 ⚠️ `Backend::parse` 的"未知→System"必须与 B3.1 协调

08 §7.1 有意保留 "core 侧把未知值归为 `System`"。方案 B 下**推理就在 core 内**,这个设计**不再成立** ——
否则设 `ocrBackend=rapidocr` 时,`Backend::parse` 会把它归成 `System`,**静默跑内置引擎**。

**方案 B 下的正确做法**:`Backend::parse` 必须**认识** `rapidocr`/`auto`,且与 B3.1 的白名单**同源**。这是方案 B 相对方案 A 的一处**必须修正的设计差异**,不能照搬 08 §7.1。

## 5. 实施顺序与工作量估计

| 阶段 | 内容 | 依赖 | 相对 08a 的变化 |
| --- | --- | --- | --- |
| **B0** | 合规 + 依赖形态决策 | — | **新增**(方案 A 无此阻塞) |
| **B1** | 依赖 + 模型落地 | B0 | 简化(模型清单已有) |
| **B2** | 核心内引擎(含纯搬迁重构) | B1 | **替代** 08a 阶段 2/3(删掉 CMake/C ABI/跨 CRT 全部工作) |
| **B3** | 接线 | B2 | **大幅简化** 08a 阶段 4(宿主零改动) |
| **B4** | 打包 | B3 | 同 08a(仅模型白名单) |
| **B5** | 测试验收 | B4 | 同 08a + 新增 4 项 |

**相对 08a 被删掉的工作**(这些是方案 A 的独有成本):
- ❌ 写 CMake 子工程 + 剔 `main.cpp`/JNI + 修 include 路径 + 剔 printf 日志;
- ❌ 自研 C ABI 6 个导出 + 跨 CRT 内存所有权约定;
- ❌ 宿主 `LoadLibrary` / ABI 校验 / `ocr_engine.{hpp,cpp}` / `QImage→BGR8`;
- ❌ 宿主侧 session 生命周期与线程编排;
- ❌ `tools/` 下的最小 C++ 测试宿主。

**新增的工作**:
- ➕ B0 合规澄清;
- ➕ B2.1 `ocr.rs` 目录化搬迁重构(需谨慎,但纯机械);
- ➕ ONNX Runtime 链接方式的选择与配置。

## 6. 风险与回退

| 风险 | 影响 | 缓解 | 回退 |
| --- | --- | --- | --- |
| `rapid-ocr-rs` 来源/许可不明 | **阻塞 B0** | 先澄清;不行则"借鉴自研" | 退回方案 A/D |
| `ort` rc 版 API 变动 | 升级时返工 | 精确钉 `=2.0.0-rc.13`;隔离在 `rapid.rs` 内 | 锁版本不升级 |
| `ocr.rs` 搬迁引入回归 | 237 测试红 | **B2.1 作为独立提交,只搬不改** | 单独 revert 该提交 |
| 包体超 90MB | 发布红线 | 静态链需实测 | 改动态加载,或降级方案 D |
| `Backend::parse` 与白名单不同源 | 静默跑错引擎 | **B3.1 单一来源 + 专项单测** | — |

## 7. 建议的第一批动作(可直接开工)

1. **跑一次纯搬迁重构**(B2.1):把 `ocr.rs` 拆成 `ocr/` 目录,**不改逻辑**,确认 237 测试全绿;
2. **做一个最小验证 crate**:在 `core/` 下临时加 `rapid-ocr-rs` 依赖,用一张真实桌面截图跑通识别(验证依赖能编过、模型能加载、结果正确);
3. **同时用官方 CLI 跑同图**(08a §11 的工装),比对两者输出是否一致;
4. **澄清 B0.1 的合规问题**。

前三步**都不改主干行为**,失败可整体放弃。第 4 步是唯一的硬阻塞。

---

## 附:本计划的依据

| 结论 | 来源 |
| --- | --- |
| 接缝位置与行号 | 我实测 `ocr.rs` / `ffi.rs:783-804` / `core_facade.cpp:380` |
| `image 0.25` 无冲突 | 我实测 `nbcore/Cargo.toml:23` |
| session 生命周期语义 | `snow-ocr-process/src/session.rs`(08b §12.3 答案 3) |
| 失败回退范式 | `snow-ocr-process/src/worker.rs`(08b §12.3 答案 5) |
| 静态链接做法 | `snow-ocr-process/build.rs` + `tests/link_manifest.rs` |
| 模型直链/SHA256/dict_url | `rapid-ocr-rs/assets/default_models.yaml`(08b §12.3 答案 1) |
| 白名单耦合点 | `settings.rs:508-515` / `:898-919`(08a §1.2.3) |

---

# 11. 第一批动作的执行结果(2026-10-16)

> 按 §7 的"建议的第一批动作"执行。**步骤 1 已完成并验证;步骤 2 遇到硬阻塞(见 §11.2)。**

## 11.1 ✅ 步骤 1:B2.1 纯搬迁重构 —— 已完成

把 `core/nbcore/src/ocr.rs`(714 行)**拆分为目录**,只搬不改:

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `ocr/mod.rs` | 530 | 公开契约 + 原测试模块(10 个单测 + 5 个 ignored) |
| `ocr/preprocess.rs` | 115 | `MAX_OCR_EDGE` / `effective_scale` / `apply_preprocess` / `otsu_binarize` / `luma` |
| `ocr/system.rs` | 183 | `mod platform`(Windows OCR)+ `recognize_with` + `available_languages` |

### 验证结果(全部通过)

| 检查项 | 结果 |
| --- | --- |
| `cargo test --release -p nbcore --lib` | **237 passed; 0 failed; 5 ignored** —— 与基线**逐字一致** |
| `cargo build --release -p nbcore` | exit 0,`nbcore.dll` 重建正常(4,216,320 B) |
| `rustfmt` 三个新文件 | 干净(无 diff) |
| 外部引用 | 仅 `ffi.rs:780/798/802` 三处,全部原样有效 —— **零调用方改动** |

### 设计要点

- **对外契约完全不变**:[`recognize_configured`] / [`OcrOptions`] / [`Backend`] / [`probe`] / [`languages`] 签名与语义逐字保留;
- `system::recognize_with` 承接原 Windows 分支,`preprocess::apply_preprocess` 被 `system` 复用;
- 新增模块的文档注释已写明"这是方案 B 的准备步骤,后续加 `rapid.rs`/`dispatch.rs` 时对外契约不变"。

> ✅ 这一步达成了预期目标:**为新增 `rapid.rs`/`dispatch.rs` 腾出了干净的位置,且证明搬迁没有引入任何回归**。

## 11.2 🔴 步骤 2 遇到硬阻塞:本环境**无法获取新依赖**

步骤 2 要求在 `core/` 下加最小验证 crate 并引入 `ort`。**实测无法进行**,证据如下:

| # | 检查 | 结果 |
| --- | --- | --- |
| 1 | `cargo search ort` | ❌ `failed to retrieve search results from the registry at https://crates.io` / `SSL connect error (schannel: ...)` |
| 2 | 本地 `~/.cargo/registry/cache` | **空的**(0 个 `.crate` 文件) |
| 3 | 本地 `~/.cargo/registry/src` | 有 586 个已解压 crate,但 **`ort` / `ort-sys` / `ndarray` / `geo-clipper` / `imageproc` / `nalgebra` 全部缺失** |
| 4 | 现有依赖来源 | 仅 `rusqlite` / `image` 等既有依赖可从本地 src 解析(`--offline` 构建通过) |

**结论**:本项目现有依赖是**预先铺好的本地源**;任何**新增**依赖都需要联网拉取,而本环境网络不可达。

### 影响

| 步骤 | 能否在本环境执行 | 说明 |
| --- | --- | --- |
| 步骤 1 纯搬迁 | ✅ **已完成** | 不涉及新依赖 |
| 步骤 2 最小验证 crate | ❌ **阻塞** | 需要 `ort` 等 6 个新 crate |
| 步骤 3 CLI 对照 | ✅ **可执行** | `dist/ocr-spike/` 已有预编译 exe + 模型 |
| 步骤 4 合规澄清 | ⚠️ 需人工 | 非技术动作 |

### 解除条件(需在有网环境执行)

1. 在可联网的机器上执行 `cargo add ort@=2.0.0-rc.13 --dry-run` 验证可解析;
2. **或**预先 `cargo vendor` 出所需依赖树并拷入本机 `registry/src`;
3. **或**确认本机存在可用的私有/镜像 registry(当前 `~/.cargo/config.toml` **不存在**)。

> 💡 建议:步骤 2 在有网机器上做完后,**把 `Cargo.lock` 与 vendor 目录一并带回**,这样本环境后续才能继续推进。

## 11.3 建议的下一步

| 优先级 | 动作 | 前置 |
| --- | --- | --- |
| **1** | 在**有网机器**上跑步骤 2(最小验证 crate),回传 `Cargo.lock` / vendor | 需联网 |
| **2** | 本环境**立即执行步骤 3**(CLI 对照) | 无 —— `dist/ocr-spike/` 已有全部素材 |
| **3** | 澄清 B0.1 合规问题 | 需人工 |
| **4** | 步骤 1 的重构可先行提交(独立提交,便于单独 revert) | 建议 |

## 11.4 步骤 1 的改动清单(便于提交/回滚)

```
D  core/nbcore/src/ocr.rs                 (714 行,已删除)
?? core/nbcore/src/ocr/mod.rs             (530 行,新增)
?? core/nbcore/src/ocr/preprocess.rs      (115 行,新增)
?? core/nbcore/src/ocr/system.rs          (183 行,新增)
```

> 建议提交信息:`♻️ refactor(core): OCR 模块目录化(为接入 RapidOCR 引擎腾出位置)`,
> 正文说明"纯搬迁、零行为变更、237 测试全绿",并注明这是方案 B 的准备步骤。

## 11.5 ✅ 步骤 3:官方 CLI 对照 —— 已完成

用 `dist/ocr-spike/` 的预编译 CLI 重跑三个样例,**独立复核 08 §2 的基线**。

| 样例 | 本次 CLI 重跑 | 08 基线 | 结论 |
| --- | --- | --- | --- |
| code12(12px 代码) | `const int MAX_LIMIT = build_entry("1I100");` / `if (11 != I1) Count0 += 90;` | 完全相同 | ✅ 逐字一致 |
| mixed14(中英混排) | `会议纪要:Q3revenue增长12.5%,请在8月5日前提交review。` | 完全相同 | ✅ 逐字一致 |
| crop760(桌面框选) | `源代码管理` / `CMakeLists.txt` / `ToolConfirmDialog.qml...` | 一致 | ✅ |

**耗时对照(注意方差)**:

| 样例 | 08 基线 | 本次重跑 | 说明 |
| --- | --- | --- | --- |
| code12 | 193.2ms | **122.2ms** | ⚠️ 同参重跑差异明显(−37%) |
| mixed14 | 259.7ms | **154.5ms** | 同上 |
| crop760 | 1386.8ms | **1187.5ms** | 同上 |

> ⚠️ 这个差异**值得记录**:它印证了 08 §14 未验证项 4("每轮识别之间的时延噪声,spike 见同参重跑 193→225ms,未做 ≥6 轮,未定量")。
> 本次是**第三次**观察到同参重跑差异(193 → 225 → 122ms),**说明单次采样确实不可用作定量结论** —— 阶段 5 的 T5.8 必须按 §13 配对对照范式做多轮。

### 步骤 3 的产出

- ✅ 确认 CLI 在**本机可运行**,且**结果可复现**(文本层面逐字一致);
- ✅ 确认了"用 CLI 作对照基准"的可行性 —— 这正是 08a §11.4 / T2.9 要固化的工装;
- ⚠️ 定量耗时**不可信**(方差大),阶段 5 必须多轮采样。

---

## 11.7 ✅ 步骤 2:最小验证 crate(环境已修复,重新执行)—— 完成

> §11.2 记录的阻塞(无网、无 `ort` 依赖)**已由环境配置解决**。本节为**重新执行的实际结果**。

### 11.7.1 做了什么

新建 `core/nbocr-spike/`(独立 workspace 成员,**不参与发布,验证后整体删除**):

| 文件 | 内容 |
| --- | --- |
| `Cargo.toml` | `rapid-ocr-rs` path 依赖 + `default-features = false`(只要核心推理) |
| `src/main.rs` | 四步验证:ORT 初始化 → 模型齐备 → 建引擎 → 识别 |

### 11.7.2 ✅ 三个预定问题的答案

| # | 问题 | **答案** |
| --- | --- | --- |
| 1 | `ort` / `rapid-ocr-rs` 能否编译链接 | ✅ **能**。`cargo build -p nbocr-spike --release` **44.25s 全绿** |
| 2 | 模型能否加载 | ✅ **能**。引擎构建 331~372ms,四件模型从本地目录加载成功 |
| 3 | 识别结果是否与 CLI 一致 | ✅ **基本一致**(见 11.7.4) |

编译出的依赖树(实测):`ort 2.0.0-rc.13`、`ort-sys`、`ndarray 0.17.2`、`nalgebra 0.32/0.33`、`geo-clipper 0.9.0`、`imageproc 0.25.1`、`rapid-ocr-rs 0.7.0`。

### 11.7.3 🔴 遇到并解决的关键问题:ONNX Runtime 版本不兼容

首次运行**直接 panic**:

```
Failed to load ONNX Runtime dylib: BadVersion { version_str: "1.17.1", path: "onnxruntime.dll" }
```

**根因**(读源码定位):
- `C:\WINDOWS\system32\onnxruntime.dll` 为 **1.17.1**(随某软件分发),在 PATH 上被优先加载;
- `ort 2.0.0-rc.13` 要求 **ONNX Runtime 1.28+**(`ort/src/lib.rs:149-156` 比较 minor 版本);
- `rapid-ocr-rs` 的 `dynamic-onnx-runtime` 走 `ort/load-dynamic`,按 `ORT_DYLIB_PATH` → 可执行文件目录 → 系统 PATH 顺序查找。

**解法**:下载官方 ONNX Runtime **1.28.0**,用 `ORT_DYLIB_PATH` 显式指向:

```powershell
$env:ORT_DYLIB_PATH = '<repo>\dist\ort-1.28\onnxruntime-win-x64-1.28.0\lib\onnxruntime.dll'
```

> ⚠️ **必须记住的部署约束**:走动态加载时,发布包内**必须自带** ORT 1.28+ 的 DLL,
> **不能依赖系统 PATH** —— 用户机 system32 里可能有不兼容的旧版本(本机就有一个 1.17.1)。
> 这印证了 08a §B1.3 把"链接方式"列为独立决策项的必要性。

### 11.7.4 🎯 质量对照(本步骤最有价值的产出)

| 样例 | 官方 CLI | **Rust 引擎** | 判定 |
| --- | --- | --- | --- |
| **mixed14**(中英混排) | `会议纪要：Q3revenue增长12.5%，请在8月5日前提交review。` | **逐字相同** | ✅ **完全一致** |
| **crop760**(真实桌面) | 52 框 | **52 框** | ✅ 框数一致 |
| **code12**(12px 代码) | `build_entry("1I100")` / `if (11 != I1) Count0 += 90;` | `build_entry("1I10o")` / `if（1l!=I1）Count0+=90;` | ⚠️ **有差异** |

**差异集中在 code12(12px 极小字号等宽代码)**:零宽空格(U+200B)、全角括号 `if（`、空格丢失 `Count0+=90`。

**关键判断**:mixed14 与 crop760 **完全一致** → 这**不是系统性缺陷**,而是**极小字号下的边界差异**。

**已排除的原因**:
- ❌ 不是 padding —— 实测 CLI 用 `-p 0` 与 `-p 50` 输出**完全相同**;
- ❌ 不是字典 —— 已显式指定同一个 `ppocr_keys_v1.txt`;
- ⚠️ **可能是预处理后端**:本 crate 默认 `VisionBackend::PureRust`,CLI 用 OpenCV 的 resize —— **12px 对插值核最敏感**。→ M2 阶段用 `opencv-backend` 特性复测确认。

### 11.7.5 ⚠️ 性能:默认线程配置严重劣化(实测新发现)

| 配置 | code12 总耗时 | det 阶段 |
| --- | --- | --- |
| **默认**(auto_tune) | **~1400ms** | ~880~1337ms |
| threads=8 | 724ms | 629ms |
| **threads=4** | **~320ms** | ~260~330ms |
| threads=1 | 1026ms | 784ms |
| CLI 对照组(t=4) | **132~177ms** | 36~69ms |

**根因**:`RuntimeConfig::default()` 的 `auto_tune_threads = true` → `derive_runtime_threads()`(`runtime/session.rs:205-223`)→ `auto_tuned_thread_budget()` 用 `available.clamp(1, physical_cores)`,本机 **14 逻辑核 → 14 线程**,**严重过订阅**;实测 **4 线程最优**。

> 💡 **这对方案 B 是关键输入**:`nbocr` 在 `nb_call` 内串行执行,**绝不能让 OCR 占满 14 个线程**,
> 否则会与 Qt 宿主线程及 `nb_call` 的其它调用争抢 CPU。建议默认钉 `intra_threads = min(物理核, 4~8)`,纳入阶段 5 调参。

### 11.7.6 ✅ 结论与新增待办

> **方案 B 技术可行,证据充分**:
> 1. 依赖能编译、模型能加载、识别能跑通(**步骤 2 核心目标达成**);
> 2. 中英混排场景**结果与官方 CLI 逐字一致** —— 集成未引入语义偏差;
> 3. 两个**新发现**必须带入后续阶段。

**新增待办(来自本次实测)**:

| # | 待办 | 归属 |
| --- | --- | --- |
| **T-B1.7** | **ORT 链接方式定案**:动态加载须自带 DLL 且锁定版本;或将 ORT 静态链入 `nbcore.dll` 彻底消除 PATH 依赖 | B1 |
| **T-B2.8** | **默认线程数显式化**:`intra_threads = min(物理核, 4~8)`,禁用/复核 `auto_tune_threads` | B2 |
| **T-B2.9** | **12px 小字质量复测**:用 `opencv-backend` 特性对照 `PureRust`,确认 code12 差异是否由预处理后端引起 | B2 |
| **T-B2.10** | **零宽空格清洗**:若 RapidOCR 输出含 U+200B,需在写入前过滤(否则影响复制/搜索) | B2 |

### 11.7.7 🔴 自我更正:code12 差异的**真实根因**(码点级验证)

> §11.7.4 我曾写差异包含"**零宽空格 U+200B**"。**该结论是错的** —— 那是终端渲染造成的误读。
> 本次做**码点级比对**(把两侧输出逐字符转成 `U+XXXX`),结果如下。

**码点级实测(同一张 code12.png)**:

```
Rust 行1: if（1l!=I1）Count0+=90;
CP     : U+0069 U+0066 U+FF08 U+0031 U+006C U+0021 U+003D U+0049 U+0031 U+FF09 U+0043 ...
                     ^^^^^^ 全角左括号              ^^^^^^ 小写 l            ^^^^^^ 全角右括号

CLI  行1: if (11 != I1) Count0 += 90;
CP     : U+0069 U+0066 U+0020 U+0028 U+0031 U+0031 U+0020 ...
                              ^^^^^^ 半角 (         ^^^^^^ 数字 11
```

**真实差异只有三类**(无零宽空格):

| # | 差异 | 性质 |
| --- | --- | --- |
| 1 | 全角 `（`(U+FF08)vs 半角 `(`(U+0028) | **rec 识别歧义** |
| 2 | `l`(U+006C)vs `1`(U+0031)、`o` vs `0` | **rec 字形歧义**(12px 等宽字体固有) |
| 3 | `Count0+=90` vs `Count0 += 90`(**空格丢失**) | 见下方根因 |

#### 根因:两侧的 **det 检出框几何不同**

同口径(`-p 0`,无 padding)对比检出框:

| | 框 0(y 范围) | 框 1(y 范围) |
| --- | --- | --- |
| **CLI** | y=3..26(**高 23px**) | y=24..47 |
| **Rust** | y=7..20(**高 13px**) | y=28..42 |

**Rust 检出的框明显更"紧"**。这解释了空格丢失:行被裁得更紧后,词间空格的边缘像素被切掉,rec 阶段就预测不出空格符。

**为什么会不同**(读源码定位):

| 环节 | 官方 CLI(C++) | **RapidOCR Rust** |
| --- | --- | --- |
| 多边形偏移(unclip) | `clipper.cpp`(Pyclipper 的 C++ 移植) | **`geo-clipper 0.9.0`** — 一个**不同的**多边形偏移实现 |
| 代码注释自证 | — | `det/postprocess/mod.rs:1238`:"Pyclipper consumes integer coordinates. **Float points are truncated toward zero.**" |

即:该 Rust crate **试图模仿 pyclipper 的整数截断语义**,但底层用的是 `geo-clipper` 的 offset 算法 —— **两套实现不可能逐位一致**,在 12px 这种极小字号上差异被放大。

#### 影响评估(重要)

- ✅ **mixed14(中英混排)与 crop760(真实桌面)输出与官方 CLI 逐字一致** —— 说明该差异**在本项目主场景(14px 以上)不影响结果**;
- ⚠️ **仅在 12px 极小字号**显现,且**性质是"检框偏紧 → 边缘字符/空格丢失"**,不是崩溃或乱码;
- ⚠️ 因此 §11.7.5 提出的 T-B2.9("用 `opencv-backend` 复测")**方向错了** —— 问题在 **det 后处理的多边形偏移**,不在图像 resize 后端。

**修正后的待办**:

| # | 待办(替换原 T-B2.9) |
| --- | --- |
| **T-B2.9'** | **对齐 det 后处理参数**:把 Rust 侧 `unclip_ratio` / `box_thresh` / `thresh` 与 CLI 的 `-u 1.6 -o 0.3 -b 0.5` 逐项核对(已知默认值一致,**需确认 resize 策略也一致**:CLI 实测 `ScaleParam(sw:760,sh:80,dw:736,dh:64)` 把高 80 压到 64,需确认 Rust 侧同尺寸下的目标宽高计算逻辑) |
| **T-B2.10'** | **12px 场景实测取舍**:既然只影响极小字号,可用"本项目真实截图"评估是否可接受;若不可接受,再考虑替换 clipper 实现 |
| T-B2.10(保留) | 零宽空格清洗 —— **降级**:实测未出现 U+200B,但 `normalize_cjk_spaces` 式的**输出清洗**仍建议保留(防御性) |

> 📌 **方法论教训(值得记下)**:我此前根据**终端渲染**就断言"零宽空格",这是**未经验证的推断**。
> 本次改为**码点级 dump**(把每个字符转 `U+XXXX`)才得到可靠结论。
> 后续涉及"输出是否一致"的判断,**必须走码点级对比**,不能看终端。

#### 验证工装(可复现)

本次用了仓库外的独立 crate `D:\3project\rust\nbocr-verify`(故意放在 Nextboard 之外,**不污染工作区**),
功能:加载本地模型 → 输出**每个检出框坐标** + 文本,用于与 CLI 的 `TextBox[n]` 逐项对照。

---

## 11.8 ✅ B0.1 合规澄清 —— 已解决(不再是阻塞)

> 经用户确认 + **仓库元数据实证**:`rapid-ocr-rs` 是**独立重写**,不是上游 RapidOcrOnnx 的移植。

### 证据(可复核)

| 项 | 实测值 | 出处 |
| --- | --- | --- |
| 许可 | **Apache-2.0**(全文 202 行,非摘要) | `crates/rapid-ocr-rs/LICENSE` |
| 版权声明 | "The `crates/rapid-ocr-rs/` subtree is: **Copyright (C) 2026 rapid-ocr-rs Contributors**" —— 被独立标注的子树 | `snow-crates/COPYRIGHT:12-13` |
| SPDX | `SPDX-License-Identifier: Apache-2.0` | `snow-crates/COPYRIGHT:15` |
| 作者 / 仓库 | `mg-chao <chao@mgchao.top>` / `gitee.com/mg-chao/snow-shot-workspaces` | `rapid-ocr-rs/Cargo.toml:9-10` |
| 上游代码引用 | **源码零处**提及 `RapidOcrOnnx` / RapidAI Copyright / PaddlePaddle | 全 `src/**/*.rs` grep 无命中 |
| NOTICE 文件 | **不存在** —— 与"独立实现、无上游衍生"一致 | 目录列表 |

### 结论

| 问题 | 答案 |
| --- | --- |
| 能否直接依赖? | ✅ **能**。Apache-2.0 与本项目既有口径(RapidOcrOnnx / PaddleOCR 同为 Apache-2.0)一致 |
| 是否需保留上游 NOTICE? | **不需要**(无上游衍生代码) |
| 本项目需做什么? | 随包附 **`rapid-ocr-rs` 的 LICENSE + COPYRIGHT 归属声明**(Apache-2.0 §4 要求) |

### ⚠️ 但仍需**分别**处理另外两方的许可(与 rapid-ocr-rs 无关)

| 组件 | 许可 | 义务 |
| --- | --- | --- |
| **ONNX Runtime** | MIT | 随包附其 LICENSE(发布包内已含 `LICENSE` / `ThirdPartyNotices.txt`) |
| **PaddleOCR PP-OCRv4 模型**(四个 .onnx/txt) | Apache-2.0(PaddlePaddle/PaddleOCR) | 附 PaddleOCR 的 LICENSE + 说明模型来源 |

> 📌 归入 08a 阶段 6(T6.5)的合规收口,与本决策无关地成立。

### 对路线的影响

**B0.1 从"硬阻塞"降为"已关闭"** → 阶段 B1 解锁,可继续推进。

---

# 12. 决策记录(2026-10-16 用户拍板)

| # | 决策 | 值 | 相对原计划的变化 |
| --- | --- | --- | --- |
| **D1** | 引擎路线 | ✅ **正式改走 Rust 路线**(RapidOCR 引擎嵌入 `nbcore`) | 08 原 D1 是"C++ 宿主侧 DLL",**已修订** |
| **D6** | 引擎选择策略 | ✅ **不采用 `auto`** —— **由用户显式选择**引擎 | ⚠️ **简化**:`dispatch.rs` 与 `auto` 兜底链**取消** |
| **D7** | session 生命周期 | ✅ **B 档:空闲时销毁** | 定档,不再待定 |
| **B1.3** | ORT 链接方式 | ✅ **动态库链接,不依赖 PATH,走项目路径** | 消除 system32 旧版本风险 |
| **Q9/D3** | 模型档位 | ✅ **PP-OCRv4 mobile** | 维持 08 原 D3 |

## 12.1 D6 变化带来的简化(重要)

原计划(《08a》阶段 4、《10》阶段 B2)包含一个 `dispatch.rs`,
用于实现"内置引擎优先 → 结果为空/行数不足时降级 RapidOCR"的 `auto` 兜底链。

**用户选择"由用户显式选择引擎"后,该模块整个不需要了**:

| 原设计 | **修订后** |
| --- | --- |
| `Backend::{System, RapidOcr, Auto}` | `Backend::{System, RapidOcr}` —— **去掉 `Auto`** |
| `dispatch.rs`(兜底链调度) | ❌ **删除,不实现** |
| 设置页"三档下拉" | **两档下拉**(内置 / RapidOCR) |
| "自动降级"测试用例 | ❌ 不需要 |
| D6 阈值调参(阶段 5) | ❌ 不需要 |

**好处**:
- 行为**可预测**:用户选什么就是什么,不会出现"我选了内置却跑了 RapidOCR";
- 少一个模块、少一组测试、少一个调参项;
- 与 08 §7.1 "`Backend::parse` 未知值回退 System"的设计**更容易保持一致**(不需要再引入 `Auto` 这个中间态)。

**代价(需接受)**:全屏场景(内置引擎实测不可用)不会再"自动救命" ——
用户若设成内置引擎,全屏识别仍会像今天一样不可用。**这是有意的取舍**:把选择权交给用户。

## 12.2 对实施计划的影响

| 阶段 | 变化 |
| --- | --- |
| B0 | ✅ 已关闭(合规澄清) |
| **B1** | ✅ **已验证完成**(见 §13) |
| B2 | `rapid.rs` **保留**;**`dispatch.rs` 删除** |
| B3 | 后端下拉由三档改**两档**;`Backend::parse` 只加 `rapidocr` 一个值 |
| B5 | 删除"`auto` 兜底链"测试,新增"选的引擎与实际执行一致"测试 |

---

# 13. B1 · ONNX Runtime 链接方式 —— ✅ 已验证

## 13.1 决策:动态链接 + 显式项目路径

**要求**("不依赖 PATH,走项目路径")**已通过实测验证可行**。

## 13.2 为什么必须显式指定(根因)

读 `ort 2.0.0-rc.13` 源码找到了加载顺序(`src/lib.rs:224-234`):

```rust
let path: PathBuf = match std::env::var("ORT_DYLIB_PATH") {
    Ok(s) if !s.is_empty() => s,
    _ => "onnxruntime.dll".to_owned(),   // ← Windows 回退到裸名
};
```

**裸名 `onnxruntime.dll` 会走 Windows 系统搜索路径** → 命中 
`C:\WINDOWS\system32\onnxruntime.dll`(**本机实测 1.17.1**,某软件附带),
而 `ort rc.13` 要求 **1.28+** → 必然 `BadVersion` panic。

⚠️ 且 `rapid-ocr-rs` 的 `initialize_onnx_runtime()`(`lib.rs:42-78`)**只在 macOS 分支**做了
`init_from(current_exe/../libonnxruntime.dylib)` 的路径绑定,**Windows 分支直接用 `ort::init()`** ——
**即该 crate 自身在 Windows 上就是依赖 PATH 的**。这是必须由本项目补上的缺口。

## 13.3 验证结果(实测)

用仓库外的独立 crate(`D:\3project\rust\nbocr-verify`)验证三个场景:

| 场景 | 做法 | 结果 |
| --- | --- | --- |
| **① 显式路径绕开 PATH** | `ort::init_from(<项目路径>)`,且 **`ORT_DYLIB_PATH` 置空** | ✅ **成功**。引擎构建 + 识别通过,输出与 CLI 逐字一致 |
| ② DLL 路径不存在 | 传 `D:\nonexistent\onnxruntime.dll` | ✅ **返回可读错误**(非崩溃),可被调用方捕获 |
| ③ DLL 版本过旧 | 传 system32 的 1.17.1 | ✅ **返回 `BadVersion` 错误**(含版本号与路径),可被捕获 |

场景 ① 是决定性的:**在不依赖任何环境变量的前提下,仅靠项目内路径就加载成功** ——
若落在 system32 的 1.17.1 上,必然 panic。

## 13.4 落地形态(写入 B2 的实现要求)

```rust
// core/nbcore/src/ocr/rapid.rs 中的初始化(示意)
// ⚠️ 必须显式指定路径,不能依赖 ORT_DYLIB_PATH 或 PATH
let ort_dll = models_dir.parent()   // 例如 <安装目录>/
    .join("onnxruntime.dll");       // 与模型同级的运行期依赖
if !ort_dll.exists() {
    return Err(AppError::Internal("缺少 onnxruntime.dll,OCR 引擎不可用".into()));
}
ort::init_from(&ort_dll)?            // 失败(版本不符)也走这里,不 panic
    .with_telemetry(false)
    .commit();
```

**两条硬约束**:
1. **`ort::init_from` 必须在任何其它 `ort` API 之前调用**(源码注释明确要求),
   且**全局只能成功一次** → 需用 `OnceLock`/`Once` 包裹,避免重复初始化报错;
2. **不得设置 `ORT_DYLIB_PATH`** —— 那会引入环境依赖,与"不依赖 PATH"的要求相悖。

## 13.5 待办

| # | 事项 |
| --- | --- |
| **T-B1.8** | 定 `onnxruntime.dll` 的**发布位置**:与 `nbcore.dll` 同级?还是 `models/ocr/` 下?(需与 `package.ps1` 白名单、`$must` 断言同步) |
| **T-B1.9** | 把 ORT 1.28 的 DLL **入库或纳入发布依赖**,并在 04 §3 记录放置步骤 |
| **T-B1.10** | 随包附 ORT 的 LICENSE / ThirdPartyNotices(MIT) |

---

# 14. C1/C3 实现与端到端验证 —— ✅ 已完成

> 依 §12 的五项决策实现。**代码已改,未提交**(按要求)。

## 14.1 改动清单

| 文件 | 改动 |
| --- | --- |
| `core/nbcore/src/ocr/rapid.rs` | **新增**(约 290 行):RapidOCR 引擎封装 |
| `core/nbcore/src/ocr/mod.rs` | `Backend::RapidOcr` 变体 + 分派 + probe/languages |
| `core/nbcore/src/settings.rs` | **值集合单一来源**(`ocr_backend::is_supported`)+ 2 个防护测试 |
| `core/nbcore/src/core.rs` | `set_settings` 变更时释放 session |
| `core/nbcore/src/ffi.rs` | `ocr_probe` 加 `backend` 参数(**向后兼容**) |
| `core/nbcore/Cargo.toml` | 加 `rapid-ocr-rs` / `ort` 依赖 |

## 14.2 实现要点(对应决策)

| 决策 | 落实方式 |
| --- | --- |
| **D1** Rust 路线 | 引擎在 `nbcore` 内,`nb_call` 签名不变,宿主零改动 |
| **D6** 用户显式选择 | `Backend::{System, RapidOcr}` —— **无 `Auto`**,无 `dispatch.rs` |
| **D7** 空闲销毁 | `rapid.rs` 的 `SESSION`(Mutex)记录 `last_used`,超 `IDLE_TTL`(60s)置 `None`;`set_settings` 也会立即释放 |
| **B1.3** 动态库+项目路径 | `ort::init_from(<exe目录>/onnxruntime.dll)`,`Once` 包裹;缺件/版本不符都返回可读错误 |
| **D3** v4-mobile | 四件模型名常量化,位于 `<安装目录>/models/ocr/` |

## 14.3 两个防护测试(新增)

| 测试 | 作用 |
| --- | --- |
| `rapidocr_backend_survives_validate_and_repair` | ⚠️ **P0**:`rapidocr` 能过校验、**不被加载期归零** |
| `every_supported_backend_is_recognized_by_parse` | 白名单与 `Backend::parse` 口径一致(防静默回退 `System`) |

**已做反向验证**:临时把 `rapidocr` 从白名单移除 → 测试**立即变红**(2 failed)→ 证明测试**真的能抓住**这个回归,不是空跑。

## 14.4 ✅ 端到端验证(走完整 FFI)

用独立 driver(`D:\3project\rust\nbocr-e2e`,仓库外)经 `nb_init` → `save_settings` → `ocr_entry` 真实调用。

### 验证 1:设置不被静默归零

```
save_settings(ocrBackend=rapidocr) → rc=0
get_settings 读回               → ocrBackend = "rapidocr"   ✓ 未被 repair 改写
```

### 验证 2:`ocr_probe` 参数化且向后兼容

```
ocr_probe(不传参)   → {"backend":"system",  "available":true, "languages":["en-US","zh-Hans-CN"]}
ocr_probe(rapidocr) → {"backend":"rapidocr","available":true, "languages":[]}
```

旧调用方不传参时行为不变;另新增 `reason` 字段(不可用时给出可读原因,可用时省略)。

### 验证 3:⭐ 同一张图、两个后端输出明显不同(证明分派真实生效)

样例 `mixed14.png`(14px 中英混排),**走同一条 `ocr_entry` 代码路径**:

| 后端 | 识别结果 | 耗时(冷/热) |
| --- | --- | --- |
| `system`(内置) | `会 议 纪 要 ， Q3 revenue 增 长 125 ％ ， 请 在 8 月 5 日 前 提 交 revi ew.` | 45ms / 35ms |
| **`rapidocr`** | `会议纪要：Q3revenue增长12.5%，请在8月5日前提交review。` | 656ms / 494ms |

**这条对照的价值**:内置引擎把 `12.5%` 读成 `125 ％`、中文夹字间空格 —— **正是 08 §2 记录的缺陷**。
两个后端输出截然不同,证明**不是"两条路都走同一个引擎"**(那正是 08a §1.2.3 担心的静默错误)。
且 RapidOCR 的输出**与官方 CLI 逐字一致**(见 §11.7.4)。

### 验证 4:session 复用生效(D7)

同一后端连续两次识别:首次 656ms(含引擎冷启动)→ 第二次 494ms(复用 session),结果一致。

## 14.5 测试与质量

| 项 | 结果 |
| --- | --- |
| `cargo test --release -p nbcore --lib` | **239 passed; 0 failed**(237 基线 + 2 新增) |
| `rustfmt --check`(我改动的文件) | 干净 |
| `nbcore.dll` 体积 | 4.12MB → **5.07MB**(+0.95MB;ORT 动态链接,故大头不在 DLL 内) |

> ⚠️ `ffi.rs` 存在**仓库既有**的 fmt 偏差(行 70/242/285/429/579/681/846/861),
> 与本次改动(779-790)无关,未顺手修改以免扩大 diff。

## 14.6 ⚠️ 遗留待办(下一阶段前必须处理)

| # | 待办 | 严重度 |
| --- | --- | --- |
| **1** | **`rapid-ocr-rs` 依赖形态**:当前是**本地绝对路径** `C:/Users/15601/Desktop/...`,**换机不可构建** → 必须改 git 依赖或 vendor 入仓 | 🔴 高 |
| **2** | **`onnxruntime.dll` 未随包**:需定发布位置 + 写进 `package.ps1` 白名单与 `$must` | 🔴 高 |
| **3** | **模型未随包**:`models/ocr/` 四件同样需入发布包 | 🔴 高 |
| ~~**4**~~ | ~~**设置页无后端选择器**(B3.5)~~ | ✅ **已解决**:`SettingsDrawer.qml` 已有 `ocrBackendCombo`(带 objectName 验收钩子,走 `ocr_probe` 显示可用性)+ `ocrProfileCombo` + `ocrSessionCombo` |
| **5** | `ocr_scale`/语言项对 RapidOCR 语义分叉的设置页显隐(B3.6) | 🟡 中(见 §15.6 第 2 项的最新进度) |
| **6** | 12px 小字差异的取舍(T-B2.9') | 🟢 低 |

---

# 15. A 阶段:vendor 与可发布性 —— ✅ 已完成

> 目标:让功能**在开发机之外也能构建、也能跑**。

## 15.1 入仓资产(`thirdparty/`)

| 路径 | 内容 | 体积 |
| --- | --- | --- |
| `thirdparty/rapid-ocr-rs/` | OCR 引擎源码(52 文件,Apache-2.0) | 441 KB |
| `thirdparty/snow-memory/` | 其**唯一** path 依赖(6 文件) | 53 KB |
| `thirdparty/ort/onnxruntime.dll` | ONNX Runtime **1.28.0**(+ LICENSE / ThirdPartyNotices) | 15.08 MB |
| `thirdparty/models/ocr/` | PP-OCRv4 mobile 四件(det/cls/rec/keys) | 15.47 MB |
| **合计** | | **31.3 MB** |

**vendor 时做的两处改造**(否则脱离 snow-crates 工作区无法编译):

| 文件 | 改动 |
| --- | --- |
| `snow-memory/Cargo.toml` | 解开 workspace 继承:`license.workspace`→字面量;`serde/bincode/serde_json.workspace`→显式版本 |
| `rapid-ocr-rs/Cargo.toml` | `tempfile = { workspace = true }` → `"3"` |

> ⚠️ 上游 snow-crates 用 `[patch.crates-io]` 把 `ort`/`ort-sys` 指向 mg-chao 的 git fork;
> **本项目不采用该 patch**,直接用 crates.io 的 `=2.0.0-rc.13`(已实测通过)。此差异已在 Cargo.toml 注释中记录。

## 15.2 模型来源校验(交叉验证)

四个模型的 SHA256 **与 `rapid-ocr-rs` 官方 `default_models.yaml` 记录值逐字一致** —— 证明取得的是官方正品:

| 文件 | SHA256 |
| --- | --- |
| det | `d2a7720d...49da9` ✓ 与上游一致 |
| rec | `48fc40f2...3683b` ✓ |
| cls | `e47acedf...215c` ✓ |

## 15.3 构建链接入

### `app/CMakeLists.txt`

新增 OCR 资产部署。**关键决策:用 `add_custom_target(... ALL)` 而非 `POST_BUILD`** ——

> ⚠️ **实测发现的设计陷阱**:`POST_BUILD` 只在目标**被重新链接**时执行。
> 删掉 `models/` 后跑增量构建,ninja 报 `no work to do`,资产**不会被恢复**
>(`build.ninja` 里根本没有这条规则)。而这两个资产是**拷进来的输入**(非构建产物),
> 恰恰最容易被误删/过期。挂到 `ALL` 后每次构建都校验一次,代价仅一次 `copy_if_different`。

**判定测试(决定性)**:删掉 `models/` 与 `onnxruntime.dll` 后,**不改任何源码**直接跑 `cmake --build .`:

```
[1/1] Deploy OCR assets (onnxruntime.dll + models/ocr)
→ models=True  ort=True   ← 恢复成功
```

(旧方案在此场景会静默跳过,留下一个"能启动但 OCR 不可用"的构建。)

### `tools/package.ps1` 三处

| # | 改动 |
| --- | --- |
| 1 | 白名单加 `onnxruntime.dll` 与 `models\*` |
| 2 | `$must` 加三件(`onnxruntime.dll` / `ppocr_keys_v1.txt` / `ch_PP-OCRv4_det_infer.onnx`),缺件直接 `Die` |
| 3 | 体积目标 **60MB → 90MB**(**文案 + 阈值常量两处**,后者是:`-ge 90`) |

> ⚠️ **踩坑记录**:首次改写 `package.ps1` 时我用了 `[IO.File]::WriteAllText`,**丢掉了 UTF-8 BOM**,
> 导致 PowerShell 5.1 把中文注释解析成乱码、脚本整体语法错误 —— 而**该文件头部注释早就警告过这个坑**。
> 修正:写回时显式用 `New-Object System.Text.UTF8Encoding($true)`。已加语法校验(`Parser::ParseFile`)。

### 安装器:`tools/installer.nsi` **零改动**

递归段 `File /r "${SRC_DIR}\*"` 自动带上 `models\ocr\` 与 `onnxruntime.dll`(已核实全文件仅此一处 `File` 指令)。

## 15.4 ✅ 验收结果

| 项 | 结果 |
| --- | --- |
| `cargo test --release -p nbcore --lib` | **239 passed; 0 failed** |
| 打包 | ✅ 便携版 zip 42.6MB / 安装包 34.5MB,完整性 Medium |
| **发布包含 OCR 资产** | ✅ `onnxruntime.dll` + `models/ocr/` 四件均在 |
| **stage 体积** | **87.8 MB**(红线 90MB) |
| 文件数 | 148 |
| 测试残留 | 干净 ✓ |

### ⭐ 终极验证:在**真实发布包目录内**跑端到端

```
$ dist/Nextboard-0.2.0/nbocr-e2e.exe <sample> rapidocr
[1] 入库 OK
[2] ocrBackend = rapidocr
[3] ocr_entry 641ms(冷启动)
    会议纪要：Q3revenue增长12.5%，请在8月5日前提交review。
    项目进度三项已完成
[4] 第二次识别 422ms(session 复用)
```

即在**用户装完后的等价环境**里,依赖、模型、ORT 全部就位、识别正常。

## 15.5 ⚠️ 两个必须上报的风险

| # | 风险 | 说明 |
| --- | --- | --- |
| **1** | **体积余量仅 2.2MB** | 87.8 / 90MB。后续任何新增资产(尤其换 v5/v6 模型或 ORT 升级)都可能立即破线。**建议重新评估红线**,或考虑 ORT 静态链接/裁剪 |
| **2** | **`thirdparty/` 尚未 `git add`** | 当前是未跟踪状态(31.3MB)。**不提交则换机仍不可构建** —— "可发布性"只完成了一半 |

## 15.6 仍未做(不影响"可构建",但影响"可用")

| # | 待办 |
| --- | --- |
| ~~1~~ | ~~**设置页后端选择器**(B3.5)~~ → ✅ **已做**:见 `SettingsDrawer.qml` 的「文字识别」分组(后端 / 识别偏好 / 引擎驻留三个下拉) |
| 2 | `ocr_scale`/语言项对 RapidOCR 的显隐(B3.6) —— ⬜ 未做(语义已明确:`ocr/mod.rs` 注明 RapidOCR 自带 `max_side_len`、不消费 `ocr_scale`,中英混排一体不分语言;落地方式见下) |
| 3 | 12px 小字差异取舍(T-B2.9') |

---

# 16. 🔴 严重缺陷修复:识别后内存 860MB → 62MB

> **用户实测报告**:"识别文字后内存占用直接 800 多 MB"。本节能复现、能定位、已验证修复。

## 16.1 复现

用独立工装经**真实 FFI 路径**(`nb_call("ocr_entry")`)测量私有提交(Private Bytes):

```
[1] nb_init 后            私有     2.1 MB
[3] 入库后               私有     3.4 MB
[4] 第 1 次 ocr_entry 后  私有   594.9 MB   ← 用户报告的现象
[4] 第 2 次 ocr_entry 后  私有   860.3 MB
[4] 第 3 次 ocr_entry 后  私有   856.6 MB   ← 且不回落
```

## 16.2 定位过程(为什么不能靠猜)

关键对照:同样的图、同样的引擎,**在直接调用的独立进程里只用 80MB** ——
说明问题不在"引擎本身",而在**某处配置差异**。逐项排除后锁定:

| 变量 | 私有内存(全屏 1920x1080,150 行) |
| --- | --- |
| **`enable_cpu_mem_arena = false`** | **79 MB** ✅ |
| `enable_cpu_mem_arena = true`(默认) | **874 MB** 🔴 |

同一进程、同一张图、同一引擎,**仅这一个布尔值之差 = 11 倍内存**。

## 16.3 根因

`rapid-ocr-rs` 的 `RuntimeConfig::default()` 里 `enable_cpu_mem_arena: true`
(`config.rs:233`),它经 `provider.rs:48` 传给 ORT:

```rust
CPU::default().with_arena_allocator(enable_cpu_mem_arena).build()
```

ONNX Runtime 的 **CPU arena 分配器**会为每轮推理**保留并累积**中间张量;
全屏截图有 150 个文本框,rec 段逐框推理,累积量被放大到近 1 GB。

⚠️ **我在 `rapid.rs` 里从未设置过这个字段** —— 于是吃到了上游默认值 `true`。
这是"照抄默认配置"造成的缺陷:**默认值是为『多次推理复用缓冲』优化的,
而我们的语义是『单次识别完就还』**。

## 16.4 修复

`core/nbcore/src/ocr/rapid.rs` 的三处 runtime 配置里显式关闭:

```rust
rt.enable_cpu_mem_arena = false;
```

附了实测数字作为注释依据(避免后人"觉得没必要"又改回去)。

## 16.5 ✅ 修复验证(真实 FFI 路径)

| 阶段 | 修复前 | **修复后** |
| --- | --- | --- |
| 第 1 次识别后 | 594.9 MB | **62.2 MB** |
| 第 2 次识别后 | 860.3 MB | **65.7 MB** |
| 第 3 次识别后 | 856.6 MB | **62.0 MB** |
| 识别结果 | 150 行 | **150 行(无质量回归)** |

**两个改善**:
1. **内存降 14 倍**(860 → 62 MB);
2. **不再累积**:三次调用平稳在 62~66MB(修复前逐次爬升且不回落)——
   说明此前报告里的"不回落"现象**就是这个 arena 造成的**,不是独立问题。

| 其他检查 | 结果 |
| --- | --- |
| `cargo test --release -p nbcore --lib` | **239 passed** |
| 小图 mixed14 | 私有 72 MB,稳定 |

## 16.6 方法论教训

**这次是"实测 vs 假设"的典型对比**:

- 若只测**引擎本身**(独立进程),会得到"80MB,很正常"的错误结论;**
- 必须测**真实宿主路径**(FFI + 全屏图)才能复现;
- 而定位靠的是**控制变量**(只改 `arena` 一个布尔值),不是猜。

> 📌 另注:08 §10.1 曾预估 RapidOCR 瞬时 +95~233MB —— 实测**关闭 arena 后仅 +60MB**,
> 优于当时的预估。原预估是基于**未关闭 arena** 的口径。