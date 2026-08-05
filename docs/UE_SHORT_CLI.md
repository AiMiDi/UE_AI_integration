# UE 短操作 CLI

`ue` 是轻量短操作 CLI。普通能力连接运行中的 Unreal Editor；manifest 声明
`localTrace` 的 `.utrace` import/query/export/open 可由 Engine 匹配的本地 Worker
在 Editor 关闭时执行。CLI 把随包分发的 capability 动态映射为传统命令行，
首参数就是完整 capability ID：

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
ue trace doctor
ue trace target list
ue trace start|status|stop
ue trace import|analyze|providers
ue trace query <provider>
ue trace export|open
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

普通 `ue <capability>` 仍是一条命令执行后立即退出。CLI 不复制 UE handler；
Editor capability 仍以 Game Thread 调度和 Editor 结果为唯一权威，本地 Trace
能力则以 `UEAITraceWorker + UEAITraceAnalysisCore` 为唯一权威。manifest 声明为
`localProject`、`localAsset`、`localRecipe`、`localSal` 或
`developmentRuntime` 的能力由 CLI 调用与 MCP 相同的受限 executor；因此离线
路径边界、敏感配置脱敏、Recipe 审批和 Runtime attach 合同不会形成第二套实现。

`backend=auto|editor|local` 是调用方选择语义；成功 envelope 的
`meta.executionBackend` 会返回实际的精确 backend 名称。全局 `--output` 不用于
本地 backend，artifact 必须使用能力自身声明的输出参数。

## Trace 快捷命令与后端

`ue trace` 是 `production.trace.*` capability 的稳定快捷语法：

```powershell
ue trace doctor
ue trace target list
ue trace import --trace-path D:\Traces\sample.utrace --backend local
ue trace providers --trace-id trace-local-...
ue trace query timing --trace-id trace-local-... --operation frames --limit 100
ue trace export --trace-id trace-local-... --provider timing --operation timers --format json
ue trace open --trace-id trace-local-... --view timing
```

`backend=auto|editor|local` 仍由 capability manifest 校验。Editor/PIE start 固定
走 Editor，Development start 固定走受约束本地 Launch Profile；带本地 Trace/
Analysis/Launch ID 命名空间的 status、query 和 Job 操作回到 Worker。显式选择的
后端失败时不静默切换。

Worker 使用当前用户专属本地 IPC，不监听 TCP/HTTP；按需启动，握手验证 Engine
版本、协议和 contract digest。CLI 会优先使用显式 Engine、环境变量或已安装项目
`.uproject` 的精确 `EngineAssociation`，验证 `Build.version` 的 major/minor 后以
`-EngineDir` 传给 Worker；已声明但无效或不匹配的来源不会静默回退。所有 Worker
启动固定带 `-NoLog -NoDefaultLog -SaveToUserDir`。完整路径解析顺序、允许读取的目录、Provider 适配矩阵以及
“不操作 Insights UI”的边界见
[渲染调试证据与离线 Unreal Insights](UE_TRACE_INSIGHTS.md)。

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

复杂 JSON 在 PowerShell 中应优先使用 `--params-file <path>`；需要管道时使用
`--params-file -`。`--params <json>` 仅保留给简单对象和已经正确处理引号的
调用方，不再作为 PowerShell 的推荐写法：

```powershell
ue blueprint.scan --params-file .\scan-params.json
Get-Content .\scan-params.json -Raw | ue blueprint.scan --params-file -
ue blueprint.scan --params '{"roots":["/Game"],"minimumSeverity":"medium"}'
```

`--params` 与 `--params-file` 互斥，且完整对象模式不能再混用 schema 生成的
`--field` 参数。CLI 仍依据本地或在线 descriptor 检查字段、required 与基础
JSON 类型，Editor 负责最终业务校验。

Windows PowerShell 5.1 在进入 CLI 前就可能把管道中的非 ASCII 字符替换为
`?`；CLI 无法恢复已经丢失的字符。使用管道前必须显式设置
`$OutputEncoding`，或直接传入 UTF-8/带 BOM UTF-16 文件。完整编码合同和
示例见 [能力搜索与 Windows Unicode](CAPABILITY_SEARCH_AND_UNICODE.md)。

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
| 4 | Editor/Worker 不可达、超时或 capability unavailable |
| 5 | Editor/Worker 已接收但 operation 执行失败 |

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

完整插件 staging 还包含与 Engine 次版本匹配的
`Tools/Trace/<platform>/<engineVersion>/UEAITraceWorker(.exe)`、Trace 协议
manifest 和 Insights Action Mapping。普通跨平台 CMake CLI 安装不假定存在 UE
源码树，因此不会凭空生成 Worker。

正式 staging 的 Worker 验证使用 `-NoLog -NoDefaultLog -SaveToUserDir`，并要求匹配 Engine 中存在同次版本
Unreal Insights。`Tools/UEAITraceWorker/Saved` 是运行时生成物；若它出现在 staging
或安装目录，安装前置检查会拒绝该包。

通过 `scripts/build_plugin.bat` 或 `scripts/build_plugin.sh` 生成的
source-capable 插件包还保留根 `CMakeLists.txt`、`CLI` 与 `Workflow` 源码，
因此可以在 Win64、Linux、Mac 宿主机上重建对应平台的 CLI。`CLI/bin` 下
已经生成的程序只适用于打包时的宿主平台。

MCP `ue_cli` 结果保留原 Workflow locator 字段，并在 `shortCli` 中按
`UE_CLI`、packaged、`PATH`、development 的顺序报告短操作 CLI。
