# 高精度Linux性能分析工具

本项目是一个基于Linux `perf_event` 子系统的高性能采样分析器。它能够实时地将运行时指令地址（IP）转换为具体的函数符号，从而帮助开发者精确地定位用户态和内核态的性能热点。

## 核心能力

- **全栈符号化**: 同时支持用户态（ELF符号）和内核态（kallsyms符号）的地址解析。
- **双模式采样**: 支持基于软件定时器（默认，通用）和基于LBR硬件（可选，高精度）的两种采样模式。
- **实时符号化**: 采样地址到函数名的转换延迟极低，小于1毫秒。
- **系统级监控**: 能够监控系统上的所有进程，提供全局性能视图。
- **三级核心缓存**: 进程、ELF文件和内核符号三级缓存，最大化减少重复IO和解析开销。
- **高可靠栈回溯**: 可选LBR或Frame Pointer模式，用于生成高度精确的调用栈和火焰图。
- **内存安全**: 通过引用计数和定期清理机制，确保零内存泄漏。

## 依赖安装

在编译前，请确保已安装以下必要的开发库。

以Debian/Ubuntu为例：
```bash
sudo apt-get update
sudo apt-get install build-essential libelf-dev libsqlite3-dev
```

- `build-essential`: 提供 `gcc` 和 `make` 等基础编译工具。
- `libelf-dev`: ELF文件解析所需的核心库。
- `libsqlite3-dev`: 用于（可选的）数据存储功能。

## 快速开始

### 1. 克隆项目
```bash
git clone <https://github.com/Vivy33/Profiling.git>
cd <Profiling>
```

### 2. 编译
直接运行 `make` 命令即可编译生成可执行文件 `profiling_tool`。
```bash
make
```

### 3. 运行

**重要提示**: 运行本工具需要 `root` 权限，因为它依赖于 `perf_event_open` 系统调用。
```bash
更多参数请查看 `./start_profiling.sh --help`
查询参数查看   `./smart_query.sh --help`
```

**场景1: 标准性能分析 (默认软件采样)**
此模式适用于快速定位消耗CPU时间最长的“热点”函数。
```bash
# 以100Hz的频率监控所有进程（默认30hz）
sudo ./profiling_tool --frequency=100
```

**场景2: 高精度调用栈分析 (LBR硬件采样)**
此模式使用CPU的LBR硬件功能，以极高的精度追踪函数调用路径，是生成可靠火焰图的首选。
```bash
# 启用LBR模式进行高精度分析
sudo ./profiling_tool --frequency=100 --lbr
```

**场景3: 只分析内核空间**
```bash
# 仅对内核函数进行采样
sudo ./profiling_tool --frequency=100 --filter=kernel
```
