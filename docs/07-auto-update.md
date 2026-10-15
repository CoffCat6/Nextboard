# 07 · 自动更新（S6.4「检查更新 + 打开下载页」）

| 项 | 内容 |
| --- | --- |
| 版本 | v1.0 |
| 日期 | 2026-10-15 |
| 状态 | **v1 已实施并实测**（2026-10-15）:core `check_update` + 宿主接线 + 设置页「关于」区 + `--update-flow` 环回六模式全绿。渠道地址仍**空置**（`NB_UPDATE_CHECK_URL` 留空即禁用） |
| 关联 | [03-roadmap.md](./03-roadmap.md) S6.4/S6.7 · [04-build-package-release.md](./04-build-package-release.md) §2.3/§5.3/§7.1–7.3/§8/§10.1/§11 · [05-todo.md](./05-todo.md)（确认卡/reveal 教训同源） |

---

## 1. 目标与非目标

### 1.1 v1 做

设置页「关于」区一个「检查更新」按钮 → 请求发布渠道 → 语义化版本比较 →
有新版则提示版本与发布时间,并提供**「打开下载页」**（交系统浏览器）。全程真实链路、可断言。

### 1.2 空置原则（本轮的关键约束）

发布渠道**尚未确定**（实测:`git remote` 为空;`dist/` 只有 0.2.0 的 Setup/zip/SHA256 三件;
04 §7.2 步骤 4/8「push tag + gh release」当前跑不通）。故检查地址**留空**落地代码:

- 空态下入口**可见但禁用**,状态行给出诚实说明;不发请求、不报错、不隐藏。
- 渠道定了**只改一行常量**并重编即启用 —— UI、FFI 契约、比较逻辑零改动。

### 1.3 v1 明确不做

静默下载、差量升级、应用内安装（04 §7.3 既定）;启动后台自动检查;托盘入口;
检查结果持久化。**自动更新服务端**同样不在范围（04 §1）。v2 方向见 §10。

---

## 2. 现状基线（逐条核实,含出处）

| 事实 | 出处 | 对方案的影响 |
| --- | --- | --- |
| 应用内**零**更新代码（全仓 `QNetworkAccessManager` 仅出现在 04 §7.3 文字里;无 `check_update`） | grep 实测 | 从零设计,但方向已有文档约束 |
| 宿主**已链接** `Qt6::Network`,部署集含 `Qt6Network.dll` | 链接命令行;04 §2.3 | 无论落点选谁都**不扩依赖面** |
| core 有统一 HTTP 传输:WinHTTP 主路 + ureq SOCKS5 补位、四段超时（`clamp(5,120)`）、4MB 响应上限、代理三态 | `translate/http.rs`:20/236/262/267;`translate/mod.rs`:8（`pub mod http`,跨模块可见） | 检查更新**直接复用**,不新写传输 |
| 复用样板就 3 行:`ProxyConfig::parse(&settings.proxy.mode, Some(&settings.proxy.manual))` → `http::send()` | `translate/mod.rs`:320/348（`proxy_probe` 即此路径） | 增量成本极低 |
| 版本单一来源:app **0.2.0**（CMake `project(... VERSION)` → `NB_APP_VERSION`）;core **0.1.0**（Cargo workspace） | `app/CMakeLists.txt`:3/128;`core/Cargo.toml`:7 | 比较只用 app 版本 |
| 安装器已具升级前提:`/S` 静默、安装段 `taskkill /F` 旧实例、覆盖安装保留 `data\` | `tools/installer.nsi`:91–115;04 §10.1 实测 | v2 闭环无需改安装器 |
| 错误契约固定:`{ok:false, error:{code,message}}`;变体集合 `Db/Json/Io/ClipboardWin/Image/Capture/Translate/Windows/InvalidInput/Internal/Panicked` | `error.rs`:36–56;`ffi.rs`:136 | **不新增变体**,复用 `Translate`/`InvalidInput` |
| core 请求 UA 仍是第一代产品名 `clipboard-plus/<CARGO_PKG_VERSION>` | `http.rs`:280 | D-g 顺手清理 |

---

## 3. 决策记录（本轮拍板）

| # | 事项 | 结论 | 理由与代价 |
| --- | --- | --- | --- |
| **D-a** | 实现落点 | **core 新增 FFI `check_update`**（本文**改判** 04 §7.3 的「app 侧 `QNetworkAccessManager`」） | ① 代理一致:QNAM **不会自动**使用应用内 `settings.proxy`,要手写 system/manual/SOCKS5 + 认证 + bypass 的翻译与接线,等于把 `http.rs` 已实测覆盖的部分在 C++ 再实现一遍,而 core 侧天然同一套;② 可单测:本仓**无 C++ 测试设施**,版本比较/解析放 Rust 才能表驱动覆盖;③ 线程模型现成:`AsyncCall` 线程池 + `*Finished` 信号与 backup/proxy 同型。**代价**:回填 04 §7.3 一句（§12 待办） |
| **D-b** | 空置常量位置 | **CMake 编译期** `NB_UPDATE_CHECK_URL`（默认空） | 渠道属**发布者**决策,不该做成用户可改设置(省一个设置项 + 校验 + 迁移);与 `NB_APP_VERSION` 同处,符合 04 §7.1 单一来源。验收另有 `--update-url` 覆盖(仅宿主注入) |
| **D-c** | 空态呈现 | 按钮**可见但禁用** + 说明文案 | 既不制造"点了没反应"的歧义,也不隐藏入口;填上常量后无需改 UI |
| D-d | 托盘入口 | 不做 | 托盘菜单已近 10 项;检查更新是低频动作,设置页足够 |
| D-e | 预发布版本 | 忽略 | tag 带 `-rc/-beta` 后缀视为预发布 → 不提示更新,避免把测试版推给用户 |
| D-f | 结果持久化 | 不做 | 不动 `app_config.json` schema;重开抽屉回到空闲态可接受 |
| D-g | UA 遗留 | 顺手改为 `nextboard/<CARGO_PKG_VERSION>` | 单测未断言 UA(实测核对 `http.rs` 测试只断言请求行/头/体);但需真机复核"按 UA 放行的企业代理"(§12) |

---

## 4. 架构与数据流

```
设置页「关于」[检查更新]
  → Core.checkUpdate()                                  (CoreFacade,UI 线程只做入队)
  → AsyncCall{ method:"check_update",
               req:{url, currentVersion} }               (QThreadPool,离 UI 线程)
  → nbcore::update::check()
       · url 为空 → 短路返回 {configured:false}(不发请求、不是错误)
       · translate::http::send(GET, 代理三态, 超时 8s)
       · 解析(GitHub latest / 自定义 manifest) → 语义化比较
  → emit updateFinished(QVariantMap) → QML 状态行/按钮切换
  → [打开下载页] → Core.openExternalUrl(url) → QDesktopServices::openUrl
```

**触点与预估行（估算,非实测）**

| 文件 | 动作 | 预估 |
| --- | --- | --- |
| `core/nbcore/src/update.rs` | 新增 `check()` + 解析 + 比较 + 单测 | ≈120 |
| `core/nbcore/src/lib.rs` | `mod update;` | 1 |
| `core/nbcore/src/ffi.rs` | dispatch 分支 `check_update` | ≈10 |
| `app/CMakeLists.txt` | `NB_UPDATE_CHECK_URL` 空常量 + 宏注入 | ≈6 |
| `app/src/main.cpp` | `--update-url` 解析;`--update-flow` 入隔离守卫 | ≈12 |
| `app/src/panel/core_facade.{hpp,cpp}` | `checkUpdate()` 异步 + `updateFinished` + `openExternalUrl` + `updateCheckUrl` 常量属性 + 验收降级标志 | ≈45 |
| `app/panel/qml/SettingsDrawer.qml` | 「关于」区按钮/状态行/objectName/reveal | ≈45 |
| `app/src/host/acceptance.{cpp,hpp}` | `runUpdateFlow`（STEP* 日志） | ≈70 |
| `tools/update_fixture.py` | 环回 fixture 服务（新版/已最新/失败） | ≈25 |

合计 ≈330 行。

**实测（2026-10-15 完成）**：代码与测试共 **799 行** —— `update.rs` 524（实现 305 / 单测 219）、
宿主 112（`core_facade` 88 + `main.cpp` 18 + CMake 6）、QML 96、验收流 49、`ffi`/`lib`/UA 18;
另加 `tools/update_fixture.py` 117 行与文档 366 行（本文 + 04）。
**330 行的估算偏低约 2.4 倍**，差在两处:把"契约与决策注释"当成零成本;单测按 8 例估、
实做 13 例（多出的都是实测逼出来的边界:空 URL 短路顺序、非 http 链接、UTF-8 截断、
404/非 JSON/两种形状都不认这三种失败）。

---

## 5. 空置常量与启用步骤（D-b）

```cmake
# app/CMakeLists.txt —— 与 project(... VERSION) 同一处管理
set(NB_UPDATE_CHECK_URL "" CACHE STRING "更新检查地址(留空 = 功能禁用)")
target_compile_definitions(nextboard PRIVATE NB_UPDATE_CHECK_URL="${NB_UPDATE_CHECK_URL}")
```

| 渠道 | 填法 |
| --- | --- |
| GitHub Releases（04 §8 D7 推荐） | `https://api.github.com/repos/<owner>/<repo>/releases/latest` |
| 自建静态 manifest | 任意 https 地址,如 `https://…/nextboard-update.json`（形状**按响应字段**识别,不依赖后缀） |

- **启用动作**:改这一行(或构建时 `-DNB_UPDATE_CHECK_URL=…`)→ `build.ps1` 重编。全项目**无第二处**配置。
- QML 用 `Core.updateCheckUrl === ""` 直接判定禁用态 —— 空态连一次请求都不会发出。
- 验收覆盖:`--update-url <url>` 由 `main.cpp` 解析后注入 `CoreFacade`,**只有宿主能设**,产品 UI 不暴露;`--update-flow` 同列入 04/`main.cpp` 的 `--data-dir` 兜底守卫。

---

## 6. FFI 契约

```jsonc
// 请求
{ "url": "https://api.github.com/repos/…/releases/latest", "currentVersion": "0.2.0" }

// 正常响应
{ "ok": true, "data": {
    "configured":   true,
    "hasUpdate":    true,
    "upToDate":     false,
    "currentVersion": "0.2.0",
    "latestVersion":  "0.3.0",
    "downloadUrl":    "https://github.com/…/releases/tag/v0.3.0",
    "title":          "v0.3.0",
    "notes":          "…(超 400 字截断)",
    "publishedAt":    "2026-10-20T09:00:00Z",
    "checkedAtMs":    1760000000000 } }

// 未配置(不是错误,不发请求)
{ "ok": true, "data": { "configured": false } }

// 失败(网络 / 解析 / 版本非法)
{ "ok": false, "error": { "code": "Translate", "message": "网络不可达或超时…" } }
```

- 非 2xx 一律 `ok:false` + 中文可读 `message` —— 不引入 `status` 枚举,避免状态爆炸与 UI 分叉。
- `error.code` 只用既有变体,归属看**问题出在哪一端**:远端/网络一侧(状态码、响应不是 JSON、
  缺 `tag_name`/`version`、远端 tag 写坏)归 `Translate`——它的 Display 是**裸文案**;
  本地入参(检查地址非 http(s)、`currentVersion` 无法解析)归 `InvalidInput`,Display 自带
  「无效参数:」前缀。⚠️ 这条规则是实测逼出来的:`--update-flow /garbage` 起初显示成
  「检查失败:无效参数:更新源返回了无法解析的内容」,双重前缀读起来像 bug。
- `publishedAt` 收敛为**展示用日期**(`YYYY-MM-DD`;解析不出就空串,UI 自动省略括号),
  不把整条 ISO 串丢给设置页。
- 宿主只在 `hasUpdate` 为真**且** `downloadUrl` 非空时渲染「打开下载页」;`notes`/`title` 缺失不影响判定。

---

## 7. 更新源形状与解析

| 形状 | 取用字段 | 说明 |
| --- | --- | --- |
| GitHub `releases/latest` | `tag_name`、`html_url`、`name`、`published_at`、`body` | **下载页取 `html_url`** —— 不再单独配置第二个链接,避免"检查地址与下载页不一致" |
| 自定义 manifest | `version`、`url`、`notes?`、`title?`、`date?` | `url` 缺失/非法 → 只提示版本,不出打开按钮 |
| `assets[]` | v1 **不解析** | 名字/大小/`browser_download_url` 已在响应里,v2 直接取用不必改协议 |

**渠道类型按响应字段判定**（有 `tag_name` → GitHub 形状;否则要求 `version`）。实测时把
原设计的"URL 以 `.json` 结尾"这条规则去掉了:后缀规则埋着"忘了写 `.json` 就静默按另一种
形状解析"的隐坑,而两个键名不会互相冒充。两种形状归一到同一内部结构（`Release`）再比较。

预发布**两种标记都认**:GitHub 的 `prerelease` 布尔字段,以及 tag 里的 `-rc/-beta` 后缀 ——
命中任一都不提示更新(D-e)。

⚠️ **安全约束**:打开的 URL 只接受 `http`/`https` scheme（与 `http::parse_url` 同族思路),
拒绝 `file:`/`javascript:` 等 —— manifest 内容来自发布者,但校验不能假设它可信;
宿主 `openExternalUrl` 再挡一道(它对 QML 通用,而 `openUrl` 走 shell 语义)。

---

## 8. 版本比较规则

- 剥前导 `v` 与空白;按 `.` 分点;逐段**按数字**比较 —— `0.10.0 > 0.9.0`,**字典序会判反**。
- 段数不足补 0:`0.2` == `0.2.0`。
- 段内非数字（`0.2.0a`）、空串 → 本地入参报 `InvalidInput`,**远端 tag** 报 `Translate`
  (归属规则见 §6),UI 原样显示。
- 预发布（D-e）:远端带 `prerelease=true` **或** tag 含 `-` 后缀 → 忽略,`hasUpdate=false`、`upToDate=true`,
  且**不出现**「打开下载页」(实测 `/prerelease` 模式:「已是最新版本 0.2.0」)。
- **只比 app 版本**（`Core.appVersion` = 0.2.0）。nbcore 版本节奏不同（0.1.0,实测两处不一致）,
  不参与比较;「关于」区照原样显示两行版本。

---

## 9. UI 规格与文案（关于区状态机）

| 状态 | 触发 | 按钮 | 状态行 |
| --- | --- | --- | --- |
| 1 未配置 | `Core.updateCheckUrl === ""` | 「检查更新」**disabled** | 次级色:「更新源未配置:发布渠道确定后在 CMake 填入检查地址」 |
| 2 空闲 | 有渠道、未检查/重开抽屉 | 「检查更新」可点 | 空 |
| 3 检查中 | 点击后 | 「检查中…」disabled（复用 `exportRunning` 型态） | 次级色:「正在检查…」 |
| 4 已最新 | `upToDate`（含被忽略的预发布） | 「检查更新」可点,**不出现**「打开下载页」 | 次级色:「已是最新版本 0.2.0」 |
| 5 发现新版 | `hasUpdate` | 「检查更新」+ **「打开下载页」**（secondary） | 强调色:「发现新版本 0.3.0（2026-10-20）」 |
| 6 失败 | `ok:false` | 「检查更新」可点 | `isError` 红字 + `WrapAnywhere`,透传 message |

- `objectName`:`updateCheckBtn` / `updateOpenPageBtn`（后者 `objectName: visible ? … : ""` ——
  隐藏期间仍带名字会让验收 DFS 把它当命中目标、点到别处,`clearDoneLink` 同教训）。
- 验收读回（`acceptanceState`）:`updateHintText` / `updateDownloadUrl` / `updateEnabled` /
  `updateNoteVisible`。后两项是**空置态的判据**(D-c 承诺"可见但禁用 + 说明"),
  实测 `UPDATE STEP2 … checkBtnEnabled: false 未配置说明可见: true`。
- 点击后 `revealItem(关于区块)`:⚠️ 沿用 05/04 实测教训 —— `visible` 刚变 true 时该块还没有真实
  y,必须 `Qt.callLater` + 120ms 定时器兜底,否则滚动位置错、点击落空（`--restore-flow` 当年抓出）。
- 隐私:请求是**公开 release JSON 的 GET**,不外发任何用户数据（仅版本号与 UA）;
  故不加常驻说明文案,保持关于区精简。

**失败文案映射**（core 统一措辞,UI 透传）

| 情形 | 文案 |
| --- | --- |
| HTTP 404 | 渠道还没有发布版本（404） |
| HTTP 403 / 限流 | 检查过于频繁或渠道限流,请稍后再试（GitHub 未认证按 IP 限流） |
| 连接失败/超时（如 `0x80072EE2`/`0x80072EFD`） | 网络不可达或超时,请检查网络/代理设置 |
| 代理拒凭据 407 | 沿用 `http.rs` 既有措辞 |
| 响应非 JSON / 字段缺失 | 更新源返回了无法解析的内容 |

---

## 10. v2 路线（本轮不实施,只预留契约）

1. 下载 `Nextboard-Setup-<ver>.exe` 到 `%TEMP%`;
2. SHA256 与渠道清单比对（`dist/Nextboard-<ver>-SHA256.txt` 已是既有产物,04 §10）;
3. `QProcess::startDetached(setup, {"/S"})` 后自身退出 —— 安装段已有 `taskkill /F`,
   覆盖安装保留 `data\`（04 §10.1 实测）;
4. 安装器 `MUI_FINISHPAGE_RUN` 把新版拉回。

**前置条件**:04 §8 D6 代码签名必须落定（未签名会撞 SmartScreen,下载来的文件还带
`Zone.Identifier`,更严）;04 §11.1 的完整性标签教训对"应用内启动的 exe"同样适用。

---

## 11. 验收标准（DoD）与测试矩阵

### 11.1 单测（Rust,表驱动;实测 13 例全绿）

| 用例 | 断言 |
| --- | --- |
| 比较规则（数字序） | `0.2.0<0.3.0`;相等;`0.10.0>0.9.0`（**字典序会判反**）;`1.2.3<2.0.0`;`v` 前缀;`0.2==0.2.0`;`0.2<0.2.1`;`0.3.0>0.2.9`;`""`/`v`/`0..1`/`0.2.0a`/`beta`/`1.0.0-` 全报错 |
| 版本号展示 | `strip_v` **仅在后面是数字时**剥 —— `version-1.2` 不被啃成 `ersion-1.2` |
| GitHub latest 形状 | tag/name/html_url/published_at/body 正确;`name` 缺失用版本号兜底;`tag_name` 空报错;`prerelease` 布尔与 `-rc` 后缀两种都认 |
| 自定义 manifest | version/url/notes 解析;`javascript:`/`file:///`/相对路径三种非法链接一律丢空(§7 安全约束) |
| UTF-8 截断 | 汉字按**字符**截(按字节切会在边界 panic);不超长时原样 |
| 日期提取 | ISO 与空格分隔都取 `YYYY-MM-DD`;`最新`、`2026-1-20T…` 返回空 |
| 空 URL 短路 | `configured:false` —— 且**故意传坏 `currentVersion` 仍返回 Ok**,证明判定早于解析、更早于任何网络请求 |
| 检查地址非法 | `file:///…`、`example.com/latest` 一律 `InvalidInput` |
| 环回端到端 ×4 | 复用 `translate::test_server` 真发请求:200→发现新版并核对请求行 `GET /latest` + `Accept` 头;`99.0.0-rc.1`→不提示;404/非 JSON/两种形状都不认 → 可读文案;远端 tag 写坏(`nightly-build`)→ 当场报错而非"已最新" |

### 11.2 端到端 `--update-flow`（真实点击 + 隔离数据目录;实测六模式全绿）

流程:点 `settingsButton` → 开抽屉 → `revealUpdateCheck` 滚到关于区 → 点 `updateCheckBtn`
→ 读回 `acceptanceState`(状态行/下载链接/按钮可用性/未配置说明) → 点 `updateOpenPageBtn`。

| 模式(fixture 路径) | fixture 内容 | 实测结果 |
| --- | --- | --- |
| 空置(不给 `--update-url`) | 不发请求 | `checkBtnEnabled: false`、`未配置说明可见: true`、状态行空、无下载链接 ✅ D-c |
| `/new` | `v99.0.0` + `html_url` | 「发现新版本 99.0.0（2026-10-20）」+ 按钮可用 → 点击后日志 `openExternalUrl(验收模式已降级…)` ✅ |
| `/same` | `v0.2.0` | 「已是最新版本 0.2.0」且**不出现**打开按钮 ✅(首轮曾错误出现,已修:链接只在 `hasUpdate` 时下发) |
| `/prerelease` | `v99.0.0-rc.1`、`prerelease:true` | 「已是最新版本 0.2.0」—— 更新的预发布不推给用户 ✅ D-e |
| `/manifest` | `{version:99.1.0,url,notes}` | 「发现新版本 99.1.0」(无日期自动省略)+ 链接正确 ✅ |
| `/notfound` | 404 | 「检查失败:渠道还没有发布版本（404）」✅ |
| `/garbage` | 200 + HTML | 「检查失败:更新源返回了无法解析的内容」✅(原为「检查失败:无效参数:…」双重前缀,已按 §6 归属规则改掉) |

fixture 侧同步打印 `REQ /new -> 200` 等行 —— 那是"客户端确实发出了请求"的独立证据
(空置态那一支根本不发请求,对应模式也就没有 REQ 行)。

⚠️ 验收模式把浏览器启动**降级为记日志**(否则每次跑都弹一个抢焦点的窗口,也没法反复跑);
真实打开由 §11.3 的人工冒烟覆盖。

运行形态（沿用既有验收约定,绝不写用户真实数据）:

```
python tools/update_fixture.py                    # 打印 "PORT <n>",按路径给六种响应
app/build/nextboard.exe --update-flow --data-dir <隔离目录> \
    --update-url http://127.0.0.1:<n>/new
grep -a "UPDATE STEP" <隔离目录>/nextboard.log
```

### 11.3 回归与发布前（本轮实测）

- `cargo test --release --workspace`:**236 通过 / 0 失败 / 5 ignored**,无警告;`build.ps1` 零警告 ✅
- `--settings-flow`(STEP1–12)、`--clear-all-flow`(counts 归零)、`--clear-done-flow`(doneLeft 0)、
  `--restore-flow`(STEP1–8 含幂等)全部重跑绿 ✅
- 真机冒烟（**待做**,发布前）:填真实渠道 URL → 真实浏览器打开下载页;
  干净机器（无 VS/Qt）装旧版后能检查出新版。

---

## 12. 风险与待办

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 渠道长期不定 | 入口常驻禁用态、功能空转 | 禁用态说明诚实到位 + 启用只需一行常量;不与"渠道定没定"耦合 |
| GitHub API 限流与境内可达性 | 检查失败被误读为"没有新版" | 失败一律 `ok:false` + 分级文案;保留自建 manifest 退路 |
| UA 改名影响按 UA 放行的企业代理 | 该环境下检查失败（低概率） | 真机复核;必要时把 UA 做成可配置（本轮不做） |
| 恶意/错误 manifest 诱导打开链接 | 用户被带到非预期站点 | 只接受 http(s);检查地址来自发布者填入的编译期常量,不来自用户输入 |
| 验收真弹浏览器 | 干扰桌面与断言 | 验收模式降级为日志（§11.2） |
| 与 04 §7.3 口径冲突 | 两份文档各说一种落点 | **已回填**（2026-10-15）:04 §7.3 改为 core 落点并链到本文,§8 新增 D12 记录改判。roadmap 6.4 表格无状态列,不改 |

---

## 13. 实施顺序（2026-10-15 四步全部完成）

| 步 | 内容 | 状态（实测） |
| --- | --- | --- |
| ① | core:`update.rs`（实现 305 行 + 单测 219 行）+ `lib.rs`/`ffi.rs` 接线 | ✅ `cargo test -p nbcore update::` 13 绿;全量 236 绿、无警告 |
| ② | 宿主:CMake 空置常量 + `CoreFacade`（`checkUpdate`/`updateFinished`/`openExternalUrl`/`updateCheckUrl`/验收降级）+ `main.cpp` 的 `--update-url`、`--update-flow` 与隔离守卫 | ✅ 构建零警告（过程中抓到 `QUrl::isHttp()` 在 Qt 6.9 并不存在,改为自己比 scheme） |
| ③ | UI:SettingsDrawer「关于」区按钮/状态行/`objectName`/reveal + 四个验收读回字段 | ✅ 六模式实测见 §11.2 |
| ④ | `--update-flow` + `tools/update_fixture.py` + 四条流回归 + 回填 04 | ✅ `--settings/--clear-all/--clear-done/--restore` 全绿;04 §7.3 改判 + §8 新增 D12 |

**两处实测逼出的改动**（设计时没写到,已落回本文）:已最新与被忽略的预发布**不该**出现
「打开下载页」(`/same`、`/prerelease` 抓到 → §6/§9);远端响应类错误改归 `Translate`,
消掉「检查失败:无效参数:…」双重前缀(`/garbage` 抓到 → §6)。

---

## 附录 A:更新源样例（示意,非实测抓取）

```jsonc
// GitHub Releases latest(节选) —— 检查地址指向 api.github.com/repos/<owner>/<repo>/releases/latest
{
  "tag_name": "v0.3.0",
  "name": "Nextboard 0.3.0",
  "html_url": "https://github.com/<owner>/<repo>/releases/tag/v0.3.0",
  "published_at": "2026-10-20T09:00:00Z",
  "body": "变更摘要…",
  "assets": [
    { "name": "Nextboard-Setup-0.3.0.exe", "size": 16678748,
      "browser_download_url": "https://github.com/…/Nextboard-Setup-0.3.0.exe" }
  ]
}

// 自建 manifest(URL 以 .json 结尾) —— 三字段最小集
{ "version": "0.3.0",
  "url": "https://<host>/nextboard/download",
  "notes": "本版变更摘要" }
```

## 附录 B:本轮核实过的关键坐标

`core/nbcore/src/translate/mod.rs`:8（`pub mod http` 可见性）/320/348（proxy_probe 复用样板）;
`translate/http.rs`:20（4MB 上限）/236（代理分派）/
262（WinHTTP 实现）/267（超时 clamp）/280（UA 常量）;
`settings.rs`:121–133（`proxy: ProxySettings`）;`error.rs`:36–56 + `ffi.rs`:136（错误契约）;
`app/CMakeLists.txt`:3/126–128（版本单一来源）;`core/Cargo.toml`:7（core 版本）;
`tools/installer.nsi`:91–115（`taskkill /F` + 覆盖安装）;04 §2.3/§7.1–7.3/§8/§10.1/§11。
