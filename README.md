# Artificial-Satellite (`yrs-cli`)

一个面向 C++ 竞赛 / 刷题工作流的零依赖 C++23 命令行工具, 与 `Competitive Companion` + VSCode `cph-ng` 深度联动, 用来展开本地头文件并复制到剪贴板, 收拢比赛套题, 自动分拣 AC 单题, 清理 `.cph-ng` 缓存, 回填模板并归档历史代码

`bundle` 支持 Linux Wayland (`wl-copy`), X11 (`xclip` / `xsel`) 和 Windows / WSL (`clip.exe`)

## 功能概览

- `bundle [<FILE>]`: 查找当前工作区里最新修改的 `.cpp` 文件 (或显式传入 `<FILE>`), 递归展开 `library_root` 下的本地 `#include "..."`, 剥离行尾注释与 `#pragma once` 后复制到剪贴板
- `check`: 读取当前目录下的 `yrs.toml`, 打印工作区和各个关键路径配置
- `cover-latest`: 用模板文件覆盖当前工作区里最新修改的 `.cpp` 文件
- `pack`: 扫描工作区根目录按比赛聚合的 `.cpp` 文件与 `.cph-ng/*.bin` 元数据 (支持 `qoj`, `cf`, `gym`, `atcoder`, `uoj`, `nowcoder`, `luogu` 比赛), 列出根目录比赛或将指定比赛一键收拢至 `<CONTEST_DIR>/`:
  - 列出比赛: `yrs-cli pack`
  - 收拢比赛: `yrs-cli pack [-n|--dry-run] [<CONTEST_ID>] <CONTEST_DIR>` (根目录仅有一场比赛时可省略 `<CONTEST_ID>`; 写过代码的题自动注入首行 `// <url>`, 保留 `mtime`, 按题号重命名为 `A.cpp, B.cpp...` 并同步迁移重写 `.cph-ng/*.bin` 保留测试样例; 未动过的纯模板空题连同 `.bin` 自动清理)
- `stage`: 联动 `.cph-ng/*.bin` 元数据, 将已完成代码移入 `catalog_root`:
  - 单题自动分拣: `yrs-cli stage [-n|--dry-run]` (筛选全部启用测试点均为 `AC` 的单题 `.cpp`, 注入首行 `// <url>`, 保留原修改时间后分拣至 `luogu/`, `luogu/B/`, `library_checker/`, `loj/`, `uoj/`, `yukicoder/XXXX-YYYY/` 并清理 `.bin`; 自动跳过 QOJ / Codeforces / AtCoder 单题)
  - 比赛目录归档: `yrs-cli stage [-n|--dry-run] <SRC_DIR> <WAIT_SUBDIR>` (将整场比赛目录移入 `catalog_root/<WAIT_SUBDIR>`, 补齐首行 `// <url>` 并清理对应 `.bin` 与空目录)
- `move`: 把 `catalog_root` 中的文件移动到 `record_root`, 按 `UTC+8` 修改日期分组渲染目录树并更新 `summary_file`, 完成后自动清理孤儿 `.cph-ng/**/*.bin`
- `clean [-n|--dry-run]`: 清理工作区下源文件已移走或删除的孤儿 `.cph-ng/**/*.bin` 缓存文件

## `cph-ng` 协同工作流

1. **VSCode + `cph-ng` 平铺路由**: 将本仓库的 `tools/cph-route.js` 放入刷题工作区 `.vscode/cph-route.js`, 并在工作区 `.vscode/settings.json` 中配置自动模板与自定义路径脚本:
   ```json
   {
     "cph-ng.languages.cppCompilerArgs": "-O2 -DMeIoN -std=c++23 -Wall -DCPH -I/home/yorisou/Yorisou_alg_space",
     "cph-ng.problem.templateFile": "${workspace}/0.cpp",
     "cph-ng.companion.customPathScript": "${workspace}/.vscode/cph-route.js"
   }
   ```
   - 无论单题还是整场套题, 均静默以唯一题号文件名平铺生成在工作区根目录 (如 `qoj8328.cpp`, `2050A.cpp`, `abc395a.cpp`, `P1001.cpp`), 做题时直接 `#include "YRS/all.hpp"`
2. **QOJ 专用增强** (`tools/qoj-cph.user.js`):
   - 安装浏览器油猴扩展 (Tampermonkey / Violentmonkey) 后, 导入本仓库的 `tools/qoj-cph.user.js`
   - **单题页**: 无需从 PDF 题面切走, 点击页面顶部 **`⚡ 导入 CPH`** (或按 `Alt+Q`) 直接扫描题目附件链接 (`/download.php?type=attachments&id=<pid>`, `/problem/<pid>/attachments` 子页等) 在浏览器内存中解压 ZIP 提取全套大小样例; 无附件时自动回退至三层防错位题面 `<pre>` 解析 (或非 PDF 语言分支) 并推送到 `cph-ng` (`127.0.0.1:27121`)
   - **比赛页**: 点击比赛标题下方的 **`📦 整场导入 CPH`**, 6 路并发拉取全场时限/空限与附件样例后整批推送至 `cph-ng`, 直接在根目录平铺生成整场源码文件
3. **本地调试与提交**: 编写与本地测试后, 执行 `yrs-cli bundle` (或 `yrs-cli bundle <FILE>`) 展开本地头文件并复制到剪贴板提交
4. **赛后收拢与归档**:
   - 比赛打完 (或先暂存) 时执行 `yrs-cli pack [<CONTEST_ID>] <CONTEST_DIR>` 一键收进 `<CONTEST_DIR>/A.cpp...` (自动清理空模板, 保留半成品样例)
   - 整场补完后用 `yrs-cli stage <CONTEST_DIR> <WAIT_SUBDIR>` + `yrs-cli move` 归档入库; 单题全 `AC` 后直接 `yrs-cli stage` 自动分拣后 `yrs-cli move`

## 编译与安装

```bash
make          # 编译 yrs-cli (约 155KB)
make test     # 运行 ASan + UBSan 单元与集成测试
make install  # 安装到 $(INSTALL_DIR)/yrs-cli (默认 ~/.cargo/bin/yrs-cli)
make clean    # 清理编译与测试产物
```

## 配置文件 (`yrs.toml`)

```toml
library_root = "/home/yorisou/Yorisou_alg_space/"
template_source = "0.cpp"
catalog_root = ".wait"
record_root = "/home/yorisou/Yorisou_record/"
summary_file = "summary.md"
```

字段说明:
- `library_root`: 头文件根目录 (必填, 必须为绝对路径). `bundle` 只会从该目录内展开本地 `#include "..."`
- `catalog_root`: 待归档代码目录 (必填), `stage` 会将代码移入这里, `move` 会从这里搬运文件
- `record_root`: 归档目录 (必填), 历史代码和摘要文件均位于此处
- `summary_file`: 历史摘要文件名 (必填, 必须是 `record_root` 下的单个文件名)
- `template_source`: 模板源文件 (可选, 默认 `"0.cpp"`), `cover-latest` 用它覆盖最新 `.cpp`, `pack` 也会用它比对识别未修改的纯模板空题
