# Artificial-Satellite

一个面向 C++ 竞赛 / 刷题工作流的 Rust 命令行工具, 用来整理临时代码, 归档提交记录, 回填模板, 把最终提交内容快速复制到剪贴板, 同时支持命令行提交代码和模板库自动测试, 并为模板库的静态网页维护一系列信息

当前项目更适合在 WSL 环境下使用, 因为 `bundle` 子命令依赖 `clip.exe`. 

## todo 

## 功能概览

- `check`: 读取当前目录下的 `yrs.toml`, 打印工作区和各个关键路径配置. 
- `move`: 把 `catalog_root` 中的文件移动到 `record_root`, 并更新历史摘要文件. 
- `cover-latest`: 用模板文件覆盖当前工作区里最新修改的 `.cpp` 文件. 
- `bundle`: 找到当前工作区里最新修改的 `.cpp` 文件, 展开本地 `#include "..."` 后复制到剪贴板. 
- `submit`: 读取显式指定的源码入口文件, 自动展开本地 `#include "..."` 后提交到 BJTU OJ, 并等待最终评测结果. 
- `test_template`: 扫描模板仓库下的 `test/**/*.cpp`, 根据 `git diff` 和头文件依赖选出受影响的测试, 顺序提交到 BJTU OJ, 并写出状态快照. 

## 安装与运行

```bash
cargo install --path crates/yrs-cli

cargo uninstall yrs-cli
```

查看全部命令: 

```bash
cargo run -p yrs-cli -- --help
```

## 配置文件

项目通过当前目录下的 `yrs.toml` 读取配置. 一个最小示例如下: 

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

[template_test]
template_repo_root = "/absolute/path/to/YRS"
state_file = ".yrs/test_template/state.toml"
default_language = "GNU C++ 11.4.0"
blacklist = ["IO/fast_io.hpp"]
```

字段说明: 

- `library_root`: 头文件根目录, 必须使用绝对路径. `bundle` 和 `submit` 只会从这个目录展开本地 `#include "..."`, include 路径需要相对于这个目录书写. 
- `catalog_root`: 待归档代码目录, `move` 会从这里搬运文件. 
- `record_root`: 归档目录, 历史代码和摘要文件都位于这里. 
- `summary_file`: 历史摘要文件名, 必须是 `record_root` 下的文件名, 不能写成嵌套路径. 
- `template_source`: 模板源文件, `cover-latest` 会用它覆盖最新的 `.cpp`. 
- `bundle_output`: 预留的打包输出路径配置；当前 CLI 里没有直接使用这个字段. 
- `[submit]`: BJTU OJ 提交配置. 
- `submit.base_url`: OJ 根地址, 例如 `https://icpc.bjtu.edu.cn`. 
- `submit.cookie`: 直接写入请求头的原始 Cookie 字符串. 
- `submit.timeout_secs`: 单次提交流程的等待上限, 默认 `30` 秒. 
- `submit.poll_interval_secs`: 轮询状态页的间隔, 默认 `1.0` 秒. 
- `[template_test]`: 模板测试配置. 
- `template_test.template_repo_root`: 模板仓库根目录, 必须是绝对路径, 必须是一个 git 仓库根目录, 并且需要位于 `library_root` 之下. 
- `template_test.state_file`: 模板测试状态快照输出位置. 绝对路径会直接使用；相对路径会解析到 `template_repo_root` 下. 
- `template_test.default_language`: `test_template` 默认使用的提交语言名称, 需要能在目标 OJ 的语言列表里匹配到. 
- `template_test.blacklist`: 可选的头文件黑名单, 列表中的路径必须是相对 `template_repo_root` 的 `.hpp` 路径. 它们不会出现在测试依赖输出, 模板覆盖列表和模板依赖图里, 但这些文件的改动仍然会触发相关测试. 

## 使用示例

检查当前配置是否生效: 

```bash
yrs-cli check
```

把 `.wait` 中的代码移动到 `record`, 并更新 `summary.md`: 

```bash
yrs-cli move
```

用模板文件覆盖当前目录最近修改的 `.cpp`: 

```bash
yrs-cli cover-latest
```

展开当前目录最近修改的 `.cpp` 的本地头文件, 并复制到剪贴板: 

```bash
yrs-cli bundle
```

提交指定源码入口文件到 BJTU OJ, 并等待结果: 

```bash
yrs-cli submit --problem 9584 --source main.cpp --lang "GNU C++ 11.4.0"
```

根据模板仓库的变更范围跑受影响测试, 并输出 TOML 报告: 

```bash
yrs-cli test_template --base origin/main --head HEAD --toml
```

强制全量重跑模板测试: 

```bash
yrs-cli test_template --base origin/main --all
```

只调试部分测试, 并限制本次最多执行 2 个用例: 

```bash
yrs-cli test_template --base origin/main --filter "test/fps/" --max-cases 2
```

## `test_template` 说明

`test_template` 主要面向模板库仓库的回归验证场景. 它会在 `template_test.template_repo_root` 下执行如下流程: 

1. 读取 `git diff <base>...<head>` 的变更路径. 
2. 扫描 `test/**/*.cpp`. 
3. 复用现有 bundler 的 include 展开逻辑, 重建每个测试的传递 `.hpp` 头文件依赖. 
4. 选出“测试文件本身改动”或“依赖头文件改动”的测试. 
5. 顺序提交这些测试到 BJTU OJ. 
6. 重写 TOML 状态报告到 `state_file`. 

测试文件约定: 

- 测试源文件必须位于模板仓库的 `test/**/*.cpp`. 
- 首行需要写成 `// https://.../problem/<id>`. 
- 依赖触发只认模板仓库中的 `.hpp` 文件；其他后缀即使被 `#include`, 也不会作为回归触发条件. 
- 如果某个 `.hpp` 被写进 `template_test.blacklist`, 它仍然参与依赖触发判断, 但不会出现在最终报告的依赖信息里. 
- 如果某个测试文件的首行 URL 非法, 它不会被提交, 但会以 `invalid` 状态记录到报告, 并让本次命令以失败结束. 
- 每次运行都会额外汇总模板覆盖情况, 把非 `test/` 目录下的 `.hpp` 模板分成 `all_passed`, `has_failures` 和 `unused` 三类. 
- TOML 报告中的 `tests.<case>.dependencies` 表示该测试可见的传递 `.hpp` 依赖. 
- 顶层 `template_dependencies` 记录“每个可见模板依赖哪些可见头文件”. 
- 顶层 `template_dependents` 记录“每个可见头文件被哪些可见头文件直接或间接依赖”, 它和 `template_dependencies` 是同一张可见依赖图的反向索引. 

常用参数: 

- `--base <rev>`: 必填, `git diff` 的 base revision. 
- `--head <rev>`: 可选, 默认 `HEAD`. 
- `--toml`: 把本次运行报告打印为 TOML. 
- `--all`: 忽略 diff 结果, 直接重跑所有已发现测试. 
- `--filter <pattern>`: 按测试相对路径做区分大小写的子串过滤. 
- `--max-cases <n>`: 在过滤后的选中集合上再截断前 `n` 个测试. 

退出码约定: 

- `0`: 命令运行完成, 且本次没有失败或 `invalid` 测试. 
- `1`: 命令运行完成, 但至少有一个测试失败, 或者扫描到非法测试文件. 
- `2`: 系统级失败, 例如配置缺失, git diff 失败, 状态文件损坏或写入失败. 

## 一个典型工作流

1. 在工作区中编写或修改 `.cpp` 文件. 
2. 运行 `move`, 把 `.wait` 中已完成的代码归档到 `record`. 
3. 运行 `cover-latest`, 用模板快速回填当前最新的题解文件. 
4. 运行 `bundle`, 展开本地头文件并直接复制提交内容. 
5. 运行 `submit`, 显式指定题号, 入口文件和语言, 等待 OJ 返回最终结果. 
6. 在模板仓库改动后, 运行 `test_template` 做一轮受影响测试回归, 并保留 `state.toml` 供后续静态页面或 CI 使用. 

## 注意事项

- 当前目录缺少 `yrs.toml` 时, CLI 会直接报错退出. 
- `library_root` 是必填项, 且必须是绝对路径. 
- `cover-latest` 会直接覆盖当前工作区中最新修改的 `.cpp` 文件, 请在确认后使用. 
- `move` 是“移动”而不是“复制”；文件会从 `catalog_root` 挪到 `record_root`. 
- `bundle` 当前依赖 `clip.exe`, 更适合 Windows / WSL 环境；本地头文件只会从 `library_root` 展开. 
- `bundle` 目前是复制到剪贴板, 不会直接写出打包文件. 
- `submit` 会读取 `yrs.toml` 里的原始 Cookie 字符串；请自行保护好配置文件中的登录态. 提交前的源码展开同样只会从 `library_root` 查找本地头文件. 
- `submit` 当前只支持这个 BJTU OJ, 不做多 OJ 适配. 
- `test_template` 依赖 `git` 命令可用, 并假定模板仓库本身是一个独立 git 仓库. 
- `test_template` 会把状态快照写回模板仓库目录；如果你在 CI 中使用它, 记得保留或上传 `state_file`. 

## 项目结构

这是一个 Cargo workspace, 包含两个 crate: 

- `crates/yrs-core`: 核心逻辑, 包括配置加载, 最新文件查找, 归档历史整理, bundle 和 submit 处理. 
- `crates/yrs-cli`: 命令行入口, 负责解析子命令并调用 `yrs-core`. 
