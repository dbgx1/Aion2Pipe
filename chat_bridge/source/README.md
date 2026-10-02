# Client

Python 客户端负责运行 mitmproxy addon：

- 监听 AION2 WebSocket/STOMP `MESSAGE`。
- 解析聊天 JSON。
- 自动缓存 Bearer、本地角色、服务器、用户名、目标玩家等运行时信息。
- 接收控制台命令并调用 `sendWhisper`。

## 通信方式

客户端仅使用 MQTT 接收指令、上报聊天和执行回执，不再建立 WebRTC 连接，也不再需要 aiortc 或 TURN/STUN 配置。现有房间、客户端 ID 和 AION2_SIGNAL_PREFIX 环境变量保持兼容；旧的 signal/agent/all 主题仅用于 MQTT 客户端发现。

## 安装

可靠性更新见 [RELIABLE-MQTT-UPDATE.md](RELIABLE-MQTT-UPDATE.md)。通信模块版本为 `2026-09-10.reliable-mqtt`，离线事件和指令执行记录使用 SQLite 持久化。

在项目根目录执行：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install --trusted-host pypi.org --trusted-host files.pythonhosted.org -r .\client\requirements.txt
```

## 启动

```powershell
$env:AION2_RELAY_ROOM="aion2-local"
.\.venv\Scripts\python.exe .\client\run_client.py
```

默认启动 mitmweb 抓包 UI，并自动识别网络拓扑。TUN/虚拟网卡或直连模式接管游戏的 PlayNC HTTPS/WSS；本地 SOCKS/转发型加速器接管 Aion2Pipe 的上游。非 HTTP 游戏协议使用 raw TCP 原样转发：

```text
--mode local:AION2.exe       # TUN / 虚拟网卡 / 直连
--mode regular@127.0.0.1:18080 # 本地 SOCKS / 转发型加速器；由 Aion2Pipe 串联
--allow-hosts ^(?:limep2-pub\.global|lime-p2-api\.global|lime-arizona-p4-(?:api|pub))\.plaync\.com:\d+$
--set rawtcp=true
```

抓包 UI 页面默认会自动打开：

```text
http://127.0.0.1:8081/
```

这种模式不需要给游戏配置代理地址。启动顺序为：开启加速器虚拟网卡/TUN → Aion2Pipe → 游戏。Aion2Pipe 负责登录/世界协议，并把 `AION2.exe` 的 TCP 443 选择性送到本组件；本组件仅解析 PlayNC HTTPS/WSS，所有外连继续沿 Windows 当前路由发送。程序不检测任何加速器进程或动态端口。

首次启动时，客户端会在当前电脑的 `%USERPROFILE%\.mitmproxy` 目录生成独立 CA，检查其是否已受 Windows 信任，并在未安装时弹出一次确认窗口。确认后证书会安装到当前用户的“受信任的根证书颁发机构”；以后启动不会重复安装。

可选环境变量：

```powershell
$env:AION2_NETWORK_MODE="auto"          # auto（默认）、tun、direct 或 relay
$env:AION2_MITM_PROCESS="Aion2Pipe.exe" # 可选：强制指定接管进程
$env:AION2_RELAY_PROXY_PORT="18080"       # 本地转发共存端口；须与 Aion2Pipe 一致
$env:AION2_MITM_MODE="regular@127.0.0.1:18080" # mitmproxy 模式（显式覆盖）
$env:AION2_ALLOW_HOSTS="^(?:limep2-pub\.global|lime-p2-api\.global|lime-arizona-p4-(?:api|pub))\.plaync\.com:\d+$" # 仅接管聊天 API/公屏 WSS；登录域名保持原样直通
$env:AION2_MITM_TOOL="mitmweb"          # mitmweb 或 mitmdump
$env:AION2_WEB_PORT="8081"              # mitmweb 页面端口
$env:AION2_WEB_OPEN_BROWSER="1"         # 1 自动打开抓包 UI，0 不自动打开
$env:AION2_AUTO_INSTALL_CERT="1"        # 1 无人值守安装，0 不检查/安装；不设置则首次弹窗确认
$env:AION2_CERT_STORE="user"            # user 当前用户（默认），machine 本机（需要管理员）
$env:AION2_MITM_CONFDIR="$env:USERPROFILE\.mitmproxy" # CA 与 mitmproxy 配置目录
```

每台电脑必须生成自己的 CA。分发客户端时不要复制 `.mitmproxy` 目录，也不要分发包含私钥的 `mitmproxy-ca.pem`。

如果你要只开命令行、不显示抓包页面：

```powershell
$env:AION2_MITM_TOOL="mitmdump"
.\.venv\Scripts\python.exe .\client\run_client.py
```

## 编译 exe

在项目根目录执行：

```powershell
.\.venv\Scripts\python.exe -m pip install pyinstaller
.\scripts\build-chat-bridge.ps1
```

产物位置：

```text
build\aion2-client\aion2-client.exe
```

这是 PyInstaller `onedir` 产物，运行时需要保留同目录下的 `_internal` 文件夹。

复制到其它电脑时，需要复制整个目录：

```text
build\aion2-client\
```

不要只复制 `aion2-client.exe`。如果启动后闪退，查看 exe 同目录下的 `aion2-client.log`；local 模式通常需要右键“以管理员身份运行”。
