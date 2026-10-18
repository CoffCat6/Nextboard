# 06 内存收口修复设计

> 依据:2026-10-13 内存实测(22 组隔离实验,方法见 §0)。本文给**设计与建议改法**,
> 落地顺序见 §9。
>
> **实施状态(2026-10-14):§1–§7 已落地并复测**(详见文末"实施结果与回填数据")。
> 未落地:§3 阶段 2/3、§8.2 的产品决策项。
>
> **修订记录**
> - **第 4 版(实施版)**:§1–§7 全部实施完成;回填实测数据;新增 §11 实施结果;
>   记录实施中新发现的一个真实缺陷(冻结路径漏装载,见 §6.3 注)与一项既有问题
>   (遮罩窗口 QPainter 引擎错误,见 §10)。
> - **第 3 版**:补测**默认配置(硬件渲染)基线**——实测隐藏态 **231MB**,即
>   **开箱即用的默认配置本身已超预算 91MB**,升为 §1 首要项(此前完全未被识别)。
> - 第 2 版:第 1 版把"编辑器 +63.5/+95.5MB"当作编辑器成本,实为**遮罩会话与编辑器
>   重叠**的读数(测量方法缺陷,见 §0 坑 3 与 §2.1);并修正第 1 版 §5 方案不可行点、
>   补入遗漏的遮罩会话问题、修正预算口径。
>
> 数字口径:**私有字节**(PrivateUsage)为常驻预算口径;**工作集**(WorkingSet)
> 含共享页,只用于观察瞬态峰值。

## 0. 实测方法与基线

### 复现手段

```powershell
# 1. 隔离数据目录(复制真实 data/;--data-dir 会跳过单实例门禁)
$scratch = "D:\3project\rust\Nextboard\core\target\tmp\memtest"   # 见下方⚠️ 2
Copy-Item app\build\data\* $scratch -Recurse
# 2. 隐藏启动(如需切后端,先改 app_config.json 的 softwareRender)
app\build\nextboard.exe --hidden --data-dir $scratch
# 3. 采样:100ms 轮询 Get-Process 的 PrivateMemorySize64 / WorkingSet64 → CSV
# 4. 驱动:全局热键 Ctrl+Shift+V(面板)/ Ctrl+Shift+Q(全屏截图)/
#          Ctrl+Shift+A(框选)/ Ctrl+Shift+P(贴图)/ Ctrl+Shift+T(翻译页)
#          Esc(取消遮罩会话);鼠标拖拽用 SetCursorPos + mouse_event
# 5. 读数:<data-dir>\nextboard.log 的 mem[...] 行 + 采样 CSV
```

⚠️ 三个实测坑(第 1 版文档漏了第 2、3 条):

1. **配置文件不能带 BOM**:PowerShell 5.1 的 `Set-Content -Encoding UTF8` 会写 BOM,
   Rust 侧 `serde_json` 解析失败 → 静默回退默认设置(实测最坏 +84MB,见 §7)。
   ```powershell
   $cfg = [IO.File]::ReadAllText("$scratch\app_config.json")
   [IO.File]::WriteAllText("$scratch\app_config.json", $cfg, (New-Object Text.UTF8Encoding($false)))
   ```
2. **隔离目录别放 `%TEMP%`**:受限执行环境(沙箱/CI)下工作区内 exe 对 `%TEMP%`
   的写入被拒(`PermissionDenied`),数据目录与采样 CSV 都建不出来。
   放**工作区内**路径(`core\target\tmp\...`)任何环境都可用。
3. **`mem[]` 探针有位置陷阱**(第 1 版翻车根因):`logMemory("editor-open")`
   (`editor_window.cpp:234`)在**马赛克纹理(`:238-246`)与 `buildToolBar()`(`:266`)
   之前**触发,且此刻 `closeAllOverlays()` 的**异步销毁尚未完成** —— 该探针读到的是
   "遮罩未退 + 编辑器刚建"的混合值,**不是编辑器成本**;同理
   `logMemory("capture-start")`(`capture_session.cpp:200`)在遮罩创建**之前**。
   判读必须用**采样窗口峰值**(等状态稳定后取),或另加探针。

### 基线数据(软件渲染 = 当前 data 目录配置;3 屏:1755×1097 + 1080×1920 + 1920×1080)

| 场景 | 私有字节 | 说明 |
| --- | --- | --- |
| 启动后无输入(隐藏) | **87~90MB** | 平坦,无增长 |
| 列表显示态(稳态) | **122MB** | 含首屏 + 缩略图纹理 |
| 列表隐藏态(稳态) | **137MB** | 比显示态高 15MB(§6) |
| 首个显隐循环 | **+42MB 一次性** | ≈1.1MB/条目 × 38 条;空库仅 +5MB |
| 之后每个循环 | +0.5MB 并归零 | **非泄漏** |
| **遮罩(截图)会话** | **+77.5MB** | 87→164.5MB;会话结束才回落(§3) |
| 编辑器(0.26MB 图 / 4.3MB 图) | **+8MB / +16.6MB** | 采样峰值,§2 |
| 贴图每张(1755×1097 屏) | **+18.3MB** | = 图 7.7 + backing store 7.7 + 开销(§4) |
| 贴图 10 张 | **+189MB** | 第 11 张才触发数量淘汰 |
| 全屏截图入库 | 峰值 +24~33MB | 残留 0 |
| 框选入库(1500×950) | WS 峰值 **299.8MB** | 私有 +17MB,瞬态(§5) |
| 翻译页首次打开 | +49MB(旧日志) | 懒构建,backlog(§10) |

---

## 1. 渲染后端:默认配置(硬件 D3D11)本身即超预算(首要项)

### 1.1 现象与实测数据

`settings.rs:1317-1320` 断言 **默认 `software_render == false`**,即
**开箱即用的配置走硬件 D3D11**。同机、同数据目录、只改这一个开关的对照实测:

| 时点 | 软件渲染(`true`) | **硬件渲染(默认 `false`)** | 差 |
| --- | --- | --- | --- |
| panel-built(QML 树) | 71.6MB | **71.6MB** | **0** |
| panel-show(首帧) | 91.2MB | **146.4MB** | +55.2MB |
| startup | 86.2MB | **148.2MB** | +62.0MB |
| panel-hide(稳态) | 129.9MB | **230.9MB** | **+101.0MB** |
| 工作集峰值 | 189.4MB | **293.3MB** | +103.9MB |

**结论:默认配置下,主面板隐藏态的常驻就是 231MB——已超 140MB 预算 91MB,
且与贴图、编辑器、截图全无关系。** 这与 `progress.md`(2026-10-09)记录的
"差额全在首次渲染这一步:场景图 GPU 资源 + RHI(D3D11)设备/驱动/交换链/着色器,
约 70MB 工作集 / 65MB 私有"一致,本次复测确认,并且发现**稳态差距会进一步拉大**
(129.9 vs 230.9)。

`panel-built` 两边完全相等,证明这块开销**不是 QML 对象树**,纯粹是渲染后端。

### 1.2 根因(代码级)

```cpp
// app/src/main.cpp:176-182(启动时按设置决定后端;必须早于第一个 QQuickWindow)
    if (softwareBackend || softwareFromSettings) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qInfo() << "Qt Quick graphics API = Software" << (...);
    } else {
        qInfo() << "Qt Quick graphics API = Hardware (default/D3D11)";
    }
```

```rust
// core/nbcore/src/settings.rs:1317-1320(单测断言默认值)
        assert!(
            !settings.software_render,
            "默认硬件渲染（D3D11）；软件后端为可选项"
        );
// 默认值定义:settings.rs:377(Default 实现)  software_render: false,
```

即:硬件路径要建 RHI 设备、交换链、着色器缓存与场景图 GPU 资源;软件路径省掉这些,
改用 CPU 光栅(代价是滚动/动画吃 CPU)。当前"软件渲染"是设置里的**可选项**,
需要用户主动勾选才生效——所以**绝大多数用户跑在 231MB 的默认配置上**。

### 1.3 修复方案

**阶段 1(推荐,改默认值)**:软件渲染改为默认开启。

```rust
// core/nbcore/src/settings.rs:377(Default 实现)
-        software_render: false,
+        // 实测:硬件(D3D11)后端比软件(CPU 光栅)多 ~55MB 首帧 / ~101MB 稳态
+        // (231MB vs 130MB 隐藏态),是唯一"开箱即超预算"的来源。软件后端的代价
+        // 是滚动/动画吃 CPU —— 需过一遍 60fps 手感验收(见 §1.5 门槛)。
+        software_render: true,
```

```rust
// core/nbcore/src/settings.rs:1317-1320(断言同步改)
        assert!(
            settings.software_render,
            "默认软件渲染(省 ~100MB 稳态);硬件后端由设置项/--software 反向选择"
        );
```

⚠️ **改默认值会影响老用户吗?** 不会:`app_config.json` 里已存 `softwareRender`
字段的用户按自己的值走,只有配置缺失/新装用户吃新默认。
另需同步 `SettingsDrawer.qml` 里"软件渲染(省内存,重启生效)"的说明文案——
从"可选优化项"改为"默认开启,可关闭"(措辞改动,不是功能改动)。

**阶段 2(可选,保留开关)**:`--hardware` 命令行开关,便于 A/B 与排障
(与现有 `--software` 对称)。零风险,纯便利。

### 1.4 预期收益

| 项 | 修复前(默认硬件配置,实测) | 修复后(默认软件,实测) |
| --- | --- | --- |
| 隐藏态常驻 | **230.9MB** | **129.9MB**(省 **101.0MB**) |
| 列表显示态 | 166.8MB | 89.9MB(省 76.9MB) |
| 工作集峰值 | 293.3MB | 189.4MB(省 103.9MB) |
| 距 140MB 预算 | **超 91MB** | 达标(面板单独口径) |

**这是全部修复项中单项收益最大的一项**(101MB ≫ 其余各项之和),
且**改动量最小**(一个默认值 + 一条断言)。

#### CPU 代价实测(2026-10-14 补测)

用"20 次显隐(含图片列表渲染)"这一**与焦点无关、可复现**的负载对比两后端
(全局热键驱动,读数取进程 `TotalProcessorTime`):

| 后端 | 私有内存 | 20 次显隐 CPU 时间 | CPU/墙钟 |
| --- | --- | --- | --- |
| 硬件 D3D11 | **240.3MB** | 812.5ms | 3.9% |
| 软件 CPU 光栅 | **122.4MB** | 906.2ms | 4.4% |

即:**软件后端的 CPU 代价约 +0.5 个百分点**(绝对 94ms / 20 次 ≈ **每次呼出多 ~4.7ms**),
换来 **118MB** 常驻内存。两者不在一个量级。

### 1.5 影响面与风险

**先明确一件事:这个开关只影响一个界面。** 全工程只有一个 Qt Quick 表面 ——
主面板的 `QQuickWidget`(`panel_window.cpp:135`;翻译页与设置抽屉都已并入它,
见 `main.cpp:204` 的"不再有第二个 QQmlEngine")。而:

| 表面 | 类型 | 是否受影响 |
| --- | --- | --- |
| 主面板(历史/搜索/设置/翻译) | `QQuickWidget`(Qt Quick) | **受影响** |
| 贴图窗口 `PinWindow` | `QWidget` + `QPainter` | 不受影响 |
| 截图遮罩 `OverlayWidget` | `QWidget` + `QPainter` | 不受影响 |
| 截图编辑器 `EditorWindow` | `QWidget` + `QGraphicsView` | 不受影响 |

所以"切软件渲染"影响的只是主面板的绘制路径,贴图/遮罩/编辑器的绘制与性能完全一样。

**门槛:手感**(唯一放行条件)。需要真机确认的不是"能不能用",而是主观跟手度:

- 软件后端渲染画面**与硬件一致**(自截已确认列表/图片/计数/设置抽屉齐全);
- 已知代价是 CPU 光栅,但实测增量很小(见上表);
- 待人工确认项:大图列表滚动是否掉帧、抽屉滑入/卡片 hover/新条目淡入是否顺滑、
  遮罩框选是否跟手(遮罩本身走 QWidget 路径,两后端相同)。

**风险与回退**:软件后端不依赖 D3D11 驱动/交换链,在虚拟机、远程桌面、老显卡上
反而更稳(少一类"驱动相关黑屏/花屏"故障)。若主观手感不可接受,
把 `settings.rs` 的 `software_render` 改回 `false` 即可(单点开关,或启动时加
`--hardware`)——**不需要回滚任何其他改动**。

### 1.6 验证方法

```
对照实验(两次启动,只改 softwareRender):
  1) app_config.json 置 softwareRender=false → 启动 → 读 mem[startup] 与
     mem[panel-hide];预期 ~148MB / ~231MB
  2) 置 true → 再启动 → 预期 ~86MB / ~130MB
手感验收(阶段 1 的放行条件,必做):
  - 造 50 张图片条目,上下滚动多屏 → 目视/录屏确认无掉帧;
  - 设置抽屉滑入、卡片 hover、新条目淡入 → 动画顺滑;
  - 截图遮罩框选跟手(遮罩也走软件光栅)。
  两项都过 → 可以改默认值;任一项不过 → 维持可选,记录结论。
```

---

## 2. 编辑器:整图多份拷贝(实测 ≈2~3× 图 + ~7MB 固定)

### 2.1 现象与实测数据(第 1 版此处结论错误)

第 1 版引用 E1/E2 的 `mem[editor-open]` 增量 **+63.5MB(0.26MB 图)/ +95.5MB(5.7MB 图)**,
据此推出"6× 图尺寸 + ~50MB 固定"。**该结论不成立**,原因是 §0 坑 3:
`editor-open` 探针在遮罩异步销毁未完成、马赛克与工具栏尚未构建时触发。

重新实测(采样窗口峰值,遮罩已销毁、编辑器已构建完成):

| 组 | 选区 | 图尺寸 | 隐藏基线 | **编辑器窗口峰值** | **编辑器净增** | 关闭后残留 |
| --- | --- | --- | --- | --- | --- | --- |
| R1 | 300×220 | 0.26MB | 86.0MB | 94.0MB | **+8.0MB** | +5.3MB |
| R2 | 1300×830 | 4.3MB | 89.3MB | 105.9MB | **+16.6MB** | +1.8MB |

同批数据里遮罩会话读数是 163~166MB(§3),可见第 1 版把这块算进了编辑器。
两点线性拟合:**≈ 2.1× 图尺寸 + ~7.4MB 固定**;按代码计数稳态 ≈ 3× 图。
编辑器**不是**内存大头;关闭后回落干净(残留 2~5MB 为堆增长,非泄漏)。

### 2.2 根因(代码级)

`app/src/editor/editor_window.cpp` 构造函数(:220)起,一份裁剪图被复制多份:

```cpp
// :229 背景项:QPixmap::fromImage 是整图拷贝(QPixmap 为光栅对象,不与 QImage 共享)
auto* background = new QGraphicsPixmapItem(QPixmap::fromImage(m_base));

// :236-246 马赛克纹理:开编辑器就建,与用不用马赛克无关
const QImage pixelated = m_base.scaled(smallW, smallH, ...)   // 中间图(整图,构造期临时)
                              .scaled(m_base.size(), ...);      // 又是整图(临时)
m_mosaicSource = QPixmap::fromImage(pixelated);               // 常驻(整图)
```

稳态同时持有的整图缓冲:`m_base`(1)+ 背景 QPixmap(1)+ 马赛克 QPixmap(1)= **3×**;
构造马赛克时另有 2 份整图临时;`compose()`(:648)在提交/另存时再产生 1 份输出。
实测斜率 2.1× 低于计数模型 3×,推测是临时缓冲错峰释放,估算取"2~3×"区间。

对照证据:模糊纹理已是懒构建(`setTool` 内 `:393-400` 首次用模糊才生成),
马赛克没有照抄这个模式——是遗漏而非设计。

### 2.3 修复方案

**阶段 1(零风险)**:马赛克纹理改懒构建,完全镜像模糊的现有写法。

```cpp
// before:editor_window.cpp:236-246(构造函数内,整块删除)
    // 马赛克底图:先缩到 1/kMosaicBlock 再放大回来 = 像素化(FastTransformation
    // 保证取整不插值),作为马赛克图元的纹理刷子按路径"揭示"。
    {
        const int smallW = qMax(1, m_base.width() / kMosaicBlock);
        const int smallH = qMax(1, m_base.height() / kMosaicBlock);
        const QImage pixelated = m_base.scaled(smallW, smallH, Qt::IgnoreAspectRatio,
                                               Qt::FastTransformation)
                                     .scaled(m_base.size(), Qt::IgnoreAspectRatio,
                                             Qt::FastTransformation);
        m_mosaicSource = QPixmap::fromImage(pixelated);
    }

// after:setTool(:386)内,紧挨模糊纹理那段(:393-401)之前
    // 马赛克纹理惰性生成:不用马赛克工具的图不必付这份全尺寸拷贝
    // (构造时就建 = 每个编辑器会话都白付,且带一次 2× 整图的构造期临时峰值)。
    if (tool == EditorTool::Mosaic && m_mosaicSource.isNull()) {
        const int smallW = qMax(1, m_base.width() / kMosaicBlock);
        const int smallH = qMax(1, m_base.height() / kMosaicBlock);
        const QImage pixelated = m_base.scaled(smallW, smallH, Qt::IgnoreAspectRatio,
                                               Qt::FastTransformation)
                                     .scaled(m_base.size(), Qt::IgnoreAspectRatio,
                                             Qt::FastTransformation);
        m_mosaicSource = QPixmap::fromImage(pixelated);
    }
```

`editor_window.hpp:104` 注释同步改为"首次用马赛克工具时惰性生成"
(与 `:105` 模糊纹理注释对齐)。用法点 `:551`(`QBrush(m_mosaicSource)`)
必然发生在 `setTool(Mosaic)` 之后,无需判空。

**阶段 2(低风险)**:背景项改自绘 QImage,去掉 `QPixmap::fromImage` 整图拷贝。

```cpp
// after:editor_window.cpp 匿名 namespace 内(建议放在 EditorCanvas 之前)
/// 底图背景项:直接持 QImage 自绘,免去 QPixmap::fromImage 的整图拷贝
/// (QPixmap 是光栅对象,convert 时必然复制;QImage 与 m_base 隐式共享)。
class ImageBackgroundItem : public QGraphicsItem {
public:
    explicit ImageBackgroundItem(const QImage& image) : m_image(image) {}
    QRectF boundingRect() const override {
        return QRectF(QPointF(0, 0), m_image.size());
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->drawImage(QPointF(0, 0), m_image);
    }
private:
    QImage m_image;   // 与 m_base 隐式共享,不复制
};

// before::229-231
auto* background = new QGraphicsPixmapItem(QPixmap::fromImage(m_base));
background->setPos(0, 0);
m_scene->addItem(background);
// after:
auto* background = new ImageBackgroundItem(m_base);
background->setPos(0, 0);
m_scene->addItem(background);
```

`compose()`(:648)的输出份是功能必需(提交/另存都要整图),**不动**。

> 顺带修测量:`editor-open` 探针建议移到构造函数末尾(`buildToolBar()` 之后),
> 否则读数永远偏低且混杂遮罩残留;也可保留 `:234`、另加 `editor-ready` 探针。

### 2.4 预期收益

| 项 | 修复前 | 阶段 1 后 | 阶段 2 后 | 口径 |
| --- | --- | --- | --- | --- |
| 稳态整图缓冲份数 | 3× | 2× | 1× | 代码计数 |
| 编辑器净增(4.3MB 图) | **+16.6MB** | ~+12MB | ~+8MB | 实测→估算 |
| 编辑器净增(0.26MB 图) | **+8.0MB** | ~+8MB | ~+8MB | 实测→估算(固定成本为主) |
| 4K 裁剪(33MB)编辑器 | ~76~106MB | ~43~73MB | ~10~40MB | 估算(两点外推) |
| 4K 马赛克构造瞬时峰值 | +66MB | 0(不用马赛克时) | 0 | 估算(2× 整图临时) |

第 1 版"4K 编辑器 255MB → 149MB"高估了编辑器规模(把遮罩算进来了)。
修正后 4K 编辑器约 76~106MB,阶段 1+2 降到 ~10~40MB——相对收益比例更大(省 2/3),
绝对量级小一档;真正的大头是 §3 遮罩会话与 §1 渲染后端。

### 2.5 风险与 UX 影响

- 阶段 1:**零影响**。首次切到马赛克工具时多一次 ~10ms 缩放(与模糊工具现状一致)。
- 阶段 2:低风险。渲染路径不同,需自截确认画面一致(缩放、透明区域、标注叠加);
  repaint 每次从 QImage 光栅化,理论略慢,但编辑器 repaint 只在拖标注时发生。
  **有差异即回退阶段 2,阶段 1 独立成立。**

### 2.6 验证方法

复跑编辑器实验(配置 `regionSkipEditor=false`;用采样窗口峰值判读,**不要用 `mem[editor-open]`**):

```powershell
$cfg = [IO.File]::ReadAllText("$scratch\app_config.json")
$cfg = $cfg.Replace('"regionSkipEditor": true', '"regionSkipEditor": false')
[IO.File]::WriteAllText("$scratch\app_config.json", $cfg, (New-Object Text.UTF8Encoding($false)))
# 启动 → Ctrl+Shift+A → 拖选 → 等 4s(遮罩销毁 + 编辑器构建完成)→ 取采样峰值 → Alt+F4
```

预期:R1/R2 的"编辑器窗口峰值 − 隐藏基线"从 +8.0/+16.6MB 降到阶段 1 后估算值;
`mem[editor-closed]` 仍回落到基线 ±5MB。切马赛克画一笔确认涂抹正常,撤销无残留。

---

## 3. 遮罩(截图)会话:+77.5MB,会话期常驻(第 1 版遗漏)

### 3.1 现象与实测数据

实验 S/S2(3 屏,`Ctrl+Shift+A` 开框选遮罩后不操作,100ms 采样):

```
基线(隐藏) 87.0MB → 遮罩会话 164.0~164.5MB   ⇒ 会话净增 +77.5MB
会话保持 15s 无操作:稳定在 164.0MB(不自行回落)
```

会话真正结束时回落干净:实验 C2 的 `mem[capture-end]` 后读数 158.8 → 94.8MB,
与基线(94.0MB)持平——**不是泄漏,是"会话开着就常驻"**。

### 3.2 根因(代码级)

每屏一块全屏遮罩,各自持本屏冻结底图(`capture_session.cpp:270-281`,
`OverlayWidget::m_frozen`),冻结底图由 core 捕获后经零拷贝 RGBA 通道交给 QImage
(`core_bridge.cpp:185-201`):

| 屏 | 冻结底图(RGBA) |
| --- | --- |
| DISPLAY1 1755×1097 | 7.7MB |
| DISPLAY2 1080×1920 | 8.3MB |
| DISPLAY3 1920×1080 | 8.3MB |
| 小计(仅底图) | **24.3MB** |

实测多出 53.2MB(77.5 − 24.3),来自:3 个全屏置顶窗口在**软件渲染下的 backing store**
(各自 ≈ 屏尺寸×4,合计 24.3MB)、遮罩淡出层/选区绘制的合成缓冲、
以及每次重绘的临时缓冲(QPainter 在软件光栅下画全屏半透明层需要额外缓冲)。
遮罩存活期由 `overlayIdleSecs` 控制,**默认 600s(10 分钟)**
(`settings.rs:375` 默认值;`capture_session.cpp:259-269` 读取,`:331` 起计时)。

### 3.3 修复方案

**阶段 1(推荐,改默认值)**:空闲超时默认 600s → **60s**。

```rust
// core/nbcore/src/settings.rs:375(Default 实现内)
-        overlay_idle_secs: 600,
+        // 遮罩会话实测常驻 +77.5MB(3 屏),而"误触发热键后走开"是常见场景;
+        // 10 分钟太长,60s 足够完成框选,超时自动取消并回收(与手动 Esc 同语义)。
+        overlay_idle_secs: 60,
```

⚠️ **必须同步改断言**:`settings.rs:1313-1316` 断言默认值是 600:

```rust
// before
        assert_eq!(
            settings.overlay_idle_secs, 600,
            "遮罩空闲超时默认 10 分钟（0 = 不自动取消）"
        );
// after
        assert_eq!(
            settings.overlay_idle_secs, 60,
            "遮罩空闲超时默认 1 分钟（0 = 不自动取消）"
        );
```

设置项与 UI(设置抽屉的输入框)已存在,老用户配置不受影响。

**阶段 2(可选,改销毁时机)**:确认/取消后立即进入销毁队列。

```cpp
// capture_session.cpp:66-73(closeAllOverlays 内)
-        overlay->close();
+        // close 只隐藏,真正删除要等事件循环;显式 deleteLater 让销毁意图明确,
+        // 也便于将来排障(会话内存 +77.5MB 全压在这些窗口上)。
+        overlay->close();
+        overlay->deleteLater();
```

**阶段 3(可选,收益最大但需 UX 决策)**:多屏时只捕获**光标所在屏**的底图,
其余屏在鼠标首次进入时才捕获(懒捕获)。收益:会话从 +77.5MB 降到 ~+26MB;
代价:鼠标移入未捕获屏时有一次捕获延迟(实测量级 ~30ms,需真机确认无感知)。

### 3.4 预期收益

| 项 | 修复前(实测) | 阶段 1 | 阶段 2 | 阶段 3 |
| --- | --- | --- | --- | --- |
| 会话期常驻 | **+77.5MB** | 不变 | 不变 | **~+26MB** |
| 会话最长持有时间 | 600s | **60s** | 60s | 60s |
| 结束到回收延迟 | 异步排队 | 略短 | 更短 | 同 |

阶段 1 的价值不是省内存,而是**把"最长 10 分钟的 +77.5MB"压到 1 分钟**——
最划算的一步(改一个默认值 + 一条断言)。阶段 3 才真正减半,需 UX 确认。

### 3.5 风险与 UX 影响

- 阶段 1:正常框选 2~3s,60s 余量充足;超时自动取消已有既有提示逻辑。
  **唯一可感变化**:发呆超过 1 分钟会看到会话自动取消。担心的话取 120s 更保守。
- 阶段 2:零感知(只是提前回收)。
- 阶段 3:有感知风险(跨屏延迟),需 UX 评审;不做也不影响前两阶段。

### 3.6 验证方法

```
复跑实验 S/S2:Ctrl+Shift+A 后不操作,读采样峰值(遮罩期)。
预期:阶段 1 后 60s(或设定值)自动取消、内存回落到基线 ±5MB;
      阶段 3 后会话峰值从 164MB 降到 ~113MB(基线 87 + ~26)。
回归:框选/全屏/截图翻译/截图取字四条链路正常;跨屏框选(在其他屏拖选)正常。
```

---

## 4. 贴图:字节预算漏算窗口 backing store(实际成本 ~2×)

### 4.1 现象与实测数据

实验 D(1755×1097 屏,贴图 ×12,每张 +18.3MB 私有):

| 张数 | 私有增量 | 淘汰 |
| --- | --- | --- |
| 1–10 | 每张 +18.3MB(累计 +189MB) | 未触发 |
| 11–12 | 持平 274.2MB | `pin-add(evicted)` |

图片本身 1755×1097×4 = **7.7MB**,实测 18.3MB/张 —— 差额是软件渲染下每个置顶窗口的
**backing store**(窗口物理尺寸 ×4,约等于图尺寸)加窗口/工具条开销。

### 4.2 根因(代码级)

```cpp
// app/src/pin/pin_window.hpp:60-61
/// 当前显示位图占用的字节数(贴图淘汰的字节预算据此计算)。
qint64 imageBytes() const { return m_image.sizeInBytes(); }   // 只算图片

// app/src/panel/core_facade.cpp:398-403
constexpr qint64 kPinByteBudget = 96LL * 1024 * 1024;   // 只按 imageBytes() 累加
```

淘汰循环(`core_facade.cpp:425-440`)用 `imageBytes()` 累加,窗口 backing store
完全没进账。1755×1097 屏上 10 张 = 77MB(图)< 96MB,数量上限(10 张)先触发,
真实占用却 154MB+。4K 屏(33MB/张)预算会触发,但同样低估约 2×。

### 4.3 修复方案

把"图片字节"升级为"这张贴图真实占用"(图片 + 窗口 backing store):

```cpp
// after:app/src/pin/pin_window.hpp(替换 :60-61;currentDpr 已在其私有区 :104)
/// 这张贴图的**真实内存占用**:图片 + 软件渲染下置顶窗口的 backing store
/// (≈ 窗口物理尺寸 ×4)。淘汰预算据此计算 —— 只算图片会低估一半
/// (实测 1755×1097 图 7.7MB/张,实际 +18.3MB/张)。
qint64 memoryBytes() const;

// after:app/src/pin/pin_window.cpp(实现放 currentDpr 附近)
qint64 PinWindow::memoryBytes() const {
    const qreal dpr = currentDpr();
    const qint64 backing = static_cast<qint64>(width()) * height() * dpr * dpr * 4;
    return m_image.sizeInBytes() + backing;
}
```

`core_facade.cpp` 淘汰循环三处改用它(`:429` 累加、`:430` incoming、`:437` 淘汰扣减);
`kPinByteBudget` 数值不变,语义由"图片字节"变为"实际字节"(注释同步)。
淘汰循环每轮**从头重算** `totalBytes`(`:428-429`),窗口尺寸变化造成的漂移自行收敛。

> 为什么不用"预算 ÷2"的近似:跨屏 DPR、文本贴图(渲染位图尺寸 ≠ 窗口尺寸)、
> 缩放后的窗口尺寸都让固定 2× 失真,按窗口实算才准。

### 4.4 预期收益

| 场景 | 修复前(实测/估算) | 修复后 |
| --- | --- | --- |
| 1755×1097 屏,贴满上限 | 10 张 / **+189MB**(实测) | ~5~6 张 / **+96MB 封顶**(估算) |
| 4K 屏,贴满上限 | 1~2 张 / +66~132MB(估算) | 1 张 / +66MB(估算) |
| 小图(200×150)×10 | 10 张 / +~12MB(估算) | 10 张 / +~12MB(不变) |

行为变化只有一种:**大图更早淘汰**——正是 96MB 预算的设计意图
(`core_facade.cpp:400-401` 注释已写明"一张 4K 就是 33MB,10 张吃掉整个预算")。

### 4.5 风险与 UX 影响

- 画质零影响;交互上大图用户更早看到"已达上限,最旧一张已关闭",与既有语义一致。
- ⚠️ **本项不解决预算缺口**:`kPinByteBudget=96MB` 本身占了 140MB 预算的大头,
  详见 §8.2。

### 4.6 验证方法

```
复跑实验 D:Ctrl+Shift+P ×12,100ms 采样。
预期:第 5~6 张即出现 mem[pin-add(evicted)];稳态增量封顶 ~+96MB(修复前 +189MB)。
回归:贴图缩放/旋转/双击关闭/编辑回写/另存 全正常;文本贴图不异常淘汰。
```

---

## 5. 截图入库:RGBA 双份拷贝 + base64 往返(WS 瞬态 ~300MB)

### 5.1 现象与实测数据

实验 E3(框选 1500×950 = 5.7MB 图,`regionSkipEditor=true` 直通入库):

- 工作集峰值 **299.8MB**(`mem[capture-end]` 的 PeakWorkingSet),比会话基线
  (~152MB WS)高 ~148MB;
- 私有字节仅 **+17MB** 且随后回落 —— **瞬态,非常驻**。

实验 B(全屏截图 ×5):私有峰值 +24~33MB/次,残留 0。旧日志 342MB 峰值同源
(当时会话还开着翻译页 + 贴图,基数更高)。

### 5.2 根因(代码级)

```rust
// core/nbcore/src/ffi.rs:740-744(ingest_capture_png)
let img = image::load_from_memory(&png)      // 解码出 DynamicImage(1 份)
    .map_err(AppError::Image)?
    .to_rgba8();                              // 已是 RGBA8 仍整体拷贝(1 份)
let (width, height) = img.dimensions();
let stored = crate::clip::ingest_image(core, img.as_raw(), width, height, &source)?;
```

```rust
// core/nbcore/src/clip.rs:396-400(ingest_image 内部还有一次)
let item = ClipItem::Image {
    rgba: rgba.to_vec(),          // &[u8] → Vec,又 1 份
    ...
};
```

叠加 Qt 侧 `capture_session.cpp:76-81` `quickPngBase64`:PNG 编码 + `toBase64()`
生成 **1.33× PNG 大小**的 QString,再经 JSON 交给 core 解码。合计瞬态 ≈
图 ×(1 解码 + 1 to_rgba8 + 1 to_vec)+ base64 串 + PNG 缓冲 + 剪贴板 DIB
(`copy_entry` 回读)+ 监听线程回读。

### 5.3 修复方案

**必做(零行为变化)**:解码结果本就是 RGBA8 时直接取走缓冲。

```rust
// after:core/nbcore/src/ffi.rs:740-744
let decoded = image::load_from_memory(&png).map_err(AppError::Image)?;
// PNG 带 alpha 解码出来就是 RGBA8:直接取走底层 Vec,免 to_rgba8 的整图拷贝
// (4K 图即 33MB);其它格式(灰度/RGB 等)才需要转换那一份。
let (rgba, width, height) = match decoded {
    image::DynamicImage::ImageRgba8(img) => {
        let (w, h) = img.dimensions();
        (img.into_raw(), w, h)
    }
    other => {
        let converted = other.to_rgba8();
        let (w, h) = converted.dimensions();
        (converted.into_raw(), w, h)
    }
};
let stored = crate::clip::ingest_image(core, &rgba, width, height, &source)?;
```

**可选(同批,改 crate 内签名)**:`ingest_image` 收 `Vec<u8>` 而非 `&[u8]`,
去掉 `clip.rs:397` 的 `to_vec()`。

```rust
// after:core/nbcore/src/clip.rs:388-400
pub fn ingest_image(
    core: &Core,
    rgba: Vec<u8>,          // 调用方已持有缓冲,直接转入,免再复制一份
    width: u32,
    height: u32,
    source: &str,
) -> AppResult<Option<ClipEntry>> {
    ...
    let item = ClipItem::Image { rgba, width, height };
```

调用方仅 `ffi.rs:744` 一处(已核实 `ingest_image(` 全仓只此一个调用点),
签名变更无波及;`ingest_image` 不在 FFI 边界(FFI 只经 `nb_call` JSON 门面)。

**同一模式的遗留**:`ffi.rs:718-720`(`ocr_entry`)是同样的
`load_from_memory(..).to_rgba8()` + `into_raw()`,可同批改掉。

### 5.4 预期收益

| 项 | 修复前 | 必做后 | 必做+可选后 |
| --- | --- | --- | --- |
| 整图拷贝份数(入库链) | 3× | 2× | 1× |
| 1500×950 入库 WS 峰值 | 299.8MB(实测) | ~-5.7MB(估算) | ~-11.4MB(估算) |
| 4K 入库 WS 峰值 | ~+150MB over 基线(估算) | ~-33MB(估算) | ~-66MB(估算) |
| 私有常驻 | +17MB 后回落(实测) | 不变 | 不变 |

⚠️ 本项**不解决** WS 峰值主要来源:base64 往返(PNG 编码 + 1.33× 字符串 + 解码)
比这三次拷贝更大,要动 FFI 契约(改 RGBA 直传)。**该契约已于 2026-10-18 落地**,
见下面 5.7。

### 5.7 RGBA 直传(2026-10-18 落地)

原路径(宿主 → 核心):`QImage → PNG 编码 → base64(1.33×,且 QString 是 UTF-16
= 2.66× 字节)→ JSON 序列化 → 核心 base64 解码 → PNG 解码 → to_rgba8 → Vec`。

改为**原生 RGBA 通道**(与 `nb_ocr_rgba` / `nb_stitch_append` 同一套约定):

| 新导出 | 作用 | 出参契约 |
| --- | --- | --- |
| `nb_ingest_capture_rgba` | RGBA 直入入库(编码 PNG 落盘/缩略图/去重全在核心) | **裸** `{"entry": …}`(超限为 `null`) |
| `nb_copy_rgba_to_clipboard` | 同一块像素直接写 CF_DIBV5 | 返回码 |

宿主侧 `CoreBridge::ingestCaptureRgba` / `copyRgbaToClipboard` 包装;
`capture_session.cpp` 的 `ingestImageAndCopy` 优先走直入通道,**旧核心(DLL 无该导出)
自动回退**原 PNG 通道(`hasRgbaIngest()` 探测)。

顺带去掉的第二次往返:入库后的"写剪贴板"原先是 `copy_entry(id)` → **读 blob PNG
→ 解码 → to_rgba8 → DIB**;现在同一块内存像素直接写 DIB。

净效果:入库链路的整帧拷贝从 >5 份降到 **1 份**(宿主切片 → 核心 `Vec`,入库需要
所有权);PNG 解码与 base64 编解码全部消失。

**顺带去掉的第 2 份**:`clip::encode_png_with` 原先 `RgbaImage::from_raw(.., rgba.to_vec())`
—— 调用方手上是 `&[u8]`,为了构造 `RgbaImage` 必然多拷一整帧(4K 33MB)。改用编码器的
`write_image(rgba, …)` 直接读切片,输出字节完全一致。

#### 实测(2026-10-18,`examples/ingest_mem_probe.rs`,真实截图 blob 回放)

方法:自定义 `GlobalAlloc` 统计进程内 malloc 峰值,取「峰值 − 宿主那一帧」作为
**入库链路的瞬态抬升**;两条路径跑在**独立进程**里,避免互相抬高水位。
`old` 精确复现原链路(宿主 PNG 编码 → base64 → UTF-16 QString → JSON → 核心
`to_string_lossy` → serde 解析 → base64 解码 → PNG 解码 → 入库 → 读 blob 再解码写 DIB)。

| 样本(真实截图) | 帧大小 | 改前峰值抬升 | 改后峰值抬升 | 降幅 | 耗时 |
| --- | --- | --- | --- | --- | --- |
| 1651×852 | 5.4MB | 23.6MB(4.40×) | **15.5MB**(2.89×) | **−34%** | 39→19ms |
| 1846×1323 | 9.3MB | 53.0MB(5.69×) | **26.8MB**(2.88×) | **−49%** | 118→46ms |
| 1061×6184(长截图) | 25.0MB | 92.7MB(3.70×) | **38.1MB**(1.52×) | **−59%** | 138→82ms |

降幅随图片变大而提高 —— 因为被消掉的那部分(base64 串 + JSON 副本 + 解码前后的
中间缓冲)在图越大时膨胀越多,而剩下的是**与图大小无关的固定骨架**(见下)。

改后剩余的 2.9×(以 1846×1323 为例)拆解:

| 步骤 | 峰值抬升 | 说明 |
| --- | --- | --- |
| 核心侧 `to_vec()` 接管帧 | 9.3MB(1.00×) | 入库需要所有权;FFI 传的是借用切片 |
| `encode_png` 落盘编码 | 6.7MB(0.72×) | PNG 压缩工作缓冲 + 输出(已无 to_vec 那份) |
| 缩略图(`resize` + 编码) | 23.4MB(2.51×) | **新的大头**:CatmullRom 重采样 + 中间位图 + 编码 |
| 写 DIB(`rgba_to_dibv5`) | 1×帧 | 构造 BITMAPV5HEADER 载荷,Windows 剪贴板格式要求 |

⚠️ **下一步可做(未做)**:缩略图那 2.51× 与入库编码不共享缓冲 —— 目前是
「先编原图、再 resize 原图」。若先 resize 出 640px 小图、只对小图编码,**并在小图
生成后立刻释放原帧**,峰值可再降一档。风险:需要改 `build_entry` 的所有权顺序
(原帧目前 move 进缩略图),且 PNG 落盘仍需原帧 —— 属于结构性改动,单独立项。

#### 5.7.1 与图片大小无关的那 70MB:遮罩窗口,不是入库(2026-10-18 深挖)

用户实测反馈:「无论截图多大,内存都有 ~70MB 的增幅」。日志与隔离实验证实这是
**两个独立的固定项**,和入库链路无关 —— 入库的瞬态(上一节)只占其中一小部分。

**日志证据**(`app/build/data/nextboard.log`,124 次会话统计):

| 观测 | 数据 |
| --- | --- |
| `capture-end` 峰值 − `capture-start` 工作集 | 中位 **122.3MB**,最小 77.4,最大 248.9 |
| 同一进程内连续会话之间 | Δ工作集 平均仅 **+6.6MB**(上一张已回收,差异只在当前这张) |
| 小选区(<2MB RGBA)vs 大选区(>6MB RGBA) | Δ私有 **17.2MB** vs **15.5MB** —— **几乎相同** |

最后一行是决定性的:选区大小差 3 倍以上、增量却持平 ⇒ 增量不是图片内容带来的。

**隔离实验**(独立 Qt 探针,直接读 `GetProcessMemoryInfo` 的私有字节;本机三屏
1920×1080 / 3071×1920 / 1080×1920):

| 步骤 | 私有内存增量 | 与像素缓冲的关系 |
| --- | --- | --- |
| 分配三屏整屏 RGBA 冻结底图 | **+38.3MB** | = Σ(物理宽×高×4),1:1 |
| 三块全屏半透明遮罩窗口上屏(不画任何内容) | **+42.9MB** | ≈ 又一份 Σ(物理像素×4) |
| 遮罩里再画上底图 | **+0** | 绘制不额外分配 |
| 改为不透明窗口 | +42.6MB | 与半透明**无差别** |
| `WA_PaintOnScreen` / `WA_OpaquePaintEvent` / `WA_NoSystemBackground` | 不变 | 都省不掉这份缓冲 |

逐屏累加(私有内存,实测量):

| 屏 | 物理分辨率 | 像素缓冲理论值 | 实测增量 | 比值 |
| --- | --- | --- | --- | --- |
| Mi Monitor | 1920×1080 | 7.9MB | 8.6MB | 1.09 |
| TL160MDMP03-0 | 3071×1920 | 22.5MB | 22.6MB | 1.00 |
| S24 PRO | 1080×1920 | 7.9MB | 8.0MB | 1.01 |
| **合计** | | **38.3MB** | **39.2MB** | |

**结论:遮罩会话的固定开销 ≈ 2 × 整屏像素缓冲**

```
冻结底图(核心 → 宿主,每屏一份 RGBA)        ≈ 38.3MB
遮罩窗口后备缓冲(Qt,每屏一块,与底图等大)   ≈ 38.3MB
────────────────────────────────────────────────────
合计                                           ≈ 76.6MB   ← 与用户观测的 ~70MB 吻合
```

之所以两者等大且都不随选区变化:**整屏**捕获(框选前必须先冻结整屏画面)+
Qt 为每个全屏窗口分配的**整窗**后备缓冲 —— 选区只是画在其上的一小块矩形。

**为什么后备缓冲那块省不掉**(逐项试过,均无效):

- 不透明窗口:结果一样(+42.6MB),Qt 仍走 `QWidgetBackingStore`;
- `WA_PaintOnScreen`(直画窗口 DC):Windows 上 `Tool`+`FramelessWindowHint` 组合仍创建后备缓冲;
- `WA_OpaquePaintEvent` / `WA_NoSystemBackground`:只影响"要不要擦背景",不影响分配;
- 绘制内容(空白 / 底图 / 底图+蒙版):完全没差别 —— 缓冲在**窗口上屏**那一刻就分配了。

**可做的方向(未做,需产品取舍)**:

| 方案 | 可省 | 代价 |
| --- | --- | --- |
| 只在**光标所在屏**建遮罩,其余屏不冻结不建窗 | 每少一屏省 ~2×该屏像素 | 跨屏框选不再支持(现在支持) |
| 底图与遮罩共享同一块内存 | 0 | 实测缓冲与底图无关,共享也省不掉 |
| 用**单个**覆盖整个虚拟桌面的窗口代替每屏一块 | 略 | 混合 DPI 下坐标/绘制复杂;总缓冲仍 ≈ 虚拟桌面像素 |

其中**第一条收益最确定**:本机三屏下若只冻结光标屏,固定开销从 76.6MB 降到
约 26MB(光标在 3071×1920 那块时)。但"三屏同时框选/跨屏对比"会失去支持 ——
属 UX 决策,本轮未擅自改。

### 5.5 风险与 UX 影响

- **零行为变化**:像素完全一致(RGBA8 分支只跳过恒定拷贝);入库/去重/落盘不变。
- 可选签名改动的调用点已核实唯一。

### 5.6 验证方法

1. `cargo test -p nbcore`:226 项中 224 通过(2 项为既有 `%TEMP%` 环境失败,
   见 §10 末条),**不得新增失败**;
2. 复跑实验 E3:读 `mem[capture-end]` 的 PeakWorkingSet,预期降 ~6/11MB(估算),
   私有回落值不变;
3. 复跑实验 B(全屏截图 ×5):峰值同步下降,残留仍为 0;
4. 内容一致性:修复前后各入库一张同图,比对 blob 的 blake3 哈希应相同。

---

## 6. 首个显隐循环 +42MB 委托 churn(第 1 版方案不可行,已修正)

### 6.1 现象与实测数据

实验 G(38 条目、无图库,排除缩略图变量)+ 实验 A(含图):

| 观测 | 数据 |
| --- | --- |
| 无输入对照 | 90.4MB 平坦 |
| 首个显隐循环后 | **+42MB 一次性,不回落**(≈1.1MB/条目) |
| 空库(0 条目)首循环 | 仅 +5MB |
| 稳态:显示 / 隐藏 | 122MB / **137MB**(隐藏反而高 15MB) |
| 之后每循环 | +0.5MB 并归零,**非泄漏** |
| DB 体积 | 0.11MB(排除"模型持全文"假设) |

### 6.2 根因(代码级)

```cpp
// app/src/panel/qml_bootstrap.cpp:130-135(PanelListLifecycle::sync)
if (m_visible && m_panel->mode() == PanelWindow::Mode::List)
    m_model->acquire();      // 显示
else
    m_model->release();      // 隐藏 / 进翻译页

// app/src/panel/clip_entry_model.cpp:136-149
void ClipEntryModel::release() {
    ...
    beginResetModel();
    m_rows.clear();          // → QML 委托整批销毁(堆 churn)
    endResetModel();
}
// :151-154
void ClipEntryModel::acquire() {
    m_released = false;
    refreshNow();            // → reload() → beginResetModel + 清空 + 重填
}
```

机制:销毁 38 张复杂 EntryCard 委托的堆 churn 撑大私有堆(Windows 堆不归还),
随后的重建复用已释放块、不再增长——所以只有**首个**循环留下 +42MB 台阶。
纹理释放已由 `panel_window.cpp:143-144` 的 `setPersistentSceneGraph(false)` 负责,
`release()` 在当前数据量下净负。

### 6.3 修复方案(修正)

⚠️ **第 1 版方案有致命漏洞**:它只加 `freeze()`(隐藏时不销毁行),
但 `acquire()`(:151-154)仍**无条件** `refreshNow() → reload() →
beginResetModel() + 清空 + 重填` —— **每次显示照样把全部委托销毁重建**,
churn 一次不少,反而因隐藏期保留行而抬高隐藏态水位(严格更差)。

正确做法:`acquire()` 在"冻结期间数据没变"时**只解冻、不重载**。

```cpp
// after:app/src/panel/clip_entry_model.hpp(私有区)
    /// 冻结:保留已加载的行,仅拦刷新(配合 PanelListLifecycle 的隐藏策略)。
    Q_INVOKABLE void freeze();
    ...
    bool m_frozen = false;
    /// 冻结期间收到过刷新请求(剪贴板/设置变化)→ 解冻时必须重载。
    bool m_dirtyWhileFrozen = false;

// after:app/src/panel/clip_entry_model.cpp
/// 冻结:保留已加载的行(隐藏态不再销毁委托 → 无 churn),仅拦刷新。
void ClipEntryModel::freeze() {
    if (m_frozen) return;
    m_frozen = true;
    m_debounce.stop();
}

void ClipEntryModel::acquire() {
    const bool needReload = m_released || m_dirtyWhileFrozen;
    m_frozen = false;
    m_dirtyWhileFrozen = false;
    m_released = false;
    // 关键:冻结期间数据未变 → 行仍然有效,只解冻、不重载,
    // 避免"隐藏→显示"把全部委托销毁重建(实测 +42MB 台阶的成因)。
    if (needReload) refreshNow();
}
```

三个入口守卫(`:124` `refresh()`、`:130` `refreshNow()`、`:479` `reload()`)改为:

```cpp
// refresh():隐藏/冻结期间不刷,但要记账,解冻时补一次
    if (m_released || m_frozen) { m_dirtyWhileFrozen = true; return; }
// refreshNow()/reload():真正执行入口仍硬拦
    if (m_released || m_frozen) return;
```

`PanelListLifecycle::sync()` 分支修正(第 1 版会把**翻译页**也走 freeze,
与"进翻译页仍走 release"的意图矛盾):

```cpp
// after:app/src/panel/qml_bootstrap.cpp(实验开关)
class PanelListLifecycle : public QObject {
    ...
    /// 实验开关(默认 false = 维持现状):true = 列表页隐藏时保留行、仅拦刷新。
    /// 动机:实测显示态 122MB < 隐藏态 137MB —— 销毁委托的堆 churn 比保留更贵;
    /// 纹理已由 setPersistentSceneGraph(false) 负责释放。
    static constexpr bool kKeepRowsWhenHidden = false;

    void sync() {
        const bool listMode = m_panel->mode() == PanelWindow::Mode::List;
        if (m_visible && listMode) {
            m_model->acquire();
        } else if (kKeepRowsWhenHidden && listMode) {
            // 仅"列表页 + 隐藏"保留行;进翻译页仍 release(整页用不到列表)
            m_model->freeze();
        } else {
            m_model->release();
        }
    }
};
```

实现时注意状态互斥:`release()` 应同时清 `m_frozen` / `m_dirtyWhileFrozen`,
避免状态残留。

### 6.4 预期收益

| 项 | 修复前(实测) | 修正方案通过后(预估) |
| --- | --- | --- |
| 隐藏态稳态 | 137MB | **~122MB**(= 显示态水位) |
| 首个显隐循环 | +42MB 一次性 | **0**(冻结期数据未变 → 不重载) |
| 显示延迟 | 首次呼出重建场景 | 不变或更快(无需重建委托) |

### 6.5 风险与 UX 影响

- 剪贴板通知在冻结期被记账而非执行,呼出时若期间有变化会重载一次(必要 churn),
  无变化则零 churn —— 用户视角"呼出即最新",与现状一致。
- 保留行 = 隐藏态常驻一份委托对象(JS 堆);需压力场景验证(见 6.6)。
- 开关是编译期常量,A/B 只改一个布尔重编译,回滚零成本。

### 6.6 验证方法

1. 复跑实验 A/G(10 次显隐循环 + 30s 空闲),开关置 `true`:
   预期 `mem[panel-hide]` ≈ `mem[panel-show]`(差 <5MB),且无首个循环 +42MB 台阶;
2. **压力对照**:造 50 张图片条目(缩略图齐全)重复实验 A,
   确认"保留行"不劣于"销毁重建";劣化则维持现状;
3. 功能回归:① 隐藏期间复制新内容 → 呼出列表立即出现新条目(**最关键**,
   验证 `m_dirtyWhileFrozen` 记账生效);② 进翻译页 → 回列表页,列表正常;
   ③ 隐藏期间删除条目 → 呼出后不显示已删条目。

---

## 7. 配置损坏静默回退(软件渲染被悄悄关掉)

### 7.1 现象与实测数据

实验 C:用 PowerShell 改写 `app_config.json`(带 BOM)后启动,
`nextboard.log` 出现 `Qt Quick graphics API = Hardware (default/D3D11)`,
`mem[panel-show]` 私有 **155.8MB**,比软件渲染基线 95.4MB 高 **+60MB**
(结合 §1 的对照数据,整会话水位差可达 +84~101MB)。用户侧完全无提示——
所有设置被一次性重置为默认值。

### 7.2 根因(代码级)

```rust
// core/nbcore/src/settings.rs:804-824
pub fn load(data_dir: &Path) -> AppResult<Settings> {
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Ok(Settings::default());          // :806-807 文件缺失:正常首启,静默合理
    };
    let Ok(mut root) = serde_json::from_str::<Map<..>>(&text) else {
        warn!("配置文件解析失败,使用默认设置: {path:?}");   // :811 只进 nbcore.log
        return Ok(Settings::default());
    };
    let Some(mut value) = root.remove("settings") else {
        return Ok(Settings::default());          // :814-816 缺 "settings" 键:**完全静默**
    };
    ...
    Ok(serde_json::from_value(value).unwrap_or_else(|err| {
        warn!("配置字段解析失败,使用默认设置: {err}");       // :820-823 只进 nbcore.log
        Settings::default()
    }))
}
```

三个问题:① 警告只进 `nbcore.log`(Rust `log` crate 的文件出口),排障不看那里;
② 文案没说"哪些设置被重置、有什么后果";③ 缺 `settings` 键的路径完全没有警告。

### 7.3 修复方案(修正:第 1 版"落 nextboard.log"这句不成立)

⚠️ **可行性修正**:Rust 侧 `log` 只写入 `<data_dir>/nbcore.log`
(`lib.rs:28-40` 的 `SimpleFileLogger`),与宿主的 `nextboard.log`
(`main.cpp` 的 `qInstallMessageHandler`)是**两个独立出口**。所以"让 `nb_init`
落一条 nextboard.log"在 `nb_init` 内**做不到**,必须由宿主(C++)取到警告后 `qWarning()`。

**步骤 1(直接修掉真实故障):BOM 容错**。实际触发场景是外部工具写入 BOM,
解析前剥掉即可,不必回退默认值:

```rust
// after:core/nbcore/src/settings.rs:806 之后
    // 外部工具(记事本另存、PowerShell Set-Content -Encoding UTF8 等)可能写入 BOM,
    // serde_json 会因此整体解析失败,并把**所有设置**打回默认(实测渲染后端切回
    // D3D11,稳态 +100MB 量级)。BOM 是纯前缀噪声,剥掉再解析,避免无谓的全量回退。
    let text = text.trim_start_matches('\u{feff}');
```

**步骤 2(可观测性)**:三段回退都补警告,并让宿主落 `nextboard.log`。

```rust
// after:settings.rs —— 回退点补警告(文案含后果)
    let Ok(mut root) = serde_json::from_str::<serde_json::Map<String, serde_json::Value>>(text)
    else {
        warn!("配置文件解析失败,已回退默认设置(渲染后端/热键/翻译配置等全部重置): {path:?}");
        return Ok(Settings::default());
    };
    let Some(mut value) = root.remove("settings") else {
        warn!("配置文件缺少 settings 键,已回退默认设置(后果同上): {path:?}");
        return Ok(Settings::default());
    };
```

新增一条只读命令,由宿主启动时取一次并 `qWarning()` 落宿主日志
(比改 `load()` 返回值对调用面冲击小,也不动 `get_settings` 的既有响应形状):

```rust
// after:core/nbcore/src/ffi.rs —— 新增只读命令(dispatch 内)
        // 配置加载警告(空数组 = 干净)。宿主启动时拉一次写进自己的日志 ——
        // Rust 的 log 出口是 nbcore.log,而排障第一现场通常是 nextboard.log。
        "settings_warnings" => Ok(serde_json::json!(crate::settings::take_warnings())),
```

```cpp
// after:app/src/host/core_bridge.cpp —— init 末尾(或 main.cpp core-ready 之后)
    const QJsonObject w = call("settings_warnings");
    for (const QJsonValue& v : w.value("data").toArray())
        qWarning().noquote() << "配置警告:" << v.toString();
```

若不想加命令,退一步的等价方案:警告在 `nb_init` 里 `log::info!` 汇总
(至少 nbcore.log 有完整线索)+ 在 `docs/` 排障章节写明"渲染后端异常先看
nbcore.log 的配置警告"。**但宿主日志可见性只有新增命令能做到。**

### 7.4 预期收益

| 项 | 修复前 | 修复后 |
| --- | --- | --- |
| 带 BOM 的配置 | 全部设置重置,稳态 +60~100MB | **正常工作**(BOM 剥掉),零损失 |
| 真损坏的配置 | nbcore.log 一行 warn(或缺) | nbcore.log + nextboard.log 双份,文案含后果 |
| 常驻内存 | 不变 | 不变(本项防劣化,不省内存) |

### 7.5 风险与 UX 影响

- 步骤 1 零风险(只剥前缀);步骤 2 只加日志与一条只读命令,不动 `get_settings` 形状。
- `settings_warnings` 是新增命令,`dispatch` 的 `_ =>` 分支对未知方法报错,
  旧宿主不会调用它;宿主与旧 dll 错配时 `call` 返回 `ok:false`,判 `ok` 后静默忽略即可。

### 7.6 验证方法

1. `cargo test -p nbcore` 不得新增失败;
2. 手工复现:① 给隔离目录写**带 BOM 的合法配置** → 启动后 `softwareRender`
   **保持生效**(日志显示 `Software (设置项)`)→ 步骤 1 生效;
   ② 写**语法错误**的配置 → 启动后 `nextboard.log` 与 `nbcore.log` 都有配置警告;
   ③ 删掉 `settings` 键 → 同样有警告(修复前静默)。

---

## 8. 总对比表(修正:预算口径)

### 8.1 场景对比

| 时点 | 修复前(实测) | 修复后(预估) | 影响项 |
| --- | --- | --- | --- |
| **启动/隐藏(默认硬件配置)** | **231MB** | **~130MB** | **§1(单项最大)** |
| 列表显示态(软件渲染) | 122MB | 122MB | — |
| 列表隐藏态(软件渲染) | **137MB** | **~122MB** | §6(需 A/B 通过) |
| 首个显隐循环 | +42MB 台阶 | 0 | §6(需 A/B 通过) |
| 遮罩会话 | **+77.5MB / 最长 600s** | +77.5MB / **最长 60s**;阶段 3 后 ~+26MB | §3 |
| 编辑器(4.3MB 图 / 4K 图) | +16.6MB / ~76~106MB | ~+8MB / ~10~40MB | §2 |
| 贴图贴满上限 | +189MB(10 张) | **+96MB 封顶** | §4 |
| 截图入库 WS 峰值 | 299.8MB(5.7MB 图) | ~288~294MB | §5 |

### 8.2 ⚠️ 预算口径(第 1 版结论错误,第 2 版漏了默认后端)

第 1 版写"稳态常驻 ~122MB,距 140MB 有 18MB 余量"——**错的**。
`docs/01-architecture.md:19` 明确口径是 **"~100–140MB(主面板 + 10 张贴图)"**,
`:52` 把贴图按 **3–10MB/张** 估。实际有两处对不上:

**① 默认后端就超预算(§1)**:默认 `software_render=false` → 面板隐藏态 **231MB**,
**与贴图无关就已超 91MB**。

**② 贴图实际成本是预算假设的 2~6 倍(§4)**:实测 18.3MB/张(架构按 3–10MB/张估)。

| 配置 | 现状 | 本方案落地后 | 140MB |
| --- | --- | --- | --- |
| 面板(默认硬件后端) | **231MB** | — | ❌ 超 91MB |
| 面板(软件后端,§1 改默认) | 130MB | ~122MB(§6) | ✅ 达标 |
| 面板 + 10 张全分辨率贴图(软件后端) | 130 + 189 = **319MB** | 122 + 96 = **218MB** | ❌ 仍超 78MB |
| 面板 + 3 张全分辨率贴图 | 130 + 55 = 185MB | 122 + 55 = 177MB | ❌ 仍超 |

`面板 + 10 张贴图` 与 140MB **在数学上不可兼得**:仅 10 张全分辨率贴图就 183MB。
三个选项(**需产品决策,非纯技术**):

| 选项 | 做法 | 效果 | 代价 |
| --- | --- | --- | --- |
| **A. 降 `kPinByteBudget`** | 96MB → **24~32MB** | 面板 122 + 32 = **154MB**,接近达标;1755×1097 屏 ≈ 2 张、4K 屏 1 张 | 可见行为变化:贴图上限从 10 张降到 ~2 张 |
| **B. 贴图存降采样副本** | 超过屏幕尺寸的图按 ~1920 边降采样显示;原图仅用于复制/编辑时按需读 blob | 4K 贴图 66MB → ~8MB | **有画质感知**(放大贴图会糊),需 UX 决策 |
| **C. 修订预算口径** | 承认"面板常驻 <140MB"为达标口径;贴图按字节预算封顶(§4) | 面板 122MB ✅;含贴图 218MB | 需修订 `docs/01-architecture.md:19/52` 的口径与数字 |

**建议:C(诚实修口径)+ A(收到 32MB)先做,B 留待 UX 评审。**

---

## 9. 落地顺序与依赖

| 顺序 | 项 | 依赖 | 风险 | 工作量 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 1 | §1 阶段 1(默认软件渲染)+ 断言/文案 | **需手感验收** | 中(滚动/动画手感) | 1h + 验收 | ✅ 已落地(**待人工手感验收**) |
| 2 | §7 步骤 1(BOM 容错) | 无 | 零 | 0.5h | ✅ 已落地 |
| 3 | §3 阶段 1(空闲超时 600→60s)+ 断言 | 默认值需确认 | 零(超时行为已有) | 0.5h | ✅ 已落地 |
| 4 | §5 必做(去 `to_rgba8` 拷贝)+ ocr_entry 同改 | 无 | 零(像素一致) | 0.5h | ✅ 已落地 |
| 5 | §5 可选(`ingest_image` 收 `Vec`) | 4 | 低(单调用点) | 0.5h | ✅ 已落地 |
| 6 | §2 阶段 1(马赛克懒构建) | 无 | 零(镜像模糊既有模式) | 0.5h | ✅ 已落地 |
| 7 | §4 贴图 `memoryBytes()` | 无 | 低(更贴设计意图) | 1h | ✅ 已落地 |
| 8 | §7 步骤 2(警告 + `settings_warnings` + 宿主日志) | 2 | 零(只加日志/只读命令) | 1.5h | ✅ 已落地 |
| 9 | §2 阶段 2(背景项自绘) | 6 | 低(需自截验渲染) | 1h | ✅ 已落地(自截确认渲染正常) |
| 10 | §6 A/B 实验(`freeze` + 脏标记) | 无 | 实验性,可回滚 | 3h + 复测 | ✅ 已落地(A/B 已跑,冻结胜出) |
| 11 | §3 阶段 3(多屏懒捕获) | UX 评审 | 中(跨屏延迟) | 4h | ⬜ 未做 |
| 12 | §8.2 的 A(预算收到 32MB)/ B(降采样) | **产品决策** | 行为/画质变化 | 各 0.5~2h | ⬜ 待决策 |

1–10 已落地并复测(数据见 §11);11/12 需评审或决策。
**每项落地后按对应"验证方法"复测,把实测值回填本文(替换"预估"口径)。**

---

## 10. 未纳入本次的问题(backlog)

| 问题 | 规模 | 不纳入的理由 |
| --- | --- | --- |
| ~~截图 base64 往返(PNG 编码 + 1.33× 字符串 + 解码)~~ | ~~WS 瞬态主要来源(>§5 的三次拷贝)~~ | **已完成(2026-10-18)**:见 §5.7,FFI 新增 RGBA 直传通道 |
| 遮罩会话里的 backing store 与合成缓冲(§3 未拆到那 53MB) | ~53MB/3 屏 | 涉及遮罩渲染路径重写,收益需先由阶段 3 验证 |
| 翻译页首次打开 +49MB | 一次性 | 懒构建已到位;TranslateView 内部瘦身需先定位 |
| 缩略图 640 边降尺寸 / 贴图缩放降质 | 省 15~20% 纹理 | **损害画质/交互,明确不做** |
| QML 树地板 71.6MB / 长期隐藏卸载整棵 QML | 数十 MB | 呼出重建 + 丢搜索词/滚动位置,交互代价大 |
| ~~遮罩窗口的 QPainter 引擎错误~~ | 不影响内存 | **2026-10-18 定性完成:是无害探测,首帧没有丢**。详见 §10.1 |
| `settings.rs:1584/1618` 两个测试用 `%TEMP%` | 非产品问题 | **已顺手修掉**(改 `CARGO_MANIFEST_DIR/target/tmp`),套件在沙箱/CI 也能全绿 |

---

### 10.1 遮罩首帧 `QPainter` 告警的定性(2026-10-18)

**结论:是无害的平台层探测,首帧画面没有丢,不需要修。**

#### 现象

日志里每次 `capture-start` 后固定出现一批:1 条
`QPainter::begin: Paint device returned engine == 0, type: 3`,随后 19 条
`QPainter::xxx: Painter not active`(三屏时 3+57 条)。124/133 次会话命中。

#### 定位过程

| 探针 | 结果 |
| --- | --- |
| 单窗口最小复现 | **不复现** |
| 三窗口 + `raise/activateWindow`(产品同序) | 复现,但**我们自己的 `paintEvent` 里 `beginFail == 0`** |
| 全局事件过滤器看谁收到 `QEvent::Paint` | 失败的那次投递对象是 **`QWidgetWindow`**(QPA 平台窗口对象),**不是** `QWidget` |

调用点特征(事件过滤器实打实打出来的):

```
PaintEvent -> obj=... class=QWidget        visible=1   <- 我们的遮罩,active=1,绘制成功
PaintEvent -> obj=... class=QWidgetWindow  visible=-1   <- 平台窗口对象
QPainter::begin: Paint device returned engine == 0, type: 3   <- 发生在这里
```

`QWidgetWindow` 是 QPA 的 `QWindow` 实现,它的 paint 发生在窗口表面(`QPlatformWindow`)
尚未就绪时 —— 此时 `QPainter` 拿到的 pixmap 没有引擎。这是 Qt 内部的**探测性绘制**,
与产品代码无关,也不受我们设置的任何 widget 属性影响。

#### 是不是真的丢了首帧

**不是。** 用屏幕抓取(与用户眼睛看到的同一条路径)验证:遮罩铺一块纯红底图后,
在 `showFullScreen()` 返回后**立刻**(0ms)、16ms、60ms 三个时点抓屏,红色占比均
**100.0%**,稳定后同样 100.0%。即首帧已经是完整画面,那些告警只是噪音。

#### 已做的处理

`OverlayWidget::paintEvent` 与倒计时遮罩加了 `if (!painter.isActive()) return;`
(`🐛 fix(capture): 遮罩首帧绘制加引擎就绪守卫`)。作用仅限于:

- 若将来某个平台/后端下我们**自己的** paint 真的引擎未就绪,不会继续 apply 一串
  无操作的 draw 调用(那些调用本身也会刷告警);
- **挡不住** `QWidgetWindow` 那批 —— 它不在我们的代码路径上,也没有公开 API 可挡。

#### 若一定要让日志干净

唯一可行的做法是在日志层过滤(而非修代码):`main.cpp` 的 `fileMessageHandler`
里对 `Paint device returned engine == 0` / `Painter not active` 两类消息降级或丢弃。
但要清楚代价:**这会一并掩盖真实的绘制失败**,排障时反而更麻烦。当前选择保留。

---

*本文"实测"数字来自 2026-10-13 的 22 组隔离实验(§0 方法);"估算"均标注口径与依据。
第 3 版补入**默认后端即超预算**这一首要发现;第 2 版修正了第 1 版的三处实质性错误。
第 4 版为实施版,回填数据见 §11。*

---

## 11. 实施结果与回填数据(2026-10-14)

### 11.1 落地清单

| 项 | 改动位置 | 状态 |
| --- | --- | --- |
| §1 默认软件渲染 | `settings.rs:376-381`(默认值)、`:1317-1320`(断言)、`main.cpp:89-97/167-186`(`--hardware` + 后端选择)、`SettingsDrawer.qml:631-646`(文案) | ✅ 已落地 |
| §2 阶段 1 马赛克惰性 | `editor_window.cpp` 构造函数删除马赛克块;`setTool()` 内按 `tool == EditorTool::Mosaic` 惰性生成;`editor_window.hpp:104` 注释 | ✅ 已落地 |
| §2 阶段 2 背景项自绘 | `editor_window.cpp:145-163` 新增 `ImageBackgroundItem`;构造处由 `QGraphicsPixmapItem(QPixmap::fromImage(...))` 换用它 | ✅ 已落地 |
| §3 阶段 1 空闲超时 | `settings.rs:375`(600→60)、`:1313-1316`(断言) | ✅ 已落地 |
| §4 贴图真实字节 | `pin_window.hpp:60-67` + `pin_window.cpp:373-382` 新增 `memoryBytes()`;`core_facade.cpp:430-446` 淘汰循环改口径 + incoming 按 2× 估算 | ✅ 已落地 |
| §5 去 `to_rgba8` 拷贝 | `ffi.rs` 新增 `decode_png_to_rgba()`,`ingest_capture_png` 与 `ocr_entry` 共用 | ✅ 已落地 |
| §5 `ingest_image` 收 `Vec` | `clip.rs:388-400` 签名改 `Vec<u8>`,去掉内部 `to_vec()` | ✅ 已落地 |
| §6 冻结替代释放 | `qml_bootstrap.cpp:104-160`(`kFreezeWhenHidden = true` + 分支修正)、`clip_entry_model.{hpp,cpp}`(`freeze()`/脏标记/入口守卫) | ✅ 已落地 |
| §7 BOM 容错 + 警告 | `settings.rs:804-836`(剥 BOM、三段 `push_load_warning`、`take_warnings()`)、`ffi.rs` 新增 `settings_warnings` 命令、`core_bridge.cpp:110-116` 宿主落日志 | ✅ 已落地 |
| (附)测试可用性 | `settings.rs` 两个用 `%TEMP%` 的用例改 `CARGO_MANIFEST_DIR/target/tmp` | ✅ 套件由 221 通过/2 失败 → **221 通过/0 失败** |

### 11.2 复测数据(全部实测,同一台机器同一协议)

**§1 渲染后端(默认值变更)**

| 时点 | 改前(默认硬件) | 改后(默认软件) | 省 |
| --- | --- | --- | --- |
| 隐藏态常驻 | 230.9MB | **129.9MB** | **101.0MB** |
| 启动(全新配置,无 `softwareRender` 字段) | (无此配置) | `mem[startup]` **76.8MB**,日志 `Software (设置项/默认)` | — |

验证方式:配置里**删掉** `softwareRender` 字段启动 → 日志显示软件后端,启动读数 76.8MB。

**§3 遮罩空闲超时**

| 时点 | 改前(600s) | 改后(60s) |
| --- | --- | --- |
| 遮罩会话常驻 | +77.5MB,持续最长 10 分钟 | 同样 +77.5MB,但 **60s 自动取消** |
| 复测 | — | t=4s 154.8MB → t=66s **77.6MB**(回落干净,日志有 `capture-end`) |

**§4 贴图淘汰(字节口径修正)**

| 项 | 改前 | 改后 |
| --- | --- | --- |
| 淘汰触发点(1755×1097 屏) | 第 11 张(仅数量上限) | **第 6 张**(字节预算先触发) |
| 贴满上限的私有增量 | **+189MB**(10 张) | **+109MB**(5 张,平台期 195.1MB − 基线 86MB) |

**§6 显隐循环(冻结 vs 释放,A/B 同协议同数据)**

| 指标 | 释放(旧,`kFreezeWhenHidden=false`) | **冻结(新,`=true`)** | 差 |
| --- | --- | --- | --- |
| 10 次循环峰值 | 138.7MB | **107.9MB** | **−30.8MB** |
| 循环后稳态 | 124.1MB(锯齿 118~139) | **99.9MB**(平坦) | **−24.2MB** |
| 曲线形态 | 首个循环抬升 ~30MB 后不落 | 平稳无台阶 | — |

**§6 功能回归(冻结路径)**——用 `--screenshot` 自截验证:
面板隐藏期间复制 `ZZMARKER49906` → 再次呼出,列表**首条即该 marker**
(证明冻结期脏标记记账 → 解冻重载生效,隐藏期间的剪贴板变化不会丢)。
分类计数、缩略图、卡片渲染均正常。

**§7 配置容错与可观测性**

| 用例 | 改前 | 改后(实测) |
| --- | --- | --- |
| 带 BOM 的**合法**配置 | 解析失败 → 全量回退默认(渲染后端被切走) | **正常工作**:读成配置里的值(`Hardware (设置项)`),无警告 |
| 语法损坏的配置 | 仅 nbcore.log 一行 warn | nextboard.log 出现 `配置警告: 配置文件解析失败…(渲染后端/热键/翻译配置等会一并重置)` |
| 缺 `settings` 键 | **完全静默** | nextboard.log 出现 `配置警告: 配置文件缺少 settings 键…` |

**§2 编辑器(马赛克惰性 + 背景自绘)**——以 `capture-end − editor-closed` 度量编辑器持有量:

| 项 | 改前 | 改后 |
| --- | --- | --- |
| 4.12MB 裁剪的编辑器持有 | 30.6MB | **22.5MB**(−8.1MB ≈ 省下一份整图 + 其构造临时) |
| 关闭后残留(相对 capture-start) | +1.8MB | **+0.1MB** |

> ⚠️ `mem[editor-open]` 探针**不能**用来判读编辑器(§0 坑 3):它在马赛克/工具栏构建
> 前触发、且此刻遮罩仍在异步销毁,改前改后都在 ~175MB,看不出差别。
> 复测请用采样窗口峰值或 `capture-end − editor-closed`。

### 11.3 实施中新发现并修掉的缺陷

**冻结路径首屏漏装载(实施 §6 时引入并当场修复)**:
`acquire()` 最初按"`m_released || m_dirtyWhileFrozen`"判断是否需要重载,但**首次显示**
时两个标志都没置位、行也从未装载 → 跳过装载 → **面板空白**(实测 `panel-show`
83.4MB ≈ 空库水位 80.4MB)。
修复:新增 `m_loadedOnce`(装载成功后置位,`release()` 清位),
`needReload = m_released || m_dirtyWhileFrozen || !m_loadedOnce`。

**冻结期改查询/筛选会丢变更**:`setQuery`/`setView`/`setTag`/`setSource` 走
`reload()`,冻结期被拦后若只记账不重载,解冻时列表会停留在旧条件。
修复:`reload()` 与 `refreshNow()` 在冻结期改为**置脏**(而非静默返回),
`refresh()` 同样记账 → 解冻时统一补一次重载。

### 11.4 仍未落地

| 项 | 原因 |
| --- | --- |
| §3 阶段 2(显式 `deleteLater`) | 阶段 1 已把最长持有时间压到 60s,收益有限;可与阶段 3 一起做 |
| §3 阶段 3(多屏懒捕获) | 需 UX 评审(跨屏首次进入有一次捕获延迟) |
| §8.2 A(预算收到 24~32MB)/ B(贴图降采样) | **产品决策**:会改变贴图数量上限或引入画质感知 |
| §10 遮罩窗口 `QPainter` 引擎错误 | 既有问题(见下),非本次引入。**2026-10-18 已加守卫**:`OverlayWidget::paintEvent` 与倒计时遮罩在 `begin()` 失败(`!painter.isActive()`)时直接返回,不再刷 `Painter not active`;Qt 会重发 paint,冻结底图不受影响 |

### 11.5 验收结论

- **默认配置**的稳态常驻从 231MB 降到 **~130MB**(§1,单项 101MB);
- 列表隐藏态与显隐循环:峰值 107.9MB / 稳态 99.9MB,**无台阶增长**(§6);
- 贴图贴满上限 **+109MB**(改前 +189MB),且**大图更早淘汰**符合预算设计意图(§4);
- 编辑器持有量 −8.1MB、关闭后残留 +0.1MB(§2);
- 配置损坏不再静默(§7),BOM 不再导致全量回退;
- `cargo test -p nbcore` **221 passed / 0 failed**;
- 应用构建无警告,自截确认列表/设置抽屉渲染正常。

**仍需人工确认的一项**:§1.5 的**手感验收**(软件光栅下大图列表滚动、抽屉滑入、
卡片 hover、新条目淡入、遮罩框选跟手)。这是默认后端变更的唯一放行条件——
建议造 50 张图片条目滚动多屏实测;若有明显掉帧,把 `settings.rs` 的
`software_render` 改回 `false` 即可回退(单点开关)。


---

## 12. 遮罩会话 78MB 的分阶段拆解(2026-10-14 补测)

§3.2 当年只写到"多出 53.2MB 来自 backing store 与合成缓冲",没有拆开。本次在
`startCaptureSession` 的循环里临时插了三个时点的 `logMemory`(抓取后 / 构造后 /
show 后)+ 首绘两个时点(paint0 / paint400),跑 `overlay` 场景两轮,读数(私有 MB):

| 时点 | 第 1 轮 | 第 2 轮 | 增量 |
| --- | --- | --- | --- |
| capture-start(地板) | 88.0 | 88.2 | — |
| 抓取 屏1 1755×1097 | 95.9 | 96.2 | **+7.9**(= 该屏帧,吻合) |
| 抓取 屏2 **1080×1920 竖屏** | 118.5 | 122.9 | **+22.6 / +26.7**(该屏帧只有 8.3) |
| 抓取 屏3 1920×1080 | 126.4 | 130.8 | **+7.9** |
| 遮罩构造后 / showFullScreen 后 | 126.4 | 130.8 | **+0** ← 后备缓冲不在这里分配 |
| **首绘(paint0)** | **165.0** | **169.1** | **+38.6 / +38.3** |
| 取消后会话结束 | 104.0 → 400ms 后 **88.2** | 108.2 | **回落到地板,无泄漏** |

### 12.1 结论(修正 §3.2 的口径)

会话 +77.5MB = **抓取阶段 ~38.4MB** + **首绘阶段 ~38.6MB**,两段几乎各占一半。

1. **抓取阶段比"三帧之和(24.3MB)"多约 14MB,且全部集中在竖屏那一帧**
   (+22.6 vs 自身 8.3)。横屏两帧的增量与帧尺寸逐 MB 吻合。指向 xcap 的 GDI 路径
   对旋转显示器**先按原生方向抓、再软件旋转**(2~3 帧),多出的缓冲被释放但把堆
   顶高了一截。本项目 `capture.rs` 的 `ScreenCapturer` trait(决策 D3 预留的替换缝)
   正是为这种情况准备的 —— 可直接改为按桌面 DC `BitBlt`(返回的已是旋转后的像素)。
2. **首绘才是另一半的归属**:`showFullScreen()` 本身 **0 字节**,后备缓冲与绘制侧
   中间缓冲都在**第一次 paintEvent** 时分配。24.3MB 是三块全屏后备缓冲
   (窗口尺寸×4),余下 ~14MB 是绘制侧 —— `OverlayWidget::paintEvent` 在 DPR=1 下
   仍走 `painter.scale(1/scale)` + `drawImage(RGBA8888 → 后备 ARGB32_Premultiplied)`,
   格式转换与半透明罩叠加都要中间缓冲。可试:底图直接存预乘 ARGB32(与后备同格式,
   blit 免转换),代价是入库前多一次转换。
3. **§3 阶段 3(逐屏懒捕获)的预期收益按此重算仍然成立**:只抓光标屏时,会话从
   +77 降到约 +26(1 帧 8~12 + 1 块后备 8.3 + 首绘侧 ~13),与原文档估的 ~+26 吻合。
   本次拆解没有推翻它,只是说清了这 26MB 里抓取与首绘各占一半。

> 探针为临时插桩,测完已撤除(不留在产品代码里,每次会话刷 11 行日志)。
> 复跑方法:`measure.ps1 -Scenario overlay -Loops 2`(工装在
> `core/target/tmp/memlab/`,协议见 §0),插桩位置见本节 §12 表头的三个时点。

---

## 13. §6 的后续:进翻译页也改冻结(2026-10-14 落地并配对复测)

§6 的冻结只覆盖"列表页 + 隐藏",`PanelListLifecycle::sync()` 里**进翻译页仍走
`release()`**(`qml_bootstrap.cpp` 原 `else` 分支)—— 那是全工程最后一处"整批销毁委托"。
本轮把它一并改成冻结(开关更名 `kFreezeWhenOffline`,置 false 即回到 §6 原策略)。

同协议 6 轮 `列表 → 翻译 → 收起 → 列表` 配对对照(私有 MB,同一隔离数据目录):

| 轮次 | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| 回列表 HEAD | 103.6 | 134.4 | 138.9 | 140.3 | 141.5 | **140.9** |
| 回列表 改后 | 105.1 | 116.5 | 121.4 | 119.7 | 120.2 | **122.0** |
| 翻译页 HEAD | 108.1 | 119.7 | 121.5 | 128.2 | 127.6 | 127.3 |
| 翻译页 改后 | 116.7 | 120.5 | 121.5 | 121.9 | 122.1 | 123.0 |
| 隐藏态 HEAD | 102.7 | 116.1 | 121.8 | 120.9 | 119.4 | 120.4 |
| 隐藏态 改后 | 109.3 | 117.5 | 116.4 | 117.2 | 118.2 | 118.5 |

**结论**
- 收益集中在**回到列表页**这一步:HEAD 每回一次列表就把全部委托销毁重建,读数一路
  爬到 140.9 才饱和;改后不再重载(第 2 轮起 −17.9,第 6 轮 **−18.9**)。
  判据旁证:改后 `list-2` 与上一轮 `hidden-1` 读数相同 → 确实没重建。
- **代价要写清**:首轮进翻译页反而 **+8.6MB**(108.1 → 116.7),因为列表行不再被清掉;
  换来的是此后不再逐轮增长。隐藏态与翻译页稳态差在噪声带内(−1.9 / −4.3)。
- 功能回归:停在翻译页期间复制 `ZZT-BB-222` → 回列表首条即该条(冻结期脏标记记账生效),
  分类计数与缩略图正常;按被测进程窗口矩形截图确认,不受桌面其他程序干扰。
