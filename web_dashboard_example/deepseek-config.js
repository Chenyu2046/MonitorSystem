"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");

const configRoot = process.env.APPDATA
  ? path.join(process.env.APPDATA, "KernScope")
  : path.join(process.env.XDG_CONFIG_HOME || path.join(os.homedir(), ".config"), "KernScope");
const configPath = path.join(configRoot, "deepseek-api-key.json");
const startupEnvironmentKey = process.env.DEEPSEEK_API_KEY || "";
let source = startupEnvironmentKey ? "environment" : "none";

function load() {
  try {
    const saved = JSON.parse(fs.readFileSync(configPath, "utf8"));
    if (typeof saved.apiKey === "string" && saved.apiKey.trim()) {
      process.env.DEEPSEEK_API_KEY = saved.apiKey;
      source = "local";
      return;
    }
  } catch (error) {
    if (error.code !== "ENOENT") {
      console.warn("DeepSeek key config could not be read; configure it again in Settings.");
    }
  }

  if (startupEnvironmentKey) {
    process.env.DEEPSEEK_API_KEY = startupEnvironmentKey;
    source = "environment";
  }
}

function save(apiKey) {
  fs.mkdirSync(configRoot, { recursive: true, mode: 0o700 });
  const tempPath = `${configPath}.${process.pid}.${Date.now()}.tmp`;
  let tempFd;
  let ownsTemp = false;
  try {
    tempFd = fs.openSync(tempPath, "wx", 0o600);
    ownsTemp = true;
    fs.writeFileSync(tempFd, JSON.stringify({ apiKey }), "utf8");
    fs.closeSync(tempFd);
    tempFd = undefined;
    fs.renameSync(tempPath, configPath);
  } finally {
    if (tempFd !== undefined) {
      try { fs.closeSync(tempFd); } catch (_) { /* Preserve the original write error. */ }
    }
    if (ownsTemp) {
      try { fs.unlinkSync(tempPath); } catch (error) { if (error.code !== "ENOENT") throw error; }
    }
  }
  try { fs.chmodSync(configPath, 0o600); } catch (_) { /* Windows uses the profile directory ACL. */ }
  process.env.DEEPSEEK_API_KEY = apiKey;
  source = "local";
}

function clear() {
  try { fs.unlinkSync(configPath); } catch (error) { if (error.code !== "ENOENT") throw error; }
  if (startupEnvironmentKey) {
    process.env.DEEPSEEK_API_KEY = startupEnvironmentKey;
    source = "environment";
  } else {
    delete process.env.DEEPSEEK_API_KEY;
    source = "none";
  }
}

function status() {
  return {
    configured: Boolean(process.env.DEEPSEEK_API_KEY),
    source,
    model: process.env.DEEPSEEK_MODEL || "deepseek-flash"
  };
}

module.exports = { load, save, clear, status };
