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

AI 功能需要**服务端环境变量**（不要把 Key 放在浏览器或提交到 Git）：

```bash
OPENAI_API_KEY=... OPENAI_MODEL=gpt-4.1-mini npm start
```

默认 Dashboard 只监听 127.0.0.1。Codespaces 需要端口转发时可显式设置 `HOST=0.0.0.0`，并将转发端口设为 Private。Manager 的 gRPC 目前未启用 TLS，请不要直接开放公网访问。

## 接入边界

- Web 页面直接复用上游的 React/MUI/Recharts + Socket.IO；保留所有通用趋势和查询页。
- `server.js` 载入本仓库 `../proto/query_api.proto`，读取 `QueryLatestScore`、`QueryTrend`、`QueryAnomaly` 和各类详情；评分切换为 `RESOURCE_SCORE` / `HEALTH_SCORE`。
- `QueryCpuCoreDetail` 不存在于 KernScope 的 proto 中，MVP 的 CPU 页面仅展示主机级使用率和趋势。
- `QueryNetDetail` 速率单位为 **KiB/s**，避免将它当 bytes/s 再次除以 1024。
- Node.js 内置只读 Agent：`POST /api/agent`，通过 LLM Tool Calling 调用四个现有 gRPC 接口：`QueryLatestScore`、`QueryTrend`、`GetActiveDiagnosis`、`GetIncidentDetail`。无需单独运行 HolmesGPT；设计思路参考其 SRE 调查流程。
- 不修改 Worker、Manager、MySQL Schema 或任何 Protobuf 文件。

## 当前限制

- 无真实数据时页面为空；本分支未在完整 eBPF + MySQL 环境完成端到端验证。
- 未配置 `OPENAI_API_KEY` 时监控页面仍可使用，AI 请求返回 503 配置提示。
- AI 只读、不执行命令；模型回答并不等同于已证实的根因。
- 默认采用浏览器 CDN 加载 React/MUI/Recharts，断网无法完成页面初始化。
- 修改基于原仓库 `main` @ `6f5099e8743521e08d380fb68d9ada8ba41982ec`。

## 验证

```bash
node --check server.js
node --check agent.js
npm ci && npm start
```

启动 KernScope Manager 后，在 Dashboard 中核对评分、时间戳、CPU/网络单位，再配置 Key 用「AI 分析」验证事件与 Evidence 查询。
