"use strict";

// Read-only investigation agent. The browser never receives the API key.
const TOOLS = [
  {type: "function", function: {name: "get_host_status", description: "Read the selected host's current metrics and health status.", parameters: {type: "object", properties: {}, additionalProperties: false}}},
  {type: "function", function: {name: "get_metric_trend", description: "Read the selected host's recent CPU, I/O, memory, network and score trends.", parameters: {type: "object", properties: {minutes: {type: "integer", description: "Lookback in minutes, 5-60."}}, additionalProperties: false}}},
  {type: "function", function: {name: "get_active_incidents", description: "Read active diagnosis incidents for the selected host.", parameters: {type: "object", properties: {}, additionalProperties: false}}},
  {type: "function", function: {name: "get_incident_detail", description: "Read evidence and rule-based candidate root causes for a known incident ID.", parameters: {type: "object", properties: {incident_id: {type: "string"}}, required: ["incident_id"], additionalProperties: false}}}
];

function range(minutes) {
  const now = Math.floor(Date.now() / 1000);
  return {start_time: {seconds: now - minutes * 60}, end_time: {seconds: now}};
}

module.exports = function createAgentHandler(rpc) {
  return async function agentHandler(req, res) {
    const serverName = String(req.body?.server_name || "").trim();
    const question = String(req.body?.question || "").trim();
    if (!serverName || !question || serverName.length > 256 || question.length > 1000) {
      return res.status(400).json({error: "请选择主机并输入问题（最多 1000 字）。"});
    }
    if (!process.env.OPENAI_API_KEY) {
      return res.status(503).json({error: "Agent 未配置：请在服务端设置 OPENAI_API_KEY。"});
    }

    const callTool = async (name, args) => {
      if (name === "get_host_status") {
        const response = await rpc("QueryLatestScore", {});
        const server = (response.servers || []).find(s => s.server_name === serverName);
        return server ? {server} : {error: "主机不存在或暂无最新样本"};
      }
      if (name === "get_metric_trend") {
        const minutes = Math.max(5, Math.min(60, Number(args.minutes) || 30));
        const response = await rpc("QueryTrend", {
          server_name: serverName,
          time_range: range(minutes),
          interval_seconds: 60
        });
        return {
          minutes,
          records: (response.records || []).slice(-60).map(s => ({
            timestamp: s.timestamp,
            cpu_percent: s.cpu_percent,
            io_wait_percent: s.io_wait_percent,
            mem_used_percent: s.mem_used_percent,
            disk_util_percent: s.disk_util_percent,
            rcv_rate_kib_s: s.rcv_rate,
            send_rate_kib_s: s.send_rate,
            health_score: s.health_score,
            health_valid: s.health_valid,
            anomaly_score: s.anomaly_score
          }))
        };
      }
      if (name === "get_active_incidents") {
        const response = await rpc("GetActiveDiagnosis", {server_name: serverName});
        return {incidents: (response.incidents || []).slice(0, 20).map(i => ({
          id: i.id, severity: i.severity, state: i.state, start_time_ms: i.start_time_ms,
          root_causes: i.root_causes, evidence_count: (i.evidence || []).length
        }))};
      }
      if (name === "get_incident_detail") {
        if (!/^\d+$/.test(String(args.incident_id || ""))) return {error: "无效 Incident ID"};
        const response = await rpc("GetIncidentDetail", {incident_id: String(args.incident_id)});
        const incident = response.incident;
        if (!incident || incident.server_name !== serverName) return {error: "该主机没有对应 Incident"};
        return {incident: {...incident, evidence: (incident.evidence || []).slice(0, 40)}};
      }
      return {error: "不支持的工具"};
    };

    const messages = [
      {role: "system", content: "你是 KernScope 的只读 Linux 性能诊断助手。必须基于工具返回的真实主机指标、Incident 和 Evidence 回答。先查询相关工具再分析；优先报告异常现象、原始证据、候选根因及排查建议。区分规则引擎候选根因与已证实根因。数据不足明确说明。不要编造进程、堆栈、指标、时间或诊断结果。不要提供自动执行命令的行为。回复简洁，使用中文。"},
      {role: "user", content: "主机：" + serverName + "\n问题：" + question}
    ];

    try {
      for (let step = 0; step < 3; step++) {
        const response = await fetch("https://api.openai.com/v1/chat/completions", {
          method: "POST",
          headers: {"Content-Type": "application/json", Authorization: "Bearer " + process.env.OPENAI_API_KEY},
          body: JSON.stringify({
            model: process.env.OPENAI_MODEL || "gpt-4.1-mini",
            messages,
            tools: TOOLS,
            tool_choice: step === 0 ? "required" : step === 2 ? "none" : "auto",
            temperature: 0.1
          }),
          signal: AbortSignal.timeout(30000)
        });
        if (!response.ok) throw new Error("LLM API 请求失败（HTTP " + response.status + "）");
        const data = await response.json();
        const message = data.choices?.[0]?.message;
        if (!message) throw new Error("LLM 未返回有效响应");
        if (!message.tool_calls?.length) {
          return res.json({server_name: serverName, answer: message.content || "数据不足，暂无法形成可靠分析。"});
        }
        messages.push(message);
        for (const call of message.tool_calls) {
          let result;
          try {
            const params = JSON.parse(call.function.arguments || "{}");
            result = await callTool(call.function.name, params);
          } catch (error) {
            result = {error: "查询失败：" + error.message};
          }
          messages.push({role: "tool", tool_call_id: call.id, content: JSON.stringify(result)});
        }
      }
      return res.status(502).json({error: "Agent 未能形成结论。"});
    } catch (error) {
      return res.status(502).json({error: error.message || "Agent 请求失败"});
    }
  };
};
