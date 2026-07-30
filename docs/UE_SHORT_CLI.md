# UE 短操作 CLI

`ue` 是连接运行中 Unreal Editor 的轻量短操作 CLI。它把随 CLI 分发的
manifest capability 动态映射为传统命令行，首参数就是完整 capability ID：

```powershell
ue blueprint.asset.get --name /Game/UI/WBP_Login
ue scene.actor.spawn --type PointLight --name KeyLight --location '[0,0,300]'
ue production.job.status --job-id job-123
```

`ue` 与六个领域 MCP 工具共享 `/api/capabilities` 和 `/api/execute`，但 MCP
不会启动 `ue` 子进程。`ue-workflow` 只负责确定的连续资产编辑 DSL；调试、
查询和 Job 启动等单次操作不进入 Workflow。

## 命令

```text
ue status
ue capabilities [filters]
ue skills [filters]
ue help <capability>
ue <capability> --help
ue <capability> [capability parameters]
ue shell [--live-schema]
ue --version
```

全局参数：

```text
--endpoint <http://127.0.0.1:port>
--timeout-ms <milliseconds>
--request-id <id>
--params <json-object>
--params-file <path|->
--confirm-write
--output <path>
--live-schema
--capability-root <path>
--skill-root <path>
--json
```

默认 endpoint 是 `http://127.0.0.1:9847`。`UE_PORT` 与 `UE_TIMEOUT_MS`
可设置默认值，显式 `--endpoint` 和 `--timeout-ms` 优先。

所有层级的 `--help` 都在加载本地 manifest 或创建 Editor 连接前处理。例如
`ue blueprint.scan --help --live-schema` 只显示语法帮助，不会扫描资产，也不会
产生 HTTP 请求。需要 schema 生成的精确参数说明时使用
`ue help blueprint.scan`。

## 双模式 schema 与连接复用

默认模式从安装包内 `Resources/Capabilities`（CMake 安装布局为
`share/ue-workflow/Capabilities`）加载 schema，参数转换后只发送一次
`POST /api/execute`。Editor 仍会使用当前注册表完成最终 schema、availability、
业务约束和权限校验，因此本地 manifest 不会绕过服务端验证。

诊断能力漂移或必须先确认当前工程 availability 时，添加 `--live-schema`：

```text
GET /api/capabilities?operation=<id>&detail=full
POST /api/execute
```

该选项对 `help`、`capabilities` 和 capability 执行都生效。默认本地
`capabilities` 的 `available` 为 `null`；`--available-only` 必须和
`--live-schema` 一起使用。`--capability-root` 与 `UE_CAPABILITY_ROOT`
仅用于开发、测试或诊断本地目录。

`ue shell` 是持久模式：启动时加载一次本地 capability 与 Skill 目录，并在
多条命令间复用同一个 HTTP keep-alive 连接。shell 内也可先运行 `skills`
加载 recipe，再运行 `help`、执行和验证命令。也可只给某一条命令添加
`--live-schema`；若
shell 启动时带该选项，则所有在线 API 发现与执行命令都强制在线校验。
session 的 endpoint、
timeout、capability root 和 skill root 在启动后固定：

```text
ue shell
ue> skills --name ue-blueprint-diagnose --json
ue> scene.pie.status
ue> blueprint.asset.get --name /Game/BP_A --live-schema
ue> exit
```

`skills` 始终读取本地包；即使 shell 以 `--live-schema` 启动，它也不会连接
Editor。显式输入 `skills --live-schema` 仍会作为无效组合拒绝。

普通 `ue <capability>` 仍是一条命令执行后立即退出。CLI 不复制 UE handler，
也不提供本地 UE 执行；插件始终是 Game Thread 调度与执行结果的唯一权威来源。

## 调用方会话

每个 `ue` 进程生成一个 `invocationId`，并最佳努力调用
`/api/v1/clients/register`。注册成功后，同一普通命令或 shell 中的所有请求
携带同一个 `X-UEAI-Session-Id`；正常退出时使用独立、最长一秒的连接注销。
长请求在服务端会 pin 住会话，不依赖 CLI 心跳。

注册接口返回 404 时，当前进程固定退回 Legacy HTTP；网络错误、503 或无效响应
会在下一次业务请求重新尝试。已注册请求若收到 `client_session_expired` 或
`client_session_not_found`，CLI 会重新注册，并且只重放原请求一次。会话与
invocation 仅用于 Editor 状态菜单的归属和统计，不参与授权，也不会改变
capability 的执行结果。

## Agent Skill recipe

`ue skills` 从本地 `skills/*/skill.json` 搜索机器 recipe，不连接 Editor：

```powershell
ue skills --query blueprint
ue skills --name ue-blueprint-diagnose --recipe scan-and-verify --detail full --json
```

支持 `--query`、`--name`、`--recipe`、`--domain`、`--operation`、`--risk`、
`--detail`、`--offset` 和 `--limit`。`UE_SKILL_ROOT` / `--skill-root` 仅覆盖
本地 Skill 包位置。普通 capability 调用不会加载 SkillCatalog。

Skill recipe 不执行命令，也不复制 capability 参数 schema。按 recipe 选定
operation 后，使用 `ue help <operation>` 发现参数，再执行普通短操作和
recipe 的 verify readback。完整设计见 [UE Agent Skills](UE_AGENT_SKILLS.md)。

## 参数映射

- schema 的 lowerCamel 字段生成 kebab-case 主 flag，例如
  `assetPath` 对应 `--asset-path`；同时接受 `--assetPath`。
- string、integer、number、boolean 按 schema 转换。
- boolean 接受 `--enabled`、`--enabled false`、`--no-enabled`。
- primitive array 可重复 flag；也可直接传完整 JSON 数组。
- object 和 object array 使用 JSON。`@file.json` 从文件读取 JSON，
  `@@value` 表示字面量 `@value`。
- 未知参数、重复非数组参数与缺少 required 参数在客户端拒绝；enum、范围和
  UE 业务约束由 Editor 最终验证。
- schema 声明 `requestId` 时自动生成。`--request-id` 用于可控重试。
- `--confirm-write` 只在 schema 声明 `confirmWrite` 时注入。

完整参数对象也可以避免 PowerShell 的逐字段转义：

```powershell
ue blueprint.scan --params-file .\scan-params.json
Get-Content .\scan-params.json -Raw | ue blueprint.scan --params-file -
ue blueprint.scan --params '{"roots":["/Game"],"minimumSeverity":"medium"}'
```

`--params` 与 `--params-file` 互斥，且完整对象模式不能再混用 schema 生成的
`--field` 参数。CLI 仍依据本地或在线 descriptor 检查字段、required 与基础
JSON 类型，Editor 负责最终业务校验。

## 输出和退出码

默认成功输出是单行摘要，最多 8 个优先标量或集合计数，且不超过 1 KiB：

```text
OK blueprint.asset.get assetPath=/Game/UI/WBP_Login graphs=3 variables=5
```

Base64、完整节点树和大型数组不会进入默认摘要。省略字段时会提示使用
`--json`，该选项输出完整稳定的 `{ok,data|error}` envelope。错误默认写
stderr，并带第一条 validation error。

退出码：

| Code | 含义 |
|---:|---|
| 0 | 成功 |
| 2 | 参数、schema 或 capability ID 错误 |
| 4 | Editor 不可达、超时或 capability unavailable |
| 5 | Editor 已接收但 operation 执行失败 |

`--output` 支持 Base64 与 `ue.artifact.v1` 分块结果，校验连续 offset、
`sizeBytes`、EOF 和 SHA-256 后再原子替换目标文件。未指定输出路径时默认摘要
不会打印 Base64。

## 构建与分发

```powershell
cmake -S . -B build-workflow -DUE_WORKFLOW_BUILD_TESTS=ON
cmake --build build-workflow --config Release
cmake --install build-workflow --config Release --prefix C:\Tools\ue
```

Linux/macOS 使用同一 CMake 工程构建本机程序：

```bash
cmake -S . -B build-workflow -DCMAKE_BUILD_TYPE=Release -DUE_WORKFLOW_BUILD_TESTS=ON
cmake --build build-workflow
cmake --install build-workflow --prefix "$HOME/.local"
```

安装和插件包均包含：

```text
CLI/bin/ue(.exe)
CLI/bin/ue-workflow(.exe)
Resources/Capabilities/*.json
skills/*/SKILL.md
skills/*/skill.json
```

通过 `scripts/build_plugin.bat` 或 `scripts/build_plugin.sh` 生成的
source-capable 插件包还保留根 `CMakeLists.txt`、`CLI` 与 `Workflow` 源码，
因此可以在 Win64、Linux、Mac 宿主机上重建对应平台的 CLI。`CLI/bin` 下
已经生成的程序只适用于打包时的宿主平台。

MCP `ue_cli` 结果保留原 Workflow locator 字段，并在 `shortCli` 中按
`UE_CLI`、packaged、`PATH`、development 的顺序报告短操作 CLI。
