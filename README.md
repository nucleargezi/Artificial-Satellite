# Artificial-Satellite

一个面向 C++ 竞赛 / 刷题工作流的 Rust 命令行工具，用来整理临时代码、归档提交记录、回填模板、把最终提交内容快速复制到剪贴板，并直接向 PK OJ 提交代码。

当前项目更适合在 Windows / WSL 环境下使用，因为 `bundle` 子命令依赖 `clip.exe`。

## todo 

为特定的集合添加不同的 文件名显示逻辑

添加命令成功时的颜色

## 功能概览

- `check`：读取当前目录下的 `yrs.toml`，打印工作区和各个关键路径配置。
- `move`：把 `catalog_root` 中的文件移动到 `record_root`，并更新历史摘要文件。
- `cover-latest`：用模板文件覆盖当前工作区里最新修改的 `.cpp` 文件。
- `bundle`：找到当前工作区里最新修改的 `.cpp` 文件，展开本地 `#include "..."` 后复制到剪贴板。
- `submit`：读取显式指定的源码入口文件，自动展开本地 `#include "..."` 后提交到 PK OJ，并等待最终评测结果。

## 安装与运行

```bash
cargo install --path crates/yrs-cli

cargo uninstall yrs-cli
```

查看全部命令：

```bash
cargo run -p yrs-cli -- --help
```

## 配置文件

项目通过当前目录下的 `yrs.toml` 读取配置。一个最小示例如下：

```toml
library_root = "/absolute/path/to/headers"
template_source = "0.cpp"
catalog_root = ".wait"
record_root = "record"
summary_file = "summary.md"
bundle_output = "pre_zip.cpp"

[submit]
base_url = "https://icpc.bjtu.edu.cn"
cookie = "PHPSESSID=...; other=..."
timeout_secs = 30
poll_interval_secs = 1.0
```

字段说明：

- `library_root`：头文件根目录，必须使用绝对路径。`bundle` 和 `submit` 只会从这个目录展开本地 `#include "..."`，include 路径需要相对于这个目录书写。
- `catalog_root`：待归档代码目录，`move` 会从这里搬运文件。
- `record_root`：归档目录，历史代码和摘要文件都位于这里。
- `summary_file`：历史摘要文件名，必须是 `record_root` 下的文件名，不能写成嵌套路径。
- `template_source`：模板源文件，`cover-latest` 会用它覆盖最新的 `.cpp`。
- `bundle_output`：预留的打包输出路径配置；当前 CLI 里没有直接使用这个字段。
- `[submit]`：PK OJ 提交配置。
- `submit.base_url`：OJ 根地址，例如 `https://icpc.bjtu.edu.cn`。
- `submit.cookie`：直接写入请求头的原始 Cookie 字符串。
- `submit.timeout_secs`：等待最终 verdict 的总超时时间，默认 `30` 秒。
- `submit.poll_interval_secs`：轮询状态页的间隔，默认 `1.0` 秒。

## 使用示例

检查当前配置是否生效：

```bash
yrs-cli check
```

把 `.wait` 中的代码移动到 `record`，并更新 `summary.md`：

```bash
yrs-cli move
```

用模板文件覆盖当前目录最近修改的 `.cpp`：

```bash
yrs-cli cover-latest
```

展开当前目录最近修改的 `.cpp` 的本地头文件，并复制到剪贴板：

```bash
yrs-cli bundle
```

提交指定源码入口文件到 PK OJ，并等待结果：

```bash
yrs-cli submit --problem 9584 --source main.cpp --lang "GNU C++ 11.4.0"
```

## 一个典型工作流

1. 在工作区中编写或修改 `.cpp` 文件。
2. 运行 `move`，把 `.wait` 中已完成的代码归档到 `record`。
3. 运行 `cover-latest`，用模板快速回填当前最新的题解文件。
4. 运行 `bundle`，展开本地头文件并直接复制提交内容。
5. 运行 `submit`，显式指定题号、入口文件和语言，等待 OJ 返回最终结果。

## 注意事项

- 当前目录缺少 `yrs.toml` 时，CLI 会直接报错退出。
- `library_root` 是必填项，且必须是绝对路径。
- `cover-latest` 会直接覆盖当前工作区中最新修改的 `.cpp` 文件，请在确认后使用。
- `move` 是“移动”而不是“复制”；文件会从 `catalog_root` 挪到 `record_root`。
- `bundle` 当前依赖 `clip.exe`，更适合 Windows / WSL 环境；本地头文件只会从 `library_root` 展开。
- `bundle` 目前是复制到剪贴板，不会直接写出打包文件。
- `submit` 会读取 `yrs.toml` 里的原始 Cookie 字符串；请自行保护好配置文件中的登录态。提交前的源码展开同样只会从 `library_root` 查找本地头文件。
- `submit` 当前只支持这个 PK OJ，不做多 OJ 适配。

## 项目结构

这是一个 Cargo workspace，包含两个 crate：

- `crates/yrs-core`：核心逻辑，包括配置加载、最新文件查找、归档历史整理、bundle 和 submit 处理。
- `crates/yrs-cli`：命令行入口，负责解析子命令并调用 `yrs-core`。
