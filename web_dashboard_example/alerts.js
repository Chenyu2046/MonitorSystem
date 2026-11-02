"use strict";

// Incident episode identity, polling and in-memory dedup. Patterns adapted from Keep/HolmesGPT;
// no database or message queue is needed for this single Dashboard process.
module.exports = function createAlerts(rpc, io, analyzeIncident) {
  const alerts = new Map();
  const running = new Map();
  const pending = [];
  let draining = false;
  let initialized = false;
  let polling = false;
  let timer;

  const episodeKey = i => [i.server_name, String(i.id), String(i.start_time_ms)].join(":");
  const snapshot = a => ({
    key:a.key,incident_id:a.incident_id,server_name:a.server_name,severity:a.severity,
    state:a.state,active:a.active,start_time_ms:a.start_time_ms,
    acknowledged:a.acknowledged,ai_status:a.ai_status,summary:a.summary || ""
  });
  const list = (serverName = "") => [...alerts.values()]
    .filter(a => !serverName || a.server_name === serverName)
    .sort((a,b) => Number(b.start_time_ms)-Number(a.start_time_ms))
    .map(snapshot);
  const emit = (type, entry) => io.emit(type, snapshot(entry));

  function remember(incident, initial = false) {
    const key = episodeKey(incident);
    let entry = alerts.get(key);
    if (entry) {
      const changed = entry.state !== incident.state || entry.severity !== incident.severity;
      const rank = {WARNING:1,HIGH:2,CRITICAL:3};
      const escalated = (rank[incident.severity] || 0) > (rank[entry.severity] || 0);
      Object.assign(entry,{state:incident.state,severity:incident.severity,active:!!incident.active});
      if (escalated) {
        entry.acknowledged=false;
        emit("diagnosis_alert",entry);
      } else if (changed) emit("diagnosis_updated",entry);
      return {entry,created:false,escalated};
    }
    entry={
      key,incident_id:String(incident.id),server_name:incident.server_name,
      severity:incident.severity,state:incident.state,active:!!incident.active,
      start_time_ms:incident.start_time_ms,acknowledged:initial,
      ai_status:process.env.OPENAI_API_KEY ? "pending" : "unavailable",summary:""
    };
    alerts.set(key,entry);
    // Bounded history in memory; persistent Incident history stays in Manager.
    if(alerts.size>100) alerts.delete(alerts.keys().next().value);
    if(!initial) emit("diagnosis_alert",entry);
    return {entry,created:true};
  }

  async function analyze(incident, force = false) {
    const {entry}=remember(incident,true);
    const key=entry.key;
    if (running.has(key)) return running.get(key);
    if (!force && entry.report?.ai_status === "ready") return entry.report;
    const task=(async()=>{
      entry.ai_status=process.env.OPENAI_API_KEY ? "analyzing" : "unavailable";
      emit("diagnosis_updated",entry);
      let report;
      try {
        report=await analyzeIncident(rpc,incident);
      } catch(error) {
        report={incident_id:String(incident.id),server_name:incident.server_name,
          root_causes:incident.root_causes || [],evidence:incident.evidence || [],
          summary:"",analysis:"",recommendations:[],
          ai_status:"failed",error:error.message || "AI 分析失败"};
      }
      entry.report=report;
      entry.summary=report.summary || "";
      entry.ai_status=report.ai_status;
      emit("diagnosis_updated",entry);
      return report;
    })();
    running.set(key,task);
    try {return await task;} finally {running.delete(key);}
  }

  // Keep AI calls sequential and bounded; notifications still fire immediately.
  async function drain() {
    if (draining) return;
    draining = true;
    try {
      while (pending.length) {
        const incident = pending.shift();
        await analyze(incident, true);
      }
    } finally {draining = false;}
  }

  async function poll() {
    if(polling)return;
    polling=true;
    try {
      const activeResult=await rpc("GetActiveDiagnosis",{server_name:""});
      const active=(activeResult.incidents || []).filter(i=>i.server_name && i.id != null && i.start_time_ms);
      // Include recently resolved episodes that could appear and disappear between 20s polls.
      let recent=[];
      try {
        const end=Math.floor(Date.now()/1000);
        const history=await rpc("GetIncidents",{
          server_name:"",time_range:{start_time:{seconds:end-600},end_time:{seconds:end}},
          pagination:{page:1,page_size:50}
        });
        recent=(history.incidents || []).filter(i=>i.server_name && i.id != null && i.start_time_ms);
      }catch(error){console.warn("Recent incident lookup failed:",error.message);}
      const merged=new Map();
      for(const incident of recent)merged.set(episodeKey(incident),incident);
      for(const incident of active)merged.set(episodeKey(incident),incident);
      const activeKeys=new Set(active.map(episodeKey));
      const fresh=[];
      for(const incident of merged.values()) {
        const {created,escalated}=remember(incident,!initialized);
        if(initialized && (created || escalated)) fresh.push(incident);
      }
      if(initialized) {
        for(const entry of alerts.values()) {
          if(entry.active && !activeKeys.has(entry.key)) {
            entry.active=false;
            emit("diagnosis_updated",entry);
          }
        }
      }
      initialized=true;
      // Every episode gets a notification. Bounded queue limits the number of LLM calls.
      if (process.env.OPENAI_API_KEY) {
        for (const incident of fresh) {
          if (pending.length < 50) pending.push(incident);
        }
        drain().catch(error => console.error("Incident analysis:",error));
      }
    } catch(error) {
      // A failed poll is not evidence that incidents have recovered.
      console.warn("Incident polling failed:",error.message);
    } finally {polling=false;}
  }
  return {
    start() {
      if(timer)return;
      poll();
      timer=setInterval(poll,20000);
      timer.unref?.();
    },
    stop() {if(timer)clearInterval(timer);timer=null;},
    poll,list,analyze,
    acknowledge(key) {
      const alert=alerts.get(key);
      if(!alert)return false;
      alert.acknowledged=true;
      emit("diagnosis_updated",alert);
      return true;
    },
    unread() {return [...alerts.values()].filter(a=>!a.acknowledged).length;},
    report(incident) {return alerts.get(episodeKey(incident))?.report || null;},
    getByKey(key) {const alert=alerts.get(key);return alert ? snapshot(alert):null;}
  };
};
