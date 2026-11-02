// Isolated AI drawer: no new frontend build system required.
const AgentPanel = ({ serverName, servers, onServerChange }) => {
    const [open, setOpen] = React.useState(false);
    const [question, setQuestion] = React.useState("");
    const [messages, setMessages] = React.useState([]);
    const [loading, setLoading] = React.useState(false);
    const { Drawer, Box, Button, Typography, Select, MenuItem, TextField, CircularProgress, Paper } = MaterialUI;

    const send = async () => {
        const content = question.trim();
        if (!content || !serverName || loading) return;
        setMessages(prev => [...prev, { role: "user", text: content }]);
        setQuestion("");
        setLoading(true);
        try {
            const response = await axios.post("/api/agent", { question: content, server_name: serverName });
            setMessages(prev => [...prev, { role: "assistant", text: response.data.answer }]);
        } catch (err) {
            setMessages(prev => [...prev, { role: "error", text: err.response?.data?.error || err.message || "分析失败" }]);
        } finally {
            setLoading(false);
        }
    };

    return (
        <>
            <Button className="agent-launch" variant="contained" onClick={() => setOpen(true)}
                sx={{position: "fixed", bottom: 24, right: 24, zIndex: 1200, borderRadius: 5, px: 3, boxShadow: 5}}>
                AI 分析
            </Button>
            <Drawer className="agent-drawer-root" anchor="right" open={open} onClose={() => setOpen(false)} PaperProps={{ className: "agent-drawer" }}>
                <Box className="agent-panel" sx={{width: {xs: "100vw", sm: 420}, height: "100%", bgcolor: "#0b0c0e", color: "white",
                    display: "flex", flexDirection: "column", p: 2, gap: 2}}>
                    <Box display="flex" justifyContent="space-between" alignItems="center">
                        <Typography variant="h6">KernScope · AI 分析</Typography>
                        <Button onClick={() => setOpen(false)}>关闭</Button>
                    </Box>
                    <Select value={serverName || ""} onChange={e => {onServerChange(e.target.value); setMessages([]);}}
                        size="small" displayEmpty>
                        {!serverName && <MenuItem value="" disabled>请选择主机</MenuItem>}
                        {(servers || []).map(s => <MenuItem key={s.server_name} value={s.server_name}>{s.server_name}</MenuItem>)}
                    </Select>
                    <Typography variant="caption" color="text.secondary">
                        只读分析 · 基于 KernScope 指标、诊断事件与证据，不执行修复操作。
                    </Typography>
                    <Box sx={{flex: 1, overflowY: "auto", display: "flex", flexDirection: "column", gap: 1}}>
                        {messages.length === 0 && <Typography color="text.secondary" sx={{p: 2}}>
                            例如：这台主机最近 30 分钟是否异常？有没有可以支持的根因证据？
                        </Typography>}
                        {messages.map((m, i) => (
                            <Paper key={i} sx={{p: 2, bgcolor: m.role === "user" ? "#292b30" : "#1e2023",
                                whiteSpace: "pre-wrap", overflowWrap: "anywhere"}}>
                                <Typography variant="caption" color="text.secondary">
                                    {m.role === "user" ? "提问" : m.role === "error" ? "错误" : "AI 分析"}
                                </Typography>
                                <Typography variant="body2">{m.text}</Typography>
                            </Paper>
                        ))}
                        {loading && <Box display="flex" gap={1} alignItems="center">
                            <CircularProgress size={18} /> <Typography variant="body2">正在查询监控证据...</Typography>
                        </Box>}
                    </Box>
                    <TextField multiline minRows={2} maxRows={4} value={question}
                        onChange={e => setQuestion(e.target.value)}
                        placeholder="输入性能诊断问题" inputProps={{maxLength: 1000}} />
                    <Button disabled={!serverName || !question.trim() || loading} onClick={send}
                        variant="contained">分析选中主机</Button>
                </Box>
            </Drawer>
        </>
    );
};

window.AgentPanel = AgentPanel;
