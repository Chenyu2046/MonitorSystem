// Modified for KernScope: score semantics, CPU fallback and AI sidebar.
// Upstream: superlxh02/linux_monitor_system (Apache-2.0).
// Imports from global scope (CDN)
const {
    createTheme, ThemeProvider, CssBaseline, AppBar, Toolbar, Typography, Container, Grid, Paper, Box,
    Card, CardContent, Chip, Table, TableBody, TableCell, TableContainer, TableHead, TableRow,
    IconButton, LinearProgress, Button, Dialog, DialogTitle, DialogContent, DialogActions,
    List, ListItem, ListItemButton, ListItemIcon, ListItemText, Divider, Drawer, Checkbox, FormControlLabel, FormGroup,
    MenuItem, Select, TextField, InputAdornment
} = MaterialUI;

const {
    LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, Legend, ResponsiveContainer, AreaChart, Area, ReferenceLine
} = Recharts;

// --- Theme ---
const theme = createTheme({
    palette: {
        mode: "dark",
        primary: { main: "#e0e1e5", light: "#f4f4f6", dark: "#b7b9c0" },
        secondary: { main: "#979ca8" },
        background: { default: "#131417", paper: "#191b1e" },
        success: { main: "#70bd91" },
        warning: { main: "#d7b56d" },
        error: { main: "#d57474" },
        text: { primary: "#e9eaed", secondary: "#a9acb3" },
        divider: "rgba(255,255,255,.055)"
    },
    components: {
        MuiCard: { styleOverrides: { root: { backgroundImage: "none", backgroundColor: "#191b1e", border: "1px solid rgba(255,255,255,.032)", borderRadius: 11, boxShadow: "none" } } },
        MuiPaper: { styleOverrides: { root: { backgroundImage: "none", backgroundColor: "#191b1e", border: "1px solid rgba(255,255,255,.032)", borderRadius: 10 } } },
        MuiButton: { styleOverrides: { root: { textTransform: "none", borderRadius: 8, fontWeight: 600 } } },
        MuiOutlinedInput: { styleOverrides: { root: { borderRadius: 8 } } }
    }
});

const UNIT_MAP = {
    score: "分",
    cpu_percent: "%",
    usr_percent: "%",
    system_percent: "%",
    nice_percent: "%",
    idle_percent: "%",
    io_wait_percent: "%",
    irq_percent: "%",
    soft_irq_percent: "%",
    mem_used_percent: "%",
    disk_util_percent: "%",
    mem_total: "GB",
    mem_free: "GB",
    mem_avail: "GB",
    rcv_rate: "kB/s",
    send_rate: "kB/s",
    rcv_bytes_rate: "B/s",
    snd_bytes_rate: "B/s",
    rcv_packets_rate: "包/s",
    snd_packets_rate: "包/s",
    err_in: "次",
    err_out: "次",
    drop_in: "次",
    drop_out: "次",
    read_bytes_per_sec: "B/s",
    write_bytes_per_sec: "B/s",
    read_iops: "次/s",
    write_iops: "次/s",
    avg_read_latency_ms: "ms",
    avg_write_latency_ms: "ms",
    util_percent: "%",
    total: "GB",
    free: "GB",
    avail: "GB",
    buffers: "GB",
    cached: "GB",
    swap_cached: "GB",
    active: "GB",
    inactive: "GB",
    active_anon: "GB",
    inactive_anon: "GB",
    active_file: "GB",
    inactive_file: "GB",
    dirty: "GB",
    writeback: "GB",
    anon_pages: "GB",
    mapped: "GB",
    hi: "次",
    timer: "次",
    net_tx: "次",
    net_rx: "次",
    block: "次",
    irq_poll: "次",
    tasklet: "次",
    sched: "次",
    hrtimer: "次",
    rcu: "次"
};

const FIELD_LABEL_MAP = {
    server_name: "节点名称",
    timestamp: "采集时间",
    score: "综合评分",
    status: "在线状态",
    cpu_percent: "CPU总使用率",
    usr_percent: "用户态占比",
    system_percent: "内核态占比",
    nice_percent: "Nice占比",
    idle_percent: "空闲占比",
    io_wait_percent: "IO等待占比",
    irq_percent: "硬中断占比",
    soft_irq_percent: "软中断占比",
    load_avg_1: "1分钟负载",
    load_avg_3: "3分钟负载",
    load_avg_15: "15分钟负载",
    mem_used_percent: "内存使用率",
    mem_total: "总内存",
    mem_free: "空闲内存",
    mem_avail: "可用内存",
    disk_util_percent: "磁盘利用率",
    send_rate: "发送速率",
    rcv_rate: "接收速率",
    cpu_percent_rate: "CPU变化率",
    mem_used_percent_rate: "内存变化率",
    disk_util_percent_rate: "磁盘变化率",
    load_avg_1_rate: "负载变化率",
    send_rate_rate: "发送速率变化",
    rcv_rate_rate: "接收速率变化",
    anomaly_type: "异常类型",
    severity: "严重级别",
    value: "异常值",
    threshold: "阈值",
    metric_name: "异常指标",
    net_name: "网卡",
    err_in: "接收错误数",
    err_out: "发送错误数",
    drop_in: "接收丢包数",
    drop_out: "发送丢包数",
    rcv_bytes_rate: "接收字节速率",
    snd_bytes_rate: "发送字节速率",
    rcv_packets_rate: "接收包速率",
    snd_packets_rate: "发送包速率",
    disk_name: "磁盘设备",
    read_bytes_per_sec: "读吞吐",
    write_bytes_per_sec: "写吞吐",
    read_iops: "读IOPS",
    write_iops: "写IOPS",
    avg_read_latency_ms: "平均读时延",
    avg_write_latency_ms: "平均写时延",
    total: "总内存",
    free: "空闲内存",
    avail: "可用内存",
    buffers: "Buffers",
    cached: "缓存",
    active: "活跃内存",
    inactive: "非活跃内存",
    dirty: "脏页",
    cpu_name: "CPU核心",
    hi: "HI软中断",
    timer: "TIMER软中断",
    net_tx: "NET_TX软中断",
    net_rx: "NET_RX软中断",
    block: "BLOCK软中断",
    sched: "SCHED软中断"
};

const getUnitByField = (field) => UNIT_MAP[field] || "";
const getFieldLabel = (field) => FIELD_LABEL_MAP[field] || field;

const getMetricUnitByDataKey = (dataKey = "") => {
    if (dataKey.endsWith("_cpu") || dataKey.endsWith("_mem") || dataKey.endsWith("_disk")) return "%";
    if (dataKey.endsWith("_net")) return "MB/s";
    return "";
};

const SCORING_PROFILES = [
    { value: "RESOURCE_SCORE", label: "资源评分" },
    { value: "HEALTH_SCORE", label: "健康评分" }
];

const applyScoringProfileToOverview = (overviewData, profile) => {
    const servers = (overviewData?.servers || []).map(s => ({
        ...s,
        score: profile === "HEALTH_SCORE"
            ? (s.health_valid ? Number(s.health_score) : null)
            : Number(s.resource_score ?? s.score)
    }));
    const ranked = servers.filter(s => Number.isFinite(s.score))
        .sort((a, b) => b.score - a.score);
    const avg = ranked.length
        ? ranked.reduce((sum, s) => sum + s.score, 0) / ranked.length : null;
    const online = servers.filter(s =>
        s.status === "0" || s.status === "ONLINE" || s.status === 0).length;
    return {
        ...overviewData,
        servers,
        scoring_profile: profile,
        cluster_stats: {
            ...(overviewData?.cluster_stats || {}),
            total_servers: servers.length,
            online_servers: online,
            offline_servers: servers.length - online,
            avg_score: avg,
            max_score: ranked.length ? ranked[0].score : null,
            min_score: ranked.length ? ranked[ranked.length - 1].score : null,
            best_server: ranked[0]?.server_name || "",
            worst_server: ranked[ranked.length - 1]?.server_name || ""
        }
    };
};

const toHistoryPoint = (server, time) => {
    const timestampMs = Number(server.timestamp?.seconds || server.last_update?.seconds || 0) * 1000 || Date.now();
    return {
        time: time || new Date(timestampMs).toLocaleTimeString("zh-CN", { hour12: false }),
        timestampMs,
        cpu: server.cpu_percent,
        mem: server.mem_used_percent,
        disk: server.disk_util_percent,
        net_in: server.rcv_rate,
        net_out: server.send_rate
    };
};

// --- Dashboard Components ---

const TopStats = ({ clusterStats, servers = [], scoringProfile }) => {
    const averageOf = (field) => {
        const values = servers.map((server) => Number(server[field])).filter(Number.isFinite);
        return values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : null;
    };
    const cpuAverage = averageOf("cpu_percent");
    const memoryAverage = averageOf("mem_used_percent");
    const metrics = [
        {
            label: scoringProfile === "HEALTH_SCORE" ? "集群健康评分" : "集群资源评分",
            icon: "query_stats",
            value: Number.isFinite(clusterStats.avg_score) ? clusterStats.avg_score.toFixed(1) : "—",
            unit: "/ 100",
            note: scoringProfile === "HEALTH_SCORE" ? "健康模型评分" : "资源余量评分",
            tone: "neutral"
        },
        {
            label: "在线节点",
            icon: "dns",
            value: `${clusterStats.online_servers || 0}`,
            unit: `/ ${clusterStats.total_servers || 0}`,
            note: `${clusterStats.offline_servers || 0} 台离线`,
            tone: "neutral"
        },
        {
            label: "平均 CPU 使用率",
            icon: "memory",
            value: cpuAverage === null ? "—" : cpuAverage.toFixed(1),
            unit: "%",
            note: "当前节点采样均值",
            tone: "neutral"
        },
        {
            label: "平均内存使用率",
            icon: "developer_board",
            value: memoryAverage === null ? "—" : memoryAverage.toFixed(1),
            unit: "%",
            note: "当前节点采样均值",
            tone: "neutral"
        }
    ];

    return (
        <Grid container spacing={1.5} className="metric-grid">
            {metrics.map((metric) => (
                <Grid item xs={6} lg={3} key={metric.label}>
                    <Card className={`metric-tile metric-tile-${metric.tone}`}>
                        <Box className="metric-heading">
                            <Typography className="metric-label">{metric.label}</Typography>
                            
                        </Box>
                        <Box className="metric-value-row">
                            <Typography className="metric-value">{metric.value}</Typography>
                            <Typography className="metric-unit">{metric.unit}</Typography>
                        </Box>
                        <Typography className="metric-note">{metric.note}</Typography>
                    </Card>
                </Grid>
            ))}
        </Grid>
    );
};

const ChartSection = ({ servers, nodeHistories, initialNode, initialMetric, onBack, fixedMode }) => {
    // If fixedMode is true, user cannot change selection
    
    const [selectedMetrics, setSelectedMetrics] = React.useState(() => ({
        cpu: initialMetric ? initialMetric === "cpu" : true,
        mem: initialMetric ? initialMetric === "mem" : true,
        disk: initialMetric ? initialMetric === "disk" : true,
        net: initialMetric ? initialMetric === "net" : false
    }));

    const [chartMode, setChartMode] = React.useState("focus");
    const [focusMetric, setFocusMetric] = React.useState(initialMetric || "cpu");
    const [scaleMode, setScaleMode] = React.useState("adaptive");

    const [selectedNodes, setSelectedNodes] = React.useState(() => {
        const initial = {};
        if (servers.length > 0) {
            if (initialNode) {
                servers.forEach(s => initial[s.server_name] = s.server_name === initialNode);
            } else {
                servers.forEach(s => initial[s.server_name] = true);
            }
        }
        return initial;
    });

    const initRef = React.useRef(false);

    React.useEffect(() => {
        if (!initRef.current && servers.length > 0 && Object.keys(selectedNodes).length === 0) {
            const initial = {};
            if (initialNode) {
                 servers.forEach(s => initial[s.server_name] = s.server_name === initialNode);
            } else {
                 servers.forEach(s => initial[s.server_name] = true);
            }
            setSelectedNodes(initial);
            initRef.current = true;
        }
    }, [servers, initialNode]);

    const handleMetricChange = (m) => {
        if (fixedMode) return;
        setSelectedMetrics(prev => ({ ...prev, [m]: !prev[m] }));
    };
    const handleNodeChange = (n) => {
         if (fixedMode) return;
         setSelectedNodes(prev => ({ ...prev, [n]: !prev[n] }));
    };
    const selectAllNodes = (checked) => {
        if (fixedMode) return;
        const next = {};
        servers.forEach(s => next[s.server_name] = checked);
        setSelectedNodes(next);
    };

    const metricNames = { cpu: "CPU", mem: "内存", disk: "磁盘", net: "网络" };
    const metricColors = { cpu: "#d7b56d", mem: "#79a7cf", disk: "#a391c9", net: "#cc8fa4" };
    const visibleMetrics = chartMode === "focus"
        ? { cpu: focusMetric === "cpu", mem: focusMetric === "mem", disk: focusMetric === "disk", net: focusMetric === "net" }
        : selectedMetrics;

    // Align samples by actual timestamp, not array position. Do not convert missing values to zero.
    const chartData = React.useMemo(() => {
        const timeline = new Map();
        servers.filter(server => selectedNodes[server.server_name]).forEach(server => {
            (nodeHistories[server.server_name] || []).forEach(point => {
                if (!Number.isFinite(point.timestampMs) || !point.timestampMs) return;
                const row = timeline.get(point.timestampMs) || { time: point.time, timestampMs: point.timestampMs };
                for (const metric of ["cpu", "mem", "disk"]) {
                    if (!visibleMetrics[metric]) continue;
                    const value = Number(point[metric]);
                    row[server.server_name + "_" + metric] =
                        point[metric] == null || !Number.isFinite(value) ? null : value;
                }
                if (visibleMetrics.net) {
                    const rx = Number(point.net_in), tx = Number(point.net_out);
                    row[server.server_name + "_net"] =
                        point.net_in != null && point.net_out != null && Number.isFinite(rx) && Number.isFinite(tx)
                            ? (rx + tx) / 1024 : null;
                }
                timeline.set(point.timestampMs, row);
            });
        });
        return [...timeline.values()].sort((x, y) => x.timestampMs - y.timestampMs).slice(-120);
    }, [servers, nodeHistories, selectedNodes, chartMode, focusMetric, selectedMetrics]);

    const focusDomain = React.useMemo(() => {
        if (scaleMode === "standard" && focusMetric !== "net") return [0, 100];
        const chosenHosts = servers.filter(server => selectedNodes[server.server_name]);
        const values = chartData.flatMap(row => chosenHosts.map(server =>
            row[server.server_name + "_" + focusMetric]))
            .filter(v => typeof v === "number" && Number.isFinite(v));
        if (!values.length) return [0, focusMetric === "net" ? 1 : 5];
        const min = Math.min(...values), max = Math.max(...values), spread = max - min;
        if (focusMetric === "net") {
            const pad = Math.max(spread * 0.2, max * 0.08, 0.005);
            return [Math.max(0, min - pad), max + pad];
        }
        const pad = Math.max(spread * 0.25, 0.35);
        let low = Math.max(0, Math.floor((min - pad) * 10) / 10);
        let high = Math.min(100, Math.ceil((max + pad) * 10) / 10);
        if (high - low < 1) {
            low = Math.max(0, Math.floor((min - 0.5) * 10) / 10);
            high = Math.min(100, Math.max(low + 1, Math.ceil((max + 0.5) * 10) / 10));
        }
        return [low, high];
    }, [chartData, servers, selectedNodes, focusMetric, scaleMode]);

    const anySelectedHost = servers.some(server => selectedNodes[server.server_name]);
    const hasVisibleMetric = Object.values(visibleMetrics).some(Boolean);
    const showPercentAxis = chartMode === "focus" ? focusMetric !== "net"
        : visibleMetrics.cpu || visibleMetrics.mem || visibleMetrics.disk;
    const showNetAxis = chartMode === "focus" ? focusMetric === "net" : visibleMetrics.net;
    const comparePrimary = selectedMetrics[focusMetric] ? focusMetric :
        ["cpu", "mem", "disk", "net"].find(metric => selectedMetrics[metric]);
    const percentAxisPrecision = chartMode === "focus" && focusDomain[1] - focusDomain[0] < 5 ? 1 : 0;

    const colors = ["#d7b56d", "#79a7cf", "#a391c9", "#cc8fa4", "#8fb3bb", "#c0a079", "#a5a9b8", "#b5a5cc"];
    const tooltipFormatter = (value, name, item) => {
        const unit = getMetricUnitByDataKey(item?.dataKey || "");
        if (typeof value === "number") {
            const precision = Math.abs(value) < 0.01 ? 4 : Math.abs(value) < 1 ? 3 : 2;
            return [value.toFixed(precision) + (unit ? " " + unit : ""), name];
        }
        return [value, name];
    };

    return (
        <Card className="trend-card" sx={{ height: "100%", display: "flex", flexDirection: "column", p: 0, overflow: "hidden" }}>
             {/* Toolbar */}
            <Box sx={{ p: 2, borderBottom: "1px solid rgba(255,255,255,0.05)", bgcolor: "rgba(0,0,0,0.2)" }}>
                <Box display="flex" alignItems="center" mb={1}>
                    {onBack && (
                        <Button 
                            onClick={onBack} 
                            startIcon={<span className="material-icons" aria-hidden="true">arrow_back</span>}
                            sx={{ mr: 2, color: "white", borderColor: "rgba(255,255,255,0.3)" }} 
                            variant="outlined" 
                            size="small"
                        >
                            返回
                        </Button>
                    )}
                    <Typography variant="subtitle2" color="text.secondary">
                        {fixedMode ? `节点 ${initialNode} - ${initialMetric.toUpperCase()} 历史趋势` : "性能趋势"}
                    </Typography>
                </Box>

                {!fixedMode && (
                    <Box className="trend-toolbar">
                        <Box className="trend-focus-bar">
                            <Typography variant="caption" className="trend-control-label">主指标</Typography>
                            <Box className="trend-metric-tabs" role="group" aria-label="选择重点监控指标">
                                {["cpu", "mem", "disk", "net"].map(metric => (
                                    <Button key={metric} size="small"
                                        className={"trend-metric-button " + (chartMode === "focus" && focusMetric === metric ? "is-active" : "")}
                                        onClick={() => { setFocusMetric(metric); setChartMode("focus"); }}
                                        aria-pressed={chartMode === "focus" && focusMetric === metric}
                                        sx={{ "--metric-color": metricColors[metric] }}>
                                        {metricNames[metric]}
                                    </Button>
                                ))}
                            </Box>
                            <Button size="small" className={"trend-compare-button " + (chartMode === "compare" ? "is-active" : "")}
                                onClick={() => setChartMode(mode => mode === "compare" ? "focus" : "compare")}
                                aria-pressed={chartMode === "compare"}>
                                {chartMode === "compare" ? "返回聚焦" : "多指标对比"}
                            </Button>
                            {chartMode === "focus" && focusMetric !== "net" && (
                                <Box className="trend-scale-switch">
                                    <Typography variant="caption">Y 轴</Typography>
                                    <Select size="small" value={scaleMode} onChange={e => setScaleMode(e.target.value)}
                                        inputProps={{ "aria-label": "纵轴显示范围" }}>
                                        <MenuItem value="adaptive">自适应放大</MenuItem>
                                        <MenuItem value="standard">标准 0–100%</MenuItem>
                                    </Select>
                                </Box>
                            )}
                            {chartMode === "focus" && (scaleMode === "adaptive" || focusMetric === "net") && (
                                <Typography variant="caption" className="trend-scale-hint">Y 轴局部放大</Typography>
                            )}
                        </Box>
                        {chartMode === "compare" && (
                            <FormGroup row className="trend-compare-metrics">
                                {["cpu", "mem", "disk", "net"].map(metric => (
                                    <FormControlLabel key={metric}
                                        control={<Checkbox checked={selectedMetrics[metric]}
                                            onChange={() => handleMetricChange(metric)} size="small" />}
                                        label={metricNames[metric]} />
                                ))}
                            </FormGroup>
                        )}
                        <Box className="trend-node-bar">
                            <Typography variant="caption" className="trend-control-label">节点</Typography>
                            <Box className="trend-node-list">
                                {servers.map(server => (
                                    <Chip key={server.server_name}
                                        label={server.server_name}
                                        onClick={() => handleNodeChange(server.server_name)}
                                        color={selectedNodes[server.server_name] ? "primary" : "default"}
                                        variant={selectedNodes[server.server_name] ? "filled" : "outlined"}
                                        size="small"
                                        icon={selectedNodes[server.server_name] ? <span className="material-icons" aria-hidden="true">check</span> : undefined} />
                                ))}
                            </Box>
                            <Button size="small" onClick={() => selectAllNodes(true)}>全选</Button>
                            <Button size="small" onClick={() => selectAllNodes(false)}>清空</Button>
                        </Box>
                    </Box>
                )}
            </Box>

            {/* Chart Area */}
            <Box sx={{ flexGrow: 1, p: 2, minHeight: 300, bgcolor: "transparent" }}>
                {!anySelectedHost || !hasVisibleMetric || !chartData.length ? (
                    <Box className="chart-empty">
                        <Typography variant="body2" color="text.secondary">
                            {!anySelectedHost ? "请选择至少一个节点" :
                                !hasVisibleMetric ? "请选择至少一个监控指标" :
                                "正在等待有效采样数据"}
                        </Typography>
                    </Box>
                ) : <ResponsiveContainer width="100%" height="100%">
                    <LineChart data={chartData}>
                        <CartesianGrid vertical={false} strokeDasharray="3 6" stroke="#32343a" />
                        <XAxis dataKey="time" stroke="#878c94" tickLine={false} axisLine={false} minTickGap={48} />
                        {showPercentAxis && (
                            <YAxis yAxisId="percent" stroke="#878c94"
                                domain={chartMode === "focus" ? focusDomain : [0, 100]}
                                allowDataOverflow={chartMode === "focus"}
                                tickFormatter={value => Number(value).toFixed(percentAxisPrecision) + "%"} />
                        )}
                        {showNetAxis && (
                            <YAxis yAxisId="net" orientation={chartMode === "focus" ? "left" : "right"}
                                domain={chartMode === "focus" ? focusDomain : [0, "auto"]}
                                stroke="#878c94" allowDataOverflow={chartMode === "focus"}
                                tickFormatter={value => Number(value).toFixed(value < 1 ? 2 : 1)} />
                        )}
                        <Tooltip formatter={tooltipFormatter}
                            contentStyle={{ backgroundColor: "rgba(23,24,27,.97)", border: "1px solid rgba(255,255,255,.075)", borderRadius: 8 }} />
                        <Legend wrapperStyle={{ paddingTop: 10 }} />
                        {servers.filter(server => selectedNodes[server.server_name]).flatMap((server, i) => {
                            const multiHostFocus = chartMode === "focus" &&
                                servers.filter(s => selectedNodes[s.server_name]).length > 1;
                            return ["cpu", "mem", "disk", "net"].filter(metric => visibleMetrics[metric]).map(metric => (
                                <Line key={server.server_name + "_" + metric}
                                    type="monotone"
                                    dataKey={server.server_name + "_" + metric}
                                    yAxisId={metric === "net" ? "net" : "percent"}
                                    stroke={multiHostFocus ? colors[i % colors.length] : metricColors[metric]}
                                    strokeOpacity={chartMode === "focus" || metric === comparePrimary ? 0.98 : 0.28}
                                    name={server.server_name + " " + metricNames[metric] +
                                        (metric === "net" ? "(MB/s)" : "(%)")}
                                    connectNulls={false}
                                    dot={chartData.length === 1 ? { r: 3, strokeWidth: 1 } : false}
                                    strokeWidth={chartMode === "focus" || metric === comparePrimary ? 3 : 1.5}
                                    activeDot={{ r: 4 }}
                                    isAnimationActive={false} />
                            ));
                        })}
                    </LineChart>
                </ResponsiveContainer>}
            </Box>
        </Card>
    );
};

// --- Detail Page ---

const DetailView = ({ title, metric, servers, onNodeClick, onRefresh }) => {
    // Sort logic
    const [sort, setSort] = React.useState("desc");
    
    const sorted = React.useMemo(() => {
        return [...servers].sort((a, b) => {
            const valA = metric.getValue(a);
            const valB = metric.getValue(b);
            return sort === "asc" ? valA - valB : valB - valA;
        });
    }, [servers, metric, sort]);

    return (
        <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
            <Box display="flex" justifyContent="space-between" alignItems="center" mb={4}>
                <Typography variant="h4" fontWeight="bold">
                <span className="material-icons detail-title-icon" aria-hidden="true">{metric.icon}</span> {title} 监控详情
                </Typography>
                <Button variant="outlined" onClick={onRefresh} startIcon={<span className="material-icons" aria-hidden="true">refresh</span>}>刷新数据</Button>
            </Box>

            <TableContainer component={Paper} sx={{ mb: 4 }}>
                <Table>
                    <TableHead sx={{ bgcolor: "background.default" }}>
                         <TableRow>
                            <TableCell>#</TableCell>
                            <TableCell>节点名称 (点击查看图表)</TableCell>
                            <TableCell>状态</TableCell>
                            <TableCell align="right" onClick={() => setSort(s => s==="asc"?"desc":"asc")} style={{cursor:"pointer"}}>
                                当前数值{metric.unit ? ` (${metric.unit})` : ""} {sort==="asc"?"↑":"↓"}
                            </TableCell>
                            <TableCell>趋势可视化</TableCell>
                            <TableCell>其他相关指标</TableCell>
                        </TableRow>
                    </TableHead>
                    <TableBody>
                        {sorted.map((s, idx) => {
                            const val = metric.getValue(s);
                            const displayVal = metric.format ? metric.format(val) : val.toFixed(1);
                            
                            return (
                                <TableRow 
                                    key={s.server_name} 
                                    hover 
                                    onClick={() => onNodeClick(s.server_name)}
                                    sx={{ cursor: "pointer" }}
                                >
                                    <TableCell>{idx + 1}</TableCell>
                                    <TableCell component="th" scope="row" sx={{ fontWeight: "bold", textDecoration: "underline", color: "primary.main" }}>
                                        {s.server_name}
                                    </TableCell>
                                    <TableCell>
                                        <Chip 
                                            size="small" 
                                            label={s.status === "0" || s.status === "ONLINE" ? "在线" : "离线"} 
                                            color={s.status === "0" || s.status === "ONLINE" ? "success" : "default"} 
                                            variant="outlined"
                                        />
                                    </TableCell>
                                    <TableCell align="right" sx={{ fontSize: "1.1rem", fontFamily: "monospace", color: "primary.main" }}>
                                        {displayVal}
                                    </TableCell>
                                    <TableCell sx={{ width: "25%" }}>
                                        <LinearProgress 
                                            variant="determinate" 
                                            value={Math.min( metric.normalize ? metric.normalize(val) : val, 100)} 
                                            color={val > 80 ? "error" : val > 50 ? "warning" : "success"}
                                            sx={{ height: 8, borderRadius: 4 }}
                                        />
                                    </TableCell>
                                    <TableCell>
                                        <Box display="flex" gap={1}>
                                            <Chip size="small" label={`Load(1m): ${s.load_avg_1?.toFixed(2)}`} />
                                            {metric.key !== "mem_used_percent" && <Chip size="small" label={`Mem: ${s.mem_used_percent?.toFixed(0)}%`} />}
                                        </Box>
                                    </TableCell>
                                </TableRow>
                            );
                        })}
                    </TableBody>
                </Table>
            </TableContainer>
        </Box>
    );
};

const formatCellValue = (value, unit = "") => {
    if (value === null || value === undefined) return "-";
    if (typeof value === "number") {
        const formatted = Number.isInteger(value) ? String(value) : value.toFixed(3);
        return unit ? `${formatted} ${unit}` : formatted;
    }
    if (typeof value === "object") {
        if (value.seconds !== undefined) {
            return new Date(Number(value.seconds) * 1000).toLocaleString("zh-CN", { hour12: false });
        }
        return JSON.stringify(value);
    }
    return unit ? `${String(value)} ${unit}` : String(value);
};

const SignalDetails = ({ value }) => {
    let signals = value;
    if (typeof signals === "string") {
        try { signals = JSON.parse(signals); } catch (_) { return <span>信号数据格式异常</span>; }
    }
    if (!Array.isArray(signals) || !signals.length) return <span>暂无诊断信号</span>;
    return (
        <details className="signal-details">
            <summary>{signals.length} 项诊断信号 <span className="material-icons">expand_more</span></summary>
            <div className="signal-list">
                {signals.map((entry, i) => <div key={i} className="signal-row">
                    <span className="signal-name">{entry.metric || "未知指标"}</span>
                    <span>{Number.isFinite(Number(entry.value)) ? Number(entry.value).toFixed(2) : "—"}</span>
                    <span className={entry.anomalous ? "signal-alert" : "signal-normal"}>{entry.anomalous ? "异常" : "正常"}</span>
                </div>)}
            </div>
        </details>
    );
};

const QueryTableView = ({ title, subtitle, rows, loading, error, emptyText }) => {
    const columns = React.useMemo(() => {
        if (!rows || rows.length === 0) return [];
        return [...new Set(rows.flatMap(row => Object.keys(row || {})))];
    }, [rows]);

    return (
        <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
            {title && <Typography variant="h4" mb={1}>{title}</Typography>}
            {subtitle ? <Typography variant="body2" color="text.secondary" mb={2}>{subtitle}</Typography> : null}

            {loading && <LinearProgress sx={{ mb: 2 }} />}
            {error ? <Typography color="error" mb={2}>{error}</Typography> : null}

            {!loading && (!rows || rows.length === 0) ? (
                <Paper sx={{ p: 3 }}>
                    <Typography color="text.secondary">{emptyText || "暂无数据"}</Typography>
                </Paper>
            ) : (
                <TableContainer component={Paper}>
                    <Table size="small">
                        <TableHead sx={{ bgcolor: "background.default" }}>
                            <TableRow>
                                <TableCell>#</TableCell>
                                {columns.map((c) => (
                                    <TableCell key={c}>{getUnitByField(c) ? `${getFieldLabel(c)} (${getUnitByField(c)})` : getFieldLabel(c)}</TableCell>
                                ))}
                            </TableRow>
                        </TableHead>
                        <TableBody>
                            {rows.map((row, idx) => (
                                <TableRow key={idx} hover>
                                    <TableCell>{idx + 1}</TableCell>
                                    {columns.map((c) => (
                                        <TableCell key={`${idx}_${c}`} sx={{ fontFamily: c.includes("rate") || c.includes("percent") ? "monospace" : "inherit" }}>
                                            {c === "top_signals" ? <SignalDetails value={row[c]} /> : formatCellValue(row[c], getUnitByField(c))}
                                        </TableCell>
                                    ))}
                                </TableRow>
                            ))}
                        </TableBody>
                    </Table>
                </TableContainer>
            )}
        </Box>
    );
};


// Full-width score trend with node selection built into the chart header.
const ScoreTrendView = ({ servers, selectedServer, onServerChange, hours, onHoursChange, rows, loading, error, onRefresh }) => {
    const formatTick = (time) => {
        const date = new Date(Number(time));
        return hours >= 24
            ? date.toLocaleString("zh-CN", { month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", hour12: false })
            : date.toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit", hour12: false });
    };
    const points = React.useMemo(() => (rows || [])
        .map(record => {
            const seconds = Number(record.timestamp?.seconds);
            if (!Number.isFinite(seconds) || seconds <= 0) return null;
            const resource = Number(record.resource_score ?? record.score);
            const health = Number(record.health_score);
            return {
                timestampMs: seconds * 1000,
                resource: Number.isFinite(resource) ? resource : null,
                health: record.health_valid && Number.isFinite(health) ? health : null
            };
        })
        .filter(Boolean)
        .sort((a, b) => a.timestampMs - b.timestampMs), [rows]);

    return (
        <Box className="score-trend-page">
            <Card className="score-trend-main">
                <Box className="score-trend-header">
                    <Box className="score-trend-heading">
                        <Typography variant="h6">资源与健康评分</Typography>
                        <Typography variant="body2">0–100 分 · {hours} 小时历史趋势</Typography>
                    </Box>
                    <Box className="score-trend-controls">
                        <Box className="score-trend-field">
                            <Typography component="label" variant="caption">节点</Typography>
                            <Select size="small" value={selectedServer || ""} displayEmpty
                                onChange={event => onServerChange(event.target.value)}
                                inputProps={{ "aria-label": "选择监控节点" }}>
                                {!selectedServer && <MenuItem value="" disabled>请选择节点</MenuItem>}
                                {servers.map(server =>
                                    <MenuItem value={server.server_name} key={server.server_name}>
                                        {server.server_name}
                                    </MenuItem>
                                )}
                            </Select>
                        </Box>
                        <Box className="score-trend-field score-trend-range">
                            <Typography component="label" variant="caption">时间</Typography>
                            <Select size="small" value={hours} onChange={event => onHoursChange(Number(event.target.value))}
                                inputProps={{ "aria-label": "选择历史时间窗口" }}>
                                <MenuItem value={1}>1 小时</MenuItem>
                                <MenuItem value={6}>6 小时</MenuItem>
                                <MenuItem value={24}>24 小时</MenuItem>
                                <MenuItem value={72}>72 小时</MenuItem>
                            </Select>
                        </Box>
                        <Button variant="outlined" size="small" disabled={!selectedServer || loading}
                            onClick={onRefresh} className="score-trend-refresh">刷新</Button>
                    </Box>
                </Box>
                <Box className="score-trend-body">
                    {loading && <LinearProgress className="score-trend-progress" />}
                    {!selectedServer ? (
                        <Box className="score-trend-empty">选择节点后查看历史评分。</Box>
                    ) : loading ? (
                        <Box className="score-trend-empty">正在读取所选节点的历史评分…</Box>
                    ) : error ? (
                        <Box className="score-trend-empty">趋势查询失败：{error}</Box>
                    ) : !points.length ? (
                        <Box className="score-trend-empty">当前时间窗口没有历史评分数据。</Box>
                    ) : (
                        <Box className="score-trend-canvas">
                            <ResponsiveContainer width="100%" height="100%">
                                <LineChart data={points} margin={{ top: 16, right: 28, left: 2, bottom: 14 }}>
                                    <CartesianGrid vertical={false} stroke="rgba(255,255,255,.06)" strokeDasharray="3 6" />
                                    <XAxis type="number" dataKey="timestampMs" domain={["dataMin", "dataMax"]}
                                        tickFormatter={formatTick} tickLine={false} axisLine={false}
                                        stroke="#9397a1" fontSize={12} minTickGap={45} />
                                    <YAxis domain={[0, 100]} ticks={[0, 25, 50, 75, 100]}
                                        tickLine={false} axisLine={false} stroke="#9397a1"
                                        fontSize={12} width={38} />
                                    <Tooltip labelFormatter={formatTick}
                                        formatter={(value, name) => [Number(value).toFixed(1) + " 分", name]}
                                        contentStyle={{ backgroundColor: "#202227", border: "1px solid rgba(255,255,255,.08)", borderRadius: 9 }} />
                                    <Legend verticalAlign="bottom" height={40} iconType="plainline" wrapperStyle={{ fontSize: 13, paddingTop: 12 }} />
                                    <Line name="资源评分" type="linear" dataKey="resource" connectNulls={false}
                                        stroke="#d7b56d" strokeWidth={3} dot={false}
                                        activeDot={{ r: 5, strokeWidth: 0 }} isAnimationActive={false} />
                                    <Line name="健康评分" type="linear" dataKey="health" connectNulls={false}
                                        stroke="#79a7cf" strokeWidth={3} dot={false}
                                        activeDot={{ r: 5, strokeWidth: 0 }} isAnimationActive={false} />
                                </LineChart>
                            </ResponsiveContainer>
                        </Box>
                    )}
                </Box>
            </Card>
        </Box>
    );
};

const QueryControlBar = ({
    servers,
    selectedServer,
    onServerChange,
    scoringProfile,
    onScoringProfileChange,
    hours,
    onHoursChange,
    intervalSeconds,
    onIntervalChange,
    showInterval,
    showAnomalyThresholds,
    anomalyThresholds,
    onAnomalyThresholdChange,
    onRefresh
}) => (
    <Paper sx={{ p: 2, mb: 2, display: "flex", flexWrap: "wrap", gap: 2, alignItems: "center" }}>
        <Typography variant="body2" color="text.secondary">节点</Typography>
        <Select
            size="small"
            value={selectedServer}
            onChange={(e) => onServerChange(e.target.value)}
            sx={{ minWidth: 220 }}
        >
            {servers.map((s) => (
                <MenuItem key={s.server_name} value={s.server_name}>{s.server_name}</MenuItem>
            ))}
        </Select>

        <Typography variant="body2" color="text.secondary">评分</Typography>
        <Select
            size="small"
            value={scoringProfile}
            onChange={(e) => onScoringProfileChange(e.target.value)}
            sx={{ minWidth: 180 }}
        >
            {SCORING_PROFILES.map((p) => (
                <MenuItem key={p.value} value={p.value}>{p.label}</MenuItem>
            ))}
        </Select>

        <Typography variant="body2" color="text.secondary">时间窗口</Typography>
        <Select size="small" value={hours} onChange={(e) => onHoursChange(Number(e.target.value))}>
            <MenuItem value={1}>1 小时</MenuItem>
            <MenuItem value={6}>6 小时</MenuItem>
            <MenuItem value={24}>24 小时</MenuItem>
            <MenuItem value={72}>72 小时</MenuItem>
        </Select>

        {showInterval ? (
            <>
                <Typography variant="body2" color="text.secondary">聚合间隔</Typography>
                <Select size="small" value={intervalSeconds} onChange={(e) => onIntervalChange(Number(e.target.value))}>
                    <MenuItem value={60}>60 秒</MenuItem>
                    <MenuItem value={300}>300 秒</MenuItem>
                    <MenuItem value={600}>600 秒</MenuItem>
                    <MenuItem value={1800}>1800 秒</MenuItem>
                </Select>
            </>
        ) : null}

        {showAnomalyThresholds ? (
            <>
                <TextField
                    size="small"
                    type="number"
                    label="CPU阈值(%)"
                    value={anomalyThresholds.cpu_threshold}
                    onChange={(e) => onAnomalyThresholdChange("cpu_threshold", Number(e.target.value))}
                    inputProps={{ min: 1, max: 100, step: 1 }}
                    sx={{ width: 130 }}
                />
                <TextField
                    size="small"
                    type="number"
                    label="内存阈值(%)"
                    value={anomalyThresholds.mem_threshold}
                    onChange={(e) => onAnomalyThresholdChange("mem_threshold", Number(e.target.value))}
                    inputProps={{ min: 1, max: 100, step: 1 }}
                    sx={{ width: 130 }}
                />
                <TextField
                    size="small"
                    type="number"
                    label="磁盘阈值(%)"
                    value={anomalyThresholds.disk_threshold}
                    onChange={(e) => onAnomalyThresholdChange("disk_threshold", Number(e.target.value))}
                    inputProps={{ min: 1, max: 100, step: 1 }}
                    sx={{ width: 130 }}
                />
                <TextField
                    size="small"
                    type="number"
                    label="变化率阈值"
                    value={anomalyThresholds.change_rate_threshold}
                    onChange={(e) => onAnomalyThresholdChange("change_rate_threshold", Number(e.target.value))}
                    inputProps={{ min: 0.01, step: 0.01 }}
                    sx={{ width: 140 }}
                />
            </>
        ) : null}

        <Button variant="outlined" onClick={onRefresh}>刷新</Button>
    </Paper>
);

const formatTsLabel = (timestamp) => {
    if (!timestamp || typeof timestamp !== "object") return "-";
    const seconds = Number(timestamp.seconds || 0);
    if (!Number.isFinite(seconds) || seconds <= 0) return "-";
    return new Date(seconds * 1000).toLocaleString("zh-CN", { hour12: false });
};

const aggregateFlowRows = (records, mode) => {
    const map = new Map();
    records.forEach((row) => {
        const ts = Number(row?.timestamp?.seconds || 0);
        if (!ts) return;
        const key = String(ts);
        const prev = map.get(key) || { ts, input: 0, output: 0 };
        if (mode === "net") {
            prev.input += Number(row.rcv_bytes_rate || 0);
            prev.output += Number(row.snd_bytes_rate || 0);
        } else {
            prev.input += Number(row.read_bytes_per_sec || 0);
            prev.output += Number(row.write_bytes_per_sec || 0);
        }
        map.set(key, prev);
    });

    return [...map.values()]
        .sort((a, b) => a.ts - b.ts)
        .map((r) => {
            const inputMB = r.input / 1024 / 1024;
            const outputMB = r.output / 1024 / 1024;
            return {
                time: new Date(r.ts * 1000).toLocaleTimeString("zh-CN", { hour12: false }),
                timestampLabel: new Date(r.ts * 1000).toLocaleString("zh-CN", { hour12: false }),
                input: inputMB,
                output: outputMB,
                total: inputMB + outputMB
            };
        });
};

const percentile = (values, p) => {
    if (!values || values.length === 0) return 0;
    const sorted = [...values].sort((a, b) => a - b);
    const idx = Math.min(sorted.length - 1, Math.max(0, Math.floor((sorted.length - 1) * p)));
    return Number(sorted[idx] || 0);
};

const FlowTripleMonitorView = ({
    mode,
    selectedServer,
    rows,
    loading,
    error
}) => {
    const [alertsOnly, setAlertsOnly] = React.useState(false);
    const aggregated = React.useMemo(() => aggregateFlowRows(rows || [], mode), [rows, mode]);

    const thresholds = React.useMemo(() => {
        const build = (key) => {
            const vals = aggregated.map((r) => Number(r[key] || 0));
            return {
                warn: percentile(vals, 0.90),
                critical: percentile(vals, 0.98)
            };
        };
        return {
            input: build("input"),
            output: build("output"),
            total: build("total")
        };
    }, [aggregated]);

    const title = mode === "net" ? "网络流量监控详情" : "磁盘 IO 监控详情";
    const subtitle = mode === "net"
        ? "展示输入流量、输出流量、总流量（聚合所有网卡）"
        : "展示读流量、写流量、总流量（聚合所有磁盘）";

    const sections = [
        { key: "input", title: mode === "net" ? "输入流量趋势" : "读流量趋势", color: "#79a7cf" },
        { key: "output", title: mode === "net" ? "输出流量趋势" : "写流量趋势", color: "#cc8fa4" },
        { key: "total", title: "总流量趋势", color: "#a391c9" }
    ];

    const getSeverity = React.useCallback((sectionKey, value) => {
        const t = thresholds[sectionKey] || { warn: 0, critical: 0 };
        if (value >= t.critical && t.critical > 0) return "critical";
        if (value >= t.warn && t.warn > 0) return "warning";
        return "normal";
    }, [thresholds]);

    return (
        <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
            <Box display="flex" justifyContent="space-between" alignItems="center" mb={1}>
                <Typography variant="h4" fontWeight="bold">{title}</Typography>
                <FormControlLabel
                    control={<Checkbox checked={alertsOnly} onChange={(e) => setAlertsOnly(e.target.checked)} size="small" />}
                    label="仅看告警"
                />
            </Box>
            <Typography variant="body2" color="text.secondary" mb={2}>{selectedServer || "-"} ｜ {subtitle}</Typography>

            {loading ? <LinearProgress sx={{ mb: 2 }} /> : null}
            {error ? <Typography color="error" mb={2}>{error}</Typography> : null}

            {!loading && aggregated.length === 0 ? (
                <Paper sx={{ p: 3 }}>
                    <Typography color="text.secondary">当前时间窗口暂无可展示的流量数据</Typography>
                </Paper>
            ) : (
                <Box sx={{ display: "grid", gap: 3 }}>
                    {sections.map((section) => (
                        <Card key={section.key} sx={{ p: 2 }}>
                            {(() => {
                                const latestRows = [...aggregated].reverse().slice(0, 20);
                                const criticalCount = latestRows.filter((r) => getSeverity(section.key, Number(r[section.key] || 0)) === "critical").length;
                                const warningCount = latestRows.filter((r) => getSeverity(section.key, Number(r[section.key] || 0)) === "warning").length;
                                return (
                            <Box display="flex" justifyContent="space-between" alignItems="center" mb={1.5}>
                                <Typography variant="h6">{section.title} (MB/s)</Typography>
                                <Box display="flex" gap={1}>
                                    <Chip size="small" color="error" variant="outlined" label={`严重: ${criticalCount}`} />
                                    <Chip size="small" color="warning" variant="outlined" label={`预警: ${warningCount}`} />
                                    <Chip size="small" color="warning" label={`P90: ${thresholds[section.key].warn.toFixed(3)}`} />
                                    <Chip size="small" color="error" label={`P98: ${thresholds[section.key].critical.toFixed(3)}`} />
                                </Box>
                            </Box>
                                );
                            })()}
                            <Box sx={{ height: 260, mb: 2 }}>
                                <ResponsiveContainer width="100%" height="100%">
                                    <LineChart data={aggregated}>
                                        <CartesianGrid strokeDasharray="3 3" stroke="#32343a" />
                                        <XAxis dataKey="time" stroke="#878c94" />
                                        <YAxis stroke="#878c94" />
                                        <Tooltip formatter={(v) => [`${Number(v || 0).toFixed(3)} MB/s`, section.title]} contentStyle={{ backgroundColor: "rgba(23,24,27,.97)", border: "1px solid rgba(255,255,255,.075)", borderRadius: 8 }} />
                                        <Legend />
                                        <ReferenceLine y={thresholds[section.key].warn} stroke="#d8b55a" strokeDasharray="5 5" label={{ value: "P90", position: "right", fill: "#d8b55a" }} />
                                        <ReferenceLine y={thresholds[section.key].critical} stroke="#dc7474" strokeDasharray="5 5" label={{ value: "P98", position: "right", fill: "#dc7474" }} />
                                        <Line type="monotone" dataKey={section.key} stroke={section.color} strokeOpacity={0.82} style={{ filter: `drop-shadow(0 0 6px ${section.color}88)` }} dot={false} strokeWidth={2} name={section.title} />
                                    </LineChart>
                                </ResponsiveContainer>
                            </Box>

                            <TableContainer component={Paper}>
                                <Table size="small">
                                    <TableHead sx={{ bgcolor: "background.default" }}>
                                        <TableRow>
                                            <TableCell>#</TableCell>
                                            <TableCell>时间</TableCell>
                                            <TableCell align="right">{section.title} (MB/s)</TableCell>
                                        </TableRow>
                                    </TableHead>
                                    <TableBody>
                                        {[...aggregated].reverse().slice(0, 20).filter((row) => {
                                            if (!alertsOnly) return true;
                                            const value = Number(row[section.key] || 0);
                                            return getSeverity(section.key, value) !== "normal";
                                        }).map((row, idx) => {
                                            const value = Number(row[section.key] || 0);
                                            const severity = getSeverity(section.key, value);
                                            const isCritical = severity === "critical";
                                            const isWarning = severity === "warning";
                                            return (
                                            <TableRow
                                                key={`${section.key}_${row.timestampLabel}_${idx}`}
                                                hover
                                                sx={isCritical
                                                    ? { backgroundColor: "rgba(255, 23, 68, 0.12)" }
                                                    : isWarning
                                                        ? { backgroundColor: "rgba(255, 234, 0, 0.10)" }
                                                        : undefined}
                                            >
                                                <TableCell>{idx + 1}</TableCell>
                                                <TableCell>{row.timestampLabel}</TableCell>
                                                <TableCell align="right" sx={{ fontFamily: "monospace", color: isCritical ? "error.main" : (isWarning ? "warning.main" : "text.primary") }}>
                                                    {value.toFixed(3)}
                                                </TableCell>
                                            </TableRow>
                                        )})}
                                    </TableBody>
                                </Table>
                            </TableContainer>
                        </Card>
                    ))}
                </Box>
            )}
        </Box>
    );
};

const CpuCoreBarsView = ({
    servers,
    selectedServer,
    onServerChange,
    scoringProfile,
    onScoringProfileChange,
    hours,
    onHoursChange,
    onRefresh,
    cpuCoreRows,
    cpuCoreLoading,
    cpuCoreError,
    nodeHistories
}) => {
    const formatCpuLabel = React.useCallback((cpuName = "") => {
        const text = String(cpuName).trim().toLowerCase();
        const match = text.match(/^cpu(\d+)$/);
        if (match) {
            return `CPU ${match[1]}`;
        }
        if (text === "cpu") {
            return "CPU总览";
        }
        return String(cpuName).toUpperCase();
    }, []);

    const selectedServerInfo = React.useMemo(
        () => servers.find((s) => s.server_name === selectedServer) || null,
        [servers, selectedServer]
    );

    const coreRows = React.useMemo(() => {
        const rows = (cpuCoreRows || []).filter((r) => r.server_name === selectedServer);
        const cpuOrder = (name = "") => {
            const match = name.match(/^cpu(\d+)$/i);
            if (!match) return Number.MAX_SAFE_INTEGER;
            return Number(match[1]);
        };
        return rows.sort((a, b) => {
            const da = cpuOrder(a.cpu_name);
            const db = cpuOrder(b.cpu_name);
            if (da !== db) return da - db;
            return String(a.cpu_name).localeCompare(String(b.cpu_name));
        });
    }, [cpuCoreRows, selectedServer]);

    const historyData = React.useMemo(() => {
        const history = nodeHistories[selectedServer] || [];
        return history.map((p) => ({ time: p.time, cpu: p.cpu }));
    }, [nodeHistories, selectedServer]);

    return (
        <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
            <Typography variant="h4" fontWeight="bold" mb={1}>CPU 负载监控详情</Typography>
            <Typography variant="body2" color="text.secondary" mb={2}>
                上方展示每个 CPU 核心当前负载（柱状），下方展示节点总 CPU 负载曲线
            </Typography>

            <QueryControlBar
                servers={servers}
                selectedServer={selectedServer}
                onServerChange={onServerChange}
                scoringProfile={scoringProfile}
                onScoringProfileChange={onScoringProfileChange}
                hours={hours}
                onHoursChange={onHoursChange}
                intervalSeconds={300}
                onIntervalChange={() => {}}
                showInterval={false}
                showAnomalyThresholds={false}
                anomalyThresholds={{}}
                onAnomalyThresholdChange={() => {}}
                onRefresh={onRefresh}
            />

            <Card sx={{ p: 3, mb: 3 }}>
                <Box display="flex" justifyContent="space-between" alignItems="center" mb={2}>
                    <Typography variant="h6">{selectedServer || "-"} 核心负载</Typography>
                    <Chip
                        size="small"
                        color="primary"
                        label={`总CPU: ${selectedServerInfo ? selectedServerInfo.cpu_percent.toFixed(1) : "0.0"}%`}
                    />
                </Box>

                {cpuCoreLoading && coreRows.length === 0 ? <LinearProgress sx={{ mb: 2 }} /> : null}
                {cpuCoreError ? <Typography color="error" mb={2}>{cpuCoreError}</Typography> : null}

                {!cpuCoreLoading && coreRows.length === 0 ? (
                    <Typography color="text.secondary">当前节点在该时间窗口没有 CPU 核心明细数据</Typography>
                ) : (
                    <Box sx={{ display: "grid", gridTemplateColumns: { xs: "1fr", md: "1fr 1fr" }, gap: 1.5 }}>
                        {coreRows.map((row, idx) => {
                            const percent = Math.max(0, Math.min(100, Number(row.cpu_percent) || 0));
                            const color = percent > 80 ? "error" : percent > 50 ? "warning" : "success";
                            return (
                                <Box key={`${row.server_name}_${row.cpu_name}_${idx}`} sx={{ display: "flex", alignItems: "center", gap: 1.5 }}>
                                    <Typography sx={{ width: 52, fontFamily: "monospace", color: "text.secondary" }}>
                                        {formatCpuLabel(row.cpu_name)}
                                    </Typography>
                                    <Box sx={{ flexGrow: 1 }}>
                                        <LinearProgress
                                            variant="determinate"
                                            value={percent}
                                            color={color}
                                            sx={{ height: 10, borderRadius: 5 }}
                                        />
                                    </Box>
                                    <Typography sx={{ width: 56, textAlign: "right", fontFamily: "monospace", color: "primary.main" }}>
                                        {percent.toFixed(1)}%
                                    </Typography>
                                </Box>
                            );
                        })}
                    </Box>
                )}
            </Card>

            <Card sx={{ p: 3, mb: 3 }}>
                <Typography variant="h6" mb={1.5}>每核关键指标矩阵（性能分析重点）</Typography>
                <Typography variant="body2" color="text.secondary" mb={2}>
                    每个核心展示总使用率、用户态、内核态、IO等待、软中断、空闲占比，便于快速定位瓶颈类型。
                </Typography>

                {cpuCoreLoading && coreRows.length === 0 ? <LinearProgress sx={{ mb: 2 }} /> : null}
                {!cpuCoreLoading && coreRows.length === 0 ? (
                    <Typography color="text.secondary">当前没有可用的核心状态数据</Typography>
                ) : (
                    <TableContainer component={Paper}>
                        <Table size="small">
                            <TableHead sx={{ bgcolor: "background.default" }}>
                                <TableRow>
                                    <TableCell>CPU核心</TableCell>
                                    <TableCell align="right">总使用率(%)</TableCell>
                                    <TableCell align="right">用户态(%)</TableCell>
                                    <TableCell align="right">内核态(%)</TableCell>
                                    <TableCell align="right">IO等待(%)</TableCell>
                                    <TableCell align="right">软中断(%)</TableCell>
                                    <TableCell align="right">空闲(%)</TableCell>
                                </TableRow>
                            </TableHead>
                            <TableBody>
                                {coreRows.map((row, idx) => (
                                    <TableRow key={`${row.server_name}_${row.cpu_name}_metric_${idx}`} hover>
                                        <TableCell sx={{ fontFamily: "monospace" }}>{formatCpuLabel(row.cpu_name)}</TableCell>
                                        <TableCell align="right" sx={{ color: "primary.main", fontFamily: "monospace" }}>{Number(row.cpu_percent || 0).toFixed(1)}</TableCell>
                                        <TableCell align="right" sx={{ fontFamily: "monospace" }}>{Number(row.usr_percent || 0).toFixed(1)}</TableCell>
                                        <TableCell align="right" sx={{ fontFamily: "monospace" }}>{Number(row.system_percent || 0).toFixed(1)}</TableCell>
                                        <TableCell align="right" sx={{ fontFamily: "monospace" }}>{Number(row.io_wait_percent || 0).toFixed(1)}</TableCell>
                                        <TableCell align="right" sx={{ fontFamily: "monospace" }}>{Number(row.soft_irq_percent || 0).toFixed(1)}</TableCell>
                                        <TableCell align="right" sx={{ color: "success.main", fontFamily: "monospace" }}>{Number(row.idle_percent || 0).toFixed(1)}</TableCell>
                                    </TableRow>
                                ))}
                            </TableBody>
                        </Table>
                    </TableContainer>
                )}
            </Card>

            <Card sx={{ p: 2 }}>
                <Typography variant="h6" mb={1.5}>总CPU负载曲线</Typography>
                <Box sx={{ height: 320 }}>
                    <ResponsiveContainer width="100%" height="100%">
                        <AreaChart data={historyData}>
                            <CartesianGrid strokeDasharray="3 3" stroke="#32343a" />
                            <XAxis dataKey="time" stroke="#878c94" />
                            <YAxis stroke="#878c94" domain={[0, 100]} />
                            <Tooltip formatter={(v) => [`${Number(v || 0).toFixed(2)} %`, "CPU"]} contentStyle={{ backgroundColor: "rgba(23,24,27,.97)", border: "1px solid rgba(255,255,255,.075)", borderRadius: 8 }} />
                            <Legend />
                            <Area type="monotone" dataKey="cpu" name={`${selectedServer || "节点"} 总CPU(%)`} stroke="#7299d1" fill="#7299d1" fillOpacity={0.14} />
                        </AreaChart>
                    </ResponsiveContainer>
                </Box>
            </Card>
        </Box>
    );
};

const EndpointView = ({ endpointData }) => {
    const endpoints = endpointData?.endpoints || [];

    return (
        <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
            <Typography variant="h4" fontWeight="bold" mb={1}>查询端口总览</Typography>
            <Typography variant="body2" color="text.secondary" mb={3}>
                Manager 地址：{endpointData?.manager_address || "-"} ｜ Dashboard 地址：{endpointData?.dashboard_address || "-"}
            </Typography>

            <TableContainer component={Paper}>
                <Table>
                    <TableHead sx={{ bgcolor: "background.default" }}>
                        <TableRow>
                            <TableCell>#</TableCell>
                            <TableCell>类型</TableCell>
                            <TableCell>方法</TableCell>
                            <TableCell>查询端口 / 接口</TableCell>
                            <TableCell>目标地址</TableCell>
                            <TableCell>说明</TableCell>
                        </TableRow>
                    </TableHead>
                    <TableBody>
                        {endpoints.map((ep, idx) => (
                            <TableRow key={`${ep.type}_${ep.path}_${idx}`} hover>
                                <TableCell>{idx + 1}</TableCell>
                                <TableCell>
                                    <Chip
                                        size="small"
                                        label={ep.type}
                                        color={ep.type === "gRPC" ? "secondary" : ep.type === "HTTP" ? "primary" : "success"}
                                        variant="outlined"
                                    />
                                </TableCell>
                                <TableCell sx={{ fontFamily: "monospace" }}>{ep.method}</TableCell>
                                <TableCell sx={{ fontFamily: "monospace", color: "primary.main" }}>{ep.path}</TableCell>
                                <TableCell sx={{ fontFamily: "monospace" }}>{ep.target}</TableCell>
                                <TableCell>{ep.description}</TableCell>
                            </TableRow>
                        ))}
                    </TableBody>
                </Table>
            </TableContainer>
        </Box>
    );
};

// --- Main App Shell ---

const AppShell = () => {
    const [view, setView] = React.useState(window.location.pathname.replace(/\/+$/, "") === "/settings" ? "settings" : "dashboard");
    const [focusedIncident, setFocusedIncident] = React.useState(null);
    const [agentSeed, setAgentSeed] = React.useState(null); // dashboard | endpoints | rank | trend | anomaly | net_detail | disk_detail | mem_detail | softirq_detail | cpu | mem | disk | net
    const [subViewNode, setSubViewNode] = React.useState(null); // If set, shows chart for this node inside the current view
    
    const [data, setData] = React.useState({ servers: [], cluster_stats: {} });
    const [scoringProfile, setScoringProfile] = React.useState("RESOURCE_SCORE");
    const [histories, setHistories] = React.useState({});
    const [queryEndpointData, setQueryEndpointData] = React.useState({ endpoints: [] });
    const [selectedQueryServer, setSelectedQueryServer] = React.useState("");
    const [sidebarFilter, setSidebarFilter] = React.useState("");
    const [socketConnected, setSocketConnected] = React.useState(false);
    const [queryHours, setQueryHours] = React.useState(24);
    const [trendIntervalSeconds, setTrendIntervalSeconds] = React.useState(300);
    const [queryRows, setQueryRows] = React.useState({ rank: [], trend: [], anomaly: [], net_detail: [], disk_detail: [], mem_detail: [], softirq_detail: [] });
    const [queryLoading, setQueryLoading] = React.useState({});
    const [queryError, setQueryError] = React.useState({});
    const [cpuCoreRows, setCpuCoreRows] = React.useState([]);
    const [cpuCoreLoading, setCpuCoreLoading] = React.useState(false);
    const [cpuCoreError, setCpuCoreError] = React.useState("");
    const [anomalyThresholds, setAnomalyThresholds] = React.useState({
        cpu_threshold: 80,
        mem_threshold: 90,
        disk_threshold: 85,
        change_rate_threshold: 0.5
    });
    const scoringProfileRef = React.useRef("RESOURCE_SCORE");

    React.useEffect(() => {
        scoringProfileRef.current = scoringProfile;
    }, [scoringProfile]);

    React.useEffect(() => {
        // Init fetch
        axios.get("/api/overview", { params: { scoring_profile: scoringProfileRef.current } })
            .then(res => {
                const overview = applyScoringProfileToOverview(res.data, scoringProfileRef.current);
                setData(overview);
                const initialTime = new Date().toLocaleTimeString("zh-CN", { hour12: false });
                setHistories((prev) => {
                    const next = { ...prev };
                    overview.servers.forEach((server) => {
                        if (!(next[server.server_name] || []).length) {
                            next[server.server_name] = [toHistoryPoint(server, initialTime)];
                        }
                    });
                    return next;
                });
                Promise.all(overview.servers.map(async (server) => {
                    try {
                        const historyResponse = await axios.get(`/api/performance/${encodeURIComponent(server.server_name)}`, { params: { hours: 1 } });
                        const records = (historyResponse.data.records || [])
                            .map((record) => toHistoryPoint(record))
                            .sort((a, b) => a.timestampMs - b.timestampMs)
                            .slice(-60);
                        return [server.server_name, records];
                    } catch (error) {
                        return [server.server_name, []];
                    }
                })).then((historyEntries) => {
                    setHistories((prev) => {
                        const next = { ...prev };
                        historyEntries.forEach(([serverName, historicalPoints]) => {
                            if (!historicalPoints.length) return;
                            const latestHistoryTime = historicalPoints[historicalPoints.length - 1].timestampMs;
                            const newerLivePoints = (next[serverName] || []).filter((point) => point.timestampMs > latestHistoryTime);
                            next[serverName] = [...historicalPoints, ...newerLivePoints].slice(-60);
                        });
                        return next;
                    });
                });
            })
            .catch(console.error);
        axios.get("/api/query-endpoints").then(res => setQueryEndpointData(res.data)).catch(console.error);

        const socket = io();
        socket.on("connect", () => setSocketConnected(true));
        socket.on("disconnect", () => setSocketConnected(false));
        socket.on("overview_update", (newData) => {
            const activeProfile = scoringProfileRef.current;
            setData(applyScoringProfileToOverview(newData, activeProfile));
            // Append history
            setHistories(prev => {
                const next = { ...prev };
                newData.servers.forEach(s => {
                    const h = next[s.server_name] || [];
                            const p = toHistoryPoint(s);
                            if (!h.length || p.timestampMs > h[h.length - 1].timestampMs) {
                                next[s.server_name] = [...h, p].slice(-60);
                            }
                });
                return next;
            });
        });
        return () => socket.disconnect();
    }, []);

    React.useEffect(() => {
        axios.get("/api/overview", { params: { scoring_profile: scoringProfile } })
            .then(res => setData(applyScoringProfileToOverview(res.data, scoringProfile)))
            .catch(console.error);
    }, [scoringProfile]);

    React.useEffect(() => {
        if (!selectedQueryServer && data.servers.length > 0) {
            setSelectedQueryServer(data.servers[0].server_name);
        }
    }, [data.servers, selectedQueryServer]);

    const loadQueryViewData = React.useCallback(async (targetView) => {
        const needServerViews = ["trend", "net_detail", "disk_detail", "mem_detail", "softirq_detail"];
        if (needServerViews.includes(targetView) && !selectedQueryServer) return;

        setQueryLoading(prev => ({ ...prev, [targetView]: true }));
        setQueryError(prev => ({ ...prev, [targetView]: "" }));

        try {
            if (targetView === "trend") {
                const res = await axios.get(`/api/trend/${encodeURIComponent(selectedQueryServer)}`, {
                    params: {
                        hours: queryHours,
                        interval_seconds: trendIntervalSeconds,
                        scoring_profile: scoringProfile
                    }
                });
                setQueryRows(prev => ({ ...prev, trend: res.data.records || [] }));
                return;
            }

            if (targetView === "anomaly") {
                const res = await axios.get("/api/anomaly", {
                    params: {
                        server_name: selectedQueryServer,
                        hours: queryHours,
                        cpu_threshold: anomalyThresholds.cpu_threshold,
                        mem_threshold: anomalyThresholds.mem_threshold,
                        disk_threshold: anomalyThresholds.disk_threshold,
                        change_rate_threshold: anomalyThresholds.change_rate_threshold,
                        page: 1,
                        page_size: 100
                    }
                });
                setQueryRows(prev => ({ ...prev, anomaly: res.data.anomalies || [] }));
                return;
            }

            const endpointMap = {
                net_detail: "/api/net-detail",
                disk_detail: "/api/disk-detail",
                mem_detail: "/api/mem-detail",
                softirq_detail: "/api/softirq-detail"
            };

            if (endpointMap[targetView]) {
                const res = await axios.get(endpointMap[targetView], {
                    params: {
                        server_name: selectedQueryServer,
                        hours: queryHours,
                        page: 1,
                        page_size: 100
                    }
                });
                setQueryRows(prev => ({ ...prev, [targetView]: res.data.records || [] }));
            }
        } catch (err) {
            setQueryError(prev => ({ ...prev, [targetView]: err?.response?.data?.error || err.message || "查询失败" }));
        } finally {
            setQueryLoading(prev => ({ ...prev, [targetView]: false }));
        }
    }, [selectedQueryServer, queryHours, trendIntervalSeconds, anomalyThresholds, scoringProfile]);

    const handleAnomalyThresholdChange = (key, value) => {
        const nextValue = Number.isFinite(value) ? value : 0;
        setAnomalyThresholds(prev => ({ ...prev, [key]: nextValue }));
    };

    const loadCpuCoreDetails = React.useCallback(async (options = {}) => {
        const { silent = false } = options;
        if (!silent) {
            setCpuCoreLoading(true);
        }
        setCpuCoreError("");
        try {
            const res = await axios.get("/api/cpu-core-detail", {
                params: {
                    server_name: selectedQueryServer,
                    hours: queryHours
                }
            });
            setCpuCoreRows(res.data.records || []);
        } catch (err) {
            setCpuCoreError(err?.response?.data?.error || err.message || "CPU核心负载查询失败");
        } finally {
            if (!silent) {
                setCpuCoreLoading(false);
            }
        }
    }, [queryHours, selectedQueryServer]);

    React.useEffect(() => {
        const queryViews = ["trend", "anomaly", "net_detail", "disk_detail", "mem_detail", "softirq_detail"];
        if (view === "rank" && selectedQueryServer) {
            loadQueryViewData("trend");
        } else if (queryViews.includes(view)) {
            loadQueryViewData(view);
        }
    }, [view, selectedQueryServer, loadQueryViewData]);

    React.useEffect(() => {
        if (view === "net") {
            loadQueryViewData("net_detail");
        } else if (view === "disk") {
            loadQueryViewData("disk_detail");
        }
    }, [view, selectedQueryServer, queryHours, loadQueryViewData]);

    const sidebarItems = [
        { id: "dashboard", label: "总览", icon: "space_dashboard" },
        { id: "endpoints", label: "查询接口", icon: "account_tree" },
        { divider: true },
        { id: "header_query", label: "数据查询" },
        { id: "rank", label: "评分趋势", icon: "show_chart" },
        { id: "trend", label: "趋势聚合", icon: "show_chart" },
        { id: "diagnosis", label: "智能诊断", icon: "psychology_alt" },
        { id: "anomaly", label: "异常查询", icon: "warning_amber" },
        { id: "net_detail", label: "网络详情", icon: "lan" },
        { id: "disk_detail", label: "磁盘详情", icon: "storage" },
        { id: "mem_detail", label: "内存详情", icon: "memory" },
        { id: "softirq_detail", label: "软中断详情", icon: "bolt" },
        { divider: true },
        { id: "header", label: "主机指标" },
        { id: "cpu", label: "CPU 负载", dot: "amber" },
        { id: "mem", label: "内存使用", dot: "blue" },
        { id: "disk", label: "磁盘 I/O", dot: "purple" },
        { id: "net", label: "网络流量", dot: "pink" },
        { divider: true },
        { id: "header_settings", label: "系统设置" },
        { id: "settings", label: "API 配置", icon: "key" },
    ];
    const visibleSidebarItems = sidebarFilter.trim()
        ? sidebarItems.filter((item) => item.id && item.label.toLowerCase().includes(sidebarFilter.trim().toLowerCase()))
        : sidebarItems;
    const activeSidebarItem = sidebarItems.find((item) => item.id === view);
    const pageTitle = subViewNode ? `${subViewNode} · 性能详情` : (activeSidebarItem?.label || "总览");

    const handleViewChange = (newView) => {
        setView(newView);
        const nextPath = newView === "settings" ? "/settings" : "/";
        if (window.location.pathname !== nextPath) window.history.replaceState(null, "", nextPath);
        setSubViewNode(null); // Reset sub-view on main nav change
    };

    const handleNodeClick = (nodeName) => {
        setSubViewNode(nodeName);
    };

    const refreshOverview = async () => {
        const response = await axios.get("/api/overview", { params: { scoring_profile: scoringProfile } });
        setData(applyScoringProfileToOverview(response.data, scoringProfile));
    };

    const renderContent = () => {
        // If we are drill-down into a node chart
        if (subViewNode) {
             return (
                <Box sx={{ height: "100%", p: 3 }}>
                    <ChartSection 
                        servers={data.servers} 
                        nodeHistories={histories} 
                        initialNode={subViewNode}
                        initialMetric={view === "dashboard" ? "cpu" : view} // pass current view type as initial metric
                        onBack={() => setSubViewNode(null)}
                        fixedMode={true}
                    />
                </Box>
             );
        }

        if (view === "diagnosis") {
            return <window.KernDiagnosisCenter
                servers={data.servers}
                selectedServer={selectedQueryServer}
                onServerChange={setSelectedQueryServer}
                focusedIncident={focusedIncident}
                onSettings={() => handleViewChange("settings")}
                onAsk={incident => {
                    setSelectedQueryServer(incident.server_name);
                    setAgentSeed({id: String(incident.id), host: incident.server_name,
                        question: "请结合 Incident #" + incident.id + " 的实际证据进一步解释根因和排查步骤。"});
                }}
            />;
        }
        if (view === "settings") {
            return <window.KernSettingsPanel />;
        }
        if (view === "dashboard") {
            return (
                <Box className="dashboard-home" sx={{ minHeight: "100%", display: "flex", flexDirection: "column", p: 3, gap: 2 }}>
                    <Paper className="dashboard-toolbar" sx={{ p: 1.5, display: "flex", alignItems: "center", gap: 2 }}>
                        <Box className="score-profile-group">
                            <Typography variant="body2" color="text.secondary">评分</Typography>
                            <Select
                                className="score-profile-select"
                                size="small"
                                value={scoringProfile}
                                onChange={(e) => setScoringProfile(e.target.value)}
                                sx={{ minWidth: 180 }}
                            >
                                {SCORING_PROFILES.map((p) => (
                                    <MenuItem key={p.value} value={p.value}>{p.label}</MenuItem>
                                ))}
                            </Select>
                        </Box>
                    </Paper>
                    <Box>
                        <TopStats clusterStats={data.cluster_stats} servers={data.servers} scoringProfile={scoringProfile} />
                    </Box>
                    <Box sx={{ flexGrow: 1, minHeight: 390 }}>
                        <ChartSection servers={data.servers} nodeHistories={histories} />
                    </Box>
                </Box>
            );
        }

        if (view === "endpoints") {
            return <EndpointView endpointData={queryEndpointData} />;
        }

        if (view === "rank") {
            return (
                <ScoreTrendView
                    servers={data.servers}
                    selectedServer={selectedQueryServer}
                    onServerChange={setSelectedQueryServer}
                    hours={queryHours}
                    onHoursChange={setQueryHours}
                    rows={queryRows.trend}
                    loading={!!queryLoading.trend}
                    error={queryError.trend}
                    onRefresh={() => loadQueryViewData("trend")}
                />
            );
        }

        if (["trend", "anomaly", "net_detail", "disk_detail", "mem_detail", "softirq_detail"].includes(view)) {
            return (
                <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
                    <QueryControlBar
                        servers={data.servers}
                        selectedServer={selectedQueryServer}
                        onServerChange={setSelectedQueryServer}
                        scoringProfile={scoringProfile}
                        onScoringProfileChange={setScoringProfile}
                        hours={queryHours}
                        onHoursChange={setQueryHours}
                        intervalSeconds={trendIntervalSeconds}
                        onIntervalChange={setTrendIntervalSeconds}
                        showInterval={view === "trend"}
                        showAnomalyThresholds={view === "anomaly"}
                        anomalyThresholds={anomalyThresholds}
                        onAnomalyThresholdChange={handleAnomalyThresholdChange}
                        onRefresh={() => loadQueryViewData(view)}
                    />
                    <Box className="query-table-wrap">
                        <QueryTableView
                            title=""
                            rows={queryRows[view] || []}
                            loading={!!queryLoading[view]}
                            error={queryError[view]}
                            emptyText="该条件下暂无返回记录"
                        />
                    </Box>
                </Box>
            );
        }

        // Detail Views
        const metrics = {
            cpu: { key: "cpu_percent", icon: "memory_alt", unit: "%", getValue: s => s.cpu_percent, format: v => `${v.toFixed(1)}%` },
            mem: { key: "mem_used_percent", icon: "developer_board", unit: "%", getValue: s => s.mem_used_percent, format: v => `${v.toFixed(1)}%` },
            disk: { key: "disk_util_percent", icon: "hard_drive", unit: "%", getValue: s => s.disk_util_percent, format: v => `${v.toFixed(1)}%` },
            net: { key: "net", icon: "router", unit: "MB/s", getValue: s => (s.rcv_rate + s.send_rate)/1024, format: v => `${v.toFixed(1)} MB/s`, normalize: v => Math.min(v/10 * 100, 100) } // Mock normalization
        };

        const m = metrics[view];
        if (m && view === "cpu") {
            return <DetailView title="CPU 使用率" metric={m} servers={data.servers}
                onNodeClick={handleNodeClick} />;
        }

        if (view === "net") {
            return (
                <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
                    <QueryControlBar
                        servers={data.servers}
                        selectedServer={selectedQueryServer}
                        onServerChange={setSelectedQueryServer}
                        scoringProfile={scoringProfile}
                        onScoringProfileChange={setScoringProfile}
                        hours={queryHours}
                        onHoursChange={setQueryHours}
                        intervalSeconds={trendIntervalSeconds}
                        onIntervalChange={setTrendIntervalSeconds}
                        showInterval={false}
                        showAnomalyThresholds={false}
                        anomalyThresholds={anomalyThresholds}
                        onAnomalyThresholdChange={handleAnomalyThresholdChange}
                        onRefresh={() => loadQueryViewData("net_detail")}
                    />
                    <FlowTripleMonitorView
                        mode="net"
                        selectedServer={selectedQueryServer}
                        rows={queryRows.net_detail || []}
                        loading={!!queryLoading.net_detail}
                        error={queryError.net_detail}
                    />
                </Box>
            );
        }

        if (view === "disk") {
            return (
                <Box sx={{ p: 4, height: "100%", overflowY: "auto" }}>
                    <QueryControlBar
                        servers={data.servers}
                        selectedServer={selectedQueryServer}
                        onServerChange={setSelectedQueryServer}
                        scoringProfile={scoringProfile}
                        onScoringProfileChange={setScoringProfile}
                        hours={queryHours}
                        onHoursChange={setQueryHours}
                        intervalSeconds={trendIntervalSeconds}
                        onIntervalChange={setTrendIntervalSeconds}
                        showInterval={false}
                        showAnomalyThresholds={false}
                        anomalyThresholds={anomalyThresholds}
                        onAnomalyThresholdChange={handleAnomalyThresholdChange}
                        onRefresh={() => loadQueryViewData("disk_detail")}
                    />
                    <FlowTripleMonitorView
                        mode="disk"
                        selectedServer={selectedQueryServer}
                        rows={queryRows.disk_detail || []}
                        loading={!!queryLoading.disk_detail}
                        error={queryError.disk_detail}
                    />
                </Box>
            );
        }

        if (m) {
            return (
                <DetailView 
                    title={sidebarItems.find(x => x.id === view).label} 
                    metric={m} 
                    servers={data.servers} 
                    onNodeClick={handleNodeClick}
                    onRefresh={refreshOverview}
                />
            );
        }
        return null;
    };

    return (
        <ThemeProvider theme={theme}>
            <CssBaseline />
            <Box className="app-shell" sx={{ display: "flex", height: "100dvh", overflow: "hidden" }}>
                <Drawer
                     className="sidebar-shell"
                     variant="permanent"
                     sx={{ flexShrink: 0 }}
                >
                    <Box className="sidebar-inner">
                    <Box className="brand-lockup">
                        <span className="brand-mark material-icons" aria-hidden="true">monitor_heart</span>
                        <Box>
                            <Typography className="brand-name">KernScope</Typography>
                        </Box>
                    </Box>
                    <TextField
                        className="sidebar-search"
                        size="small"
                        placeholder="搜索功能"
                        value={sidebarFilter}
                        onChange={(event) => setSidebarFilter(event.target.value)}
                        inputProps={{ "aria-label": "搜索导航功能" }}
                        InputProps={{
                            startAdornment: <InputAdornment position="start"><span className="material-icons" aria-hidden="true">search</span></InputAdornment>,
                            endAdornment: sidebarFilter ? <InputAdornment position="end"><IconButton size="small" aria-label="清空搜索" onClick={() => setSidebarFilter("")}><span className="material-icons">close</span></IconButton></InputAdornment> : null
                        }}
                    />
                    <List className="nav-list">
                        {visibleSidebarItems.map((item, i) => {
                            if (item.divider) return <Divider key={i} sx={{ my: 2, borderColor: "rgba(255,255,255,0.1)" }} />;
                            if (item.id === "header" || item.id === "header_query" || item.id === "header_settings") return <Typography key={i} variant="caption" sx={{ px: 3, color: "text.secondary", mt: 1, display: "block" }}>{item.label}</Typography>;
                            
                            return (
                                <ListItem key={item.id} disablePadding className="nav-row">
                                    <ListItemButton
                                        className="nav-action"
                                        aria-label={item.label}
                                        aria-current={view === item.id && !subViewNode ? "page" : undefined}
                                        title={item.label}
                                        selected={view === item.id && !subViewNode}
                                        onClick={() => handleViewChange(item.id)}
                                    >
                                        <ListItemIcon className="nav-action-icon">{item.dot ? <span className={`nav-status-dot dot-${item.dot}`} aria-hidden="true" /> : <span className="material-icons nav-icon" aria-hidden="true">{item.icon}</span>}</ListItemIcon>
                                        <ListItemText primary={item.label} />
                                    </ListItemButton>
                                </ListItem>
                            );
                        })}
                        {visibleSidebarItems.length === 0 && <Typography variant="caption" sx={{ px: 2, py: 1.5, color: "text.secondary" }}>未找到匹配功能</Typography>}
                    </List>
                    <Box className="sidebar-footer">
                        <Box className={`connection-dot ${socketConnected ? "is-connected" : ""}`} />
                        <Box>
                            <Typography className="connection-label">{socketConnected ? (data.servers.length ? "监控链路已连接" : "等待采集节点") : "实时连接已断开"}</Typography>
                            <Typography className="connection-detail">{data.cluster_stats.online_servers || 0} / {data.cluster_stats.total_servers || 0} 节点在线</Typography>
                        </Box>
                    </Box>
                    </Box>
                </Drawer>
                <Box component="main" className="dashboard-main">
                    <Box component="header" className="topbar">
                        <Box className="topbar-title">
                            <Box>
                                <Typography className="topbar-eyebrow">实时性能监控</Typography>
                                <Typography className="topbar-heading">{pageTitle}</Typography>
                            </Box>
                        </Box>
                        <Box className="topbar-meta">
                            <window.KernAlertBell onOpen={record => {
                                setSelectedQueryServer(record.server_name);
                                setFocusedIncident({id: String(record.incident_id), nonce: Date.now()});
                                setSubViewNode(null);
                                handleViewChange("diagnosis");
                            }} />
                            <span className={`live-indicator ${socketConnected ? "is-connected" : ""}`} />
                            <Typography className="live-label">{socketConnected ? "实时同步" : "连接中断"}</Typography>
                            <Typography className="topbar-date">{new Date().toLocaleDateString("zh-CN", { year: "numeric", month: "long", day: "numeric" })}</Typography>
                        </Box>
                    </Box>
                    <Box component="section" className="page-stage">{renderContent()}</Box>
                </Box>
            </Box>
            <window.AgentPanel serverName={selectedQueryServer} seed={agentSeed}
                servers={data.servers} onServerChange={setSelectedQueryServer} />
        </ThemeProvider>
    );
};

const root = ReactDOM.createRoot(document.getElementById("root"));
root.render(<AppShell /> );
