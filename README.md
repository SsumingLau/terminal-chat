# Terminal Chat — 终端异地聊天

基于 [antirez/smallchat](https://github.com/antirez/smallchat) 的极简终端聊天工具，通过自建服务器实现异地终端聊天。

## 功能

- **轻量级**：服务端 + 客户端总共不到 500 行 C 代码，无任何第三方依赖
- **终端即客户端**：在终端里直接聊天，也可以 `nc`/`telnet` 连接
- **多人在线**：一个服务器，多人同时连接，消息广播给所有在线用户
- **昵称设置**：`/nick <名字>` 设置自己的显示昵称
- **单进程复用**：基于 `select()` 实现 I/O 多路复用，不需要多线程

## 架构

```
┌──────────┐   TCP    ┌──────────────┐   TCP    ┌──────────┐
│ 客户端 A  │ ◄──────► │              │ ◄──────► │ 客户端 B  │
│ (终端)    │  :7711  │   Server     │  :7711  │ (终端)    │
└──────────┘          │  (自建服务器)  │          └──────────┘
                      │              │
┌──────────┐          │  端口: 7711   │          ┌──────────┐
│ 客户端 C  │ ◄──────► │              │ ◄──────► │ 客户端 D  │
│ (nc)     │          └──────────────┘          │ (telnet)  │
└──────────┘
```

## 编译

```bash
make
```

生成两个文件：
- `smallchat-server` — 服务端，部署到服务器
- `smallchat-client` — 客户端，在本地终端使用

## 使用

### 1. 在服务器上启动服务端

```bash
./smallchat-server
```

默认监听 `0.0.0.0:7711`。保持终端开着，服务就在运行。

放到后台运行：
```bash
nohup ./smallchat-server > /dev/null 2>&1 &
```

### 2. 客户端连接

**方式一：用自带客户端（推荐）**
```bash
./smallchat-client <服务器IP> 7711
```
- 支持行编辑（退格删除）
- 输入消息后回车发送
- `Ctrl+C` 退出

**方式二：用 nc/netcat（无需客户端）**
```bash
nc <服务器IP> 7711
```

**方式三：用 telnet**
```bash
telnet <服务器IP> 7711
```

### 3. 聊天命令

| 命令 | 说明 |
|------|------|
| `/nick <名字>` | 设置昵称，如 `/nick 小明` |
| 直接输入文字 | 发送消息给所有人 |

## 服务器部署

### 要求

- Linux/macOS 服务器
- 有公网 IP（或同一内网）
- 开放 7711 端口（TCP）

### 快速部署

```bash
# 1. 在服务器上编译
git clone <本仓库>
cd terminal-chat
make

# 2. 运行
./smallchat-server

# 3. 开放防火墙端口（如果有）
# Linux (iptables):
iptables -A INPUT -p tcp --dport 7711 -j ACCEPT
# 云服务器记得在安全组里放行 7711 端口
```

### 修改端口

默认端口为 7711。要修改的话，编辑 `smallchat-server.c` 第 46 行：

```c
#define SERVER_PORT 7711  // 改成你想要的端口
```

然后重新 `make`。

## 限制与说明

这是极简实现，有意不做以下功能（保持代码在 500 行以内）：

- **无消息缓冲**：依赖内核 socket 缓冲区，如果一行消息超过 256 字节会被截断
- **无频道/私聊**：所有消息广播给所有人
- **无加密**：明文 TCP 传输。如需加密，可通过 SSH 隧道或 WireGuard 组网
- **无认证**：知道 IP 和端口就能连接
- **无消息持久化**：不存历史记录

### 安全建议

1. **用 SSH 隧道加密**（推荐）：
   ```bash
   # 客户端先建立隧道
   ssh -L 7711:127.0.0.1:7711 user@你的服务器 -N
   # 然后连接本地端口
   ./smallchat-client 127.0.0.1 7711
   ```

2. **限制访问 IP**：用 iptables 只允许特定 IP 连接

3. **不要暴露在公网**：优先在内网或 VPN 中使用

## 致谢

原始项目：[antirez/smallchat](https://github.com/antirez/smallchat) — 由 Redis 作者 Salvatore Sanfilippo (antirez) 编写，作为系统编程教学的示例代码。

## License

BSD 3-Clause（沿用原项目协议）
