"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const createAlerts = require("../alerts");
const createAgent = require("../agent");

function incident(id, active=true) {
  return {id:String(id),server_name:"host-A",start_time_ms:String(1000*id),
    end_time_ms:String(1000*id+100),severity:"HIGH",state:"DIAGNOSTIC",
    active,root_causes:[{type:"CPU_SATURATION",confidence:.8,evidence_ids:["E1"]}],
    evidence:[{id:"E1",type:"CPU",value:95,unit:"%"}]};
}
function rpcMock(state) {
  return async (method,params) => {
    if(method==="GetActiveDiagnosis")return {incidents:state.active};
    if(method==="GetIncidents")return {incidents:state.recent};
    if(method==="QueryTrend")return {records:[]};
    if(method==="GetIncidentDetail")return {incident:state.detail || incident(Number(params.incident_id))};
    if(method==="QueryLatestScore")return {servers:[]};
    throw new Error("Unexpected RPC "+method);
  };
}
test("new episodes alert exactly once; acknowledge and recovery preserve history",async()=>{
  const state={active:[],recent:[]},events=[];
  const alerts=createAlerts(rpcMock(state),{emit:(type,item)=>events.push({type,item})},
    async()=>({ai_status:"unavailable"}));
  await alerts.poll();  // Baseline: existing incidents do not create a burst.
  assert.equal(alerts.unread(),0);
  state.active=[incident(1)];state.recent=[incident(1)];
  await alerts.poll();await alerts.poll();
  assert.equal(events.filter(e=>e.type==="diagnosis_alert").length,1);
  assert.equal(alerts.unread(),1);
  assert.equal(alerts.acknowledge(alerts.list()[0].key),true);
  assert.equal(alerts.unread(),0);
  state.active=[];state.recent=[];
  await alerts.poll();
  assert.equal(alerts.list()[0].active,false);
  alerts.stop();
});
test("closed incident between polls is detected through historical query",async()=>{
  const state={active:[],recent:[]},events=[];
  const alerts=createAlerts(rpcMock(state),{emit:(t,v)=>events.push(t)},async()=>({}));
  await alerts.poll();
  state.recent=[incident(2,false)];
  await alerts.poll();
  assert.equal(alerts.list()[0].incident_id,"2");
  assert.equal(events.filter(x=>x==="diagnosis_alert").length,1);
  alerts.stop();
});
test("without an API key, diagnosis never invents AI findings",async()=>{
  const saved=process.env.DEEPSEEK_API_KEY;
  delete process.env.DEEPSEEK_API_KEY;
  try{
    const report=await createAgent.analyzeIncident(rpcMock({}),incident(3));
    assert.equal(report.ai_status,"unavailable");
    assert.equal(report.evidence[0].id,"E1");
    assert.equal(report.root_causes[0].confidence,.8);
    assert.deepEqual(report.recommendations,[]);
  }finally{
    if(saved===undefined)delete process.env.DEEPSEEK_API_KEY;
    else process.env.DEEPSEEK_API_KEY=saved;
  }
});
test("tool requests are restricted to the selected host and retain history",async()=>{
  const previousFetch=global.fetch,oldKey=process.env.DEEPSEEK_API_KEY;
  process.env.DEEPSEEK_API_KEY="mock-key";
  let turn=0,chatBodies=[];
  global.fetch=async(url,options)=>{
    assert.equal(url,"https://api.deepseek.com/chat/completions");
    assert.equal(options.headers.Authorization,"Bearer mock-key");
    const body=JSON.parse(options.body);
    assert.equal(body.model,"deepseek-flash");
    assert.deepEqual(body.thinking,{type:"disabled"});
    chatBodies.push(body);
    turn++;
    return {ok:true,json:async()=>({choices:[{message:turn===1
      ? {role:"assistant",content:null,tool_calls:[{id:"c1",type:"function",function:{
          name:"get_incidents",arguments:'{"hours":24}'}}]}
      : {role:"assistant",content:"基于已有故障证据进行排查"}}]})};
  };
  const calls=[];
  const rpc=async(method,params)=>{
    calls.push({method,params});
    return {total_count:2,incidents:[
      incident(4),{...incident(5),server_name:"host-B"}
    ]};
  };
  const res={statusCode:200,status(n){this.statusCode=n;return this;},
    json(data){this.body=data;return this;}};
  try{
    await createAgent(rpc)({body:{server_name:"host-A",question:"最近故障?",
      history:[{role:"user",content:"之前的 CPU 问题"}]}},res);
    assert.equal(res.statusCode,200);
    assert.deepEqual(res.body.tools_used,["get_incidents"]);
    assert.equal(calls[0].params.server_name,"host-A");
    assert.ok(chatBodies[0].messages.some(m=>m.content==="之前的 CPU 问题"));
    assert.equal(chatBodies[0].tool_choice,"required");
    assert.equal(chatBodies[1].tool_choice,"auto");
    const tool=chatBodies[1].messages.find(m=>m.role==="tool");
    assert.ok(tool.content.includes("host-A"));
    assert.ok(!tool.content.includes("host-B"));
  }finally{
    global.fetch=previousFetch;
    if(oldKey===undefined)delete process.env.DEEPSEEK_API_KEY;
    else process.env.DEEPSEEK_API_KEY=oldKey;
  }
});


test("AI refuses a direct answer with no successful tool evidence",async()=>{
  const oldKey=process.env.DEEPSEEK_API_KEY,oldFetch=global.fetch;
  process.env.DEEPSEEK_API_KEY="mock-key";
  global.fetch=async()=>({ok:true,json:async()=>({choices:[{message:{role:"assistant",content:"猜测 CPU 异常"}}]})});
  const res={code:200,status(n){this.code=n;return this;},json(v){this.body=v;return this;}};
  try{
    await createAgent(async()=>{throw new Error("should not call RPC")})({
      body:{server_name:"host-A",question:"有什么故障?"}
    },res);
    assert.equal(res.code,502);
    assert.match(res.body.error,/证据/);
  }finally{
    global.fetch=oldFetch;
    if(oldKey===undefined)delete process.env.DEEPSEEK_API_KEY;
    else process.env.DEEPSEEK_API_KEY=oldKey;
  }
});


test("automatic diagnosis uses DeepSeek JSON mode and retains source evidence",async()=>{
  const oldKey=process.env.DEEPSEEK_API_KEY,oldModel=process.env.DEEPSEEK_MODEL,oldFetch=global.fetch;
  process.env.DEEPSEEK_API_KEY="fake-deepseek-key";
  process.env.DEEPSEEK_MODEL="deepseek-v4-pro";
  let requests=0;
  global.fetch=async(url,options)=>{
    requests++;
    assert.equal(url,"https://api.deepseek.com/chat/completions");
    assert.equal(options.headers.Authorization,"Bearer fake-deepseek-key");
    const payload=JSON.parse(options.body);
    assert.equal(payload.model,"deepseek-v4-pro");
    assert.deepEqual(payload.thinking,{type:"disabled"});
    assert.deepEqual(payload.response_format,{type:"json_object"});
    assert.equal(payload.tools,undefined);
    return {ok:true,json:async()=>({choices:[{message:{role:"assistant",
      content:JSON.stringify({summary:"CPU 使用率异常",analysis:"Evidence E1 指示 CPU 高负载，仍需确认热点线程。",
        recommendations:["只读检查 CPU 使用率和调度延迟"]})}}]})};
  };
  try{
    const report=await createAgent.analyzeIncident(rpcMock({}),incident(6));
    assert.equal(report.ai_status,"ready");
    assert.equal(report.summary,"CPU 使用率异常");
    assert.equal(report.evidence[0].id,"E1");
    assert.equal(report.root_causes[0].confidence,.8);
    assert.equal(requests,1);
  }finally{
    global.fetch=oldFetch;
    if(oldKey===undefined)delete process.env.DEEPSEEK_API_KEY;
    else process.env.DEEPSEEK_API_KEY=oldKey;
    if(oldModel===undefined)delete process.env.DEEPSEEK_MODEL;
    else process.env.DEEPSEEK_MODEL=oldModel;
  }
});
test("DeepSeek model selection rejects providers outside the supported models",async()=>{
  const oldKey=process.env.DEEPSEEK_API_KEY,oldModel=process.env.DEEPSEEK_MODEL;
  process.env.DEEPSEEK_API_KEY="fake-key";
  process.env.DEEPSEEK_MODEL="gpt-4.1-mini";
  const res={statusCode:200,status(n){this.statusCode=n;return this;},json(v){this.body=v;return this;}};
  try {
    await createAgent(async()=>({}))({body:{server_name:"host-A",question:"查询性能"}},res);
    assert.equal(res.statusCode,502);
    assert.match(res.body.error,/DEEPSEEK_MODEL/);
  }finally {
    if(oldKey===undefined)delete process.env.DEEPSEEK_API_KEY;
    else process.env.DEEPSEEK_API_KEY=oldKey;
    if(oldModel===undefined)delete process.env.DEEPSEEK_MODEL;
    else process.env.DEEPSEEK_MODEL=oldModel;
  }
});
