# KernScope Web Dashboard + AI 诊断（MVP）

本目录的 React Dashboard、Express BFF 和 CSS 源自 [linux_monitor_system](https://github.com/superlxh02/linux_monitor_system/tree/main/web_dashboard_example)（Apache-2.0），按 KernScope 的 QueryService 进行了最小修改。原始许可证见 [LICENSE.upstream](LICENSE.upstream)。

## 启动

要求 Node.js >= 18；KernScope Manager 的 gRPC QueryService 已启动，历史查询需要 MySQL 正常工作。

```bash
cd web_dashboard_example
npm ci
MANAGER_ADDRESS=127.0.0.1:50051 npm start
```

访问 http://127.0.0.1:3000。

AI 功能可在 Dashboard 侧栏的「系统设置 → API 配置」中填写 DeepSeek API Key。Key 仅提交给本机服务端，不会回传到页面；配置保存在当前用户目录，不会写入仓库。也可以继续通过服务端环境变量配置：

```bash
DEEPSEEK_API_KEY=你的密钥 DEEPSEEK_MODEL=deepseek-flash npm start
```

Agent 的问答与自动 Incident 分析**只调用 DeepSeek**：`POST https://api.deepseek.com/chat/completions`。默认模型 `deepseek-flash`，可通过 `DEEPSEEK_MODEL=deepseek-v4-pro` 切换；不再读取 `OPENAI_API_KEY` / `OPENAI_MODEL`。为兼容首次强制 Tool Calling，显式设置 `thinking.type=disabled`；自动分析使用 `response_format=json_object`。请仅在服务端配置 Key。

UI 保存的 Key 位于 Windows `%APPDATA%\KernScope\deepseek-api-key.json`，继承当前用户 AppData 目录权限；Linux/macOS 位于 `~/.config/KernScope/deepseek-api-key.json`，文件权限设为仅当前用户可读。UI 保存的本机配置优先于服务启动环境变量；移除本机配置后，若启动环境仍提供 Key，则继续使用环境变量。

默认 Dashboard 只监听 127.0.0.1。Codespaces 需要端口转发时可显式设置 `HOST=0.0.0.0`，并将转发端口设为 Private。Manager 的 gRPC 目前未启用 TLS，请不要直接开放公网访问。

## 接入边界

- Web 页面直接复用上游的 React/MUI/Recharts + Socket.IO；保留所有通用趋势和查询页。
- `server.js` 载入本仓库 `../proto/query_api.proto`，读取 `QueryLatestScore`、`QueryTrend`、`QueryAnomaly` 和各类详情；评分切换为 `RESOURCE_SCORE` / `HEALTH_SCORE`。
- `QueryCpuCoreDetail` 不存在于 KernScope 的 proto 中，MVP 的 CPU 页面仅展示主机级使用率和趋势。
- `QueryNetDetail` 速率单位为 **KiB/s**，避免将它当 bytes/s 再次除以 1024。
- Node.js 内置只读 Agent：`POST /api/agent`，通过 LLM Tool Calling 调用五个只读接口：`QueryLatestScore`、`QueryTrend`、`GetActiveDiagnosis`、`GetIncidents`、`GetIncidentDetail`。无需单独运行 HolmesGPT；设计思路参考其 SRE 调查流程。
- 不修改 Worker、Manager、MySQL Schema 或任何 Protobuf 文件。

## 当前限制

- 无真实数据时页面为空；本分支未在完整 eBPF + MySQL 环境完成端到端验证。
- 未配置 `DEEPSEEK_API_KEY` 时监控页面仍可使用，AI 请求返回 503 配置提示。
- AI 只读、不执行命令；模型回答并不等同于已证实的根因。
- 默认采用浏览器 CDN 加载 React/MUI/Recharts，断网无法完成页面初始化。
- 修改基于原仓库 `main` @ `6f5099e8743521e08d380fb68d9ada8ba41982ec`。

## 验证

```bash
node --check server.js
node --check agent.js
npm ci && npm start
```

启动 KernScope Manager 后，在 Dashboard 中核对评分、时间戳、CPU/网络单位，再配置 DeepSeek Key 后用「AI 分析」验证事件与 Evidence 查询。

## 智能诊断与主动告警

保留此前 Dashboard，新增「智能诊断」页面及顶部告警通知。界面展示 Manager 的 `Incident`、`RootCause`、`Evidence`，AI 仅补充解释和排查建议。

- **问答**：`POST /api/agent` 支持最近 8 条 user/assistant 历史消息、5 个只读查询工具、最长 24 小时趋势与 7 天历史事件。
- **历史故障**：`GET /api/incidents?server_name=...&hours=24&page=1&page_size=30` 与 `GET /api/incidents/:id?server_name=...`。
- **手动报告**：`POST /api/incidents/:id/analyze`，请求体 `{ "server_name": "...", "refresh": true }` 可重新分析；报告保留原始 Evidence 和规则根因。
- **主动告警**：BFF 每 20 秒查询活跃事件及最近 10 分钟历史事件，按 `server_name + incident_id + start_time_ms` 去重；新事件或严重级别升级推送 Socket.IO 通知。
- **自动诊断**：配置 `DEEPSEEK_API_KEY` 后，新增 Incident 进入有界串行分析队列（最多 50 个等待任务）；未配置时只显示规则根因与证据，不冒充 AI 报告。
- **告警确认**：`GET /api/alerts`、`POST /api/alerts/ack`。告警及确认状态存 Node.js 内存；历史记录仍以 Manager Incident 为准。

安全边界：Dashboard API 同源，拒绝外部浏览器 Origin 的写请求；API Key 仅在服务端；诊断只读，不运行 Linux 命令，不自动修复。不要公开无认证的 BFF 或明文 Manager gRPC。

**限制**：单进程内存告警无法跨实例同步，重启丢失确认状态；前端历史事件仅展示第一页最多 50 条；自动历史扫描 10 分钟，BFF 停机超过窗口内的短时异常需人工查询；尚未在真实 eBPF/LLM 环境完成端到端验证。

参考：HolmesGPT 的工具驱动故障调查、Keep 的事件去重与告警管理思路；未安装框架或复制其源文件。

### 回归测试

`node --test test/diagnosis.test.js`

覆盖告警去重、确认、恢复、短时故障、无 Key 证据保留与 Agent 的主机隔离/历史上下文。真实 gRPC/Socket.IO/浏览器联调仍需现场验证。
