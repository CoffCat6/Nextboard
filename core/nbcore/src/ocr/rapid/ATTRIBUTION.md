# 归属与来源说明(OCR 引擎模块)

本目录(`core/nbcore/src/ocr/rapid/`)的引擎实现**派生自**第三方开源项目:

| 项 | 值 |
| --- | --- |
| 原名 | `rapid-ocr-rs` |
| 版本 | **v0.7.0** |
| 许可 | **Apache License 2.0**(全文见同目录 `LICENSE`) |
| 版权 | `Copyright (C) 2026 rapid-ocr-rs Contributors`(原文见下) |
| 上游仓库 | `https://gitee.com/mg-chao/snow-shot-workspaces` |
| 作者 | `mg-chao <chao@mgchao.top>` |

## 上游版权声明原文

```
rapid-ocr-rs
Copyright (C) 2026 rapid-ocr-rs Contributors

Unless otherwise noted, the original files in this directory are licensed
under the Apache License, Version 2.0. You may obtain a copy of the License at
https://www.apache.org/licenses/LICENSE-2.0.

The complete Apache License 2.0 terms are in LICENSE. Third-party components
remain subject to their respective licenses and attribution terms.

SPDX-License-Identifier: Apache-2.0
```

## 本项目做了哪些改动

按 `docs/11-rapidocr-rewrite-design.md` 的**方案 A(复制 + 裁剪)**内化:

| 改动 | 说明 |
| --- | --- |
| **删除模块** | `bin/`(CLI+bench)、`service.rs`(任务池/取消)、`output/`(JSON/Markdown/可视化)、
| | `rec/word_boxes.rs`(单字坐标)、`model_registry.rs`(模型下载路由) |
| **替换依赖** | `snow-memory` 的 `RasterBuffer` → `Vec<u8>`(等价,去掉一个外部 crate) |
| **edition 适配** | 上游为 edition 2024;本项目为 2021,故把 `let`-chain 改写为嵌套 `if` |
| **命名空间** | 模块内 `crate::` 引用统一改为 `crate::ocr::rapid::` |
| **配置** | 上游的 `cfg(feature = ...)` 保留,Cargo.toml 声明为**空特性**(固定不启用) |
| **缺陷修复** | 显式关闭 ORT 的 CPU arena 分配器(上游默认开启,实测致内存 874MB→62MB) |
| **测试适配** | 依赖 `%TEMP%` 的测试改用本仓 `target/tmp` 约定;随能力删除 1 个失效测试 |

## 分发义务(本项目需遵守)

Apache-2.0 §4 要求分发衍生作品时:

1. ✅ 附带许可副本 → **本目录的 `LICENSE`**;
2. ✅ 标明修改过的文件 → 见上表;
3. ✅ 保留版权、专利、商标与归属声明 → 本文件;
4. 若上游含 `NOTICE` 文件则需一并携带 —— **上游无 NOTICE 文件**(已核实)。

> ⚠️ 打包时(`tools/package.ps1`)不必额外拷贝本文件 —— 它是**源码树内的归属记录**,
> 不参与运行期分发。若未来改为二进制分发本模块,需重审此项。

## 相关文档

- 设计方案与裁剪依据:`docs/11-rapidocr-rewrite-design.md`
- 内存缺陷与修复:`docs/10-plan-b-implementation.md` §16
- 引擎集成决策:D1/D6/D7 见 `docs/10-plan-b-implementation.md` §12
