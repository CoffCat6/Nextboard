//! 设置读写:serde 直读写 `app_config.json`(取代第一代的 tauri-plugin-store,
//! **JSON 结构不变**——外层 `{ "settings": { … } }`,老配置文件可直接读取)。
//! 自定义数据目录的迁移流程沿用第一代 docs/phase4.md(标记文件 + 重启生效)。

use std::path::{Path, PathBuf};

use log::{info, warn};
use serde::{Deserialize, Serialize};

use crate::error::{AppError, AppResult};

const CONFIG_FILE: &str = "app_config.json";
/// 待迁移标记：位于默认数据目录，内容为迁移源目录（Phase 4）。
const DATA_MOVE_MARKER: &str = "data-move-pending.txt";

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
    /// 开机自启（M3 生效）。
    pub auto_start: bool,
    /// 暂停/恢复剪贴板监听。
    pub monitor_enabled: bool,
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
    /// 框选截图是否松开鼠标即自动确认（Phase 5 增强）：关闭时保留手柄微调 + Enter/双击确认。
    pub region_auto_confirm: bool,
    /// 框选确认后是否**跳过编辑器**：默认开启，确认即入库 + 写剪贴板（原 S2 直通行为），
    /// 与 `region_auto_confirm` 叠加即为"拖拽松手 → 已复制"一步到位；
    /// 需要标注时关闭本项（框选进编辑器），或走历史图片右键「编辑」。
    pub region_skip_editor: bool,
    /// OCR 语言：`auto`（默认，按 zh-Hans-CN → en-US → 用户配置语言）/ 指定语言标签如 `en-US`。
    pub ocr_language: String,
    /// OCR 后端：`system`（默认，Windows 内置离线引擎）/ `tesseract`（需自行安装并保证在 PATH）。
    pub ocr_backend: String,
    /// OCR 放大倍数（1~4）：小字号截图放大后再识别可显著提升准确率，默认 2。
    pub ocr_scale: i64,
    /// OCR 自动入库（Phase 9 F28，默认**关闭**）：图片条目入库时后台识别文字并存入
    /// `ocr_text` 列参与全文搜索。关闭则只在用户右键「识别文字」时识别（既有行为）。
    /// 默认关的理由：OCR 约 0.7s/条，批量导入截图时逐条跑会拖慢入库；且用户可能
    /// 并不需要搜索截图内容。
    pub ocr_enabled: bool,
    /// 翻译提供方：`openai`（默认）/ `microsoft` / `custom`（Phase 7 F21~F23）。
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
    pub endpoint: String,
    /// 区域或多服务资源必填（如 `chinaeast2`）；留空则不带区域头。
    pub region: String,
    pub api_key: String,
}

impl Default for MicrosoftProvider {
    fn default() -> Self {
        Self {
            endpoint: "https://api.translator.azure.cn".into(),
            region: String::new(),
            api_key: String::new(),
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
    /// 一键贴图：把最近使用的图片条目贴为浮窗。
    PinLatest,
    /// 打开翻译窗口（Phase 7 F24）。
    OpenTranslate,
    /// 截图翻译（Phase 7 T4）：框选 → OCR → 翻译。
    CaptureTranslate,
}

impl HotkeyAction {
    /// 全部动作，遍历注册用。
    pub const ALL: [HotkeyAction; 6] = [
        HotkeyAction::ToggleMain,
        HotkeyAction::CaptureRegion,
        HotkeyAction::CaptureFullscreen,
        HotkeyAction::PinLatest,
        HotkeyAction::OpenTranslate,
        HotkeyAction::CaptureTranslate,
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
    /// 一键贴图（最近使用的图片）。
    pub pin_latest: String,
    /// 打开翻译窗口（Phase 7）。
    pub open_translate: String,
    /// 截图翻译（Phase 7 T4）：框选 → OCR → 翻译。
    pub capture_translate: String,
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
            auto_start: false,
            monitor_enabled: true,
            theme: "system".into(),
            data_dir: String::new(),
            pin_default_opacity: 100,
            pin_doubleclick_close: true,
            region_auto_confirm: true,
            // 默认"松手即复制"：截图后立刻可粘贴，不经过编辑器
            region_skip_editor: true,
            ocr_language: "auto".into(),
            ocr_backend: "system".into(),
            // 放大 2 倍：实测对 12px 代码截图明显改善且耗时可控（docs/phase6.md §3.6）
            ocr_scale: 2,
            ocr_enabled: false,
            translate_provider: "openai".into(),
            translate_source_lang: "auto".into(),
            translate_target_lang: "zh-Hans".into(),
            translate_timeout_secs: 30,
            proxy: ProxySettings::default(),
            translate_openai: OpenAiProvider::default(),
            translate_microsoft: MicrosoftProvider::default(),
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
        // 快捷键：非空 + 合法组合 + 两两互不相同（相同组合键不能绑定多个动作）
        let mut seen: Vec<&str> = Vec::new();
        for action in HotkeyAction::ALL {
            let hotkey = self.hotkeys.get(action).trim();
            if hotkey.is_empty() {
                return Err(AppError::InvalidInput("快捷键不能为空".into()));
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
        // OCR：后端枚举 + 语言标签格式 + 放大倍数范围
        if !matches!(
            self.ocr_backend.trim().to_ascii_lowercase().as_str(),
            "" | "system" | "windows" | "tesseract"
        ) {
            return Err(AppError::InvalidInput(
                "OCR 后端只支持 system（内置）或 tesseract".into(),
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
            "" | "openai" | "microsoft" | "custom"
        ) {
            return Err(AppError::InvalidInput(
                "翻译提供方只支持 openai（AI 翻译）、microsoft 或 custom".into(),
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
        // 微软：端点与区域
        validate_endpoint("微软翻译接口地址", &self.translate_microsoft.endpoint)?;
        let region = self.translate_microsoft.region.trim();
        if !region.is_empty()
            && !region
                .chars()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
        {
            return Err(AppError::InvalidInput(
                "微软翻译区域只能是小写字母、数字与连字符（如 chinaeast2）".into(),
            ));
        }
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
    let Ok(mut root) = serde_json::from_str::<serde_json::Map<String, serde_json::Value>>(&text)
    else {
        warn!("配置文件解析失败,使用默认设置: {path:?}");
        return Ok(Settings::default());
    };
    let Some(mut value) = root.remove("settings") else {
        return Ok(Settings::default());
    };
    migrate_legacy_hotkeys(&mut value);
    migrate_legacy_proxy(&mut value);
    Ok(serde_json::from_value(value).unwrap_or_else(|err| {
        warn!("配置字段解析失败,使用默认设置: {err}");
        Settings::default()
    }))
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
    fn validate_rejects_empty_hotkey() {
        let mut settings = Settings::default();
        settings.hotkeys.capture_fullscreen = "  ".into();
        assert!(settings.validate().is_err());
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
    fn validate_checks_ocr_options() {
        let with = |backend: &str, language: &str, scale: i64| Settings {
            ocr_backend: backend.into(),
            ocr_language: language.into(),
            ocr_scale: scale,
            ..Default::default()
        };
        // 合法组合
        assert!(with("system", "auto", 2).validate().is_ok());
        assert!(with("tesseract", "en-US", 1).validate().is_ok());
        // 后端只接受枚举值
        assert!(with("paddle", "auto", 2).validate().is_err());
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
        // 六个动作的默认键必须两两不同（validate 也会拒绝重复）
        let mut all: Vec<&str> = HotkeyAction::ALL.iter().map(|a| map.get(*a)).collect();
        all.sort_unstable();
        all.dedup();
        assert_eq!(all.len(), HotkeyAction::ALL.len(), "默认快捷键不得重复");
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
        assert!(settings.region_skip_editor, "默认直通剪贴板、不进编辑器");
        assert_eq!(settings.ocr_language, "auto", "OCR 默认自动选语言");
        assert_eq!(settings.ocr_backend, "system", "OCR 默认用系统内置引擎");
        assert_eq!(settings.ocr_scale, 2, "OCR 默认放大 2 倍");
        assert!(
            !settings.ocr_enabled,
            "OCR 自动入库默认关闭（右键手动识别不受影响）"
        );
        assert!(
            settings.pin_doubleclick_close,
            "双击关贴图默认开（既有 Snipaste 习惯，设置只让它可配置）"
        );
        // Phase 7：翻译字段缺失时全部取默认（旧配置升级不阻塞启动）
        assert_eq!(settings.translate_provider, "openai");
        assert_eq!(settings.translate_source_lang, "auto");
        assert_eq!(settings.translate_target_lang, "zh-Hans");
        assert_eq!(settings.translate_timeout_secs, 30);
        assert_eq!(settings.proxy.mode, "system", "默认跟随系统代理");
        assert!(settings.proxy.manual.host.is_empty());
        assert_eq!(settings.hotkeys.open_translate, "Ctrl+Shift+T");
        assert_eq!(settings.hotkeys.capture_translate, "Ctrl+Shift+E");
        assert_eq!(
            settings.translate_microsoft.endpoint, "https://api.translator.azure.cn",
            "默认中国版端点"
        );
    }

    #[test]
    fn validate_accepts_six_distinct_hotkeys() {
        assert!(Settings::default().validate().is_ok(), "六个默认键互不重复");
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
    fn validate_checks_microsoft_region_and_proxy() {
        let with_region = |region: &str| Settings {
            translate_microsoft: MicrosoftProvider {
                region: region.into(),
                ..MicrosoftProvider::default()
            },
            ..Default::default()
        };
        assert!(with_region("").validate().is_ok(), "留空允许");
        assert!(with_region("chinaeast2").validate().is_ok());
        assert!(
            with_region("China East 2").validate().is_err(),
            "大写与空格非法"
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
}
