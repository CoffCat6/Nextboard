//! 设置读写:serde 直读写 `app_config.json`(取代第一代的 tauri-plugin-store,
//! **JSON 结构不变**——外层 `{ "settings": { … } }`,老配置文件可直接读取)。
//! 自定义数据目录的迁移流程沿用第一代 docs/phase4.md(标记文件 + 重启生效)。

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use log::{info, warn};
use serde::{Deserialize, Serialize};

use crate::error::{AppError, AppResult};

const CONFIG_FILE: &str = "app_config.json";
/// 待迁移标记：位于默认数据目录，内容为迁移源目录（Phase 4）。
const DATA_MOVE_MARKER: &str = "data-move-pending.txt";

/// 长截图采样间隔的合法范围(毫秒)。
///
/// 下限 80:4K 上一次选区捕获本身要 10~20ms,再密就挤满 CPU;
/// 上限 1000:再慢快速滚动时相邻帧重叠不足,长图会缺行。
pub const MIN_STITCH_SAMPLE_MS: i64 = 80;
pub const MAX_STITCH_SAMPLE_MS: i64 = 1000;

// 颜色校验复用文件末尾已有的 is_hex_color(#RRGGBB,大小写不敏感)——
// 同一语义不该有第二份实现。

/// 标注偏好的默认值(与浮条成员的写死初值一致:红笔 / 4px / 36px)。
///
/// 用独立函数而不是字面量,是因为 `#[serde(default = "...")]` 要求路径可调用;
/// 同时让"core 默认值"与"宿主兜底值"能一眼对上(宿主读不到设置时用同一组数)。
fn default_annotation_color() -> String {
    "#E03131".into()
}
fn default_annotation_pen_size() -> i64 {
    4
}
fn default_annotation_font_size() -> i64 {
    36
}

/// 配置加载警告(进程级,只增不减直到被取走)。
///
/// 动机:配置损坏/缺键时 `load()` 会整体回退默认值——**所有设置一起丢**
/// (实测最坏是渲染后端从软件切回硬件 D3D11,稳态 +100MB 量级)。
/// 但 Rust 侧的 `log` 出口是 nbcore.log,而排障第一现场通常是宿主的
/// nextboard.log,所以警告要有一条能被宿主取走的通道(ffi 的 settings_warnings)。
static LOAD_WARNINGS: Mutex<Vec<String>> = Mutex::new(Vec::new());

/// 记录一条加载警告:同时写 nbcore.log 与进程级列表(供宿主取走)。
fn push_load_warning(msg: String) {
    warn!("{msg}");
    if let Ok(mut guard) = LOAD_WARNINGS.lock() {
        guard.push(msg);
    }
}

/// 取走并清空加载警告(宿主启动时拉一次;重复调用返回空表)。
pub fn take_warnings() -> Vec<String> {
    LOAD_WARNINGS
        .lock()
        .map(|mut guard| std::mem::take(&mut *guard))
        .unwrap_or_default()
}

/// 应用设置，与 `src/api/types.ts` 的 `Settings` 镜像（camelCase）。
/// 反序列化时缺失字段回退默认值（容器级 `default`），配置损坏不阻塞启动。
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    /// 历史条数上限（超限淘汰未固定条目）。
    pub history_limit: i64,
    /// 单张图片大小上限（MB，按原始 RGBA 字节数计）。
    pub max_image_mb: i64,
    /// 单条文本大小上限（KB）。
    pub max_text_kb: i64,
    /// 全局快捷键表（Phase 5 F15）。
    pub hotkeys: HotkeyMap,
    /// 暂停/恢复剪贴板监听。
    pub monitor_enabled: bool,
    /// 复制条目后是否自动把内容粘贴到前台窗口（默认**关闭**）。
    ///
    /// 动机：选择一条历史 → 面板收起 → 用户还得自己按一次 Ctrl+V，是"复制"
    /// 之后的固定多余动作。开启后由宿主在面板收起后向前台窗口投一次 Ctrl+V。
    ///
    /// 默认关闭是刻意的：这是**改变既有行为**的功能（老用户升级后突然开始自动
    /// 粘贴会很意外），且**管理员权限窗口**（UIPI）接收不到模拟按键 —— 那种场景
    /// 下只会静默无效果，必须让用户自己显式打开。
    pub copy_auto_paste: bool,
    /// 界面主题。
    pub theme: String,
    /// 自定义数据目录（空串 = 默认位置；Phase 4）。
    pub data_dir: String,
    /// 贴图默认不透明度（%，30 ~ 100；Phase 5）。
    pub pin_default_opacity: i64,
    /// 双击贴图窗口是否关闭（Phase 9，默认**开启**，Snipaste 习惯——该行为此前一直
    /// 硬编码开启，本设置只是让它可配置，不改变老用户感知）。关闭后双击无动作，
    /// 关闭走工具条 ✕ 或 Esc。
    pub pin_doubleclick_close: bool,
    /// 文本贴图的背景色（`#RRGGBB`）。文本条目贴图时渲染成位图，这里控制其底板配色。
    pub pin_text_bg: String,
    /// 文本贴图的文字色（`#RRGGBB`）。
    pub pin_text_color: String,
    /// 文本贴图的字号（像素，8 ~ 96）。
    pub pin_text_font_size: i64,
    /// 文本贴图的最大宽度（像素，120 ~ 2000；超出自动换行）。
    pub pin_text_max_width: i64,
    /// 文本贴图背景的不透明度（%，0 ~ 100）。
    pub pin_text_bg_opacity: i64,
    /// 一键贴图是否「图片优先」（默认**关闭**）。
    ///
    /// - 关闭（默认）：贴出**最近复制**的内容——文本与图片按最近使用时间竞争，谁最新贴谁；
    /// - 开启：只要历史里存在图片就贴最近的图片，没有图片时才回退最近文本
    ///   （第一代 clipboard-plus F17 的口径，供"一键贴图 = 贴图"习惯的用户选用）。
    pub pin_prefer_image: bool,
    /// 框选截图是否松开鼠标即自动确认（Phase 5 增强）：关闭时保留手柄微调 + Enter/双击确认。
    /// 框选松开鼠标后：
    ///   true（默认）= 直接入库 + 写剪贴板（快，一步到位）；
    ///   false       = 选区保留并浮出工具栏，就地标注/OCR/贴图/保存/再确认。
    /// 取代原先 `regionAutoConfirm` 与 `regionSkipEditor` 两个布尔量（§18.4）：
    /// 两者叠加出四种组合，其中「松开即确认 + 进编辑器」自相矛盾，且无法向用户解释。
    pub region_auto_confirm: bool,
    /// 多屏截图是否为**每块屏**都建遮罩(默认 true = 历史行为)。关闭后只对
    /// **光标所在屏**建遮罩:多屏机器上省 N-1 次全屏抓取与 N-1 份整屏冻结底图
    /// 的会话期常驻(三屏 4K 约 100MB),适合"框选永远只在一块屏内"的用户。
    pub capture_all_screens: bool,
    /// 框选时鼠标右键是否取消本次截图(默认开启;第一代无此项,按用户习惯补)。
    pub region_right_click_cancel: bool,
    /// ⚠️ **已废弃**（§18.5）：编辑器只保留唯一入口 = 右键编辑，框选链路不再有
    /// "进编辑器"这条出口（编辑能力由浮条工具栏承担）。此字段**保留仅为兼容旧配置**
    /// —— 删掉它会让旧 app_config.json 的该键变成未知字段而告警。
    /// 代码中**不再有任何行为读它**；写入时原样回传，避免旧版本回退时丢配置。
    pub region_skip_editor: bool,
    /// 框选浮条底板的**不透明度档位**(alpha,0~255;三档 179/217/242,默认 217)。
    ///
    /// 动机(用户实测反馈):浮条压在选区底部,原先底板 alpha 硬编码 242(约 95%
    /// 不透明),会把待截内容整片盖住。三档含义见 docs/17 §17.5:
    /// 242 = 不透明(底色复杂时用)、217 = 半透(默认)、179 = 极简(尽量不挡原图)。
    /// 校验限定在这三档内,不接受任意值 —— 浮条上只有三档按钮,存别的值没有意义。
    pub capture_bar_alpha: i64,
    /// 截图标注态的**偏好**(颜色 / 粗细 / 字号),跨会话记忆。
    ///
    /// 动机(用户反馈):每次进标注态都退回写死的红笔 4px —— 常用黄笔/粗线的人
    /// 每次都要重选。而同在浮条上的"底板不透明度"却会记住,口吻不一致。
    ///
    /// 存**字符串 hex + 两个整数**,而不是结构体:save_settings 是**深合并**,
    /// 标量/数组整体替换、对象逐键合并 —— 用扁平标量最不容易出现"改了一半"的状态。
    /// 校验:颜色须 #rrggbb、粗细/字号须落在 UI 提供的档位内(见 annotation_* 常量)。
    #[serde(default = "default_annotation_color")]
    pub annotation_color: String,
    #[serde(default = "default_annotation_pen_size")]
    pub annotation_pen_size: i64,
    #[serde(default = "default_annotation_font_size")]
    pub annotation_font_size: i64,
    /// 编辑器窗口几何(四元组 x,y,w,h;空 = 尚未记录,首次按默认尺寸居中)。
    ///
    /// 动机:编辑器此前固定 resize(960,720),用户按自己习惯调过尺寸,下次打开
    /// 又被重置。记录几何后"上次多大就多大"。宽高需在合理区间;x/y 允许为负
    /// (多屏时屏可能位于负坐标区)。
    pub editor_geometry: Vec<i64>,
    /// 主面板「列表」形态的尺寸(二元组 w,h;空 = 尚未记录,用内置默认 800×600)。
    ///
    /// 动机与 editor_geometry 同源:面板此前把 800×600 写死在 panel_window.cpp 里,
    /// 用户拖过一次边框、下次呼出又被重置。窗口本身支持无边框边缘缩放
    /// (WM_NCHITTEST),能调却记不住,属"能改但白改"。
    ///
    /// ⚠️ 只记**列表形态**的尺寸:翻译形态(520×660)是内容驱动的最优尺寸,
    ///    记下来会让"第二次打开翻译页"变成用户上次意外拖出的奇怪比例。
    /// ⚠️ 不记位置:x/y 由呼出时的光标位置决定(showAtCursor),记位置会让热键
    ///    呼出行为变得不可预测。
    pub panel_size: Vec<i64>,
    /// 延时截图等待秒数（托盘「延时截图」入口用；1~10，默认 3）。
    /// 用途：先按下入口，再去展开菜单/等悬浮提示出现，倒计时结束才开始框选。
    pub capture_delay_secs: i64,
    /// 滚动长截图的**拼接高度上限**(px,默认 30000)。
    ///
    /// 动机(docs/19 §4.4 / §7 R3):入库走 ingest_capture_png(PNG + base64),
    /// 30000px × 1200 宽 RGBA 原始约 137MB、base64 后再膨胀 1.33×,PNG 压缩后
    /// 仍可能几十 MB。撞上 max_image_mb(默认 20MB)就会**静默只写剪贴板、不入库**,
    /// 用户以为存下来了其实没有。故上限必须可调,且超限时宿主给出明确提示。
    ///
    /// 取值 2000~100000(stitch::MIN_MAX_HEIGHT / MAX_MAX_HEIGHT)。
    pub stitch_max_height: i64,
    /// 判定「两帧重叠」所需的最少连续相等行占比(百分比,默认 3)。
    ///
    /// 门槛过低会把重复图案(表格线、纯色段)误认成重叠 → 长期拼图**缺行**;
    /// 过高则对"一行动态内容(时间戳/光标)"过于敏感。3% 配合 12 行的绝对下限
    /// (见 stitch::MIN_OVERLAP_ROWS)覆盖 200~4000 行的常见选区高度。
    pub stitch_overlap_ratio: i64,
    /// 滚动长截图的**字节预算**(字节,默认 128MB)。
    ///
    /// 为什么要有它(docs/19 v2 §4.2):只按像素限高是不够的 —— 1200 宽 × 30000 高
    /// 的 RGBA 就是 144MB,像素数没超、内存先炸。有效上限 = min(像素上限,
    /// 字节预算 / 每行字节),字节预算更小时以它为准。
    ///
    /// 取值 16MB~1GB(stitch::MIN_MAX_BYTES / MAX_MAX_BYTES);0 = 不限制。
    pub stitch_max_bytes: i64,
    /// 长截图的**采样间隔**(毫秒,默认 150)。
    ///
    /// 旧实现是 500ms:一是每帧要整屏捕获 + 裁剪(4K 上 60MB 级拷贝),二是当时
    /// 没有"只抓选区"的通道。v2 改为直抓选区后单帧成本降到 1/8,间隔也就能收到
    /// 150ms —— 更跟手,且快速滚动时相邻帧重叠更充分(缺行更少)。
    ///
    /// 取值 80~1000ms。
    pub stitch_sample_interval_ms: i64,
    /// 截图遮罩的**空闲超时**秒数（默认 60 秒；0 = 不自动取消）。
    ///
    /// 动机（内存）：框选遮罩持有该屏的整屏冻结底图（4K 约 33MB），会话期常驻。
    /// 若用户触发截图后离开，底图会无限期占着内存；超过本时长无任何鼠标/键盘
    /// 操作即自动取消会话并释放。取 0 可关闭该行为（不推荐）。
    pub overlay_idle_secs: i64,
    /// Qt Quick 渲染后端是否使用**软件（CPU 光栅）**。默认 `false` = 硬件（D3D11）。
    ///
    /// 动机（内存）：硬件后端的场景图 + RHI(D3D11) 设备/交换链/着色器实测约占
    /// 70MB 工作集，软件后端为 0；代价是渲染改由 CPU 光栅，含大图的列表滚动与
    /// 动画可能更吃 CPU。**重启生效** —— 渲染后端必须在创建第一个 QQuickWindow
    /// 之前设定，运行期无法切换。
    pub software_render: bool,
    /// OCR 语言：`auto`（默认，按 zh-Hans-CN → en-US → 用户配置语言）/ 指定语言标签如 `en-US`。
    pub ocr_language: String,
    /// OCR 后端：`system`（默认，Windows 内置离线引擎，别名 `windows`）/ `rapidocr`（RapidOCR，
    /// 自带 PP-OCRv4 mobile 模型，不依赖系统语言包）。取值口径见 [ocr_backend]。
    pub ocr_backend: String,
    /// OCR 放大倍数（1~4）：小字号截图放大后再识别可显著提升准确率，默认 2。
    pub ocr_scale: i64,
    /// RapidOCR 识别偏好：`quality` / `balanced`（默认）/ `compact`。
    /// 映射到 det 输入的放大上界（见 `ocr_profile` 模块与 docs/12 §14.2 实测）。
    /// ⚠️ 仅对 RapidOCR 生效 —— Windows 内置引擎走的是 `ocr_scale`，不经此参数。
    pub ocr_profile: String,
    /// OCR 引擎驻留策略：`resident`（常驻）/ `idle60`（空闲 60s 销毁）/ `ephemeral`（用完即销毁）。
    /// 映射见 `ocr_session_mode` 模块。默认 `ephemeral`：最省内存，
    /// 每次识别多一次引擎构建（实测 ~250ms，相对识别本身耗时可接受）。
    pub ocr_session_mode: String,
    /// 翻译提供方：`microsoft`（默认，免密钥）/ `openai` / `custom`（Phase 7 F21~F23）。
    /// 默认取微软免授权端点，**开箱即可翻译、无需任何配置**。
    pub translate_provider: String,
    /// 翻译源语言：`auto`（默认，交由提供方检测）或语言标签。
    pub translate_source_lang: String,
    /// 翻译目标语言：语言标签，**不允许 auto**（Phase 7）。
    pub translate_target_lang: String,
    /// 翻译请求超时（秒，5 ~ 120）。
    pub translate_timeout_secs: i64,
    /// 网络代理配置（Phase 8，取代旧的单串 `translateProxy`，见 `migrate_legacy_proxy`）。
    pub proxy: ProxySettings,
    /// OpenAI 兼容提供方配置。
    pub translate_openai: OpenAiProvider,
    /// 微软翻译（Azure AI Translator v3.0）配置。
    pub translate_microsoft: MicrosoftProvider,
    /// 彩云小译（内置；默认使用官方测试 Token）。
    pub translate_caiyun: CaiyunProvider,
    /// 自定义 HTTP 提供方配置。
    pub translate_custom: CustomProvider,
}

/// 网络代理设置（Phase 8，docs/phase8-proxy.md §4.1）。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ProxySettings {
    /// `system`（跟随系统/PAC，默认）/ `none`（直连）/ `manual`（手动）。
    pub mode: String,
    /// 手动配置；仅 `mode = manual` 时生效。
    pub manual: ManualProxy,
}

impl Default for ProxySettings {
    fn default() -> Self {
        Self {
            mode: "system".into(),
            manual: ManualProxy::default(),
        }
    }
}

/// 手动代理配置。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ManualProxy {
    /// `http` / `socks5`。
    pub kind: String,
    /// 代理主机（不含 scheme，不含路径；IPv6 字面量不支持——翻译服务场景用不到）。
    pub host: String,
    /// 代理端口（1 ~ 65535）。
    pub port: i64,
    /// 用户名（可空 = 无认证）。
    pub username: String,
    /// 密码（可空）。**明文存本机配置**（决策 D8 同一取向），绝不写日志。
    pub password: String,
    /// 绕过列表（分号分隔）。**仅 HTTP 代理生效**——SOCKS5 路径（ureq）不支持，UI 置灰。
    pub bypass: String,
}

impl Default for ManualProxy {
    fn default() -> Self {
        Self {
            kind: "http".into(),
            host: String::new(),
            port: 0,
            username: String::new(),
            password: String::new(),
            bypass: String::new(),
        }
    }
}

/// OpenAI 兼容提供方配置（Phase 7 F21）；`baseUrl` 可改以适配各类兼容实现。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct OpenAiProvider {
    /// 接口根地址，如 `https://api.openai.com/v1`；请求路径固定追加 `/chat/completions`。
    pub base_url: String,
    /// API Key（用户自备，决策 U2）；**绝不写入日志**。
    pub api_key: String,
    pub model: String,
    /// 系统提示词；留空则用内置默认（强调"只输出译文"）。
    pub system_prompt: String,
    /// 采样温度（0 ~ 1），翻译场景宜偏低。
    pub temperature: f64,
}

impl Default for OpenAiProvider {
    fn default() -> Self {
        Self {
            base_url: "https://api.openai.com/v1".into(),
            api_key: String::new(),
            model: "gpt-4o-mini".into(),
            system_prompt: String::new(),
            temperature: 0.2,
        }
    }
}

/// 微软翻译（Azure AI Translator v3.0）配置（Phase 7 F22）。
///
/// 默认取**中国版**端点（21Vianet 运营，决策 U1）；国际版用户改 `endpoint` 即可。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct MicrosoftProvider {
    /// 端点基址（末尾追加 `/translatetext`）。默认取 Edge 内置翻译的**免授权**端点；
    /// 若该非公开端点有变动，可在此换成等价实现。
    pub endpoint: String,
}

impl Default for MicrosoftProvider {
    fn default() -> Self {
        Self {
            endpoint: crate::translate::microsoft::DEFAULT_ENDPOINT.into(),
        }
    }
}

/// 彩云小译（LingoCloud）配置。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CaiyunProvider {
    /// 端点基址（末尾追加 `/v1/translator`）。
    pub endpoint: String,
    /// 访问 Token，请求头形如 `X-Authorization: token <token>`。
    ///
    /// 默认填**官方测试 Token**：官方明确"不保证该 Token 的可用性"，仅供装上就能试；
    /// 持续使用请在彩云开放平台申请正式 Token（设置页有同样提示）。
    pub token: String,
}

impl Default for CaiyunProvider {
    fn default() -> Self {
        Self {
            endpoint: crate::translate::caiyun::DEFAULT_ENDPOINT.into(),
            token: crate::translate::caiyun::DEFAULT_TOKEN.into(),
        }
    }
}

/// 自定义 HTTP 提供方配置（Phase 7 F23）：模板化请求 + 响应取值路径。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CustomProvider {
    /// `GET` / `POST`（大小写不敏感）。
    pub method: String,
    pub url: String,
    /// 每行一条 `Name: Value`；值支持 `${apiKey}`。
    pub headers: String,
    /// 支持 `${text}` `${from}` `${to}` `${apiKey}` `${model}`；替换值自动做 JSON 转义。
    pub body_template: String,
    /// 轻量路径语法，如 `data.translations[0].text`。
    pub response_path: String,
    pub api_key: String,
    /// 模型/部署名（可选）：供模板里的 `${model}` 引用（如 Azure 风格接口需要）。
    pub model: String,
}

impl Default for CustomProvider {
    fn default() -> Self {
        Self {
            method: "POST".into(),
            url: String::new(),
            headers: String::new(),
            body_template: String::new(),
            response_path: String::new(),
            api_key: String::new(),
            model: String::new(),
        }
    }
}

/// 快捷键动作：与 `HotkeyMap` 字段一一对应的固定枚举（Phase 5 F15）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum HotkeyAction {
    /// 呼出/隐藏主窗口。
    ToggleMain,
    /// 矩形区域截图。
    CaptureRegion,
    /// 一键全屏截图。
    CaptureFullscreen,
    /// 一键贴图：把最近复制的可贴条目贴为浮窗（口径受 `pinPreferImage` 影响）。
    PinLatest,
    /// 打开翻译窗口（Phase 7 F24）。
    OpenTranslate,
    /// 截图翻译（Phase 7 T4）：框选 → OCR → 翻译。
    CaptureTranslate,
    /// 延时截图（F16）：倒计时后再框选，用来截菜单/悬浮提示。
    CaptureDelayed,
    /// 截图取字入列表（S5.1 同源）：框选 → OCR → 识别文本**直接存为条目**。
    CaptureOcr,
    /// 滚动长截图(docs/19):框选 → 逐帧采集 → 纵向拼接 → 入库。
    ///
    /// 与 CaptureDelayed / CaptureOcr 不同,本动作**默认预置组合键**:
    /// 直通模式(regionAutoConfirm=true)下浮条不出现,没有热键就完全够不到该功能
    /// (docs/19 §9 Q3 的用户裁定)。
    CaptureScrolling,
}

impl HotkeyAction {
    /// 全部动作，遍历注册用。
    pub const ALL: [HotkeyAction; 9] = [
        HotkeyAction::ToggleMain,
        HotkeyAction::CaptureRegion,
        HotkeyAction::CaptureFullscreen,
        HotkeyAction::PinLatest,
        HotkeyAction::OpenTranslate,
        HotkeyAction::CaptureTranslate,
        HotkeyAction::CaptureDelayed,
        HotkeyAction::CaptureOcr,
        HotkeyAction::CaptureScrolling,
    ];
}

/// 多快捷键注册表（Phase 5 F15）：固定枚举键而非自由 map，防拼写漂移。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct HotkeyMap {
    /// 呼出/隐藏主窗口。
    pub toggle_main: String,
    /// 矩形区域截图。
    pub capture_region: String,
    /// 一键全屏截图。
    pub capture_fullscreen: String,
    /// 一键贴图（口径受 `pinPreferImage` 影响）。
    pub pin_latest: String,
    /// 打开翻译窗口（Phase 7）。
    pub open_translate: String,
    /// 截图翻译（Phase 7 T4）：框选 → OCR → 翻译。
    pub capture_translate: String,
    /// 延时截图（F16）。**默认留空**：这个动作属于"按需绑定"，由用户自己决定组合键。
    pub capture_delayed: String,
    /// 截图取字入列表。**默认留空**，同上。
    pub capture_ocr: String,
    /// 滚动长截图(docs/19)。默认 Ctrl+Shift+L（L = Long）。
    pub capture_scrolling: String,
}

impl Default for HotkeyMap {
    fn default() -> Self {
        Self {
            toggle_main: "Ctrl+Shift+V".into(),
            capture_region: "Ctrl+Shift+A".into(),
            capture_fullscreen: "Ctrl+Shift+Q".into(),
            pin_latest: "Ctrl+Shift+P".into(),
            open_translate: "Ctrl+Shift+T".into(),
            // 与既有 5 个默认键均不冲突（V/A/Q/P/T）
            capture_translate: "Ctrl+Shift+E".into(),
            // 新增动作不预置组合键：空串 = 不绑定，用户想用再自己录
            capture_delayed: String::new(),
            capture_ocr: String::new(),
            // 长截图是**独立入口**:直通模式下浮条不出现,不给默认键就等于没这个功能
            // (docs/19 §9 Q3)。L = Long,与既有 V/A/Q/P/T/E 均不冲突。
            capture_scrolling: "Ctrl+Shift+L".into(),
        }
    }
}

impl HotkeyMap {
    /// 取动作对应的快捷键描述串（如 "Ctrl+Shift+A"）。
    pub fn get(&self, action: HotkeyAction) -> &str {
        match action {
            HotkeyAction::ToggleMain => &self.toggle_main,
            HotkeyAction::CaptureRegion => &self.capture_region,
            HotkeyAction::CaptureFullscreen => &self.capture_fullscreen,
            HotkeyAction::PinLatest => &self.pin_latest,
            HotkeyAction::OpenTranslate => &self.open_translate,
            HotkeyAction::CaptureTranslate => &self.capture_translate,
            HotkeyAction::CaptureDelayed => &self.capture_delayed,
            HotkeyAction::CaptureOcr => &self.capture_ocr,
            HotkeyAction::CaptureScrolling => &self.capture_scrolling,
        }
    }
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            history_limit: 5000,
            max_image_mb: 20,
            max_text_kb: 1024,
            hotkeys: HotkeyMap::default(),
            monitor_enabled: true,
            copy_auto_paste: false,
            theme: "system".into(),
            data_dir: String::new(),
            pin_default_opacity: 100,
            pin_doubleclick_close: true,
            pin_text_bg: "#26272e".into(),
            pin_text_color: "#dfe1e8".into(),
            pin_text_font_size: 15,
            pin_text_max_width: 520,
            pin_text_bg_opacity: 92,
            // 默认"最近复制优先"：复制文字后按热键即贴该文字（关闭第一代的"图片优先"）
            pin_prefer_image: false,
            region_auto_confirm: true,
            capture_all_screens: true,
            region_right_click_cancel: true,
            capture_bar_alpha: 217,        // 半透档(docs/17 §17.5):默认不挡原图又看得清按钮
            annotation_color: default_annotation_color(),
            annotation_pen_size: default_annotation_pen_size(),
            annotation_font_size: default_annotation_font_size(),
            editor_geometry: Vec::new(),
            panel_size: Vec::new(),   // 空 = 尚未记录,首次按默认尺寸居中
            // 默认"松手即复制"：截图后立刻可粘贴，不经过编辑器
            region_skip_editor: true,
            capture_delay_secs: 3,
            // 长截图上限 30000px(用户 2026-10-17 拍板,docs/19 §9 Q4)。
            // 30000px 已能覆盖"长网页/长聊天记录",再高会让 PNG 撞 max_image_mb。
            stitch_max_height: 30_000,
            stitch_overlap_ratio: 3,
            // 128MB:1200 宽约合 26000 行,与 30000px 的像素上限基本同量级,
            // 但 4K 宽选区会被它先钳住 —— 那正是它存在的意义。
            stitch_max_bytes: 128 * 1024 * 1024,
            stitch_sample_interval_ms: 150,
            // 实测遮罩会话常驻 +77.5MB(3 屏:冻结底图 + 全屏窗口 backing store + 合成缓冲),
            // 而"误触截图热键后走开"是常见场景。10 分钟太长 —— 60s 足够完成框选,
            // 超时自动取消(与手动 Esc 同语义)即回收。0 = 不自动取消仍受支持。
            overlay_idle_secs: 60,
            // 默认软件渲染(CPU 光栅):实测比硬件(D3D11)后端省 ~55MB 首帧 / ~101MB 稳态
            // (隐藏态 231MB → 130MB),是唯一"开箱即超预算"的来源。代价是滚动/动画吃 CPU,
            // 需过手感验收(文档 docs/06-memory-optimization.md §1.5);可用设置项或
            // --hardware 切回硬件后端。
            software_render: true,
            ocr_language: "auto".into(),
            ocr_backend: "system".into(),
            // 放大 2 倍：实测对 12px 代码截图明显改善且耗时可控（docs/phase6.md §3.6）
            ocr_scale: 2,
            // 识别偏好默认「均衡」（docs/12 §14.2：小图内存 215→116M 且指纹不变）
            ocr_profile: "balanced".into(),
            // 引擎驻留默认「用完即销毁」：最省内存；重建 ~250ms 相对识别耗时可感知度低
            ocr_session_mode: "ephemeral".into(),
            translate_provider: "microsoft".into(),
            translate_source_lang: "auto".into(),
            translate_target_lang: "zh-Hans".into(),
            translate_timeout_secs: 30,
            proxy: ProxySettings::default(),
            translate_openai: OpenAiProvider::default(),
            translate_microsoft: MicrosoftProvider::default(),
            translate_caiyun: CaiyunProvider::default(),
            translate_custom: CustomProvider::default(),
        }
    }
}

impl Settings {
    /// 校验取值范围，不通过时返回 `InvalidInput`（由前端提示）。
    pub fn validate(&self) -> AppResult<()> {
        if !(100..=100_000).contains(&self.history_limit) {
            return Err(AppError::InvalidInput(
                "历史上限须在 100 ~ 100000 之间".into(),
            ));
        }
        if !(1..=100).contains(&self.max_image_mb) {
            return Err(AppError::InvalidInput(
                "图片大小上限须在 1 ~ 100 MB 之间".into(),
            ));
        }
        if !(1..=4096).contains(&self.max_text_kb) {
            return Err(AppError::InvalidInput(
                "文本大小上限须在 1 ~ 4096 KB 之间".into(),
            ));
        }
        if !(1..=10).contains(&self.capture_delay_secs) {
            return Err(AppError::InvalidInput(
                "延时截图等待须在 1 ~ 10 秒之间".into(),
            ));
        }
        // 0 = 不自动取消；上限 1 小时（再长等于没有保护）
        if !(0..=3600).contains(&self.overlay_idle_secs) {
            return Err(AppError::InvalidInput(
                "遮罩空闲超时须在 0 ~ 3600 秒之间（0 = 不自动取消）".into(),
            ));
        }
        // 滚动长截图(docs/19 §6):上限范围与 stitch 模块的常量同源,
        // 这里写死数字会让"设置页能填、核心拒绝"或反之 —— 故用常量引用。
        if !(crate::stitch::MIN_MAX_HEIGHT..=crate::stitch::MAX_MAX_HEIGHT)
            .contains(&self.stitch_max_height)
        {
            return Err(AppError::InvalidInput(format!(
                "长截图高度上限须在 {} ~ {} px 之间",
                crate::stitch::MIN_MAX_HEIGHT,
                crate::stitch::MAX_MAX_HEIGHT
            )));
        }
        // 重叠门槛 1%~20%:低于 1% 等于没有门槛(重复图案会天天误判),
        // 高于 20% 则要求近五分之一的画面重叠 —— 用户每次都得小心翼翼地小步滚。
        if !(1..=20).contains(&self.stitch_overlap_ratio) {
            return Err(AppError::InvalidInput(
                "长截图重叠门槛须在 1 ~ 20 之间(百分比)".into(),
            ));
        }
        // 字节预算(docs/19 v2 §4.2):0 = 不限制;否则须在 16MB~1GB。
        // 有下限是因为"预算比一屏还小"没有意义(核心会按至少容纳一帧处理),
        // 有上限是因为再高必然先撞 max_image_mb(入库那张 PNG 的限额)。
        if self.stitch_max_bytes != 0
            && !(crate::stitch::MIN_MAX_BYTES..=crate::stitch::MAX_MAX_BYTES)
                .contains(&self.stitch_max_bytes)
        {
            return Err(AppError::InvalidInput(format!(
                "长截图字节预算须为 0(不限制)或 {} ~ {} 字节",
                crate::stitch::MIN_MAX_BYTES,
                crate::stitch::MAX_MAX_BYTES
            )));
        }
        // 采样间隔 80~1000ms:低于 80ms 单帧捕获本身就要 10~20ms(4K),
        // 节拍会挤满 CPU;高于 1000ms 快速滚动时相邻帧重叠不足,长图缺行。
        if !(crate::settings::MIN_STITCH_SAMPLE_MS..=crate::settings::MAX_STITCH_SAMPLE_MS)
            .contains(&self.stitch_sample_interval_ms)
        {
            return Err(AppError::InvalidInput(format!(
                "长截图采样间隔须在 {} ~ {} ms 之间",
                crate::settings::MIN_STITCH_SAMPLE_MS,
                crate::settings::MAX_STITCH_SAMPLE_MS
            )));
        }
        // 快捷键：**允许留空**（空 = 不绑定该动作，用户可只绑自己需要的几个）；
        // 非空项仍需合法，且两两不重复（相同组合键不能绑定多个动作）。
        let mut seen: Vec<&str> = Vec::new();
        for action in HotkeyAction::ALL {
            let hotkey = self.hotkeys.get(action).trim();
            if hotkey.is_empty() {
                continue;
            }
            if !is_valid_hotkey(hotkey) {
                return Err(AppError::InvalidInput(format!(
                    "快捷键「{hotkey}」需至少一个修饰键（Ctrl/Alt/Shift/Super），单按 F1~F24 除外"
                )));
            }
            if seen.contains(&hotkey) {
                return Err(AppError::InvalidInput(format!(
                    "快捷键「{hotkey}」被多个动作重复使用"
                )));
            }
            seen.push(hotkey);
        }
        if !matches!(self.theme.as_str(), "system" | "light" | "dark") {
            return Err(AppError::InvalidInput("主题取值无效".into()));
        }
        if !self.data_dir.trim().is_empty() && !Path::new(self.data_dir.trim()).is_absolute() {
            return Err(AppError::InvalidInput("数据目录必须是绝对路径".into()));
        }
        if !(30..=100).contains(&self.pin_default_opacity) {
            return Err(AppError::InvalidInput(
                "贴图默认不透明度须在 30 ~ 100 之间".into(),
            ));
        }
        // 浮条底板不透明度:只接受三档(179/217/242)。刻意不放开任意值 ——
        // 浮条上只有三档按钮,存进来的其它值没有对应 UI,会变成"设置对不上界面"。
        if ![179, 217, 242].contains(&self.capture_bar_alpha) {
            return Err(AppError::InvalidInput(
                "浮条不透明度只支持 179 / 217 / 242 三档".into(),
            ));
        }
        // 标注偏好:颜色须 #rrggbb(界面只提供调色板 + 取色器,取色器也只出 hex);
        // 粗细/字号必须是 UI 档位里的值 —— 存了别的值界面上没有对应选中项,
        // 会变成"设置与界面不一致"(与上面 alpha 三档同一理由)。
        if !is_hex_color(&self.annotation_color) {
            return Err(AppError::InvalidInput(
                "标注颜色须为 #rrggbb 形式".into(),
            ));
        }
        // 档位必须与 UI 的 strokeSizes()/textFontSizes() 逐项一致
        // (app/src/editor/annotation_engine.cpp:{2,4,8} / {24,36,48})——
        // 对不上就是"core 收下了一个界面上选不到的值"。
        if ![2, 4, 8].contains(&self.annotation_pen_size) {
            return Err(AppError::InvalidInput(
                "标注粗细只支持 2 / 4 / 8 三档".into(),
            ));
        }
        if ![24, 36, 48].contains(&self.annotation_font_size) {
            return Err(AppError::InvalidInput(
                "标注字号只支持 24 / 36 / 48 三档".into(),
            ));
        }
        // 编辑器几何:空(未记录)合法;非空必须是四个数,且宽高在合理区间。
        // x/y 不限制符号 —— 多屏时副屏可以位于负坐标区。
        if !self.editor_geometry.is_empty() {
            if self.editor_geometry.len() != 4 {
                return Err(AppError::InvalidInput(
                    "编辑器几何须为 [x, y, w, h] 四个数".into(),
                ));
            }
            let (w, h) = (self.editor_geometry[2], self.editor_geometry[3]);
            if !(320..=20000).contains(&w) || !(240..=20000).contains(&h) {
                return Err(AppError::InvalidInput(
                    "编辑器窗口尺寸超出合理范围".into(),
                ));
            }
        }
        // 文本贴图样式：字号/最大宽度/背景不透明度范围 + 两个颜色须为 #RRGGBB
        if !(8..=96).contains(&self.pin_text_font_size) {
            return Err(AppError::InvalidInput(
                "文本贴图字号须在 8 ~ 96 之间".into(),
            ));
        }
        if !(120..=2000).contains(&self.pin_text_max_width) {
            return Err(AppError::InvalidInput(
                "文本贴图最大宽度须在 120 ~ 2000 之间".into(),
            ));
        }
        if !(0..=100).contains(&self.pin_text_bg_opacity) {
            return Err(AppError::InvalidInput(
                "文本贴图背景不透明度须在 0 ~ 100 之间".into(),
            ));
        }
        for (label, value) in [
            ("背景色", &self.pin_text_bg),
            ("文字色", &self.pin_text_color),
        ] {
            if !is_hex_color(value) {
                return Err(AppError::InvalidInput(format!(
                    "文本贴图{label}须为 #RRGGBB 形式"
                )));
            }
        }
        // OCR：后端枚举 + 语言标签格式 + 放大倍数范围
        if !ocr_backend::is_supported(&self.ocr_backend) {
            return Err(AppError::InvalidInput(
                "OCR 后端只支持 system（内置离线引擎）或 rapidocr（RapidOCR）".into(),
            ));
        }
        if !ocr_profile::is_supported(&self.ocr_profile) {
            return Err(AppError::InvalidInput(
                "OCR 识别偏好只支持 quality / balanced / compact".into(),
            ));
        }
        if !ocr_session_mode::is_supported(&self.ocr_session_mode) {
            return Err(AppError::InvalidInput(
                "OCR 引擎驻留只支持 resident / idle60 / ephemeral".into(),
            ));
        }
        let language = self.ocr_language.trim();
        if !language.is_empty()
            && !language.eq_ignore_ascii_case("auto")
            && !language
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
        {
            return Err(AppError::InvalidInput(
                "OCR 语言只接受 auto 或语言标签（如 en-US、zh-Hans-CN）".into(),
            ));
        }
        if !(1..=4).contains(&self.ocr_scale) {
            return Err(AppError::InvalidInput("OCR 放大倍数须在 1 ~ 4 之间".into()));
        }
        // 面板尺寸:空 = 未记录(用内置默认);非空必须是二元组且在合理区间。
        // 下限与 panel_window.cpp 的 PANEL_MIN_W/H 同源语义(低于它头部会挤变形),
        // 上限给足 4K 屏;这里**不引用 C++ 常量**(跨语言无共享头),故注释互指。
        if !self.panel_size.is_empty() {
            if self.panel_size.len() != 2 {
                return Err(AppError::InvalidInput(
                    "面板尺寸须为 [宽, 高] 二元组".into(),
                ));
            }
            let (w, h) = (self.panel_size[0], self.panel_size[1]);
            if !(620..=10000).contains(&w) || !(380..=10000).contains(&h) {
                return Err(AppError::InvalidInput(
                    "面板宽高须在 620×380 ~ 10000×10000 之间".into(),
                ));
            }
        }
        self.validate_translate()?;
        Ok(())
    }

    /// 翻译相关设置的校验（Phase 7，docs/phase7.md §5.3）。
    ///
    /// 只校验"格式与范围"，不校验 Key 是否有效（有效性只能在真实请求里发现）；
    /// 未启用的提供方允许留空，避免用户切回 OpenAI 时被无关的 custom 配置卡住保存。
    fn validate_translate(&self) -> AppResult<()> {
        if !matches!(
            self.translate_provider.trim().to_ascii_lowercase().as_str(),
            "" | "openai" | "microsoft" | "caiyun" | "custom"
        ) {
            return Err(AppError::InvalidInput(
                "翻译提供方只支持 openai（AI 翻译）、microsoft、caiyun 或 custom".into(),
            ));
        }
        if !crate::translate::lang::is_valid_tag(&self.translate_source_lang) {
            return Err(AppError::InvalidInput(
                "翻译源语言只接受 auto 或语言标签（如 en、zh-Hans）".into(),
            ));
        }
        if !crate::translate::lang::is_valid_target(&self.translate_target_lang) {
            return Err(AppError::InvalidInput(
                "翻译目标语言不能为空或 auto，须是语言标签（如 zh-Hans、en）".into(),
            ));
        }
        if !(5..=120).contains(&self.translate_timeout_secs) {
            return Err(AppError::InvalidInput(
                "翻译请求超时须在 5 ~ 120 秒之间".into(),
            ));
        }
        // 代理（Phase 8）：模式 + 手动配置（详见 validate_proxy_manual）
        if !matches!(
            self.proxy.mode.trim().to_ascii_lowercase().as_str(),
            "" | "system" | "none" | "manual"
        ) {
            return Err(AppError::InvalidInput(
                "代理模式只支持 system（跟随系统）、none（直连）或 manual".into(),
            ));
        }
        if self.proxy.mode.trim().eq_ignore_ascii_case("manual") {
            validate_proxy_manual(&self.proxy.manual)?;
        }
        // OpenAI：地址与温度；Key 是否填写留给调用时判断（便于先配好地址再填 Key）
        let openai = &self.translate_openai;
        validate_endpoint("OpenAI 接口地址", &openai.base_url)?;
        if !(0.0..=1.0).contains(&openai.temperature) {
            return Err(AppError::InvalidInput(
                "OpenAI temperature 须在 0 ~ 1 之间".into(),
            ));
        }
        // 微软：仅端点（免授权端点无需密钥/区域）
        validate_endpoint("微软翻译接口地址", &self.translate_microsoft.endpoint)?;
        // 彩云小译：仅端点（Token 在发请求时校验，缺失时给可读错误）
        validate_endpoint("彩云小译接口地址", &self.translate_caiyun.endpoint)?;
        // 自定义：仅在使用时才要求必填，避免"配了模板但当前用 OpenAI"被拦
        let custom = &self.translate_custom;
        let method = custom.method.trim().to_ascii_uppercase();
        if !matches!(method.as_str(), "" | "GET" | "POST") {
            return Err(AppError::InvalidInput(
                "自定义提供方只支持 GET 或 POST".into(),
            ));
        }
        let custom_active = self
            .translate_provider
            .trim()
            .eq_ignore_ascii_case("custom");
        if custom_active {
            if custom.url.trim().is_empty() {
                return Err(AppError::InvalidInput(
                    "使用自定义提供方时必须填写接口地址".into(),
                ));
            }
            if custom.response_path.trim().is_empty() {
                return Err(AppError::InvalidInput(
                    "使用自定义提供方时必须填写响应取值路径（如 data.translations[0].text）".into(),
                ));
            }
            if custom.body_template.trim().is_empty() && method != "GET" {
                return Err(AppError::InvalidInput(
                    "使用自定义提供方时 POST 必须填写请求体模板".into(),
                ));
            }
        }
        // 地址格式始终校验（填了就必须合法）
        validate_endpoint("自定义接口地址", &custom.url)?;
        // 响应路径填了就校验语法，避免保存成功、翻译时才报错
        if !custom.response_path.trim().is_empty() {
            crate::translate::validate_response_path(&custom.response_path)?;
        }
        Ok(())
    }
}

/// 校验接口地址：允许为空（表示"用内置默认"，由调用方回填），填了则必须是 http(s) 绝对地址。
fn validate_endpoint(label: &str, value: &str) -> AppResult<()> {
    let value = value.trim();
    if value.is_empty() {
        return Ok(());
    }
    if !value.starts_with("http://") && !value.starts_with("https://") {
        return Err(AppError::InvalidInput(format!(
            "{label}必须以 http:// 或 https:// 开头"
        )));
    }
    if value.contains(' ') {
        return Err(AppError::InvalidInput(format!("{label}不能包含空格")));
    }
    Ok(())
}

/// 校验手动代理配置并把合法值转成传输层的 [`crate::translate::http::ProxyEndpoint`]。
///
/// **字符白名单是安全约束而非格式洁癖**（docs/phase8-proxy.md §4.3）：代理串会被拼进
/// `socks5://[user:pass@]host:port` 或 WinHTTP 的 `host:port`——若 host 里允许 `/`、`@`、
/// 空格，或 username 允许 `@`/`:`，攻击者（或误输入）就能让代理串指向预期外的主机
/// （如 `socks5://user@evil:1080`），与 Phase 7 D14「按上下文转义」是同一类考虑。
pub fn validate_proxy_manual(
    manual: &ManualProxy,
) -> Result<crate::translate::http::ProxyEndpoint, AppError> {
    use crate::translate::http::{ProxyAuth, ProxyEndpoint, ProxyKind};

    let kind = match manual.kind.trim().to_ascii_lowercase().as_str() {
        "http" => ProxyKind::Http,
        "socks5" => ProxyKind::Socks5,
        other => {
            return Err(AppError::InvalidInput(format!(
                "代理类型只支持 http 或 socks5，收到「{other}」"
            )));
        }
    };
    let host = manual.host.trim();
    if host.is_empty() {
        return Err(AppError::InvalidInput("代理服务器地址不能为空".into()));
    }
    if host.contains(':') {
        return Err(AppError::InvalidInput(
            "代理服务器地址不支持 IPv6 字面量（请改用主机名或 IPv4）；\
             冒号只用于「主机:端口」写法中分隔端口"
                .into(),
        ));
    }
    if !host
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
    {
        return Err(AppError::InvalidInput(
            "代理服务器地址只能包含字母、数字与 . - _".into(),
        ));
    }
    if !(1..=65535).contains(&manual.port) {
        return Err(AppError::InvalidInput("代理端口须在 1 ~ 65535 之间".into()));
    }
    // 用户名/密码：允许为空（无认证代理）；填了则不得破坏代理串结构。
    // 密码同样禁 @ : /——SOCKS5 路径会把凭据拼进 `socks5://user:pass@host`，
    // 密码里的 @ 或 : 会让 URI 解析无法确定"主机从哪开始"（未做 percent-encoding，
    // 这是当前实现的有意简化，宁可在保存时拒绝也不让请求悄悄发往错误主机）。
    let username = manual.username.trim();
    let password = manual.password.clone();
    if username.chars().any(|c| matches!(c, '@' | ':' | '/' | ' ')) {
        return Err(AppError::InvalidInput(
            "代理用户名不能包含 @ : / 或空格".into(),
        ));
    }
    if password.chars().any(|c| matches!(c, '@' | ':' | '/' | ' ')) {
        return Err(AppError::InvalidInput(
            "代理密码不能包含 @ : / 或空格".into(),
        ));
    }
    let bypass = manual.bypass.trim();
    if bypass.split([';', ',']).any(|item| item.contains(' ')) {
        return Err(AppError::InvalidInput(
            "绕过列表各项之间用分号分隔，项内不能有空格".into(),
        ));
    }
    Ok(ProxyEndpoint {
        kind,
        host: host.to_owned(),
        port: manual.port as u16,
        auth: if username.is_empty() {
            None
        } else {
            Some(ProxyAuth {
                username: username.to_owned(),
                password,
            })
        },
    })
}

/// 校验单个快捷键组合是否可接受。
///
/// 规则：必须含至少一个修饰键（Ctrl/Alt/Shift/Super），或为单按 `F1`~`F24`。
/// 裸字母/数字会被 global-hotkey **成功注册并抢占全系统按键**（用户一按就触发，
/// 且吃掉其它程序的输入），属高危误配置，因此在保存前直接拒绝。
fn is_valid_hotkey(combo: &str) -> bool {
    let parts: Vec<&str> = combo
        .split('+')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .collect();
    match parts.len() {
        0 => false,
        1 => is_function_key(parts[0]),
        _ => true,
    }
}

/// `#RRGGBB`（大小写不敏感）。
fn is_hex_color(value: &str) -> bool {
    let bytes = value.as_bytes();
    bytes.len() == 7 && bytes[0] == b'#' && bytes[1..].iter().all(u8::is_ascii_hexdigit)
}

/// `F1`~`F24`（大小写不敏感）。
fn is_function_key(token: &str) -> bool {
    let upper = token.to_ascii_uppercase();
    match upper.strip_prefix('F').and_then(|n| n.parse::<u8>().ok()) {
        Some(number) => (1..=24).contains(&number),
        None => false,
    }
}

/// 当前生效的数据目录：设置了自定义目录则用它，否则用默认目录。
pub fn effective_data_dir(settings: &Settings, default_dir: &Path) -> PathBuf {
    let custom = settings.data_dir.trim();
    if custom.is_empty() {
        default_dir.to_path_buf()
    } else {
        PathBuf::from(custom)
    }
}

/// 写入待迁移标记（源目录 = 当前生效目录），供下次启动执行搬迁。
pub fn write_pending_move(default_dir: &Path, source: &Path) -> AppResult<()> {
    let marker = default_dir.join(DATA_MOVE_MARKER);
    std::fs::write(&marker, source.to_string_lossy().as_bytes())?;
    info!("已写入数据迁移标记: {} -> 待启动时处理", source.display());
    Ok(())
}

/// 处理待迁移标记：把源目录的数据库与 blobs 复制到当前生效目录，然后清除标记。
/// 目标已有数据库时跳过（防覆盖更新的数据）。失败仅告警，应用继续用现有数据。
pub fn take_pending_move(default_dir: &Path, target: &Path) {
    let marker = default_dir.join(DATA_MOVE_MARKER);
    let Ok(source) = std::fs::read_to_string(&marker) else {
        return;
    };
    let source = PathBuf::from(source.trim());
    let _ = std::fs::remove_file(&marker);
    if !source.join("clipboard.db").try_exists().unwrap_or(false)
        || target.join("clipboard.db").try_exists().unwrap_or(false)
    {
        info!("数据迁移跳过（源无数据或目标已有数据）");
        return;
    }
    match move_data(&source, target) {
        Ok(()) => info!(
            "数据已迁移到 {}（源目录 {} 保留）",
            target.display(),
            source.display()
        ),
        Err(err) => warn!("数据迁移失败，继续使用现有数据: {err}"),
    }
}

/// 复制数据库（含 WAL 伴随文件）与 blobs 目录到目标位置；不删除源目录。
fn move_data(source: &Path, target: &Path) -> AppResult<()> {
    std::fs::create_dir_all(target)?;
    for name in ["clipboard.db", "clipboard.db-wal", "clipboard.db-shm"] {
        let from = source.join(name);
        if from.try_exists().unwrap_or(false) {
            std::fs::copy(&from, target.join(name))?;
        }
    }
    let src_blobs = source.join("blobs");
    if src_blobs.try_exists().unwrap_or(false) {
        copy_dir_recursive(&src_blobs, &target.join("blobs"))?;
    }
    Ok(())
}

fn copy_dir_recursive(src: &Path, dst: &Path) -> AppResult<()> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let ty = entry.file_type()?;
        let to = dst.join(entry.file_name());
        if ty.is_dir() {
            copy_dir_recursive(&entry.path(), &to)?;
        } else {
            std::fs::copy(entry.path(), &to)?;
        }
    }
    Ok(())
}

/// 从数据目录下的 `app_config.json` 读取设置;文件缺失或损坏时回退默认值。
///
/// JSON 结构与第一代 tauri-plugin-store 的落盘格式完全一致(外层 `{"settings": …}`),
/// 老用户的配置文件无需任何转换。
pub fn load(data_dir: &Path) -> AppResult<Settings> {
    let path = data_dir.join(CONFIG_FILE);
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Ok(Settings::default());
    };
    // 外部工具(记事本另存、PowerShell `Set-Content -Encoding UTF8` 等)可能写入 BOM,
    // serde_json 会因此把**整份配置**判为非法并全部回退默认值(实测渲染后端被切回
    // 硬件 D3D11,稳态 +100MB 量级)。BOM 只是前缀噪声,剥掉再解析即可避免无谓回退。
    let text = text.trim_start_matches('\u{feff}');
    let Ok(mut root) = serde_json::from_str::<serde_json::Map<String, serde_json::Value>>(text)
    else {
        push_load_warning(format!(
            "配置文件解析失败,已回退默认设置(渲染后端/热键/翻译配置等会一并重置): {path:?}"
        ));
        return Ok(Settings::default());
    };
    let Some(mut value) = root.remove("settings") else {
        push_load_warning(format!(
            "配置文件缺少 settings 键,已回退默认设置(后果同上): {path:?}"
        ));
        return Ok(Settings::default());
    };
    migrate_legacy_hotkeys(&mut value);
    migrate_legacy_proxy(&mut value);
    repair_blank_microsoft_endpoint(&mut value);
    repair_removed_ocr_backend(&mut value);
    Ok(serde_json::from_value(value).unwrap_or_else(|err| {
        push_load_warning(format!("配置字段解析失败,已回退默认设置: {err}"));
        Settings::default()
    }))
}

/// 修复：微软翻译端点留空时补上默认值（免授权端点）。
///
/// 背景：默认提供方改为微软免密钥端点后，旧配置里 `translateMicrosoft.endpoint`
/// 可能是空串（早期 UI 只把默认地址当 placeholder、没写进输入框，一保存就是空）。
/// 空端点会拼出**相对地址**（`/translatetext?…`）导致请求必然失败；
/// 在加载时统一补齐，设置页的输入栏里也会**直接显示**默认地址，而不是只显示灰字提示。
fn repair_blank_microsoft_endpoint(value: &mut serde_json::Value) {
    let Some(root) = value.as_object_mut() else {
        return;
    };
    let Some(ms) = root
        .get_mut("translateMicrosoft")
        .and_then(|node| node.as_object_mut())
    else {
        return;
    };
    let blank = ms
        .get("endpoint")
        .and_then(|node| node.as_str())
        .is_none_or(|text| text.trim().is_empty());
    if blank {
        ms.insert(
            "endpoint".into(),
            serde_json::Value::String(crate::translate::microsoft::DEFAULT_ENDPOINT.into()),
        );
    }
}

/// OCR 后端取值的**单一来源**。
///
/// ⚠️ 这是本文件内两处消费点的**唯一口径**,必须同时覆盖:
///   - `Settings::validate()` —— 校验用户提交的值;
///   - `repair_removed_ocr_backend()` —— 加载时归一已移除的值。
///
/// 另需与 `crate::ocr::Backend::parse` 认识的取值保持一致(由单测
/// `every_supported_backend_is_recognized_by_parse` 交叉断言)。
///
/// 历史教训(docs/08a §1.2.3):这两处曾是同一口径的**两份副本** ——
/// 新增引擎时只改一处,会导致「设置页能选、但加载时被静默归零回 system」。
/// 因此抽成本模块,新增后端只需改这里一处。
pub mod ocr_backend {
    /// 后端标识:Windows 内置 OCR(默认,别名 `windows`)。
    pub const SYSTEM: &str = "system";
    /// 后端标识:RapidOCR(PP-OCRv4 mobile,自带模型)。
    pub const RAPIDOCR: &str = "rapidocr";

    /// 该值是否为**受支持**的 OCR 后端(空串等价于默认值)。
    pub fn is_supported(value: &str) -> bool {
        matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "" | "system" | "windows" | "rapidocr" | "rapid"
        )
    }
}

/// 识别偏好档位（单一事实源）。
///
/// 模式与 [`ocr_backend`] 完全一致：validate 与加载修复共用 [`is_supported`]，
/// 避免出现「设置页能选、加载被归零」的双副本漂移（docs/08a §1.2.3 教训）。
pub mod ocr_profile {
    /// 质量优先：det 放大上界 3.0 / 最长边 2560（与 docs §7 加固后的默认一致）。
    pub const QUALITY: &str = "quality";
    /// 均衡（默认）：2.0 / 2048。实测小图峰值 215→116M 且识别指纹不变。
    pub const BALANCED: &str = "balanced";
    /// 内存优先：1.5 / 1600。实测小图峰值 82M；密集小字界面可能漏行。
    pub const COMPACT: &str = "compact";

    /// 该值是否为受支持的档位（空串等价于默认 `balanced`）。
    pub fn is_supported(value: &str) -> bool {
        matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "" | "quality" | "balanced" | "compact"
        )
    }

    /// 归一（空串 → 默认档）。
    pub fn normalize(value: &str) -> &'static str {
        match value.trim().to_ascii_lowercase().as_str() {
            "quality" => QUALITY,
            "compact" => COMPACT,
            _ => BALANCED,
        }
    }

    /// 档位 → (det 放大倍数上界, det 输入最长边上界)。
    /// 实测依据 docs/12 §14.2：三档下 mixed14 指纹不变、内存 215/116/82M。
    pub fn det_caps(value: &str) -> (f32, usize) {
        match normalize(value) {
            QUALITY => (3.0, 2560),
            COMPACT => (1.5, 1600),
            _ => (2.0, 2048),
        }
    }
}

/// 引擎驻留策略（单一事实源）。
pub mod ocr_session_mode {
    /// 常驻：引擎不销毁，后台常驻约 40MB。
    pub const RESIDENT: &str = "resident";
    /// 空闲 60 秒销毁。
    pub const IDLE60: &str = "idle60";
    /// 用完即销毁（默认）：最省内存，每次识别多一次引擎构建（~250ms 实测）。
    pub const EPHEMERAL: &str = "ephemeral";

    /// 该值是否受支持（空串等价于默认 `ephemeral`）。
    pub fn is_supported(value: &str) -> bool {
        matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "" | "resident" | "idle60" | "ephemeral"
        )
    }

    /// 归一（空串 → 默认）。
    pub fn normalize(value: &str) -> &'static str {
        match value.trim().to_ascii_lowercase().as_str() {
            "resident" => RESIDENT,
            "idle60" => IDLE60,
            _ => EPHEMERAL,
        }
    }
}

/// 已移除 OCR 后端的配置修复：`ocrBackend` 不是当前支持的值时归一为 `system`。
///
/// 背景：Tesseract 后端要求用户自行安装并放进 PATH，而设置页从未提供后端选择器，
/// 只有手改 settings.json 才可能留下 `"tesseract"`。归一必须在**加载时**做：
/// 设置抽屉保存时回传的是整份已加载对象（`SettingsDrawer.qml` 的 `const s = cur.data`），
/// 留着这个值会让 `validate()` 拒绝**任何一次保存**，而报错指向用户从未碰过的字段。
fn repair_removed_ocr_backend(value: &mut serde_json::Value) {
    let Some(obj) = value.as_object_mut() else {
        return;
    };
    let Some(current) = obj.get("ocrBackend").and_then(|node| node.as_str()) else {
        return;
    };
    let current = current.trim().to_owned();
    if ocr_backend::is_supported(&current) {
        return;
    }
    push_load_warning(format!(
        "OCR 后端「{current}」已不再支持，已改用系统内置引擎（system）"
    ));
    obj.insert(
        "ocrBackend".into(),
        serde_json::Value::String("system".into()),
    );
}

/// 旧代理配置迁移（Phase 8）：单串 `translateProxy` → 结构化 `proxy`。
///
/// 规则（docs/phase8-proxy.md §4.2）：
/// - 空串（默认值）→ `system`（保持"跟随系统"的既有语义）；
/// - `host:port` / `http://host:port` → `manual` + `http`；
/// - 含 `socks` 前缀 → `manual` + `socks5`——**旧版会拒绝保存这种值**，故实际出现的
///   可能性极低，但一旦出现应按用户意图迁移而非丢弃；
/// - 无法解析 → `system` 并告警，不阻塞启动。
///
/// 迁移后移除 `translateProxy` 键，避免两处配置并存造成困惑。
fn migrate_legacy_proxy(value: &mut serde_json::Value) {
    use crate::translate::http::{ProxyAuth, ProxyKind};
    let Some(obj) = value.as_object_mut() else {
        return;
    };
    let Some(legacy) = obj.remove("translateProxy") else {
        return;
    };
    let legacy = legacy.as_str().unwrap_or_default().trim().to_owned();
    if legacy.is_empty() {
        return; // 等价于默认 system，无需构造 manual
    }
    // 解析 host:port（允许 http:// 与 socks*:// 前缀；其余前缀按 host 的一部分处理）
    let (kind, without_scheme) = if legacy.to_ascii_lowercase().starts_with("socks") {
        // 剥掉任意 socks 前缀变体：`socks5://h:p`、`socks://h:p`、`socks5=h:p`（WinHTTP
        // 的按协议写法，Phase 7 实测该串可被接受但 SOCKS 语义被忽略）、甚至 `socksh:p`。
        // 先按 scheme 分隔符（`:` 或 `=`）切一刀再清掉斜杠——只剥前缀本身，
        // host 里即使再出现 `:`（IPv6，虽不被支持）也不受影响。
        let rest = legacy
            .trim_start_matches(|c: char| {
                c.is_ascii_alphanumeric() || c == '_' // socks5 / socks 前缀本身
            })
            .trim_start_matches([':', '=', '/'])
            .trim_start(); // `socks5 = h:p` 这类带空格的写法
        if rest.is_empty() {
            warn!("旧代理配置「{legacy}」无法解析，已回退为跟随系统");
            return;
        }
        (ProxyKind::Socks5, rest)
    } else {
        (
            ProxyKind::Http,
            legacy
                .strip_prefix("http://")
                .or_else(|| legacy.strip_prefix("https://"))
                .unwrap_or(&legacy),
        )
    };
    // 去掉可能内嵌的认证（user:pass@host:port）
    let (auth, host_port) = match without_scheme.rsplit_once('@') {
        Some((credentials, rest)) => {
            let auth = credentials.split_once(':').map(|(u, p)| ProxyAuth {
                username: u.to_owned(),
                password: p.to_owned(),
            });
            (auth, rest)
        }
        None => (None, without_scheme),
    };
    let parsed = host_port
        .rsplit_once(':')
        .and_then(|(host, port)| port.parse::<u16>().ok().map(|port| (host.to_owned(), port)));
    let Some((host, port)) = parsed else {
        warn!("旧代理配置「{legacy}」无法解析，已回退为跟随系统");
        return;
    };
    let manual = ManualProxy {
        kind: match kind {
            ProxyKind::Http => "http".into(),
            ProxyKind::Socks5 => "socks5".into(),
        },
        host,
        port: port as i64,
        username: auth
            .as_ref()
            .map(|a| a.username.clone())
            .unwrap_or_default(),
        password: auth.map(|a| a.password).unwrap_or_default(),
        bypass: String::new(),
    };
    obj.insert(
        "proxy".into(),
        serde_json::json!({
            "mode": "manual",
            "manual": serde_json::to_value(&manual).unwrap_or_default(),
        }),
    );
    info!(
        "旧代理配置已迁移为结构化 proxy（mode=manual，类型 {}）",
        manual.kind
    );
}

/// 旧配置迁移（Phase 5 F15）：仅含 `hotkeyToggle` 单键时迁移到 `hotkeys.toggleMain`，
/// 其余两个动作取默认键。只在内存中生效，下次保存即以新结构写回。
/// 注意：落盘 JSON 为 camelCase（serde rename_all），判定键名必须用 `hotkeyToggle`。
fn migrate_legacy_hotkeys(value: &mut serde_json::Value) {
    let Some(obj) = value.as_object_mut() else {
        return;
    };
    if obj.contains_key("hotkeys") || !obj.contains_key("hotkeyToggle") {
        return;
    }
    let legacy = obj
        .get("hotkeyToggle")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_owned();
    if legacy.trim().is_empty() {
        return;
    }
    let defaults = HotkeyMap::default();
    obj.insert(
        "hotkeys".into(),
        serde_json::json!({
            "toggleMain": legacy,
            "captureRegion": defaults.capture_region,
            "captureFullscreen": defaults.capture_fullscreen,
        }),
    );
    info!("已迁移旧快捷键配置 hotkeyToggle → hotkeys.toggleMain（{legacy}）");
}

/// 写回设置并落盘(结构同 load:外层 `{"settings": …}`,原子写防半截文件)。
pub fn save(data_dir: &Path, settings: &Settings) -> AppResult<()> {
    std::fs::create_dir_all(data_dir)?;
    let value = serde_json::to_value(settings)
        .map_err(|err| AppError::Internal(format!("设置序列化失败: {err}")))?;
    let text = serde_json::to_string_pretty(&serde_json::json!({ "settings": value }))
        .map_err(|err| AppError::Internal(format!("设置序列化失败: {err}")))?;
    let path = data_dir.join(CONFIG_FILE);
    let tmp = data_dir.join(format!("{CONFIG_FILE}.tmp"));
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, &path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 测试临时目录:放 `<manifest>/target/tmp` 而非 `std::env::temp_dir()` ——
    /// 受限执行环境(沙箱/CI)下测试 exe 对 `%TEMP%` 的写入会被拒绝
    /// (`PermissionDenied`),放到工作区内两种环境都能跑(同 backup_export 的做法)。
    pub(super) fn tmp_dir(tag: &str) -> PathBuf {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("tmp");
        let _ = std::fs::create_dir_all(&base);
        base.join(format!(
            "nb_settings_{tag}_{}_{}",
            std::process::id(),
            crate::now_ms()
        ))
    }

    #[test]
    fn migrate_legacy_hotkey_toggle_to_hotkeys() {
        // 落盘 JSON 为 camelCase（serde rename_all），迁移判定必须匹配该形态
        let mut value = serde_json::json!({ "historyLimit": 5000, "hotkeyToggle": "Alt+A" });
        migrate_legacy_hotkeys(&mut value);
        let settings: Settings = serde_json::from_value(value).unwrap();
        assert_eq!(settings.hotkeys.toggle_main, "Alt+A");
        assert_eq!(settings.hotkeys.capture_region, "Ctrl+Shift+A");
        assert_eq!(settings.hotkeys.capture_fullscreen, "Ctrl+Shift+Q");
    }

    #[test]
    fn migrate_keeps_new_format_untouched() {
        let mut value = serde_json::json!({
            "hotkeys": { "toggleMain": "F1", "captureRegion": "F2", "captureFullscreen": "F3" }
        });
        migrate_legacy_hotkeys(&mut value);
        let settings: Settings = serde_json::from_value(value).unwrap();
        assert_eq!(settings.hotkeys.toggle_main, "F1");
        assert_eq!(settings.hotkeys.capture_region, "F2");
        assert_eq!(settings.hotkeys.capture_fullscreen, "F3");
    }

    #[test]
    fn validate_rejects_duplicate_hotkeys() {
        let mut settings = Settings::default();
        settings.hotkeys.capture_region = settings.hotkeys.toggle_main.clone();
        assert!(settings.validate().is_err());
    }

    /// 构造旧格式配置并跑迁移（Phase 8 代理迁移用例共用）。
    fn migrate_proxy(json: serde_json::Value) -> Settings {
        let mut value = json;
        migrate_legacy_proxy(&mut value);
        serde_json::from_value(value).unwrap()
    }

    #[test]
    fn migrate_empty_legacy_proxy_becomes_system() {
        let settings = migrate_proxy(serde_json::json!({ "translateProxy": "" }));
        assert_eq!(settings.proxy.mode, "system");
        assert!(settings.proxy.manual.host.is_empty());
    }

    #[test]
    fn migrate_host_port_legacy_proxy_becomes_manual_http() {
        let settings = migrate_proxy(serde_json::json!({ "translateProxy": "127.0.0.1:7890" }));
        assert_eq!(settings.proxy.mode, "manual");
        assert_eq!(settings.proxy.manual.kind, "http");
        assert_eq!(settings.proxy.manual.host, "127.0.0.1");
        assert_eq!(settings.proxy.manual.port, 7890);
    }

    #[test]
    fn migrate_http_scheme_legacy_proxy_strips_prefix() {
        let settings =
            migrate_proxy(serde_json::json!({ "translateProxy": "http://10.0.0.2:8888" }));
        assert_eq!(settings.proxy.mode, "manual");
        assert_eq!(settings.proxy.manual.kind, "http");
        assert_eq!(settings.proxy.manual.host, "10.0.0.2");
        assert_eq!(settings.proxy.manual.port, 8888);
    }

    #[test]
    fn migrate_socks_legacy_proxy_keeps_user_intent() {
        // 旧版保存时会拒绝 socks 前缀，这条主要是防御手工编辑过的配置文件
        let settings =
            migrate_proxy(serde_json::json!({ "translateProxy": "socks5://127.0.0.1:1080" }));
        assert_eq!(settings.proxy.mode, "manual");
        assert_eq!(settings.proxy.manual.kind, "socks5");
        assert_eq!(settings.proxy.manual.host, "127.0.0.1");
        assert_eq!(settings.proxy.manual.port, 1080);
    }

    #[test]
    fn migrate_unparseable_legacy_proxy_falls_back_to_system() {
        let settings = migrate_proxy(serde_json::json!({ "translateProxy": "not a proxy" }));
        assert_eq!(
            settings.proxy.mode, "system",
            "解析失败应回退默认而不是阻塞启动"
        );
    }

    #[test]
    fn migrate_socks5_equals_form_is_supported() {
        // WinHTTP 按协议写法 `socks5=host:port`（Phase 7 实测可被接受但 SOCKS 语义被忽略），
        // 也可能出现在旧配置里，同样按用户意图迁移
        let settings =
            migrate_proxy(serde_json::json!({ "translateProxy": "socks5=10.0.0.2:1080" }));
        assert_eq!(settings.proxy.mode, "manual");
        assert_eq!(settings.proxy.manual.kind, "socks5");
        assert_eq!(settings.proxy.manual.host, "10.0.0.2");
        assert_eq!(settings.proxy.manual.port, 1080);
    }

    #[test]
    fn migrate_socks_prefix_does_not_misfire_on_socks_hostname() {
        // 主机名本身含 "socks" 的**非代理**旧值（如域名 socks5.com:1080）：
        // 以 "socks" 开头会被当 SOCKS5 前缀剥掉，但剥完剩余部分仍是合法 host:port，
        // 结果只是 kind 判定不同——host/port 不丢失即无害
        let settings = migrate_proxy(serde_json::json!({ "translateProxy": "socks5.com:1080" }));
        assert_eq!(settings.proxy.mode, "manual");
        assert_eq!(settings.proxy.manual.port, 1080);
        assert!(
            !settings.proxy.manual.host.contains("1080"),
            "端口不应混入主机名"
        );
    }

    #[test]
    fn new_proxy_format_loads_without_migration() {
        // 新格式：迁移函数不应破坏已结构化的配置
        let settings = migrate_proxy(serde_json::json!({
            "proxy": {
                "mode": "manual",
                "manual": {
                    "kind": "socks5",
                    "host": "10.0.0.9",
                    "port": 1080,
                    "username": "u",
                    "password": "p",
                    "bypass": ""
                }
            }
        }));
        assert_eq!(settings.proxy.mode, "manual");
        assert_eq!(settings.proxy.manual.kind, "socks5");
        assert_eq!(settings.proxy.manual.username, "u");
        assert_eq!(settings.proxy.manual.password, "p");
    }

    #[test]
    fn validate_proxy_manual_accepts_valid_and_rejects_invalid() {
        let ok = ManualProxy {
            kind: "http".into(),
            host: "proxy.corp.local".into(),
            port: 8080,
            username: "alice".into(),
            password: "s3cret".into(),
            bypass: "localhost;127.0.0.1".into(),
        };
        assert!(validate_proxy_manual(&ok).is_ok());

        // kind 非法
        let mut bad = ok.clone();
        bad.kind = "socks4".into();
        assert!(validate_proxy_manual(&bad).is_err());
        // host 空 / 含非法字符
        bad = ok.clone();
        bad.host = String::new();
        assert!(validate_proxy_manual(&bad).is_err());
        bad = ok.clone();
        bad.host = "host with space".into();
        assert!(validate_proxy_manual(&bad).is_err());
        // 端口越界
        bad = ok.clone();
        bad.port = 0;
        assert!(validate_proxy_manual(&bad).is_err());
        bad = ok.clone();
        bad.port = 65536;
        assert!(validate_proxy_manual(&bad).is_err());
        // username 含 @ / :（会破坏 user:pass@host 的解析）
        bad = ok.clone();
        bad.username = "a@b".into();
        assert!(validate_proxy_manual(&bad).is_err());
        bad = ok.clone();
        bad.username = "a:b".into();
        assert!(validate_proxy_manual(&bad).is_err());
        // 密码含空格
        bad = ok.clone();
        bad.password = "a b".into();
        assert!(validate_proxy_manual(&bad).is_err());
        // bypass 项内空格
        bad = ok.clone();
        bad.bypass = "localhost; 127.0.0.1".into();
        assert!(validate_proxy_manual(&bad).is_err());
    }

    #[test]
    fn validate_proxy_manual_empty_username_means_no_auth() {
        let manual = ManualProxy {
            kind: "socks5".into(),
            host: "127.0.0.1".into(),
            port: 1080,
            username: "  ".into(),
            password: "whatever".into(),
            bypass: String::new(),
        };
        let endpoint = validate_proxy_manual(&manual).expect("合法配置应通过");
        assert!(
            endpoint.auth.is_none(),
            "用户名为空白时应视为无认证，而不是带空用户名"
        );
    }

    #[test]
    fn validate_requires_modifier_or_function_key() {
        // 裸字母会抢占全系统按键，必须拒绝
        let with = |combo: &str| Settings {
            hotkeys: HotkeyMap {
                toggle_main: combo.into(),
                ..HotkeyMap::default()
            },
            ..Default::default()
        };
        assert!(with("A").validate().is_err());
        assert!(with("5").validate().is_err());
        assert!(with("F8").validate().is_ok(), "单按功能键允许");
        assert!(with("F25").validate().is_err(), "F25 不是合法功能键");
        assert!(with("Ctrl+Shift+V").validate().is_ok());
        assert!(with("Super+Space").validate().is_ok());
    }

    #[test]
    fn validate_treats_blank_hotkey_as_unbound() {
        // 允许留空 = 不绑定该动作（用户可只绑自己需要的几个）；纯空白按未绑定处理
        let mut settings = Settings::default();
        settings.hotkeys.capture_fullscreen = "  ".into();
        assert!(settings.validate().is_ok(), "空白串视为未绑定，不再报错");
    }

    #[test]
    fn validate_checks_text_pin_style() {
        // 颜色必须是 #RRGGBB
        let mut settings = Settings::default();
        settings.pin_text_bg = "red".into();
        assert!(settings.validate().is_err(), "非 #RRGGBB 背景色应被拒");

        let mut settings = Settings::default();
        settings.pin_text_color = "#12345".into();
        assert!(settings.validate().is_err(), "位数不足的文字色应被拒");

        // 数值范围
        let mut settings = Settings::default();
        settings.pin_text_font_size = 200;
        assert!(settings.validate().is_err(), "字号越界应被拒");

        let mut settings = Settings::default();
        settings.pin_text_max_width = 50;
        assert!(settings.validate().is_err(), "最大宽度越界应被拒");

        let mut settings = Settings::default();
        settings.pin_text_bg_opacity = 101;
        assert!(settings.validate().is_err(), "背景不透明度越界应被拒");

        // 默认值必须合法(否则老配置一律保存失败)
        assert!(Settings::default().validate().is_ok());
    }

    #[test]
    fn validate_checks_pin_opacity_range() {
        let with_opacity = |opacity: i64| Settings {
            pin_default_opacity: opacity,
            ..Default::default()
        };
        assert!(with_opacity(29).validate().is_err());
        assert!(with_opacity(101).validate().is_err());
        assert!(with_opacity(30).validate().is_ok());
        assert!(with_opacity(100).validate().is_ok());
    }

    #[test]
    fn validate_checks_annotation_prefs() {
        // 默认值必须自洽:默认就应该能通过校验,否则新装用户一启动就报错
        assert!(Settings::default().validate().is_ok());

        let with = |color: &str, pen: i64, font: i64| Settings {
            annotation_color: color.into(),
            annotation_pen_size: pen,
            annotation_font_size: font,
            ..Default::default()
        };
        // UI 档位内的值全部合法(须与 strokeSizes()/textFontSizes() 一致)
        assert!(with("#E03131", 2, 24).validate().is_ok());
        assert!(with("#40C057", 4, 36).validate().is_ok());
        assert!(with("#1C7ED6", 8, 48).validate().is_ok());
        // 档位外的值拒绝:界面没有对应选中项
        assert!(with("#E03131", 3, 36).validate().is_err());
        assert!(with("#E03131", 4, 30).validate().is_err());
        // 颜色:必须 #rrggbb
        assert!(with("red", 4, 36).validate().is_err());
        assert!(with("#E0313", 4, 36).validate().is_err());   // 少一位
        assert!(with("E031311", 4, 36).validate().is_err());  // 缺 #
        assert!(with("#GGGGGG", 4, 36).validate().is_err());  // 非 hex
    }

    #[test]
    fn old_config_without_annotation_keys_falls_back_to_defaults() {
        // 向后兼容:旧配置文件里没有这三个键,靠 serde(default) 回落到默认值。
        // 若这里失败,说明 #[serde(default = ...)] 写错了名字 —— 表现为
        // 老用户升级后标注偏好变成空串/0,一进标注态就是黑笔 0px。
        let v = serde_json::json!({ "historyLimit": 100 });
        let s: Settings = serde_json::from_value(v).unwrap();
        assert_eq!(s.annotation_color, "#E03131");
        assert_eq!(s.annotation_pen_size, 4);
        assert_eq!(s.annotation_font_size, 36);
    }

    #[test]
    fn validate_checks_capture_bar_alpha_three_steps() {
        let with_alpha = |alpha: i64| Settings {
            capture_bar_alpha: alpha,
            ..Default::default()
        };
        // 三档合法
        assert!(with_alpha(179).validate().is_ok());
        assert!(with_alpha(217).validate().is_ok());
        assert!(with_alpha(242).validate().is_ok());
        // 其它值一律拒绝:浮条上只有三档按钮,存别的值没有对应 UI
        assert!(with_alpha(200).validate().is_err());
        assert!(with_alpha(0).validate().is_err());
        assert!(with_alpha(255).validate().is_err());
        // 默认值必须合法(否则老配置一律保存失败)
        assert!(Settings::default().validate().is_ok());
    }

    #[test]
    fn validate_checks_editor_geometry() {
        let with_geom = |g: Vec<i64>| Settings {
            editor_geometry: g,
            ..Default::default()
        };
        // 空 = 尚未记录,合法(首次打开按默认尺寸居中)
        assert!(with_geom(Vec::new()).validate().is_ok());
        assert!(with_geom(vec![100, 100, 1280, 800]).validate().is_ok());
        // x/y 可为负:多屏时副屏可能位于负坐标区
        assert!(with_geom(vec![-1080, 0, 1080, 1920]).validate().is_ok());
        // 元素个数必须为四
        assert!(with_geom(vec![100, 100]).validate().is_err());
        assert!(with_geom(vec![1, 2, 3, 4, 5]).validate().is_err());
        // 尺寸越界拒绝
        assert!(with_geom(vec![0, 0, 100, 800]).validate().is_err());
        assert!(with_geom(vec![0, 0, 1280, 100]).validate().is_err());
    }

    #[test]
    fn validate_checks_panel_size() {
        let with_size = |v: Vec<i64>| Settings {
            panel_size: v,
            ..Default::default()
        };
        // 空 = 尚未记录,合法(用内置默认 800×600)
        assert!(with_size(Vec::new()).validate().is_ok());
        assert!(with_size(vec![1024, 720]).validate().is_ok());
        // 边界值:最小值恰好合法
        assert!(with_size(vec![620, 380]).validate().is_ok());
        // 元素个数必须为二(四元组是 editor_geometry 的口径,别混)
        assert!(with_size(vec![800]).validate().is_err());
        assert!(with_size(vec![100, 100, 800, 600]).validate().is_err());
        // 低于下限拒绝:面板头部会挤变形(与 panel_window.cpp 的 PANEL_MIN_* 同语义)
        assert!(with_size(vec![619, 600]).validate().is_err());
        assert!(with_size(vec![800, 379]).validate().is_err());
    }

    #[test]
    fn validate_checks_ocr_options() {
        let with = |backend: &str, language: &str, scale: i64| Settings {
            ocr_backend: backend.into(),
            ocr_language: language.into(),
            ocr_scale: scale,
            ..Default::default()
        };
        // 合法组合
        assert!(with("system", "auto", 2).validate().is_ok());
        assert!(with("windows", "en-US", 1).validate().is_ok());
        // 后端只接受枚举值；已移除的外部后端不得再被接受（旧配置由 Backend::parse 回退）
        assert!(with("paddle", "auto", 2).validate().is_err());
        assert!(with("tesseract", "auto", 2).validate().is_err());
        // 语言标签格式非法（含空格/符号）
        assert!(with("system", "zh Hans", 2).validate().is_err());
        // 放大倍数范围
        assert!(with("system", "auto", 0).validate().is_err());
        assert!(with("system", "auto", 5).validate().is_err());
        assert!(with("system", "auto", 4).validate().is_ok());
    }

    #[test]
    fn hotkey_map_get_maps_actions() {
        let map = HotkeyMap::default();
        assert_eq!(map.get(HotkeyAction::ToggleMain), "Ctrl+Shift+V");
        assert_eq!(map.get(HotkeyAction::CaptureRegion), "Ctrl+Shift+A");
        assert_eq!(map.get(HotkeyAction::CaptureFullscreen), "Ctrl+Shift+Q");
        assert_eq!(map.get(HotkeyAction::PinLatest), "Ctrl+Shift+P");
        assert_eq!(map.get(HotkeyAction::OpenTranslate), "Ctrl+Shift+T");
        assert_eq!(map.get(HotkeyAction::CaptureTranslate), "Ctrl+Shift+E");
        // 新增动作默认不绑定（由用户决定是否录）
        assert_eq!(map.get(HotkeyAction::CaptureDelayed), "");
        assert_eq!(map.get(HotkeyAction::CaptureOcr), "");
        // 长截图例外:**默认预置**(直通模式下浮条不出现,没键就够不到)
        assert_eq!(map.get(HotkeyAction::CaptureScrolling), "Ctrl+Shift+L");
        // 已绑定的默认键必须两两不同（未绑定的空串不参与比较）
        let mut all: Vec<&str> = HotkeyAction::ALL
            .iter()
            .map(|a| map.get(*a))
            .filter(|combo| !combo.is_empty())
            .collect();
        all.sort_unstable();
        all.dedup();
        assert_eq!(all.len(), 7, "7 个已绑定的默认快捷键不得重复");
    }

    #[test]
    fn legacy_config_without_new_fields_falls_back_to_defaults() {
        // 旧配置（无 pinLatest、无 regionAutoConfirm/regionSkipEditor）应补默认值，不阻塞启动
        let settings: Settings = serde_json::from_value(serde_json::json!({
            "historyLimit": 5000,
            "hotkeys": { "toggleMain": "Alt+A", "captureRegion": "Ctrl+Shift+A", "captureFullscreen": "Ctrl+Shift+Q" }
        }))
        .unwrap();
        assert_eq!(settings.hotkeys.toggle_main, "Alt+A");
        assert_eq!(settings.hotkeys.pin_latest, "Ctrl+Shift+P");
        assert!(settings.region_auto_confirm, "默认框选即确认");
        // regionSkipEditor 已废弃(§18.5):字段仍在(兼容旧配置读入),但不再驱动行为。
        // 这里只断言"能正常读入默认值",不再断言其语义。
        assert!(settings.region_skip_editor, "已废弃字段仍可读入(默认 true)");
        assert_eq!(settings.capture_delay_secs, 3, "延时截图默认等待 3 秒");
        assert_eq!(settings.stitch_max_height, 30_000, "长截图默认上限 30000px(docs/19 §9 Q4)");
        assert_eq!(settings.stitch_overlap_ratio, 3, "长截图重叠门槛默认 3%");
        assert_eq!(
            settings.overlay_idle_secs, 60,
            "遮罩空闲超时默认 1 分钟（0 = 不自动取消）"
        );
        assert!(
            settings.software_render,
            "默认软件渲染（省 ~100MB 稳态）；硬件后端由设置项/--hardware 反向选择"
        );
        assert_eq!(settings.ocr_language, "auto", "OCR 默认自动选语言");
        assert_eq!(settings.ocr_backend, "system", "OCR 默认用系统内置引擎");
        assert_eq!(settings.ocr_scale, 2, "OCR 默认放大 2 倍");
        assert!(
            settings.pin_doubleclick_close,
            "双击关贴图默认开（既有 Snipaste 习惯，设置只让它可配置）"
        );
        assert!(
            !settings.pin_prefer_image,
            "一键贴图默认贴最近复制的内容（图片优先默认关，作为可选项）"
        );
        // Phase 7：翻译字段缺失时全部取默认（旧配置升级不阻塞启动）
        assert_eq!(settings.translate_provider, "microsoft");
        assert_eq!(settings.translate_source_lang, "auto");
        assert_eq!(settings.translate_target_lang, "zh-Hans");
        assert_eq!(settings.translate_timeout_secs, 30);
        assert_eq!(settings.proxy.mode, "system", "默认跟随系统代理");
        assert!(settings.proxy.manual.host.is_empty());
        assert_eq!(settings.hotkeys.open_translate, "Ctrl+Shift+T");
        assert_eq!(settings.hotkeys.capture_translate, "Ctrl+Shift+E");
        assert_eq!(
            settings.translate_microsoft.endpoint, "https://edge.microsoft.com/translate",
            "默认中国版端点"
        );
    }

    #[test]
    fn stitch_settings_validate() {
        let mut s = Settings::default();
        assert!(s.validate().is_ok(), "默认值应合法");
        // 上限:低于 MIN / 高于 MAX 均拒绝
        s.stitch_max_height = crate::stitch::MIN_MAX_HEIGHT - 1;
        assert!(s.validate().is_err(), "上限过小应被拒绝");
        s.stitch_max_height = crate::stitch::MAX_MAX_HEIGHT + 1;
        assert!(s.validate().is_err(), "上限过大应被拒绝");
        s.stitch_max_height = crate::stitch::MIN_MAX_HEIGHT;
        assert!(s.validate().is_ok());
        s.stitch_max_height = crate::stitch::MAX_MAX_HEIGHT;
        assert!(s.validate().is_ok());
        // 重叠门槛 1~20
        s.stitch_max_height = 30_000;
        s.stitch_overlap_ratio = 0;
        assert!(s.validate().is_err(), "门槛 0 等于没有门槛");
        s.stitch_overlap_ratio = 21;
        assert!(s.validate().is_err(), "门槛过高会逼用户小步滚");
        s.stitch_overlap_ratio = 1;
        assert!(s.validate().is_ok());
        s.stitch_overlap_ratio = 20;
        assert!(s.validate().is_ok());
    }

    #[test]
    fn validate_accepts_six_distinct_hotkeys() {
        assert!(Settings::default().validate().is_ok(), "默认键互不重复");
        // 翻译相关动作与既有动作重复 → 拒绝
        let mut map = HotkeyMap::default();
        map.open_translate = map.toggle_main.clone();
        assert!(Settings {
            hotkeys: map,
            ..Default::default()
        }
        .validate()
        .is_err());

        let mut map = HotkeyMap::default();
        map.capture_translate = map.capture_region.clone();
        assert!(Settings {
            hotkeys: map,
            ..Default::default()
        }
        .validate()
        .is_err());
    }

    #[test]
    fn validate_checks_translate_options() {
        let with = |provider: &str, from: &str, to: &str, timeout: i64| Settings {
            translate_provider: provider.into(),
            translate_source_lang: from.into(),
            translate_target_lang: to.into(),
            translate_timeout_secs: timeout,
            ..Default::default()
        };
        // 合法组合
        assert!(with("openai", "auto", "zh-Hans", 30).validate().is_ok());
        assert!(with("microsoft", "en", "zh-Hans", 5).validate().is_ok());
        // 注意：provider=custom 时必填项随之生效，故超时上界改在 openai 下验证
        assert!(with("openai", "auto", "en", 120).validate().is_ok());
        // 提供方只接受枚举值
        assert!(with("deepl", "auto", "en", 30).validate().is_err());
        // 目标语言不接受 auto / 空
        assert!(with("openai", "auto", "auto", 30).validate().is_err());
        assert!(with("openai", "auto", "", 30).validate().is_err());
        // 源语言非法字符
        assert!(with("openai", "zh Hans", "en", 30).validate().is_err());
        // 超时范围
        assert!(with("openai", "auto", "en", 4).validate().is_err());
        assert!(with("openai", "auto", "en", 121).validate().is_err());
    }

    #[test]
    fn validate_checks_translate_endpoints_and_temperature() {
        let with_openai = |base_url: &str, temperature: f64| Settings {
            translate_openai: OpenAiProvider {
                base_url: base_url.into(),
                temperature,
                ..OpenAiProvider::default()
            },
            ..Default::default()
        };
        // 空 baseUrl 允许（调用方回填默认值）
        assert!(with_openai("", 0.2).validate().is_ok());
        assert!(with_openai("https://api.openai.com/v1", 0.2)
            .validate()
            .is_ok());
        // 缺协议头 / 含空格 → 拒绝
        assert!(with_openai("api.openai.com/v1", 0.2).validate().is_err());
        assert!(with_openai("https://a b.com", 0.2).validate().is_err());
        // 温度越界
        assert!(with_openai("https://api.openai.com/v1", 1.5)
            .validate()
            .is_err());
        assert!(with_openai("https://api.openai.com/v1", -0.1)
            .validate()
            .is_err());
        assert!(with_openai("https://api.openai.com/v1", 1.0)
            .validate()
            .is_ok());
    }

    #[test]
    fn validate_checks_microsoft_endpoint_and_proxy() {
        let with_endpoint = |endpoint: &str| Settings {
            translate_microsoft: MicrosoftProvider {
                endpoint: endpoint.into(),
            },
            ..Default::default()
        };
        assert!(with_endpoint("https://edge.microsoft.com/translate")
            .validate()
            .is_ok());
        assert!(
            with_endpoint("edge.microsoft.com").validate().is_err(),
            "缺少协议应非法"
        );

        let with_mode = |mode: &str| Settings {
            proxy: ProxySettings {
                mode: mode.into(),
                ..ProxySettings::default()
            },
            ..Default::default()
        };
        assert!(with_mode("").validate().is_ok());
        assert!(with_mode("system").validate().is_ok());
        assert!(with_mode("none").validate().is_ok());
        assert!(with_mode("nonsense").validate().is_err());

        // manual 配置整体校验：合法/非法各一（详细规则见 validate_proxy_manual 的用例）
        let with_manual = |kind: &str, host: &str, port: i64, bypass: &str| Settings {
            proxy: ProxySettings {
                mode: "manual".into(),
                manual: ManualProxy {
                    kind: kind.into(),
                    host: host.into(),
                    port,
                    username: String::new(),
                    password: String::new(),
                    bypass: bypass.into(),
                },
            },
            ..Default::default()
        };
        assert!(with_manual("http", "127.0.0.1", 2080, "")
            .validate()
            .is_ok());
        assert!(
            with_manual("socks5", "127.0.0.1", 1080, "")
                .validate()
                .is_ok(),
            "SOCKS5 在 Phase 8 已支持（决策 D17）"
        );
        assert!(
            with_manual("http", "proxy/path", 7890, "")
                .validate()
                .is_err(),
            "host 含路径应拒绝"
        );
        assert!(
            with_manual("http", "127.0.0.1", 2080, "a b")
                .validate()
                .is_err(),
            "绕过列表项内空格应拒绝"
        );
    }

    #[test]
    fn validate_requires_custom_fields_only_when_selected() {
        // 未选用 custom：字段留空也能保存（用户先配 OpenAI，之后再补 custom）
        let inactive = Settings {
            translate_provider: "openai".into(),
            ..Default::default()
        };
        assert!(inactive.validate().is_ok());

        // 选用 custom 但缺 URL / 响应路径 / 请求体模板 → 逐项拒绝
        let base = Settings {
            translate_provider: "custom".into(),
            ..Default::default()
        };
        assert!(base.validate().is_err(), "缺 URL 应拒绝");

        let with_url = Settings {
            translate_custom: CustomProvider {
                url: "https://api.example.com/translate".into(),
                ..CustomProvider::default()
            },
            ..base.clone()
        };
        assert!(with_url.validate().is_err(), "缺响应路径应拒绝");

        let with_path = Settings {
            translate_custom: CustomProvider {
                url: "https://api.example.com/translate".into(),
                response_path: "data.translations[0].text".into(),
                ..CustomProvider::default()
            },
            ..base.clone()
        };
        assert!(with_path.validate().is_err(), "缺请求体模板应拒绝");

        let complete = Settings {
            translate_custom: CustomProvider {
                url: "https://api.example.com/translate".into(),
                response_path: "data.translations[0].text".into(),
                body_template: r#"{"text":"${text}"}"#.into(),
                ..CustomProvider::default()
            },
            ..base
        };
        assert!(complete.validate().is_ok(), "齐全后应通过");
    }

    #[test]
    fn validate_rejects_bad_custom_method_and_response_path() {
        let with = |method: &str, response_path: &str| Settings {
            translate_provider: "custom".into(),
            translate_custom: CustomProvider {
                method: method.into(),
                url: "https://api.example.com/t".into(),
                body_template: "{}".into(),
                response_path: response_path.into(),
                ..CustomProvider::default()
            },
            ..Default::default()
        };
        assert!(with("POST", "a.b").validate().is_ok());
        assert!(with("get", "a.b").validate().is_ok(), "大小写不敏感");

        // 响应路径语法错误应在保存时就被拦下，而不是翻译时才报错
        assert!(with("POST", "a..b").validate().is_err());
        assert!(with("POST", "a[0").validate().is_err());
        assert!(with("POST", "").validate().is_err());
    }

    #[test]
    fn effective_data_dir_prefers_custom_and_falls_back_to_default() {
        let default_dir = PathBuf::from("C:/default");
        let mut settings = Settings::default();
        assert_eq!(
            effective_data_dir(&settings, &default_dir),
            default_dir,
            "未设置自定义目录时应使用默认目录"
        );
        // 空白串等价于未设置(与 validate 的 trim 口径一致)
        settings.data_dir = "   ".into();
        assert_eq!(effective_data_dir(&settings, &default_dir), default_dir);
        settings.data_dir = " D:/custom ".into();
        assert_eq!(
            effective_data_dir(&settings, &default_dir),
            PathBuf::from("D:/custom"),
            "自定义目录应去首尾空白后生效"
        );
    }

    #[test]
    fn load_repairs_blank_microsoft_endpoint() {
        let root = crate::settings::tests::tmp_dir("ms");
        std::fs::create_dir_all(&root).unwrap();

        // 旧配置里端点是空串（早期 UI 只把它当 placeholder）→ 加载时补默认值
        std::fs::write(
            root.join(CONFIG_FILE),
            r#"{"settings":{"translateMicrosoft":{"endpoint":"   "}}}"#,
        )
        .unwrap();
        let settings = load(&root).unwrap();
        assert_eq!(
            settings.translate_microsoft.endpoint,
            crate::translate::microsoft::DEFAULT_ENDPOINT,
            "空端点应补默认值，否则会拼出相对地址而必然失败"
        );

        // 已填自定义端点的不得被覆盖
        std::fs::write(
            root.join(CONFIG_FILE),
            r#"{"settings":{"translateMicrosoft":{"endpoint":"https://my.example/tr"}}}"#,
        )
        .unwrap();
        let settings = load(&root).unwrap();
        assert_eq!(
            settings.translate_microsoft.endpoint,
            "https://my.example/tr"
        );

        let _ = std::fs::remove_dir_all(&root);
    }

    // ---------- OCR 后端取值同源(防"静默归零",见 docs/08a §1.2.3) ----------

    /// ⚠️ **P0 防护测试**:`rapidocr` 必须能通过校验、且**不被加载期归一改写**。
    ///
    /// 背景:校验(`validate`)与归一(`repair_removed_ocr_backend`)曾是两份口径副本。
    /// 若只放宽其中一处,会出现「设置页能选 RapidOCR,但加载时被静默改回 system」
    /// —— 表现为"选了新引擎却仍在跑内置",且**质量回归测试查不出来**(两条路都返回文本)。
    #[test]
    fn rapidocr_backend_survives_validate_and_repair() {
        // 1) 必须通过校验(否则设置页保存直接报错,用户根本存不进去)
        let settings = Settings {
            ocr_backend: "rapidocr".into(),
            ..Default::default()
        };
        assert!(settings.validate().is_ok(), "rapidocr 必须通过校验");

        // 2) 必须**不被加载期归一改写**(否则重启后静默变回 system)
        let root = crate::settings::tests::tmp_dir("ocrbackend_rapid");
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(
            root.join(CONFIG_FILE),
            r#"{"settings":{"ocrBackend":"rapidocr"}}"#,
        )
        .unwrap();
        assert_eq!(
            load(&root).unwrap().ocr_backend,
            "rapidocr",
            "rapidocr 是受支持后端,不得被 repair 归零回 system"
        );
        let _ = std::fs::remove_dir_all(&root);
    }

    /// 单一来源与 `crate::ocr::Backend::parse` 的口径必须一致:
    /// 凡 `is_supported` 认可的具名后端,`parse` 都不得当作"未知值"回退。
    #[test]
    fn every_supported_backend_is_recognized_by_parse() {
        use crate::ocr::Backend;

        for value in ["system", "windows", "rapidocr", "rapid"] {
            assert!(ocr_backend::is_supported(value), "{value} 应被白名单接受");
        }

        // 既有别名
        assert_eq!(Backend::parse("system"), Backend::System);
        assert_eq!(Backend::parse("windows"), Backend::System);
        // 新增后端:parse 必须认识,否则会静默回退 System(跑内置引擎)
        assert_eq!(
            Backend::parse("rapidocr"),
            Backend::RapidOcr,
            "parse 必须认识 rapidocr,否则会静默跑内置引擎"
        );
        assert_eq!(Backend::parse("rapid"), Backend::RapidOcr);
        assert_eq!(
            Backend::parse("RapidOCR"),
            Backend::RapidOcr,
            "大小写不敏感"
        );
    }

    /// 新设置项:合法值不被 validate 拒绝(单一事实源与字段双份的回归防线)。
    #[test]
    fn ocr_profile_and_session_mode_survive_validate() {
        for profile in ["", "quality", "balanced", "compact"] {
            let mut s = Settings::default();
            s.ocr_profile = profile.into();
            s.ocr_session_mode = "ephemeral".into();
            assert!(s.validate().is_ok(), "profile={profile:?} 应合法");
        }
        for mode in ["", "resident", "idle60", "ephemeral"] {
            let mut s = Settings::default();
            s.ocr_profile = "balanced".into();
            s.ocr_session_mode = mode.into();
            assert!(s.validate().is_ok(), "mode={mode:?} 应合法");
        }
    }

    /// 非法值被 validate 拒绝(而不是静默放行)。
    #[test]
    fn ocr_profile_and_session_mode_reject_unknown() {
        let mut s = Settings::default();
        s.ocr_profile = "turbo".into();
        assert!(s.validate().is_err(), "未知档位应被拒绝");
        let mut s = Settings::default();
        s.ocr_session_mode = "forever".into();
        assert!(s.validate().is_err(), "未知驻留应被拒绝");
    }

    /// 档位映射的口径锁定(改档位值必须意识到这里)。
    #[test]
    fn ocr_profile_det_caps_mapping() {
        use super::ocr_profile::det_caps;
        assert_eq!(det_caps("quality"), (3.0, 2560));
        assert_eq!(det_caps("balanced"), (2.0, 2048));
        assert_eq!(det_caps("compact"), (1.5, 1600));
        assert_eq!(det_caps(""), (2.0, 2048), "空串=默认均衡");
    }

    #[test]
    fn load_repairs_removed_ocr_backend() {
        let root = crate::settings::tests::tmp_dir("ocrbackend");
        std::fs::create_dir_all(&root).unwrap();

        // 已移除的后端（只有手改 settings.json 才可能出现）→ 加载时归一并留痕。
        // 不归一的话，抽屉保存时回传整份对象会被 validate() 拒绝，任何设置都存不下去。
        std::fs::write(
            root.join(CONFIG_FILE),
            r#"{"settings":{"ocrBackend":"tesseract"}}"#,
        )
        .unwrap();
        let settings = load(&root).unwrap();
        assert_eq!(
            settings.ocr_backend, "system",
            "已移除的后端应归一到内置引擎"
        );
        assert!(
            settings.validate().is_ok(),
            "归一后整份设置必须仍可保存（这正是归一的目的）"
        );
        assert!(
            crate::settings::take_warnings()
                .iter()
                .any(|warn| warn.contains("tesseract")),
            "静默改写用户配置不可接受，须在可见警告里留痕"
        );

        // 合法别名不得被改写
        std::fs::write(
            root.join(CONFIG_FILE),
            r#"{"settings":{"ocrBackend":"windows"}}"#,
        )
        .unwrap();
        assert_eq!(load(&root).unwrap().ocr_backend, "windows");

        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn pending_move_copies_db_and_blobs_to_target_then_clears_marker() {
        let root = crate::settings::tests::tmp_dir("move");
        let default_dir = root.join("default");
        let source = root.join("source");
        let target = root.join("target");
        for dir in [&default_dir, &source] {
            std::fs::create_dir_all(dir).unwrap();
        }
        std::fs::write(source.join("clipboard.db"), b"db-bytes").unwrap();
        std::fs::create_dir_all(source.join("blobs/sub")).unwrap();
        std::fs::write(source.join("blobs/a.png"), b"png").unwrap();
        std::fs::write(source.join("blobs/sub/b.png"), b"png2").unwrap();

        write_pending_move(&default_dir, &source).unwrap();
        assert!(default_dir.join(DATA_MOVE_MARKER).exists());

        take_pending_move(&default_dir, &target);
        assert_eq!(
            std::fs::read(target.join("clipboard.db")).unwrap(),
            b"db-bytes",
            "数据库文件应被搬迁"
        );
        assert_eq!(
            std::fs::read(target.join("blobs/sub/b.png")).unwrap(),
            b"png2",
            "blobs 子目录应递归搬迁"
        );
        assert!(!default_dir.join(DATA_MOVE_MARKER).exists(), "标记应清除");
        // 源目录保留(迁移不删数据,回退靠它)
        assert!(source.join("clipboard.db").exists());

        // 目标已有数据时不覆盖:再写标记,新 target 内容应保持不变
        std::fs::write(
            default_dir.join(DATA_MOVE_MARKER),
            source.to_string_lossy().as_bytes(),
        )
        .unwrap();
        std::fs::write(target.join("clipboard.db"), b"newer").unwrap();
        take_pending_move(&default_dir, &target);
        assert_eq!(
            std::fs::read(target.join("clipboard.db")).unwrap(),
            b"newer",
            "目标已有数据库时不得覆盖"
        );

        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn pin_prefer_image_roundtrips_as_camel_case() {
        // QML 侧按 `s.pinPreferImage` 读写，落盘/回传的键名必须是 camelCase
        let settings = Settings {
            pin_prefer_image: true,
            ..Default::default()
        };
        let value = serde_json::to_value(&settings).unwrap();
        assert_eq!(
            value.get("pinPreferImage").and_then(|v| v.as_bool()),
            Some(true)
        );
        let back: Settings = serde_json::from_value(value).unwrap();
        assert!(back.pin_prefer_image);
    }

    #[test]
    fn copy_auto_paste_defaults_off_and_roundtrips_as_camel_case() {
        // 默认必须**关闭**:这是改变既有行为的功能(升级后突然自动粘贴会很意外),
        // 且管理员权限窗口(UIPI)根本收不到模拟按键。QML 按 `s.copyAutoPaste` 读写。
        assert!(!Settings::default().copy_auto_paste, "复制后自动粘贴默认关闭");
        let settings = Settings {
            copy_auto_paste: true,
            ..Default::default()
        };
        let value = serde_json::to_value(&settings).unwrap();
        assert_eq!(
            value.get("copyAutoPaste").and_then(|v| v.as_bool()),
            Some(true)
        );
        let back: Settings = serde_json::from_value(value).unwrap();
        assert!(back.copy_auto_paste);
    }

    #[test]
    fn validate_bounds_overlay_idle_timeout() {
        let with_idle = |secs: i64| Settings {
            overlay_idle_secs: secs,
            ..Default::default()
        };
        assert!(with_idle(0).validate().is_ok(), "0 = 不自动取消,合法");
        assert!(with_idle(600).validate().is_ok());
        assert!(with_idle(3600).validate().is_ok(), "上限 1 小时");
        assert!(with_idle(-1).validate().is_err(), "负值非法");
        assert!(with_idle(3601).validate().is_err(), "超过 1 小时非法");
    }

    #[test]
    fn validate_rejects_relative_data_dir() {
        let mut settings = Settings::default();
        assert!(settings.validate().is_ok(), "空 = 默认目录,合法");
        settings.data_dir = "relative/path".into();
        assert!(settings.validate().is_err(), "相对路径应被拒绝");
        settings.data_dir = "D:/absolute".into();
        assert!(settings.validate().is_ok());
    }

    #[test]
    fn hotkeys_may_be_left_unbound() {
        // 新增动作默认不预置组合键（由用户决定）
        let defaults = Settings::default();
        assert!(
            defaults.hotkeys.capture_delayed.is_empty(),
            "延时截图默认不绑定"
        );
        assert!(
            defaults.hotkeys.capture_ocr.is_empty(),
            "截图取字默认不绑定"
        );
        assert!(defaults.validate().is_ok(), "含未绑定项也应通过校验");

        // 全部清空 = 一个键都不绑，仍应合法
        let mut all = Settings::default();
        all.hotkeys.toggle_main = String::new();
        all.hotkeys.capture_region = String::new();
        all.hotkeys.capture_fullscreen = String::new();
        all.hotkeys.pin_latest = String::new();
        all.hotkeys.open_translate = String::new();
        all.hotkeys.capture_translate = String::new();
        all.hotkeys.capture_scrolling = String::new();
        assert!(all.validate().is_ok(), "全部不绑定应合法");

        // 非空项仍须合法、且不得重复
        let mut invalid = Settings::default();
        invalid.hotkeys.capture_delayed = "A".into(); // 无修饰键且非 F1~F24
        assert!(invalid.validate().is_err(), "非法组合仍应被拒");

        let mut duplicated = Settings::default();
        duplicated.hotkeys.capture_delayed = duplicated.hotkeys.toggle_main.clone();
        assert!(duplicated.validate().is_err(), "重复组合仍应被拒");
    }
}
