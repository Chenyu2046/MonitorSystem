// Read-only incident investigation and proactive alerts. Matches the existing Dark Glass UI.
const KernDiagnosisCenter = ({servers, selectedServer, onServerChange, focusedIncident, onAsk}) => {
    const {Box, Paper, Typography, Button, Chip, Select, MenuItem, CircularProgress} = MaterialUI;
    const [hours,setHours] = React.useState(24);
    const [records,setRecords] = React.useState([]);
    const [selectedId,setSelectedId] = React.useState("");
    const [detail,setDetail] = React.useState(null);
    const [report,setReport] = React.useState(null);
    const [loading,setLoading] = React.useState(false);
    const [detailLoading,setDetailLoading] = React.useState(false);
    const [analyzing,setAnalyzing] = React.useState(false);
    const [error,setError] = React.useState("");
    const [refresh,setRefresh] = React.useState(0);
    const host = selectedServer || "";
    React.useEffect(() => {
        if (focusedIncident?.id) setSelectedId(String(focusedIncident.id));
    }, [focusedIncident]);
    React.useEffect(() => {
        let active = true;
        setLoading(true);
        setError("");
        axios.get("/api/incidents",{params:{server_name:host,hours,page:1,page_size:50}})
            .then(response => {
                if(!active)return;
                const list=response.data.incidents || [];
                setRecords(list);
                setSelectedId(previous => {
                    return list.some(item=>String(item.id)===previous) || (focusedIncident && String(focusedIncident.id)===previous)
                        ? previous :
                        (list[0] ? String(list[0].id) : "");
                });
            })
            .catch(err => {if(active)setError(err.response?.data?.error || err.message || "故障列表读取失败");})
            .finally(() => {if(active)setLoading(false);});
        return () => {active=false;};
    },[host,hours,refresh]);
    React.useEffect(() => {
        if(!selectedId){setDetail(null);setReport(null);return;}
        let active=true;
        setDetailLoading(true);
        setDetail(null);setReport(null);
        axios.get("/api/incidents/"+encodeURIComponent(selectedId),{params:{server_name:host}})
            .then(response => {if(active){setDetail(response.data.incident);setReport(response.data.report);}})
            .catch(err=>{if(active)setError(err.response?.data?.error || "故障详情读取失败");})
            .finally(()=>{if(active)setDetailLoading(false);});
        return()=>{active=false;};
    },[selectedId,host,refresh]);
    React.useEffect(() => {
        const socket=io();
        socket.on("diagnosis_alert",()=>setRefresh(v=>v+1));
        socket.on("diagnosis_updated",()=>setRefresh(v=>v+1));
        return ()=>socket.disconnect();
    },[]);

    const analyze=async()=>{
        if(!detail||analyzing)return;
        setAnalyzing(true);
        try{
            const response=await axios.post("/api/incidents/"+encodeURIComponent(String(detail.id))+"/analyze",
                {server_name:detail.server_name, refresh:report?.ai_status==="ready"});
            setReport(response.data.report);
        }catch(err){setError(err.response?.data?.error || "智能诊断失败");}
        finally{setAnalyzing(false);}
    };
    const date=ms=>ms ? new Date(Number(ms)).toLocaleString("zh-CN",{hour12:false}) : "—";
    return (
        <Box className="diagnosis-page">
            <Box className="diagnosis-header">
                <Box>
                    <Typography variant="h6">智能诊断</Typography>
                    <Typography variant="body2" color="text.secondary">事件、诊断证据与 AI 排查建议</Typography>
                </Box>
                <Box className="diagnosis-filters">
                    <Select size="small" value={host} displayEmpty onChange={e=>onServerChange(e.target.value)}
                        inputProps={{"aria-label":"诊断节点"}}>
                        <MenuItem value="">全部节点</MenuItem>
                        {(servers||[]).map(server=><MenuItem key={server.server_name}
                            value={server.server_name}>{server.server_name}</MenuItem>)}
                    </Select>
                    <Select size="small" value={hours} onChange={e=>setHours(Number(e.target.value))}
                        inputProps={{"aria-label":"诊断时间范围"}}>
                        <MenuItem value={1}>1 小时</MenuItem><MenuItem value={6}>6 小时</MenuItem>
                        <MenuItem value={24}>24 小时</MenuItem><MenuItem value={168}>7 天</MenuItem>
                    </Select>
                    <Button variant="outlined" size="small" onClick={()=>setRefresh(v=>v+1)}>刷新</Button>
                </Box>
            </Box>
            {error && <Typography role="alert" className="diagnosis-error">{error}</Typography>}
            <Box className="diagnosis-layout">
                <Paper className="diagnosis-events">
                    <Typography className="diagnosis-section-title">故障事件 {loading ? "· 读取中" : "· "+records.length}</Typography>
                    <Box className="diagnosis-event-list">
                        {!records.length && !loading && <Typography className="diagnosis-empty">
                            此时间段没有已记录的 Incident。没有事件不等于不存在指标波动。
                        </Typography>}
                        {records.map(record=>(
                            <Box key={String(record.id)} role="button" tabIndex={0}
                                className={"diagnosis-event "+(String(record.id)===selectedId?"is-selected":"")}
                                onClick={()=>{setError("");setSelectedId(String(record.id));}}
                                onKeyDown={e=>{if(e.key==="Enter"||e.key===" "){e.preventDefault();setSelectedId(String(record.id));}}}>
                                <Box className="diagnosis-event-head">
                                    <Typography className="diagnosis-event-host">{record.server_name}</Typography>
                                    <span className={"diagnosis-severity level-"+String(record.severity).toLowerCase()}>{record.severity || "UNKNOWN"}</span>
                                </Box>
                                <Typography className="diagnosis-event-date">{date(record.start_time_ms)}</Typography>
                                <Typography className="diagnosis-event-cause">
                                    {(record.root_causes||[]).map(r=>r.type).filter(Boolean).join(" · ") || "暂无候选根因"}
                                </Typography>
                            </Box>
                        ))}
                    </Box>
                </Paper>
                <Paper className="diagnosis-report">
                    {!detail && <Box className="diagnosis-no-selection">
                        {detailLoading ? <CircularProgress size={20}/> : "从左侧选择故障事件，查看指标与诊断证据。"}
                    </Box>}
                    {detail && (
                        <>
                            <Box className="diagnosis-report-header">
                                <Box>
                                    <Typography variant="subtitle1">Incident #{String(detail.id)}</Typography>
                                    <Typography variant="caption" color="text.secondary">
                                        {detail.server_name} · {date(detail.start_time_ms)} · {detail.active?"进行中":"已结束"}
                                    </Typography>
                                </Box>
                                <Button variant="outlined" size="small" onClick={analyze} disabled={analyzing}>
                                    {analyzing?"分析中…":report?.ai_status==="ready"?"重新分析":"AI 分析"}
                                </Button>
                            </Box>
                            <Box className="diagnosis-report-scroll">
                                <Typography className="diagnosis-section-title">规则候选根因</Typography>
                                {(detail.root_causes||[]).length ?
                                    (detail.root_causes||[]).map((cause,index)=><Box key={index} className="diagnosis-cause">
                                        <Typography>{cause.type}</Typography>
                                        <Typography variant="caption" color="text.secondary">
                                            规则置信度 {Number(cause.confidence||0).toFixed(2)} · 证据 {cause.evidence_ids?.join(", ") || "无"}
                                        </Typography>
                                        <Typography variant="body2">{cause.summary}</Typography>
                                    </Box>) :
                                    <Typography className="diagnosis-muted">暂无规则候选根因。</Typography>}
                                <Typography className="diagnosis-section-title">原始证据</Typography>
                                {(detail.evidence||[]).length ?
                                    (detail.evidence||[]).slice(0,40).map(e=><Box key={e.id} className="diagnosis-evidence">
                                        <Box className="diagnosis-evidence-top">
                                            <strong>{e.type}</strong><span>{e.id}</span>
                                        </Box>
                                        <Typography variant="body2">{e.source} · {e.target} · {e.value} {e.unit}</Typography>
                                        {e.detail && <Typography variant="caption" color="text.secondary">{e.detail}</Typography>}
                                    </Box>) :
                                    <Typography className="diagnosis-muted">暂无原始诊断证据，不能确认具体根因。</Typography>}
                                <Typography className="diagnosis-section-title">AI 诊断</Typography>
                                {!report && <Typography className="diagnosis-muted">尚未生成 AI 报告，点击右上角「AI 分析」。</Typography>}
                                {report && report.ai_status!=="ready" && <Typography className="diagnosis-muted">
                                    {report.ai_status==="unavailable"?"未配置 OPENAI_API_KEY；仍可查看规则与原始证据。":
                                        "AI 分析失败："+(report.error||"可重新尝试")}
                                </Typography>}
                                {report?.ai_status==="ready" && <>
                                    <Typography className="diagnosis-ai-summary">{report.summary}</Typography>
                                    <Typography variant="body2" className="diagnosis-ai-analysis">{report.analysis}</Typography>
                                    <Typography className="diagnosis-section-title">排查建议</Typography>
                                    {(report.recommendations||[]).map((text,i)=>
                                        <Typography key={i} className="diagnosis-recommendation">{i+1}. {text}</Typography>)}
                                    <Typography variant="caption" color="text.secondary">
                                        AI 内容为辅助解释；上方规则候选与 Evidence 来自 Manager。
                                    </Typography>
                                </>}
                            </Box>
                            <Box className="diagnosis-report-footer">
                                <Button size="small" variant="outlined" onClick={()=>onAsk?.(detail)}>
                                    继续追问此事件
                                </Button>
                            </Box>
                        </>
                    )}
                </Paper>
            </Box>
        </Box>
    );
};

const KernAlertBell = ({onOpen}) => {
    const {Box, IconButton, Badge, Popover, Typography, Button, Snackbar} = MaterialUI;
    const [anchor,setAnchor]=React.useState(null);
    const [items,setItems]=React.useState([]);
    const [unread,setUnread]=React.useState(0);
    const [toast,setToast]=React.useState("");
    const load=React.useCallback(async()=>{
        try{
            const response=await axios.get("/api/alerts");
            setItems(response.data.alerts||[]);
            setUnread(response.data.unread_count||0);
        }catch(_){}
    },[]);
    React.useEffect(()=>{
        load();
        const socket=io();
        socket.on("diagnosis_alert",record=>{
            setToast("新故障："+record.server_name+" · "+record.severity);
            load();
        });
        socket.on("diagnosis_updated",load);
        const timer=setInterval(load,30000);
        return()=>{socket.disconnect();clearInterval(timer);};
    },[load]);
    const openIncident=async record=>{
        if(!record.acknowledged){
            try{await axios.post("/api/alerts/ack",{key:record.key});}catch(_){}
        }
        setAnchor(null);
        load();
        onOpen(record);
    };
    return <>
        <IconButton size="small" aria-label={"告警通知，未读 "+unread} onClick={e=>setAnchor(e.currentTarget)}
            className="alert-bell">
            <Badge color="error" badgeContent={unread} max={99}>
                <span className="material-icons" aria-hidden="true">notifications_none</span>
            </Badge>
        </IconButton>
        <Popover anchorEl={anchor} open={!!anchor} onClose={()=>setAnchor(null)}
            anchorOrigin={{vertical:"bottom",horizontal:"right"}}
            transformOrigin={{vertical:"top",horizontal:"right"}}
            PaperProps={{className:"alerts-popover"}}>
            <Typography className="alerts-title">告警通知 · {unread} 未读</Typography>
            {items.length===0 ? <Typography className="alerts-empty">当前没有活跃告警记录。</Typography> :
                items.slice(0,12).map(record=><Box key={record.key}
                    className={"alerts-row "+(!record.acknowledged?"is-unread":"")}>
                    <Box className="alerts-row-info">
                        <Typography variant="body2">{record.server_name} · {record.severity}</Typography>
                        <Typography variant="caption" color="text.secondary">
                            {record.ai_status==="ready"?"AI 已分析":record.ai_status==="analyzing"?"分析中":
                                record.ai_status==="unavailable"?"仅规则诊断":"待分析"} · {record.active?"进行中":"已结束"}
                        </Typography>
                    </Box>
                    <Button size="small" onClick={()=>openIncident(record)}>查看</Button>
                </Box>)}
        </Popover>
        <Snackbar open={!!toast} onClose={()=>setToast("")} autoHideDuration={4500}
            message={toast} action={<Button color="inherit" size="small" onClick={()=>{setToast("");setAnchor(document.querySelector(".alert-bell"));}}>查看</Button>} />
    </>;
};
window.KernDiagnosisCenter=KernDiagnosisCenter;
window.KernAlertBell=KernAlertBell;
