# 军团成员查询分析（2026-10-01）

## 结论

存在独立的 `GuildMemberInfo_RQ / GuildMemberInfo_RS`。响应分发已确认是 `0x8A05`，包含 GuildMemberInfo 玩家数组。请求的反射描述没有业务字段，没有军团 ID。响应写入当前角色持有的军团数据，并触发 `MyGuild_ChangeMemberList`，因此现有证据指向本军团成员查询，不支持把列表页选择的任意军团 ID 填进去查询成员。

这不是对服务端权限的实测结论：本次未发包、未加入军团、未验证未加入军团时的结果。RQ 的实际发送代码在现有转储中缺失，尚未确认其消息号；不能仅因响应是 0x8A05 就把相邻的 0x8A04 标成已验证请求。

## 证据

- `0x14EE0CC88`：GuildMemberInfo_RQ 反射描述；属性指针为 0、属性数量为 0。
- `0x14EE0CCC0`：GuildMemberInfo_RS 反射描述；结果码和 members 数组。
- members 元素 getter `0x1486A70C0` 的运行时代码引用 `0x14EE29D68`，对应 `DevPacketData_Common_GuildMemberInfo`。
- `0x1491A5430`：分发构造消息号 35333 / 0x8A05，调用 `0x14920E570` 读取；每个原生成员记录步长 160 字节。
- 消费路径：`0x148CA4D70` 设置成员数量，`0x148CA4E00` 更新成员，`0x148CA5260` 触发事件 448。
- 事件表 `0x14D7E84A0` 将 448 映射到 `EAionEventName::MyGuild_ChangeMemberList`。
- `FNetworkStaticsGame::SendGuildMemberInfo` 字符串存在于 `0x14DF29920`，但发送函数体未可靠定位，不能据此生成可用请求。

## 成员响应格式

来自 `0x14920E570`：u16 结果码，varuint 成员数量，然后逐条读取以下字段。0 结果走更新分支；非 0 走错误处理。

| 顺序 | 字段 | 线格式 |
| --- | --- | --- |
| 1 | 可选字段标志 | u8 |
| 2 | account_dbid | u64 |
| 3 | char_dbid | u64 |
| 4 | pc_id | u32；具体显示含义待核对 |
| 5 | 昵称 | 字符串 |
| 6 | 等级 | u32 |
| 7 | 征服者等级 | 标志 0x01 时 u32 |
| 8 | 军团职务 | u8 |
| 9 | shared_message | 标志 0x02 时字符串 |
| 10 | guild_fame | 标志 0x04 时 u64 |
| 11 | reputation | 标志 0x08 时 u64 |
| 12 | weekly_reputation | 标志 0x10 时 u64 |
| 13 | donation_reputation | 标志 0x20 时 u64 |
| 14 | weekly_donation_reputation | 标志 0x40 时 u64 |
| 15 | 在线标志 logined | packed bool |
| 16 | 最后登出时间 | u64；消费者按 Unix 毫秒转为日历 tick |
| 17 | 今日签到 | 标志 0x80 时 packed bool |
| 18 | subzone_id | u32 |
| 19 | 装备等级 equip_item_level | u32 |
| 20 | 战斗力 combat_power | u64 |

字符串为现有协议读取器处理的变长字节字符串；packed bool 沿用共享位游标，不可当作独立 u8。反射镜像偏移与原生内存偏移不是线格式偏移。在线标志只是该次服务器响应时的状态，不能当作永久实时状态。

## 容易混淆的联盟接口

`GuildUnionMemberView_RQ` 的确带 `_guild_union_id`，其 RS 有 `_members`，但这个 ID 是联盟 ID。成员元素 getter `0x1486A76A0` 的运行时代码引用 `0x14EE29E48`，对应 `DevPacketData_Common_GuildInfoTiny`：返回的是联盟里的军团条目，不是军团里的玩家名单。

## 实现状态与下一步

本次只新增分析材料，没有添加成员查询按钮或猜测发送包。现有“军团查询”页仍为推荐列表和名称搜索。

若后续接入本军团成员查询，应先在已经加入军团的角色中观察一次原生成员页请求，核对实际 RQ 消息号及响应样本，再增加成员名单视图。没有找到能够指定任意军团 ID 获取玩家名单的已验证路径。

反射证据：`artifacts/field-analysis/guild-member-reflection-20261001.json`。分析范围为当前 2026-09-30 转储及只读运行时代码，地址可能随版本变化。
