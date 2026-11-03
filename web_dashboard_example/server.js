// Modified for KernScope: gRPC contract, metrics units, read-only Agent endpoint.
// Upstream: superlxh02/linux_monitor_system (Apache-2.0).
const express = require('express');
const path = require('path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const http = require('http');
const { Server } = require('socket.io');

const app = express();
const server = http.createServer(app);
const io = new Server(server);

const PORT = Number(process.env.PORT || 3000);
const MANAGER_ADDRESS = process.env.MANAGER_ADDRESS || '127.0.0.1:50051';
const deepseekConfig = require('./deepseek-config');
deepseekConfig.load();

const QUERY_ENDPOINTS = [
    // gRPC QueryService APIs
    { type: 'gRPC', method: 'RPC', path: 'QueryPerformance', target: MANAGER_ADDRESS, description: '时间段内性能数据（分页）' },
    { type: 'gRPC', method: 'RPC', path: 'QueryTrend', target: MANAGER_ADDRESS, description: '变化率趋势查询（可聚合）' },
    { type: 'gRPC', method: 'RPC', path: 'QueryAnomaly', target: MANAGER_ADDRESS, description: '异常数据查询（阈值过滤）' },
    { type: 'gRPC', method: 'RPC', path: 'QueryScoreRank', target: MANAGER_ADDRESS, description: '评分排序查询' },
    { type: 'gRPC', method: 'RPC', path: 'QueryLatestScore', target: MANAGER_ADDRESS, description: '最新评分查询' },
    { type: 'gRPC', method: 'RPC', path: 'QueryNetDetail', target: MANAGER_ADDRESS, description: '网络详情查询（分页）' },
    { type: 'gRPC', method: 'RPC', path: 'QueryDiskDetail', target: MANAGER_ADDRESS, description: '磁盘详情查询（分页）' },
    { type: 'gRPC', method: 'RPC', path: 'QueryMemDetail', target: MANAGER_ADDRESS, description: '内存详情查询（分页）' },
    { type: 'gRPC', method: 'RPC', path: 'QuerySoftIrqDetail', target: MANAGER_ADDRESS, description: '软中断详情查询（分页）' },

    // Exposed BFF HTTP APIs
    { type: 'HTTP', method: 'GET', path: '/api/overview', target: `0.0.0.0:${PORT}`, description: '总览数据（映射 QueryLatestScore）' },
    { type: 'HTTP', method: 'GET', path: '/api/performance/:serverName?hours=1', target: `0.0.0.0:${PORT}`, description: '历史性能（映射 QueryPerformance）' },
    { type: 'HTTP', method: 'GET', path: '/api/rank', target: `0.0.0.0:${PORT}`, description: '评分排行（映射 QueryScoreRank）' },
    { type: 'HTTP', method: 'GET', path: '/api/anomaly', target: `0.0.0.0:${PORT}`, description: '异常查询（映射 QueryAnomaly）' },
    { type: 'HTTP', method: 'GET', path: '/api/trend/:serverName', target: `0.0.0.0:${PORT}`, description: '趋势聚合（映射 QueryTrend）' },
    { type: 'HTTP', method: 'GET', path: '/api/net-detail', target: `0.0.0.0:${PORT}`, description: '网络详情（映射 QueryNetDetail）' },
    { type: 'HTTP', method: 'GET', path: '/api/disk-detail', target: `0.0.0.0:${PORT}`, description: '磁盘详情（映射 QueryDiskDetail）' },
    { type: 'HTTP', method: 'GET', path: '/api/mem-detail', target: `0.0.0.0:${PORT}`, description: '内存详情（映射 QueryMemDetail）' },
    { type: 'HTTP', method: 'GET', path: '/api/softirq-detail', target: `0.0.0.0:${PORT}`, description: '软中断详情（映射 QuerySoftIrqDetail）' },
    { type: 'HTTP', method: 'GET', path: '/api/query-endpoints', target: `0.0.0.0:${PORT}`, description: '查询端口清单' },

    // Real-time push channel
    { type: 'Socket.IO', method: 'EVENT', path: 'overview_update', target: `0.0.0.0:${PORT}`, description: '2 秒推送一次总览更新' }
];

// Middleware
app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json({limit: '32kb'}));
// Dashboard API is same-origin. Reject foreign browser origins on write endpoints.
app.use((req,res,next) => {
    const origin=req.get('origin');
    if(req.method !== 'GET' && origin) {
        let originHost='';
        try {originHost=new URL(origin).host;} catch(_) {}
        if(originHost !== req.get('host'))
            return res.status(403).json({error:'跨站请求已拒绝'});
    }
    next();
});

const requireLoopback = (req, res, next) => {
    const remoteAddress = String(req.socket?.remoteAddress || '').toLowerCase().replace(/^::ffff:/, '');
    if (remoteAddress !== '127.0.0.1' && remoteAddress !== '::1')
        return res.status(403).json({error: 'API 配置只允许本机访问。'});
    next();
};

app.get('/api/settings/deepseek', requireLoopback, (req, res) => {
    res.json(deepseekConfig.status());
});
app.put('/api/settings/deepseek', requireLoopback, (req, res) => {
    const apiKey = typeof req.body?.apiKey === 'string' ? req.body.apiKey.trim() : '';
    if (!apiKey || apiKey.length > 512 || /[\u0000-\u001f\u007f]/.test(apiKey))
        return res.status(400).json({error: '请输入有效的 API Key（最多 512 个字符）。'});
    try {
        deepseekConfig.save(apiKey);
        res.json(deepseekConfig.status());
    } catch (_) {
        res.status(500).json({error: '无法保存 API Key，请检查本机配置目录的写入权限。'});
    }
});
app.delete('/api/settings/deepseek', requireLoopback, (req, res) => {
    try {
        deepseekConfig.clear();
        res.json(deepseekConfig.status());
    } catch (_) {
        res.status(500).json({error: '无法删除本机保存的 API Key。'});
    }
});

// gRPC Setup
const PROTO_PATH = path.join(__dirname, '../proto/query_api.proto');
const packageDefinition = protoLoader.loadSync(PROTO_PATH, {
    keepCase: true,
    longs: String,
    enums: String,
    defaults: true,
    oneofs: true,
    includeDirs: [path.join(__dirname, '../proto')]
});

const protoDescriptor = grpc.loadPackageDefinition(packageDefinition);
const queryService = new protoDescriptor.monitor.proto.QueryService(
    MANAGER_ADDRESS,
    grpc.credentials.createInsecure()
);

// Helper for gRPC calls using Promises
const promisifyGrpc = (method, params = {}) => {
    return new Promise((resolve, reject) => {
        queryService[method](params, {deadline: Date.now() + 8000}, (err, response) => {
            if (err) {
                console.error(`gRPC Error [${method}]:`, err);
                reject(err);
            } else {
                resolve(response);
            }
        });
    });
};

// The BFF is the only entry to Manager and the LLM key.
const agent = require('./agent');
const alerts = require('./alerts')(promisifyGrpc, io, agent.analyzeIncident);
app.post('/api/agent', agent(promisifyGrpc));

const timestampToMillis = (timestamp) => {
    if (!timestamp) return 0;
    const seconds = Number(timestamp.seconds || 0);
    const nanos = Number(timestamp.nanos || 0);
    return seconds * 1000 + Math.floor(nanos / 1e6);
};

const toFiniteNumber = (value) => {
    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric : 0;
};

const normalizeScoringProfile = (rawProfile) => {
    return String(rawProfile || '').toUpperCase() === 'HEALTH_SCORE'
        ? 'HEALTH_SCORE' : 'RESOURCE_SCORE';
};

// KernScope NetDetailRecord already stores KiB/s, not bytes/s.
const bytesToKB = (kibPerSec) => toFiniteNumber(kibPerSec);

const buildLatestNetRateMap = (records = []) => {
    const latestByServer = new Map();

    records.forEach((record) => {
        const serverName = record.server_name;
        if (!serverName) return;

        const ts = timestampToMillis(record.timestamp);
        const current = latestByServer.get(serverName);

        if (!current || ts > current.latestTs) {
            latestByServer.set(serverName, {
                latestTs: ts,
                rcvRate: bytesToKB(record.rcv_bytes_rate),
                sendRate: bytesToKB(record.snd_bytes_rate)
            });
            return;
        }

        if (ts === current.latestTs) {
            current.rcvRate += bytesToKB(record.rcv_bytes_rate);
            current.sendRate += bytesToKB(record.snd_bytes_rate);
        }
    });

    return latestByServer;
};

const fetchLatestNetRateMap = async (serverName) => {
    const endTime = new Date();
    const startTime = new Date(endTime.getTime() - 5 * 60 * 1000);

    const response = await promisifyGrpc('QueryNetDetail', {
        server_name: serverName,
        time_range: {
            start_time: { seconds: Math.floor(startTime.getTime() / 1000), nanos: 0 },
            end_time: { seconds: Math.floor(endTime.getTime() / 1000), nanos: 0 }
        },
        pagination: { page: 1, page_size: 1000 }
    });

    return buildLatestNetRateMap(response.records || []);
};

const enrichOverviewWithNetRates = async (overviewResponse) => {
    const servers = overviewResponse.servers || [];
    const netRateEntries = await Promise.all(
        servers.map(async (server) => {
            try {
                const netRateMap = await fetchLatestNetRateMap(server.server_name);
                const net = netRateMap.get(server.server_name);
                return [server.server_name, net || null];
            } catch (err) {
                return [server.server_name, null];
            }
        })
    );

    const netRateMap = new Map(netRateEntries);
    const enrichedServers = servers.map((server) => {
        const net = netRateMap.get(server.server_name);
        return {
            ...server,
            rcv_rate: net ? net.rcvRate : 0,
            send_rate: net ? net.sendRate : 0
        };
    });

    return {
        ...overviewResponse,
        servers: enrichedServers
    };
};

// API Endpoints

// Get Latest Scores (Overview)
app.get('/api/overview', async (req, res) => {
    try {
        const scoring_profile = normalizeScoringProfile(req.query.scoring_profile || req.query.profile);
        const response = await promisifyGrpc('QueryLatestScore', {});
        const enriched = await enrichOverviewWithNetRates(response);
        res.json(enriched);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Get Performance History (Charts)
app.get('/api/performance/:serverName', async (req, res) => {
    const { serverName } = req.params;
    const { hours = 1 } = req.query;
    const scoring_profile = normalizeScoringProfile(req.query.scoring_profile || req.query.profile);
    
    // Calculate time range
    const endTime = new Date();
    const startTime = new Date(endTime.getTime() - hours * 60 * 60 * 1000);

    const request = {
        server_name: serverName,
        time_range: {
            start_time: { seconds: Math.floor(startTime.getTime() / 1000), nanos: 0 },
            end_time: { seconds: Math.floor(endTime.getTime() / 1000), nanos: 0 }
        },
        pagination: { page: 1, page_size: 1000 }, // Fetch plenty for charts
        scoring_profile
    };

    try {
        const response = await promisifyGrpc('QueryPerformance', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Get Top Ranked Servers
app.get('/api/rank', async (req, res) => {
    try {
        const order = (req.query.order || 'DESC').toUpperCase();
        const scoring_profile = normalizeScoringProfile(req.query.scoring_profile || req.query.profile);
        const page = parseInt(req.query.page, 10) || 1;
        const page_size = parseInt(req.query.page_size, 10) || 20;
        const response = await promisifyGrpc('QueryScoreRank', {
            order: order === 'ASC' ? 1 : 0,
            pagination: { page, page_size },
            score_kind: scoring_profile
        });
        if (scoring_profile === 'HEALTH_SCORE') {
            response.servers = (response.servers || []).map(s => ({
                ...s, score: s.health_valid ? s.health_score : null
            }));
        }
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// 异常数据（超阈值）
app.get('/api/anomaly', async (req, res) => {
    try {
        const serverName = req.query.server_name || '';
        const hours = parseFloat(req.query.hours, 10) || 24;
        const cpu_threshold = parseFloat(req.query.cpu_threshold, 10) || 80;
        const mem_threshold = parseFloat(req.query.mem_threshold, 10) || 90;
        const disk_threshold = parseFloat(req.query.disk_threshold, 10) || 85;
        const change_rate_threshold = parseFloat(req.query.change_rate_threshold, 10) || 0.5;
        const page = parseInt(req.query.page, 10) || 1;
        const page_size = parseInt(req.query.page_size, 10) || 50;
        const endTime = new Date();
        const startTime = new Date(endTime.getTime() - hours * 60 * 60 * 1000);
        const request = {
            server_name: serverName,
            time_range: {
                start_time: { seconds: Math.floor(startTime.getTime() / 1000), nanos: 0 },
                end_time: { seconds: Math.floor(endTime.getTime() / 1000), nanos: 0 }
            },
            cpu_threshold,
            mem_threshold,
            disk_threshold,
            change_rate_threshold,
            pagination: { page, page_size }
        };
        const response = await promisifyGrpc('QueryAnomaly', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// 趋势聚合
app.get('/api/trend/:serverName', async (req, res) => {
    try {
        const serverName = req.params.serverName;
        const hours = parseFloat(req.query.hours, 10) || 1;
        const scoring_profile = normalizeScoringProfile(req.query.scoring_profile || req.query.profile);
        const interval_seconds = parseInt(req.query.interval_seconds, 10) || 300;
        const endTime = new Date();
        const startTime = new Date(endTime.getTime() - hours * 60 * 60 * 1000);
        const request = {
            server_name: serverName,
            time_range: {
                start_time: { seconds: Math.floor(startTime.getTime() / 1000), nanos: 0 },
                end_time: { seconds: Math.floor(endTime.getTime() / 1000), nanos: 0 }
            },
            interval_seconds,
            scoring_profile
        };
        const response = await promisifyGrpc('QueryTrend', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// 详细数据通用参数：server_name, hours, page, page_size
function detailTimeRange(req) {
    const serverName = req.query.server_name || '';
    const hours = parseFloat(req.query.hours, 10) || 24;
    const page = parseInt(req.query.page, 10) || 1;
    const page_size = parseInt(req.query.page_size, 10) || 50;
    const endTime = new Date();
    const startTime = new Date(endTime.getTime() - hours * 60 * 60 * 1000);
    return {
        server_name: serverName,
        time_range: {
            start_time: { seconds: Math.floor(startTime.getTime() / 1000), nanos: 0 },
            end_time: { seconds: Math.floor(endTime.getTime() / 1000), nanos: 0 }
        },
        pagination: { page, page_size }
    };
}

app.get('/api/net-detail', async (req, res) => {
    try {
        const request = detailTimeRange(req);
        const response = await promisifyGrpc('QueryNetDetail', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

app.get('/api/disk-detail', async (req, res) => {
    try {
        const request = detailTimeRange(req);
        const response = await promisifyGrpc('QueryDiskDetail', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

app.get('/api/mem-detail', async (req, res) => {
    try {
        const request = detailTimeRange(req);
        const response = await promisifyGrpc('QueryMemDetail', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

app.get('/api/softirq-detail', async (req, res) => {
    try {
        const request = detailTimeRange(req);
        const response = await promisifyGrpc('QuerySoftIrqDetail', request);
        res.json(response);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});


// Historical Incident query and one-click analysis; all results originate in Manager.
const clampInt = (raw, def, min, max) => Number.isFinite(Number(raw))
    ? Math.min(max, Math.max(min, Math.floor(Number(raw)))) : def;
const validIncidentId = raw => /^\d{1,20}$/.test(String(raw || ""));
const getIncident = async (id, host) => {
    const response = await promisifyGrpc('GetIncidentDetail', {incident_id: id});
    if (!response.incident || (host && response.incident.server_name !== host))
        throw Object.assign(new Error('未找到指定主机的故障事件'), {status: 404});
    return response.incident;
};
const replyFailure = (res, error) => res.status(error.status || 502).json({error: error.message});

app.get('/api/incidents', async (req, res) => {
    try {
        const hours = clampInt(req.query.hours, 24, 1, 168);
        const page = clampInt(req.query.page, 1, 1, 1000);
        const pageSize = clampInt(req.query.page_size, 30, 1, 50);
        const end = Math.floor(Date.now() / 1000);
        const response = await promisifyGrpc('GetIncidents', {
            server_name: String(req.query.server_name || '').slice(0, 256),
            time_range: {start_time: {seconds: end - hours * 3600}, end_time: {seconds: end}},
            pagination: {page, page_size: pageSize}
        });
        res.json({incidents: (response.incidents || []).map(i => ({
            id: i.id, server_name: i.server_name, severity: i.severity,
            state: i.state, active: i.active, start_time_ms: i.start_time_ms,
            end_time_ms: i.end_time_ms, root_causes: i.root_causes,
            evidence_count: (i.evidence || []).length
        })), total_count: response.total_count, page, page_size: pageSize});
    } catch(error){replyFailure(res,error);}
});

app.get('/api/incidents/:id', async (req, res) => {
    if(!validIncidentId(req.params.id))return res.status(400).json({error:'无效 Incident ID'});
    try {
        const incident=await getIncident(req.params.id,String(req.query.server_name || ''));
        res.json({incident, report:alerts.report(incident)});
    }catch(error){replyFailure(res,error);}
});

app.post('/api/incidents/:id/analyze', async (req, res) => {
    if(!validIncidentId(req.params.id))return res.status(400).json({error:'无效 Incident ID'});
    try {
        const incident=await getIncident(req.params.id,String(req.body?.server_name || ''));
        const report=await alerts.analyze(incident,req.body?.refresh===true);
        res.json({report});
    }catch(error){replyFailure(res,error);}
});

app.get('/api/alerts', (req, res) => {
    res.json({alerts: alerts.list(String(req.query.server_name || '')), unread_count: alerts.unread()});
});
app.post('/api/alerts/ack', (req, res) => {
    const key=String(req.body?.key || '');
    if(!alerts.acknowledge(key))return res.status(404).json({error:'告警不存在或已过期'});
    res.json({ok:true,unread_count:alerts.unread()});
});

// Get all exposed query endpoints
app.get('/api/query-endpoints', (req, res) => {
    res.json({
        manager_address: MANAGER_ADDRESS,
        dashboard_address: `0.0.0.0:${PORT}`,
        endpoints: QUERY_ENDPOINTS
    });
});

// Socket.io for real-time pushing (Polling manager every 2s)
setInterval(async () => {
    try {
        const response = await promisifyGrpc('QueryLatestScore', {});
        const enriched = await enrichOverviewWithNetRates(response);
        io.emit('overview_update', enriched);
    } catch (err) {
        // Suppress logs for polling errors to avoid spam if manager is down
    }
}, 2000);

// Begin listening for new incident episodes, emitting deduplicated socket alerts.
alerts.start();

// Default route
app.get('*', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

server.listen(PORT, process.env.HOST || '127.0.0.1', () => {
    console.log(`Dashboard server running at http://0.0.0.0:${PORT}`);
    console.log(`Connecting to gRPC Manager at ${MANAGER_ADDRESS}`);
});
