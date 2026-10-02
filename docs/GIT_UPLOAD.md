# Git 源码目录说明

提交内容包括 `src/`、`tests/`、`scripts/`、`docs/`、`data/skin_catalog.json`、`chat_bridge/source/`、聊天组件构建配置，以及根目录的 CMake 和说明文件。`.codex/skills/` 是项目打包技能，也可随源码提交。

## 本机保留、Git 排除

- `private/`：查询服务、MQTT、角色上传等私有凭据与构建输入。
- `build/`、`dist/`、`dist-*/`、`release/`：编译结果与发布包，可能已嵌入凭据。
- `artifacts/`：调试证据、抓包、会话和临时验证文件。
- `third_party/`：由 `scripts/bootstrap.ps1` 下载并校验的依赖。
- `chat_bridge/.python/`、`.venv/`、`.venv-local/`、`build/`、`dist/`：本机 Python 环境和生成文件。
- 日志、数据库、证书配置旁的本地私有 JSON、环境变量文件和缓存。

忽略规则不删除本机文件。请通过 Git 上传源码，不要将整个工作目录直接打包公开。带有内置凭据的可执行文件和私有配置需要单独管理。

## 新机器构建

1. 安装 Windows x64 的 Visual Studio C++ 桌面开发工具和 CMake。
2. 执行 `scripts/bootstrap.ps1` 获取第三方依赖。
3. 按 [查询服务说明](QUERY_SERVICE.md) 准备自己的查询服务配置，再执行 `scripts/embed-query-credential.ps1`。`private/query_credential.hpp` 是必需的本机构建输入，不在仓库中。
4. 执行 `scripts/build.ps1` 编译和测试。部分真实抓包验证需要本机 `artifacts/` 下的样本；仓库不分发这些采集数据。
5. 构建聊天组件时，准备 Python 并执行 `scripts/build-chat-bridge.ps1`。MQTT 配置可参考 `chat_bridge/source/mqtt.private.example.json`。

角色上传令牌的可选构建默认值位于 `private/character-report-defaults.hpp`；未提供时源码默认值为空，可在客户端填写。

## 上传前检查

```powershell
& .\.codex\skills\aion2pipe-package\scripts\package.ps1 -CheckOnly
git status --short
git add --dry-run .
```

第一条命令只检查目录规则和 Git 可见文件，不构建、不打包、不暂存。已经被 Git 跟踪的产物即使符合忽略规则也会报错；需要检查后单独处理，不能仅依靠 `.gitignore`。

确认列表只有源码、文档和必要静态数据后，再暂存、提交并推送到自己的仓库。不要使用 `git add -f` 绕过私有目录的忽略规则。
