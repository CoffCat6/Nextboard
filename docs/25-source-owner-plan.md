# 25 · 应用来源追踪:剪贴板内容来自哪个应用

| 项 | 内容 |
| --- | --- |
| 版本 | v1(已实现) |
| 日期 | 2026-11-01 |
| 状态 | 已实现(`source_owner` 列 + 卡片展示;"来自 <应用>") |
| 关联 | [18-next-steps.md](./18-next-steps.md) §3.8(本设计实现其前半:追踪显示;3.7 的应用黑名单是后半,独立排期) |
| 一句话 | 每条剪贴板入库条目记录**复制时前台持有剪贴板的应用**(exe 名),列表卡片上以「来自 Chrome」小字展示 —— 回答"这段话我是从哪复制来的"。 |

---

## 1. 需求与定位

### 1.1 用户原话

> "开始考虑做应用来源追踪,入库内容显示来之那个应用"。

### 1.2 价值

剪贴板历史攒多了之后,"这段话是从哪复制的"是高频回忆场景(哪个网页、哪个聊天窗、
哪个文档)。第一代与本项目此前都只存内容不存来源,来源只能靠记忆。

### 1.3 与 source 列的区别(v5)

| 列 | 语义 | 取值 |
| --- | --- | --- |
| `source` | **入库通道**:内容怎么进的库 | clipboard / capture / translate / ocr / todo |
| `source_owner`(本设计) | **来源应用**:内容从哪个应用复制的 | chrome.exe / weixin.exe …(小写文件名) |

两列正交:截图条目 source=capture(无 owner),微信复制的文本 source=clipboard、
owner=weixin.exe。

---

## 2. 技术方案

### 2.1 采集点:WM_CLIPBOARDUPDATE 时查剪贴板属主

nbcore 的监听线程收到 `WM_CLIPBOARDUPDATE` 后,在读取内容**之前**:

```rust
GetClipboardOwner()                    // 当前剪贴板属主 HWND
  → GetWindowThreadProcessId()         // 属主 PID
  → OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)  // 限权打开(不需要更高)
  → QueryFullProcessImageNameW()       // 完整路径
  → 取文件名 + 小写化                  // "chrome.exe"
```

时序安全性:广播到达时属主已设置(WM_CLIPBOARDUPDATE 语义即"剪贴板内容已更新"),
比"复制后立刻退出"的应用竞态窗口极小;失败(权限/进程退出)按 NULL 存——
展示层对 NULL 显示为无来源,诚实降级。

### 2.2 存储只存 exe 文件名,不存完整路径

- 完整路径含用户名/安装目录,进库=扩大隐私面(备份包会被带出去);
- 展示只需要"来自 Chrome"级别的信息;
- 同应用不同安装位置(绿色版换目录)不影响展示与将来的黑名单匹配。

### 2.3 迁移 v9:ALTER 加列,不重建

`ALTER TABLE clip_entries ADD COLUMN source_owner TEXT` —— 与 v8 同手法,
零重建风险。既有条目 NULL = 未知(卡片不显示来源行);新条目起才有值。

### 2.4 与去重的交互

去重键是内容 hash:同内容再次复制会 touch 旧条目而非新建。属主信息**跟随首次
入库**(不随 touch 更新)——"这条内容第一次是从哪来的"比"最近一次"更符合直觉,
且省一次 UPDATE。此取舍记录在案,若用户反馈要"最近来源"再改。

---

## 3. 展示

- EntryCard 元信息行尾追加「来自 <应用>」小字(textTertiary 级);应用名展示为
  exe 文件名去掉 `.exe` 后缀(如 `chrome.exe` → `Chrome`?**不**,保持原样
  `chrome` —— 不做厂商映射表,避免维护负担与误导);
- `source_owner` 为 NULL 的条目(旧数据/采集失败)不显示该段;
- 截图/翻译/OCR 等应用内部入库条目不走剪贴板属主查询(owner NULL),来源已由
  source 徽标表达。

---

## 4. 边界与不做

| 不做 | 理由 |
| --- | --- |
| 存完整路径 | 隐私面(§2.2) |
| 应用名→友好名映射表 | 维护负担;exe 名已足够辨认 |
| 来源应用**黑名单/过滤**(3.7 后半) | 独立功能(需要设置 UI + 匹配策略),另立设计 |
| UWP 应用友好名 | GetApplicationUserModelId 体系复杂;UWP 剪贴板属主查到的是容器进程,本期按 NULL 处理 |

---

## 5. 验收

- 单测:迁移 v9 后列存在;owner 读写 round-trip;
- `--source-owner-flow`?(轻量:进程内模拟 SetClipboardData + owner 难以自证)
  **改为真机验收**:从浏览器/记事本各复制一段文本,卡片显示对应应用名;
  旧库升级后旧条目不显示来源行、不报错。
