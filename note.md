# 用 Rust 复刻 `ztor-rs` 的新手实战计划

> 目标：你是一个几乎不会 Rust、也不会 `cargo` 的新手，但希望**照着现在这个项目，自己从零做出一个类似的 Rust 工具**。  
> 这份计划会按“先会用，再会写，再会拆结构，再会做功能”的顺序来走。  
> 你不需要一口气看完，按阶段推进就行。

---

## 0. 最终你要做成什么

你要复刻的是一个命令行工具 `ztor`，它至少有这几类能力：

1. `template`
   把模板文件复制到目标源码。

2. `bundle`
   展开本地 `#include "..."`，并输出合并后的源码。

3. `catalog`
   扫描 `.wait/` 之类的归档目录，生成题目清单。

4. `doctor`
   显示当前工具实际使用的配置和路径。

而且结构上不是一个乱糟糟的单文件程序，而是：

- 一个 `cargo workspace`
- 一个核心库 `ztor-core`
- 一个命令行程序 `ztor-cli`

---

# 第一阶段：先把 Rust 和 Cargo 用起来

## 1. 你要先掌握的最小知识

先不用学完整 Rust，只要先理解这几个概念：

### 1.1 Rust 是什么
Rust 是一门编译型语言。  
你写 `.rs` 文件，最后编译成一个可执行程序。

### 1.2 Cargo 是什么
`cargo` 是 Rust 的官方工具，类似“项目管理器 + 构建工具 + 测试工具”。

你以后最常用的命令就这些：

```bash
cargo new
cargo build
cargo run
cargo test
cargo fmt
```

### 1.3 crate 是什么
可以粗暴理解为“一个 Rust 项目单元”。

- `library crate`：给别人调用的库
- `binary crate`：可执行程序

你这次要做两个 crate：

- `ztor-core`：库
- `ztor-cli`：命令行程序

### 1.4 workspace 是什么
workspace 可以把多个 crate 放进一个大项目里统一管理。

这次你要做的就是：

- 顶层 workspace
- 里面放 `ztor-core`
- 里面放 `ztor-cli`

---

## 2. 第一周只做环境和语法热身

### 2.1 安装 Rust

如果还没装：

```bash
rustc --version
cargo --version
```

如果没输出版本号，就去装 Rust。  
装好后确认：

```bash
rustc --version
cargo --version
```

### 2.2 新建一个练手项目

```bash
cargo new rust-hello
cd rust-hello
cargo run
```

你会看到输出类似：

```text
Hello, world!
```

### 2.3 学会改代码再运行

打开 `src/main.rs`，改成：

```rust
fn main() {
    println!("hello ztor");
}
```

然后：

```bash
cargo run
```

### 2.4 学会写函数

把 `src/main.rs` 改成：

```rust
fn add(a: i32, b: i32) -> i32 {
    a + b
}

fn main() {
    let ans = add(2, 3);
    println!("{ans}");
}
```

理解这几件事：

- `fn` 是函数
- `-> i32` 是返回值类型
- `let` 是定义变量
- `println!` 是输出

### 2.5 学会写结构体

再练：

```rust
struct User {
    name: String,
    age: u32,
}

fn main() {
    let user = User {
        name: String::from("yorisou"),
        age: 18,
    };

    println!("{} {}", user.name, user.age);
}
```

这一步是给后面做 `AppConfig`、`TemplateRequest`、`Entry` 打基础。

---

# 第二阶段：先学会用 Cargo 管项目

## 3. 你要熟悉的 Cargo 基础命令

### 3.1 构建

```bash
cargo build
```

### 3.2 运行

```bash
cargo run
```

### 3.3 测试

```bash
cargo test
```

### 3.4 格式化

```bash
cargo fmt
```

### 3.5 添加依赖

Rust 项目的依赖写在 `Cargo.toml`。  
比如：

```toml
[dependencies]
serde = { version = "1.0", features = ["derive"] }
```

---

## 4. 先做一个最小 CLI 工具

你先不要急着做 workspace。  
先做一个单独的小工具，理解命令行参数。

### 4.1 建项目

```bash
cargo new mini-cli
cd mini-cli
```

### 4.2 加 `clap`

在 `Cargo.toml` 里加：

```toml
[dependencies]
clap = { version = "4.5", features = ["derive"] }
```

### 4.3 写最小参数程序

`src/main.rs`：

```rust
use clap::Parser;

#[derive(Parser, Debug)]
struct Cli {
    name: String,
}

fn main() {
    let cli = Cli::parse();
    println!("hello {}", cli.name);
}
```

运行：

```bash
cargo run -- yorisou
```

你会看到：

```text
hello yorisou
```

### 4.4 理解这一步的意义

这就是后面 `ztor template apply`、`ztor doctor` 的基础。

---

# 第三阶段：开始正式搭 `ztor` 的项目骨架

## 5. 照着现在的结构，自己重建目录

你自己新建一个目录，比如：

```bash
mkdir my-ztor
cd my-ztor
```

### 5.1 新建 workspace 顶层 `Cargo.toml`

写成：

```toml
[workspace]
members = ["crates/ztor-core", "crates/ztor-cli"]
resolver = "2"
```

### 5.2 创建两个 crate

```bash
mkdir -p crates
cargo new crates/ztor-core --lib
cargo new crates/ztor-cli --bin
```

### 5.3 现在你的目录应该像这样

```text
my-ztor/
├── Cargo.toml
└── crates/
    ├── ztor-core/
    │   ├── Cargo.toml
    │   └── src/lib.rs
    └── ztor-cli/
        ├── Cargo.toml
        └── src/main.rs
```

---

## 6. 先让两个 crate 连起来

### 6.1 在 `ztor-core/src/lib.rs` 里写一个函数

```rust
pub fn hello() -> &'static str {
    "hello from core"
}
```

### 6.2 在 `ztor-cli/Cargo.toml` 里依赖它

```toml
[dependencies]
ztor-core = { path = "../ztor-core" }
```

### 6.3 在 `ztor-cli/src/main.rs` 里调用

```rust
fn main() {
    println!("{}", ztor_core::hello());
}
```

### 6.4 运行

在项目顶层执行：

```bash
cargo run -p ztor-cli
```

如果能输出：

```text
hello from core
```

说明你的 workspace 已经打通了。

---

# 第四阶段：先做最简单的功能 `doctor`

## 7. 为什么先做 `doctor`

因为它最简单，但能帮你熟悉：

- 模块拆分
- 配置读取
- CLI 子命令
- 路径操作

这是最适合新手的第一块真实功能。

---

## 8. 先定义配置结构

在 `ztor-core` 里新建：

- `src/config.rs`
- `src/workspace.rs`

然后在 `src/lib.rs` 里导出：

```rust
pub mod config;
pub mod workspace;
```

### 8.1 先做最小版配置结构

```rust
use std::path::PathBuf;

#[derive(Debug, Clone)]
pub struct AppConfig {
    pub workspace_root: PathBuf,
    pub template_source: PathBuf,
    pub bundle_output: PathBuf,
    pub catalog_roots: Vec<PathBuf>,
}
```

### 8.2 先不要急着读 TOML
第一步你可以先写一个“默认配置函数”。

```rust
impl AppConfig {
    pub fn default_for_dir(dir: PathBuf) -> Self {
        Self {
            workspace_root: dir.clone(),
            template_source: dir.join("0.cpp"),
            bundle_output: dir.join("zip_pre.cpp"),
            catalog_roots: vec![dir.join(".wait")],
        }
    }
}
```

---

## 9. 在 CLI 里做 `doctor`

### 9.1 用 `clap` 定义命令

目标是支持：

```bash
ztor doctor
```

### 9.2 先做最小版命令树

```rust
use clap::{Parser, Subcommand};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    Doctor,
}

fn main() {
    let cli = Cli::parse();

    match cli.command {
        Commands::Doctor => {
            println!("doctor");
        }
    }
}
```

### 9.3 接入配置

调用默认配置，然后输出：

```rust
use std::env;
use ztor_core::config::AppConfig;

fn main() {
    let cli = Cli::parse();

    match cli.command {
        Commands::Doctor => {
            let cwd = env::current_dir().unwrap();
            let config = AppConfig::default_for_dir(cwd.clone());

            println!("cwd: {}", cwd.display());
            println!("workspace_root: {}", config.workspace_root.display());
            println!("template_source: {}", config.template_source.display());
            println!("bundle_output: {}", config.bundle_output.display());
        }
    }
}
```

如果这一步你自己能独立写出来，你就已经跨过“完全不会 Rust”的门槛了。

---

# 第五阶段：学会测试，再做 `template`

## 10. 先学测试，不要直接堆功能

你这类工具非常适合测试驱动。  
因为它们基本都是：

- 读文件
- 写文件
- 扫目录
- 检查输出

### 10.1 写你的第一个测试

在 `ztor-core/src/lib.rs` 里先随便练一个：

```rust
pub fn add(a: i32, b: i32) -> i32 {
    a + b
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_works() {
        assert_eq!(add(2, 3), 5);
    }
}
```

运行：

```bash
cargo test
```

---

## 11. 开始做 `template`

目标功能：

```bash
ztor template apply --target ./main.cpp
```

逻辑：

1. 找到模板源，比如 `0.cpp`
2. 找到目标文件
3. 把模板内容复制过去

---

## 12. `template` 的学习式拆解

### Step 1：先定义请求结构

```rust
pub struct TemplateRequest {
    pub source: PathBuf,
    pub target: PathBuf,
    pub dry_run: bool,
}
```

### Step 2：先写测试

测试要覆盖：

- 正常复制
- `dry_run` 不写文件
- 模板不存在时报错

### Step 3：再写实现

你要学会这些 Rust 标准库接口：

- `std::fs::read_to_string`
- `std::fs::write`
- `std::path::PathBuf`

### Step 4：接到 CLI

命令长这样：

```bash
ztor template apply --target ./main.cpp
```

---

# 第六阶段：做 `bundle`

## 13. 这是你第一个“稍微复杂”的模块

它的本质是：

1. 读一个 `.cpp`
2. 找里面的 `#include "xxx.hpp"`
3. 定位 header 文件
4. 把 header 内容替换进来
5. 避免重复展开
6. 检测循环依赖
7. 可选做简单压缩

---

## 14. 新手做 `bundle` 的正确顺序

不要一上来就想做完整预处理器。  
按下面顺序做。

### 14.1 只支持这一种 include

```cpp
#include "foo.hpp"
```

先不管：

- `#include <vector>`
- 宏
- 条件编译
- 多行宏

### 14.2 先只做单层展开

例如：

`main.cpp`

```cpp
#include "a.hpp"
int main() {}
```

`a.hpp`

```cpp
int x = 1;
```

输出：

```cpp
int x = 1;
int main() {}
```

### 14.3 再做多层展开

`a.hpp` 里再 include `b.hpp`

### 14.4 再做去重

同一个头文件不要展开两次。

### 14.5 再做循环依赖检测

如果：

- `a.hpp` include `b.hpp`
- `b.hpp` include `a.hpp`

就报错。

### 14.6 最后再做压缩

先做最简单的两条：

- 去掉 `#pragma once`
- 去掉行尾 `// comment`
- 压缩连续空行

---

## 15. `bundle` 适合怎么拆模块

建议你按这个思路拆：

- `parse_include(line: &str) -> Option<String>`
- `resolve_include(current_file, header, include_paths)`
- `expand_file(path, state)`
- `optimize_output(lines)`

这样每个函数都短，适合新手调试。

---

# 第七阶段：做 `catalog`

## 16. `catalog` 是很适合新手练“数据建模”的模块

你要做的是：

1. 扫描目录
2. 找到所有 `.cpp`
3. 从路径提取字段
4. 排序
5. 输出为字符串或 JSON

---

## 17. 先定义条目结构

例如：

```rust
pub struct Entry {
    pub date: String,
    pub oj: String,
    pub contest: Option<String>,
    pub problem: String,
    pub path: PathBuf,
}
```

### 17.1 你会练到这些 Rust 能力

- `walkdir` 扫目录
- `Path` / `PathBuf` 操作路径
- 排序
- `Option`
- 字符串处理

---

## 18. `catalog` 的新手实现顺序

### Step 1：先扫描所有 `.cpp`

### Step 2：先从路径里提取字段

比如：

```text
.wait/luogu/P1000.cpp
```

得到：

- `oj = "luogu"`
- `contest = None`
- `problem = "P1000"`

再比如：

```text
.wait/cf/900/A1.cpp
```

得到：

- `oj = "cf"`
- `contest = Some("900")`
- `problem = "A1"`

### Step 3：先只做最普通输出

比如：

```text
luogu P1000
cf 900 A1
```

### Step 4：再加 OJ 特殊格式规则

例如：

- `luogu` -> `Luogu P1000`
- `cf/900/A1.cpp` -> `cf_900 - A`

### Step 5：再加 JSON 输出

这是练 `serde` 的很好机会。

---

# 第八阶段：把配置文件 `ztor.toml` 接进来

## 19. 为什么配置要放后面学

因为你如果太早碰：

- `serde`
- `toml`
- 反序列化
- 默认值
- 相对路径归一化

会同时学习太多东西。

先把功能跑起来，再把硬编码改成配置，学习体验更好。

---

## 20. 你要学的配置相关依赖

在 `Cargo.toml` 里加：

```toml
serde = { version = "1.0", features = ["derive"] }
toml = "0.8"
```

### 20.1 配置结构体加 `Deserialize`

```rust
use serde::Deserialize;
use std::path::PathBuf;

#[derive(Debug, Deserialize)]
pub struct AppConfig {
    pub workspace: WorkspaceConfig,
}

#[derive(Debug, Deserialize)]
pub struct WorkspaceConfig {
    pub root: PathBuf,
}
```

### 20.2 从文件读取 TOML

```rust
let text = std::fs::read_to_string("ztor.toml").unwrap();
let config: AppConfig = toml::from_str(&text).unwrap();
```

---

## 21. 你最后要把这些内容都配置化

- `workspace.root`
- `template.source`
- `bundle.include_paths`
- `bundle.output`
- `catalog.roots`

等你做到这一步，你就基本能自己维护这个项目了。

---

# 第九阶段：把 CLI 做完整

## 22. 推荐命令树

等你前面几块功能都做出来后，再收束成完整 CLI：

```bash
ztor config init
ztor template apply
ztor bundle run
ztor bundle deps
ztor catalog build
ztor catalog ls
ztor doctor
ztor completion
```

---

## 23. CLI 开发顺序

不要一次把所有命令都写完。  
推荐顺序：

1. `doctor`
2. `template apply`
3. `bundle run`
4. `bundle deps`
5. `catalog build`
6. `config init`
7. `completion`

---

# 第十阶段：学习路线安排

## 24. 你可以按这个节奏来

## 第 1 周：只学 Rust 和 Cargo 基础

目标：

- 会 `cargo new`
- 会 `cargo run`
- 会 `cargo test`
- 会写函数
- 会写 struct
- 会读简单编译错误

完成标准：

- 你能自己写一个带参数的小 CLI
- 你知道 `Cargo.toml` 是干什么的

---

## 第 2 周：搭 workspace 和最小 `doctor`

目标：

- 会建 workspace
- 会拆 `core` 和 `cli`
- 会让 `cli` 调 `core`
- 会输出当前路径和默认配置

完成标准：

- 你能运行 `cargo run -p ztor-cli -- doctor`

---

## 第 3 周：做 `template`

目标：

- 学会文件读写
- 学会 `PathBuf`
- 学会为文件操作写测试

完成标准：

- 你能实现模板复制
- 你能用测试验证 dry-run 和报错场景

---

## 第 4 周：做 `bundle`

目标：

- 学会递归处理
- 学会维护状态
- 学会处理重复与循环

完成标准：

- 能展开本地 include
- 能检测循环
- 能输出压缩后的结果

---

## 第 5 周：做 `catalog`

目标：

- 学会扫目录
- 学会路径解析
- 学会排序和格式化输出

完成标准：

- 能从 `.wait` 目录生成条目列表
- 能输出 markdown/plain/json

---

## 第 6 周：配置化 + 文档 + 收尾

目标：

- 接入 `serde + toml`
- 把硬编码路径全移到 `ztor.toml`
- 整理 README

完成标准：

- 没有配置文件时能用默认值
- 有配置文件时能按配置工作
- 你自己能看懂并解释整个项目结构

---

# 第十一阶段：你每一阶段都该如何做

## 25. 每做一个功能，都按这个流程走

### 25.1 先写一句目标
例如：

> 我要实现 `template apply --target xxx.cpp`

### 25.2 列出输入输出
例如：

- 输入：模板文件路径、目标文件路径、是否 dry-run
- 输出：目标文件被覆盖，或返回错误

### 25.3 先写测试
先不要写实现。

### 25.4 运行测试，看它失败
这一步很重要。

### 25.5 写最小实现
只让当前测试通过。

### 25.6 再补下一个测试
重复。

---

# 第十二阶段：你现在就可以照抄的实战顺序

## 26. 最建议的开工顺序

今天开始可以直接这样做：

1. 新建 `my-ztor`
2. 建 workspace
3. 建 `ztor-core` 和 `ztor-cli`
4. 让 `cli` 能调用 `core`
5. 做 `doctor`
6. 做 `template`
7. 做 `bundle`
8. 做 `catalog`
9. 做 `ztor.toml`
10. 整理 README

---

# 第十三阶段：给你的几个重要提醒

## 27. 不要一开始追求“完全照抄”

你真正要复刻的是：

- 结构思路
- 模块边界
- 工作流
- 配置化方式
- CLI 体验

不是逐行逐字照搬实现。

---

## 28. 不要一开始就学高级 Rust

你这类项目前期只需要会这些就够了：

- `struct`
- `enum`
- `Vec`
- `Option`
- `Result`
- `PathBuf`
- `fs`
- `match`
- 模块拆分

先不要纠结：

- 生命周期高级写法
- 复杂泛型
- trait 设计
- unsafe
- 宏系统深水区

---

## 29. 出现编译错误时的正确心态

Rust 编译器报错多是正常的。  
你要练的是：

1. 先看第一条错误
2. 不要一口气看 20 条
3. 修第一条，再编译
4. 重复

很多后续错误只是第一条引起的连锁反应。

---

## 30. 新手最容易犯的错

- 一次写太多代码，不测试
- 还没理解模块边界就开始乱拆文件
- 还没会 CLI 就想一口气做配置系统
- `bundle` 一上来就想做完整预处理器
- 被编译错误吓到后疯狂乱改

正确策略是：  
**每次只推进一小步，并且让这一步可运行、可测试。**

---

# 第十四阶段：你可以直接执行的每日计划

## Day 1
- 安装 Rust
- 学 `cargo new / run / test`
- 写 3 个小 Rust 程序

## Day 2
- 学 `clap`
- 写一个接受命令行参数的小程序

## Day 3
- 建 `workspace`
- 建 `ztor-core` + `ztor-cli`
- 打通调用链

## Day 4
- 实现 `doctor`
- 学会 `PathBuf` 和 `current_dir`

## Day 5
- 为 `template` 写测试
- 实现模板复制

## Day 6
- 完成 `template` 的 dry-run 和错误处理

## Day 7-8
- 做 `bundle` 的单层 include

## Day 9-10
- 做 `bundle` 的递归、去重、循环检测

## Day 11-12
- 做 `catalog` 的扫描和普通输出

## Day 13-14
- 做 `catalog` 的 OJ 规则
- 加 JSON 输出

## Day 15-16
- 接入 `serde + toml`
- 实现 `ztor.toml`

## Day 17
- 做 `config init`

## Day 18
- 做 `completion`

## Day 19
- 补 README

## Day 20
- 全量测试、整理结构、查漏补缺

---

# 第十五阶段：你的最低完成标准

## 31. 如果你只做到这些，也已经算复刻成功

- 有 workspace
- 有 `ztor-core` 和 `ztor-cli`
- 有 `doctor`
- 有 `template apply`
- 有一个简化版 `bundle`
- 有一个简化版 `catalog`
- 有 `ztor.toml`
- 有基础测试

做到这里，你就已经不是“不会 Rust”了，而是在用 Rust 真正做项目了。

---

如果你愿意，我下一条可以继续直接帮你写：

1. `Rust 新手版学习清单`
2. `第一天到第三天你应该亲手敲的代码模板`
3. `照着做的最小 ztor 项目骨架`

我建议你下一步选第 3 个，这样我可以直接把“你应该先写哪些文件、每个文件先放什么代码”完整列出来。