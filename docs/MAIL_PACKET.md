# 邮件构包与调试

2026-10-03 对当前打开的 `2026_9_30/AION2_dump_64_ida_fast.exe.i64` 静态核对。尚未用本次实现向真实收件人发送；静态格式、合成回包和加密同步测试不能替代线上验收。

## 请求 0xE201

`FNetworkStaticsGame::SendSendMessageCard` 字符串 0x14DF28120，发送函数 0x148FBB5A0。写入 opcode 57857，然后一个字节及三个字符串。

| 顺序 | 字段 | 线上编码 |
| --- | --- | --- |
| 1 | 消息编号 | u16 LE，01 E2 |
| 2 | _message_card_type | u8，个人 Character=1，军团 Guild=2 |
| 3 | _receiver_nickname | ULEB128 UTF-8 字节长度 + UTF-8，无结尾 NUL |
| 4 | _title | 同上 |
| 5 | _body | 同上 |

字段顺序来自请求属性数组 0x14DC0D858：类型、收件人、标题、正文。类型枚举表 0x14DD9DED0..0x14DD9DF00：None=0，Character=1，Guild=2，Max=3。字符串写入 0x14924CA50 经 UTF-16 转 UTF-8 后调用 0x14924C8E0 写 ULEB128 长度。

整个明文帧：`ULEB128(正文长度+4) + 正文`。正文从 opcode 开始；“+4”是原生预留头部规则，并非实际变长前缀的长度。只对前缀之后的正文使用现有上行编码状态，插入后继续同步转发游戏出站流。

合成例子（不是实际收件人）：收件人 A，标题 B，正文 C：

```text
0D 01 E2 01 01 41 01 42 01 43
   opcode type  A     B     C
```

用户提供的游戏页面显示标题最多 50 字、正文最多 500 字，每封费用 500、每日 20 封。工具可选择个人（1）和军团（2），默认个人；不添加附件。军团模式的收件字段允许留空，目标范围、权限和费用尚未实测确认。输入要求非空有效 UTF-8，标题/正文保守按 UTF-16 单元计数；收件人 128 单元是本地缓冲保护，不代表已验证的游戏昵称上限。服务器仍可按等级、冷却等规则拒绝。

## 响应 0xE202

读取函数 0x1491C1220；属性数组 0x14DC09338。

| 字段 | 编码 |
| --- | --- |
| opcode | u16 LE |
| _result | u16 LE |
| _next_sent_count_reset_time | u64，Unix 毫秒（原生转 DateTime：值×10000+621355968000000000） |
| _character_message_card_sent_count | u8 |
| _guild_message_card_sent_count | u8 |

成功 result=0，原生客户端更新计数并显示提示。非零仍读取上述固定字段；尾部截断不会被工具当成已确认成功。

已核对 EResult 表 0x14DD42680..0x14DD42720：

| 代码 | 原生名称 | 显示含义 |
| --- | --- | --- |
| 0x217D | kMessageCard_InternalError | 邮件内部错误 |
| 0x217E | kMessageCard_NotFound | 邮件不存在 |
| 0x217F | kMessageCardSend_TooLongTitle | 标题过长 |
| 0x2180 | kMessageCardSend_TooLongBody | 正文过长 |
| 0x2181 | kMessageCardSend_TooManySend | 发送次数超限 |
| 0x2182 | kMessageCardSend_NotEnoughMoney | 金币不足 |
| 0x2183 | kMessageCardSend_ReceiverNotFound | 收件人不存在 |
| 0x2184 | kMessageCardSend_CannotReceive | 对方无法接收 |
| 0x2185 | kMessageCardSend_NotEnoughLevel | 等级不足 |
| 0x2186 | kMessageCardSend_ReceiverBlocked | 收件人被屏蔽或受限 |
| 0x2187 | kMessageCardSend_Cooltime | 冷却中 |

## 调试入口与状态

调试页 → 邮件测试 → 选择个人或军团类型 → 填写收件字段、标题、正文 → 点击“发送一封测试邮件”。必须使用已验证的实时世界代理连接。调用链：`App::mailDebugView → QueryProxy::requestMail → WorldMitm::mail → QueryStream::mail → encodeMailRequest`。

“构造明文包（不发送）”可以离线生成并复制完整十六进制帧。预览显示上次手动构造的内容；发送按钮始终使用输入框的当前内容重新构包。

排队不等于提交，提交不等于成功。邮件独占工具的主动操作时段，防止和资料查询、军团查询、跳跃同时排队。5 秒未能到完整帧边界则取消未发送请求。提交后等 15 秒；断线、响应截断、原生游戏邮件重叠或超时均不自动重发。无法归属的响应不会确认本次成功；超时/重叠后该连接不再接受邮件测试，需新连接。

响应观察独立于界面保留包上限，支持压缩容器内的 E202。邮件回包仍转发给游戏。日志只记录连接编号、字节数和结果码，不记录收件人、标题或正文；显式保存的会话/数据包仍含这些明文。

线上验收需由操作者手动发给自己控制的测试角色：比对原生请求及构造请求的字段，观察 E202 结果码和计数，再到收件角色确认实际到达。工具不会在构建、测试、启动或重连时自动发送邮件。
