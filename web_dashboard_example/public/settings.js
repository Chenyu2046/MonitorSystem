const KernSettingsPanel = () => {
    const {Box, Paper, Typography, TextField, IconButton, InputAdornment, Button, Chip} = MaterialUI;
    const [status, setStatus] = React.useState(null);
    const [apiKey, setApiKey] = React.useState("");
    const [showKey, setShowKey] = React.useState(false);
    const [busy, setBusy] = React.useState(false);
    const [notice, setNotice] = React.useState(null);

    const loadStatus = React.useCallback(async () => {
        try {
            const response = await axios.get("/api/settings/deepseek");
            setStatus(response.data);
        } catch (error) {
            setNotice({kind: "error", text: error?.response?.data?.error || "读取 API 配置失败。"});
        }
    }, []);

    React.useEffect(() => { loadStatus(); }, [loadStatus]);

    const saveKey = async (event) => {
        event.preventDefault();
        const value = apiKey.trim();
        if (!value || busy) return;
        setBusy(true);
        setNotice(null);
        try {
            const response = await axios.put("/api/settings/deepseek", {apiKey: value});
            setStatus(response.data);
            setApiKey("");
            setShowKey(false);
            setNotice({kind: "success", text: "API Key 已保存在本机，后续 AI 请求将使用新配置。"});
        } catch (error) {
            setNotice({kind: "error", text: error?.response?.data?.error || "保存失败，请稍后重试。"});
        } finally {
            setBusy(false);
        }
    };

    const clearKey = async () => {
        if (busy || !status?.configured) return;
        setBusy(true);
        setNotice(null);
        try {
            const response = await axios.delete("/api/settings/deepseek");
            setStatus(response.data);
            setApiKey("");
            setNotice({kind: "success", text: response.data.configured
                ? "本机配置已移除，启动环境变量仍在生效。"
                : "本机 API Key 已移除。"});
        } catch (error) {
            setNotice({kind: "error", text: error?.response?.data?.error || "移除失败，请稍后重试。"});
        } finally {
            setBusy(false);
        }
    };

    const sourceLabel = status?.source === "environment" ? "由启动环境变量提供" : "保存在当前用户配置目录";

    return (
        <Box className="settings-page">
            <Box className="settings-intro">
                <Typography variant="h5">API 配置</Typography>
                <Typography variant="body2">配置智能诊断使用的模型服务。</Typography>
            </Box>
            <Paper component="section" className="settings-panel" aria-labelledby="deepseek-settings-title">
                <Box className="settings-provider-head">
                    <Box className="settings-provider-icon" aria-hidden="true">
                        <span className="material-icons">key</span>
                    </Box>
                    <Box className="settings-provider-copy">
                        <Typography id="deepseek-settings-title" variant="subtitle1">DeepSeek</Typography>
                        <Typography variant="body2">用于事件分析与智能问答</Typography>
                    </Box>
                    <Chip
                        size="small"
                        className={`settings-status ${status?.configured ? "is-configured" : "is-empty"}`}
                        label={status === null ? "检查中" : status.configured ? "已配置" : "未配置"}
                        icon={<span className="material-icons" aria-hidden="true">{status?.configured ? "check_circle" : "circle"}</span>}
                    />
                </Box>

                <Box component="form" className="settings-key-form" onSubmit={saveKey}>
                    <Typography className="settings-field-label">API Key</Typography>
                    <TextField
                        fullWidth
                        size="small"
                        type={showKey ? "text" : "password"}
                        value={apiKey}
                        onChange={event => setApiKey(event.target.value)}
                        placeholder={status?.configured ? "输入新 Key 以替换当前配置" : "粘贴 DeepSeek API Key"}
                        autoComplete="new-password"
                        inputProps={{maxLength: 512, spellCheck: false, "aria-label": "DeepSeek API Key"}}
                        InputProps={{
                            endAdornment: (
                                <InputAdornment position="end">
                                    <IconButton
                                        size="small"
                                        type="button"
                                        aria-label={showKey ? "隐藏 API Key" : "显示 API Key"}
                                        onClick={() => setShowKey(value => !value)}
                                    >
                                        <span className="material-icons" aria-hidden="true">{showKey ? "visibility_off" : "visibility"}</span>
                                    </IconButton>
                                </InputAdornment>
                            )
                        }}
                    />
                    <Typography className="settings-field-help">
                        {status?.source === "environment"
                            ? "当前 Key 来自启动环境变量。保存新 Key 可改用本机配置；清除环境配置需从服务启动项移除 DEEPSEEK_API_KEY。"
                            : "已保存的 Key 不会回显。输入新值并保存即可替换当前配置。"}
                    </Typography>
                    <Box className="settings-actions">
                        <Button type="submit" variant="contained" disabled={busy || !apiKey.trim()}>
                            {busy ? "保存中…" : "保存 API Key"}
                        </Button>
                        <Button type="button" variant="text" disabled={busy || !status?.configured || status?.source !== "local"} onClick={clearKey}>
                            移除配置
                        </Button>
                    </Box>
                </Box>

                {notice && <Typography role={notice.kind === "error" ? "alert" : "status"}
                    className={`settings-notice is-${notice.kind}`}>{notice.text}</Typography>}

                <Box className="settings-security-note">
                    <span className="material-icons" aria-hidden="true">shield</span>
                    <Typography variant="body2">
                        Key 只提交给本机 Dashboard 服务端并保存在当前用户配置目录；页面不会读取或回显已保存的值。
                    </Typography>
                </Box>
                <Box className="settings-detail-row">
                    <Typography variant="caption">当前模型：{status?.model || "deepseek-flash"}</Typography>
                    {status?.configured && <Typography variant="caption">{sourceLabel}</Typography>}
                </Box>
            </Paper>
        </Box>
    );
};

window.KernSettingsPanel = KernSettingsPanel;
