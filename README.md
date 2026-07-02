# 向量时钟分布式日志聚合系统 - 操作文档

## 项目简介

本系统是一个基于 **向量时钟（Vector Clock）** 的分布式日志聚合工具，用于模拟多节点环境下日志的采集、因果排序和统一查询。

系统由一个 **Server（聚合服务器）** 和多个 **Agent（日志采集代理）** 组成，通过 Windows Named Pipe 进行通信。

---

## 环境要求

- **操作系统**：Windows 10/11
- **编译器**：GCC（MinGW）或 MSVC
- **运行环境**：已编译好的 `server.exe` 和 `agent.exe`

---

## 快速开始

### 第一步：准备日志文件

在程序目录下创建三个日志文件，每个文件写入若干行日志内容：

**logs_A.txt**
```
服务器A启动
处理用户请求
发送心跳到B和C
收到B和C的回复
```

**logs_B.txt**
```
服务器B启动
数据库连接超时
收到A的心跳
报告状态正常
开始数据同步
```

**logs_C.txt**
```
处理用户请求
收到A和B的状态
汇总所有节点状态
```

### 第二步：启动 Server

打开一个终端窗口，运行：

```powershell
.\server.exe
```

看到如下输出表示启动成功：
```
+------------------------------------------+
| 向量时钟分布式日志聚合服务器               |
| Pipe: \\.\pipe\vc_log_agg                |
+------------------------------------------+
| search <关键词> | list | count | exit     |
+------------------------------------------+
等待 Agent 连接...

[server] >
```

### 第三步：启动三个 Agent

打开 **三个** 新的终端窗口，分别运行：

```powershell
# 终端 2 - Agent A
.\agent.exe A logs_A.txt

# 终端 3 - Agent B
.\agent.exe B logs_B.txt

# 终端 4 - Agent C
.\agent.exe C logs_C.txt
```

每个 Agent 连接成功后会显示：
```
========================================
  日志采集代理 Agent [A]
  管道: \\.\pipe\vc_log_agg
========================================
  打字→发送 | 空行→读文件 | show | exit
```

### 第四步：交互使用

#### Agent 端操作

| 操作 | 说明 |
|------|------|
| 输入文字 + 回车 | 发送一条日志（附带向量时钟） |
| 直接回车（空行） | 自动读取日志文件中的新行并批量发送 |
| 输入 `show` | 查看当前节点的向量时钟状态 |
| 输入 `exit` | 退出 Agent |

#### Server 端操作

| 命令 | 说明 | 示例 |
|------|------|------|
| `search <关键词>` | 搜索包含关键词的日志 | `search 心跳` |
| `list` | 列出所有已交付日志 | `list` |
| `list <节点名>` | 列出指定节点的日志 | `list A` |
| `count` | 统计已交付和缓存中的日志数量 | `count` |
| `exit` | 退出 Server | `exit` |

---

## 核心概念

### 向量时钟

向量时钟是一种逻辑时钟，用于追踪分布式系统中事件的因果关系。

- 每个节点维护一个向量（如 `{'A':3, 'B':2, 'C':1}`）
- **本地事件**：自身分量 +1
- **接收消息**：逐分量取最大值合并，再自增自身分量

### 因果排序

当消息乱序到达时，Server 不会立即交付，而是放入 **因果缓冲区**，等待所有依赖消息到达后才按正确的因果顺序输出。

例如：如果 B 的一条消息声称看到了 A 的第 3 条日志，那么 Server 必须先收到 A 的前 3 条日志，才会交付这条 B 的消息。

---

## 程序架构

```
┌──────────┐  Named Pipe  ┌──────────────┐  Named Pipe  ┌──────────┐
│ Agent A  │ ──────────►  │              │ ◄──────────  │ Agent B  │
│          │              │   Server     │              │          │
└──────────┘  Named Pipe  │  (聚合中心)   │  Named Pipe  └──────────┘
                           │              │ ◄──────────  ┌──────────┐
┌──────────┐  Named Pipe  │              │  Named Pipe  │ Agent C  │
│          │ ──────────►  │              │ ──────────►  │          │
└──────────┘              └──────────────┘              └──────────┘
                            │
                  ┌─────────┴─────────┐
                  │  因果缓冲区        │
                  │  + 倒排索引        │
                  │  + 命令行查询      │
                  └───────────────────┘
```

---

## 编译说明

如果有源码需要重新编译：

```powershell
# 使用 make（需要 MinGW）
make

# 或手动编译
gcc -Wall -Wextra -std=c17 -O2 -o agent.exe agent.c vector_clock.c indexer.c -lws2_32
gcc -Wall -Wextra -std=c17 -O2 -o server.exe server.c vector_clock.c indexer.c -lws2_32
```

---

## 测试程序

### 单进程演示

```powershell
gcc -Wall -Wextra -std=c17 -O2 -o demo_all.exe demo_all.c vector_clock.c indexer.c -lws2_32
.\demo_all.exe
```

模拟 12 条乱序消息到达，展示因果排序效果。

### 端到端测试

```powershell
gcc -Wall -Wextra -std=c17 -O2 -o test_e2e.exe test_e2e.c vector_clock.c indexer.c -lws2_32
.\test_e2e.exe
```

运行四项自动化测试：向量时钟、分词、搜索、因果排序。

---

## 常见问题

### Q: Agent 连接失败，提示 "server 未启动"？
A: 请先运行 `server.exe`，等待它显示 "等待 Agent 连接..." 后再启动 Agent。

### Q: Server 收不到消息？
A: 检查 Agent 是否正常连接（应显示 "打字→发送..." 提示），并确保日志文件名正确。

### Q: 中文显示乱码？
A: 确保终端使用 UTF-8 编码，可在 PowerShell 中运行：
```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
chcp 65001
```

### Q: 如何停止程序？
A: Agent 端输入 `exit`，Server 端输入 `exit`。也可以直接关闭终端窗口。

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `server.exe` | 聚合服务器（已编译） |
| `agent.exe` | 日志采集代理（已编译） |
| `vector_clock.h/c` | 向量时钟实现 |
| `indexer.h/c` | 倒排索引 + 因果缓冲区 |
| `demo_all.c` | 单进程演示程序 |
| `test_e2e.c` | 端到端集成测试 |
| `Makefile` | 编译脚本 |
| `logs_A/B/C.txt` | 日志文件（需自行创建） |

---

## 扩展建议

1. **持久化存储**：将索引和缓冲区保存到文件，重启后恢复
2. **TCP 通信**：将 Named Pipe 改为 TCP Socket，支持跨机器部署
3. **图形界面**：用 Web 前端展示实时日志流和因果图
4. **更多节点**：扩大 `VC_MAX_NODES` 支持更多节点
5. **加密通信**：Named Pipe 添加加密层保障数据安全
