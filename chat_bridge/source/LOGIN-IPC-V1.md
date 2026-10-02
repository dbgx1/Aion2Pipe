# AION2 本机登录进程通信协议 v1

状态：供双方实现和联调。客户端通信代码已实现；实际登录由独立登录进程实现。
范围：同一台 Windows 电脑，跨语言、跨进程。协议不涉及游戏登录的内部实现。

## 1. 连接约定

- 登录进程是服务端，AION2 客户端主动连接。
- 正式管道名：`\\.\pipe\aion2-login-v1`。
- Windows 双向、字节模式命名管道。使用异步/重叠 I/O；双方独立启动、独立重启。
- 不使用 TCP 端口，不经过 MQTT 或公网转发。
- 服务端必须显式设置 ACL，只授权运行这两个程序的 Windows 用户/服务身份，拒绝远程管道客户端。首次创建应防止同名管道被抢占，不能依赖默认 ACL。
- v1 假定同一 Windows 身份为一个信任域；若要隔离同一用户下的不可信程序，需要另行增加服务身份验证。不同管理员权限级别须由服务端配置相应 ACL。
- 示例 mock 只用于测试：默认管道名带 `-mock`，使用默认权限和内存任务表，不可作为正式登录服务。

## 2. 字节格式

每帧：`4 字节无符号小端整数 N + N 字节 UTF-8 JSON`。

N 为 JSON 的字节数，不是字符数；1 <= N <= 65536。没有 BOM、结尾换行或 NUL。
多帧可以连发，单帧可以分多次读取。双方必须缓存未读完的帧。
JSON 顶层必须是对象，不允许重复键、NaN、Infinity。协议错误或非法长度应关闭连接。
不得使用 pickle、语言运行时对象序列化或 Python multiprocessing 的私有通信格式。

## 3. 公共字段

- `version`：整数，固定 1。
- `type`：request / response / event。
- `requestId`：每次请求唯一的 UUID 字符串；响应原样返回，用于匹配并发请求。
- `taskId`：一次登录任务的 UUID 字符串。发起方在发送前持久保存；重试和重连查询保持不变。
- `seq`：每个任务独立、从 1 开始递增的整数；状态变化时递增，查询不会递增，服务端重启后不能归零。
- 所有账号、服务器、角色标识均为字符串，非空，最多 128 字符。不得用数字转换导致前导零丢失。
- 请求/响应可以交错，不能依赖到达顺序。事件也可能先于创建任务的响应到达。
- 接收端忽略未知的附加字段；未知方法返回 METHOD_NOT_FOUND。不同主版本通过 hello 拒绝，不能猜测兼容。

请求：
```json
{"version":1,"type":"request","requestId":"req-001","method":"login.start","params":{"taskId":"task-001","accountRef":"account-03","serverId":"1001","characterId":"char-08","timeoutSeconds":180}}
```
成功响应：
```json
{"version":1,"type":"response","requestId":"req-001","ok":true,"result":{"taskId":"task-001","seq":1,"state":"queued","stage":"launching"}}
```
失败响应：
```json
{"version":1,"type":"response","requestId":"req-001","ok":false,"error":{"code":"BUSY","message":"登录队列已满"}}
```
以上简短 ID 用于展示，实际新建请求和任务应生成 UUID。

## 4. 方法

### hello
每条连接建立后先握手，成功后才能发送业务请求。

params：`{"client":"aion2-client","versions":[1]}`

result：
```json
{"version":1,"instanceId":"服务端每次启动生成的新UUID","maxConcurrent":1,"methods":["login.start","login.status","login.cancel","ping"]}
```
无兼容版本返回 VERSION_UNSUPPORTED 并断开。v1 要求以上四个方法全部实现。
`instanceId` 标识服务端运行实例，不能作为 taskId 的一部分；服务重启不能消除任务去重信息。

### login.start
params 必须包含：taskId、accountRef、serverId、characterId、timeoutSeconds。
`timeoutSeconds` 为 10..900 的整数，默认调用值为 180。
账号密码由登录程序自行取得，通信层只传账号引用 accountRef；双方部署时配置相同的账号引用映射。

服务端在执行登录前持久保存任务和参数，然后立即返回任务快照。不要等登录结束再回复。
同一 taskId + 相同参数：返回原任务当前快照，不得再次登录。
同一 taskId + 不同参数：TASK_CONFLICT。
执行中同一账号的不同 taskId：ACCOUNT_BUSY，防止重复登录。
并发上限由服务端 hello 声明。超过运行并发可以排队；队列容量也满时返回 BUSY。

### login.status
params：`{"taskId":"task-001"}`。返回完整最新任务快照。
未知任务：TASK_NOT_FOUND。客户端不得因为查不到任务就自动换 taskId 重新登录。
查询也恢复对该任务事件的订阅；服务端只向拥有该任务访问权限的连接发事件。

### login.cancel
params：`{"taskId":"task-001"}`。请求停止尚未完成的任务，返回当前快照。
如果正在取消，快照可以保持 running 并附加 `cancelRequested:true`。
确认已停止才能进入 cancelled；已经结束的任务返回原终态。取消应幂等。
本操作只取消登录任务，不隐含杀死游戏、登出账号、删除文件或取消其他任务。

### ping
params：`{}`，result：`{"alive":true}`。
可以在空闲连接上每 15 秒调用一次。等待任务期间 status 查询已承担连接探测。

## 5. 任务快照与事件

所有 start/status/cancel 的成功 result 都使用同一任务快照：

- 必填：taskId、seq、state。
- 可选：stage、message、cancelRequested。
- succeeded 必须有 result：accountRef、serverId、characterId、gamePid（正整数）。
- failed 必须有 error：code、message。
- waiting_user 建议附加 message，描述需要人工完成的动作；客户端不自动处理验证码。

state：queued / running / waiting_user / succeeded / failed / cancelled。
stage 建议值：launching / authenticating / selecting_server / selecting_character / entering_world。
阶段可增加，不能靠阶段推断任务成功。三个终态 succeeded、failed、cancelled 不可回退或再变更。

主动事件：
```json
{"version":1,"type":"event","method":"login.event","taskId":"task-001","seq":2,"data":{"state":"running","stage":"selecting_character","message":"正在选择角色"}}
```
成功事件：
```json
{"version":1,"type":"event","method":"login.event","taskId":"task-001","seq":3,"data":{"state":"succeeded","stage":"entering_world","result":{"accountRef":"account-03","serverId":"1001","characterId":"char-08","gamePid":12345}}}
```
失败事件：
```json
{"version":1,"type":"event","method":"login.event","taskId":"task-001","seq":3,"data":{"state":"failed","error":{"code":"AUTH_FAILED","message":"账号认证失败"}}}
```
成功必须意味着：服务端已经确认指定账号、服务器、角色进入游戏。仅启动进程或打开登录界面不算成功。
游戏后续掉线不修改已完成的登录任务；在线监控不属于本协议 v1。

## 6. 超时、恢复和去重

- 普通请求建议 5 秒内响应。超过时间只意味着结果未知，不意味着请求未执行。
- 登录任务超时从服务端接受任务时开始，包含排队时间；若未完成，服务端停止此登录任务并保存 failed / LOGIN_TIMEOUT。
- 客户端断线不取消任务。重连先 hello，再用原 taskId 查询。
- 未确认结果时禁止自动生成新 taskId 或盲目重发 start。
- 服务端必须在启动游戏前持久提交任务；重启后先核对已有进程和登录状态，不能把历史任务直接重新执行。
- 无法可靠恢复时，将任务标为 failed / RECOVERY_REQUIRED，交给人工核对，不能虚报未执行。
- 建议完整任务记录至少保留 7 天；之后可压缩成去重墓碑。墓碑保留到明确的任务数据重置，遇旧 ID 返回 TASK_EXPIRED，不能重新执行。
- 事件是通知，不是唯一事实来源：允许丢失、重复，status 是权威完整快照。
- 客户端只接受比已知 seq 更大的状态；相同 seq 应代表相同快照。较旧 seq 忽略。
- 连接/等待超时、关闭通信模块都不隐含取消登录。

## 7. 错误码

VERSION_UNSUPPORTED、METHOD_NOT_FOUND、INVALID_PARAMS、TASK_CONFLICT、TASK_NOT_FOUND、TASK_EXPIRED、ACCOUNT_NOT_FOUND、ACCOUNT_BUSY、BUSY、AUTH_FAILED、GAME_START_FAILED、SERVER_UNAVAILABLE、CHARACTER_NOT_FOUND、LOGIN_TIMEOUT、RECOVERY_REQUIRED、INTERNAL_ERROR。

error.message 用于展示，最多 2000 字符。客户端判断应使用 error.code。
不得通过错误、日志或事件传回密码、验证码、Bearer token 或其他认证凭据。

## 8. Python 客户端使用

Python 3.11+，Windows 默认 Proactor 事件循环，无新增第三方依赖。

```python
import asyncio
from login_bridge import LoginBridge

async def main():
    # 此 ID 应由调用方预先生成并保存，进程重启后仍用它查询。
    task_id = "已持久保存的UUID"
    async with LoginBridge() as bridge:
        accepted = await bridge.start(
            task_id=task_id, account_ref="account-03",
            server_id="1001", character_id="char-08")
        print(accepted)
        result = await bridge.wait(task_id)
        print(result)

asyncio.run(main())
```

API：connect/start/status/cancel/ping/wait/close。`wait` 遇断线自动重连查询，不重发登录；遇 waiting_user 即返回交给人工。失败终态以快照返回，请检查 state。
事件队列 `bridge.events` 最大 256 项，快照缓存最多 512 个任务。慢消费者会丢弃最旧通知并增加 dropped_events；需要确认结果时调用 status，不依赖缓存。缓存不能代替调用方持久保存 taskId。
一个 LoginBridge 实例只在一个事件循环内使用。已有非 Proactor 循环的程序可在独立线程运行 asyncio.run，通过线程安全投递调用。等待通信不会阻塞该循环内其他聊天任务。

命令行联调（在 client 目录）：
```powershell
python login_ipc_cli.py ping
python login_ipc_cli.py start --account account-03 --server 1001 --character char-08
python login_ipc_cli.py status <taskId>
python login_ipc_cli.py wait <taskId>
python login_ipc_cli.py cancel <taskId>
```

模拟服务端（完全不操作游戏）：
```powershell
python mock_login_server.py
python login_ipc_cli.py --pipe '\\.\pipe\aion2-login-v1-mock' ping
python login_ipc_cli.py --pipe '\\.\pipe\aion2-login-v1-mock' start --account test --server 1001 --character test
python test_login_ipc.py
```

实际登录进程只需按第 1–7 节实现。无需 Python，也不必引用这里的源码。
现已增加网页/MQTT 换号入口，扩展协议见第 9 节；测试没有对实际游戏执行换号。

## 9. UI 控制台 → 客户端 → 登录进程：换号扩展

完整链路：控制台 MQTT 指令 → 客户端持久保存任务 → 管道 login.switch → 登录进程执行 → login.event/status → 客户端 MQTT control_progress → 控制台实时状态面板。

### 登录进程新增 login.switch

参数、返回快照、去重与超时规则与 login.start 相同，但语义为：退出当前受管游戏会话并切换到指定账号/区服/角色。具体如何退出和登录由登录进程负责。
hello.methods 必须包含 login.switch；缺少能力时客户端明确报错，不能将 switch 偷换为 start。
第一版每个客户端/管道控制一个游戏会话。若一台机器控制多个独立会话，为每个客户端配置不同 AION2_LOGIN_PIPE，由对应的登录服务实例管理；不要通过猜测 PID 关闭其他游戏。
建议阶段增加 logging_out（退出当前账号）。不允许退出无关游戏或改变其他账号会话。

### MQTT 下行指令

主题：`{prefix}/{room}/control/agent/{agentId}`，QoS 1，retain=false。
只接受明确 target=当前 agentId 的定向命令，拒绝广播和 retained 换号指令。

```json
{"type":"switchAccount","requestId":"请求UUID","taskId":"贯穿三端的任务UUID","target":"客户端ID","accountRef":"account-03","serverKey":"1001","characterId":"char-08","timeoutSeconds":180,"expiresAt":1789527630000}
```

expiresAt 为当前时间后 30 秒的 Unix 毫秒值，上面的数字仅为格式示例，不能直接使用。
客户端将 serverKey 映射为管道 params.serverId，其他目标字段语义不变。
requestId 用于一次 MQTT 请求，taskId 用于整次换号；管道每次请求可有自己的 requestId，taskId 始终不变。

其他命令：
- switchAccountStatus：taskId 指定任务；taskId 为空时查询客户端最近任务，用于新页面发现正在执行的任务。
- cancelSwitchAccount：taskId 指定任务；确认取消前 UI 不能显示已取消。
- resumeAccountChat：核对游戏当前角色后显式恢复聊天，携带当前 serverKey、characterId。客户端校验实际识别到的身份，且所有换号任务必须已经结束。

### MQTT 实时回传

主题：`{prefix}/{room}/events/{agentId}/receipts`，QoS 1，retain=false。

```json
{"type":"control_progress","command":"switchAccount","agentId":"客户端ID","requestId":"请求或任务UUID","taskId":"任务UUID","time":"2026-09-16T03:00:00Z","switchTask":{"taskId":"任务UUID","revision":3,"loginSeq":2,"state":"running","stage":"authenticating","accountRef":"account-03","serverId":"1001","characterId":"char-08","updatedAt":1789527600000,"chatPaused":true,"message":"正在登录目标账号"}}
```

- revision 为客户端任务快照版本，持久保存；loginSeq 为登录进程任务版本。不要混用。
- UI 根据 agentId + taskId + revision 匹配任务，旧版本不能覆盖新版本。
- 客户端额外状态 received（收到指令）和 unknown（结果未确认）不能伪装成成功或失败。
- 参数无效、任务冲突、过期等拒绝通过 control_result / ok=false / status=not_sent / errorCode 回传。
- 客户端启动状态带 accountSwitchProtocol=1。老版本客户端未声明能力时，UI 禁用换号按钮。

### UI 行为

管理员在“控制台 → 远程换号与实时状态”选择客户端并填写账号引用、区服和角色，然后发送。
UI 实时显示受理、阶段、等待人工、成功、失败或取消；保留任务编号。
客户端接收登录事件，并以约 1 秒的状态查询补偿丢失的事件。UI 每 5 秒查询一次补偿 MQTT 消息丢失；活动任务超过 15 秒没有新回报时显示状态未确认。
页面刷新或 MQTT 重连只查询原任务，绝不自动再次发送换号。
客户端重启后使用 SQLite 中的原任务继续查询，不能把未确认的任务自动当成新登录执行。
换号期间客户端拒绝新聊天发送；完成后需核对当前身份再显式恢复聊天，以免旧托管任务借用新账号继续发消息。

## 10. 部署与联调

网页入口已经接入，客户端必须运行本次新版 addon 和 account_switch/login_bridge/login_protocol 模块。
源代码运行入口 run_client.py 已加入模块打包引用。对于既有 onedir EXE，可关闭客户端后将更新包的 `_internal` 内容合并到客户端 `_internal`，再启动；不需要修改账号密码配置。
更新包不要直接作为完整客户端运行；它只包含增量 Python 模块。
登录程序仍须实现真实 login.switch 后才能执行实际换号。mock 仅用于测试，不启动或退出游戏。

验证：test_account_switch.py 覆盖实际 MQTT 接收分发、SQLite 任务记录、独立模拟登录进程、实时阶段、重复任务、断线/重启查询、取消、过期、定向/retained 限制和聊天暂停；test_login_ipc.py 覆盖基础命名管道协议。
