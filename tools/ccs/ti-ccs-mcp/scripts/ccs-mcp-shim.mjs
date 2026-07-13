#!/usr/bin/env node
import crypto from "node:crypto";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import readline from "node:readline";

const SERVER_NAMES = new Set(["project", "debug", "sysconfig", "serial"]);
const serverName = process.argv[2];

if (!SERVER_NAMES.has(serverName)) {
  console.error(`Usage: node ccs-mcp-shim.mjs <${[...SERVER_NAMES].join("|")}>`);
  process.exit(2);
}

const protocolVersion = "2025-06-18";
const pollMs = numberFromEnv("CCS_MCP_POLL_MS", 1000);
const requestTimeoutMs = numberFromEnv("CCS_MCP_REQUEST_TIMEOUT_MS", 300000);
const ccsPluginDist = process.env.CCS_AI_PLUGIN_DIST ||
  "/Applications/ti/ccs2100/ccs/Code Composer Studio.app/Contents/Resources/app/plugins/ccs-ai/extension/dist";
const portFile = process.env.CCS_AI_PORT_FILE || getPortFilePath(ccsPluginDist);

let backend = null;
let backendPresent = false;
let initialized = false;
let sawInitializedNotification = false;
let nextBackendId = 1;

function numberFromEnv(name, fallback) {
  const raw = process.env[name];
  if (!raw) {
    return fallback;
  }
  const parsed = Number(raw);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function getPortFilePath(pluginDist) {
  let normalized = path.normalize(pluginDist);
  if (process.platform === "win32" || process.platform === "darwin") {
    normalized = normalized.toLowerCase();
  }
  const suffix = crypto.createHash("sha256").update(normalized).digest("hex").slice(0, 7);
  return path.join(os.tmpdir(), "ccs-ai", `ccs-ai-port-${suffix}`);
}

function log(message) {
  if (process.env.CCS_MCP_SHIM_DEBUG === "1") {
    console.error(`[ccs-mcp-${serverName}] ${message}`);
  }
}

function writeMessage(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`);
}

function respond(id, result) {
  writeMessage({ jsonrpc: "2.0", id, result });
}

function respondError(id, code, message, data) {
  const error = data === undefined ? { code, message } : { code, message, data };
  writeMessage({ jsonrpc: "2.0", id, error });
}

function notify(method, params) {
  const message = { jsonrpc: "2.0", method };
  if (params !== undefined) {
    message.params = params;
  }
  writeMessage(message);
}

function toolResult(text, isError = false) {
  return {
    content: [{ type: "text", text }],
    isError
  };
}

function unavailableText() {
  return [
    `CCS ${serverName} MCP backend is not available.`,
    `Start Code Composer Studio and wait for its AI extension to publish ${portFile}.`,
    "This shim does not open, close, restart, or kill CCS."
  ].join("\n");
}

function statusTool() {
  return {
    name: "ccsMcpStatus",
    description: "Report whether the resilient CCS MCP shim is connected to the CCS IDE backend.",
    inputSchema: {
      type: "object",
      properties: {},
      additionalProperties: false
    }
  };
}

function statusPayload() {
  return {
    server: serverName,
    backendAvailable: Boolean(backend?.ready),
    backendUrl: backend?.url ?? null,
    portFile,
    ccsPluginDist,
    safeBehavior: "wait-and-reconnect-only"
  };
}

async function readBackendUrl() {
  const raw = await fs.readFile(portFile, "utf8");
  const data = JSON.parse(raw);
  const entry = data?.servers?.[serverName];
  if (!entry?.serverUrl) {
    throw new Error(`Port file has no ${serverName} server URL`);
  }
  return entry.serverUrl;
}

async function fetchBackend(url, body, sessionId) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), requestTimeoutMs);
  try {
    const headers = {
      "accept": "application/json, text/event-stream",
      "content-type": "application/json"
    };
    if (sessionId) {
      headers["mcp-session-id"] = sessionId;
    }
    const response = await fetch(url, {
      method: "POST",
      headers,
      body: JSON.stringify(body),
      signal: controller.signal
    });
    const text = await response.text();
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${text || response.statusText}`);
    }
    const parsed = text ? parseBackendResponse(text, response.headers.get("content-type") || "") : undefined;
    return {
      sessionId: response.headers.get("mcp-session-id") || sessionId,
      body: parsed
    };
  } finally {
    clearTimeout(timer);
  }
}

function parseBackendResponse(text, contentType) {
  if (contentType.includes("text/event-stream")) {
    const events = [];
    let current = [];
    for (const line of text.split(/\r?\n/)) {
      if (line === "") {
        if (current.length > 0) {
          events.push(current.join("\n"));
          current = [];
        }
      } else if (line.startsWith("data:")) {
        current.push(line.slice(5).trimStart());
      }
    }
    if (current.length > 0) {
      events.push(current.join("\n"));
    }
    const last = events.filter(Boolean).at(-1);
    return last ? JSON.parse(last) : undefined;
  }
  return JSON.parse(text);
}

async function connectBackend() {
  const url = await readBackendUrl();
  if (backend?.ready && backend.url === url) {
    return backend;
  }

  const initId = nextBackendId++;
  const initRequest = {
    jsonrpc: "2.0",
    id: initId,
    method: "initialize",
    params: {
      protocolVersion,
      capabilities: {},
      clientInfo: {
        name: `ccs-mcp-${serverName}-resilient-shim`,
        version: "0.1.0"
      }
    }
  };

  const initResponse = await fetchBackend(url, initRequest);
  if (initResponse.body?.error) {
    throw new Error(initResponse.body.error.message || "Backend initialize failed");
  }
  if (!initResponse.body?.result) {
    throw new Error("Backend initialize returned no result");
  }

  const sessionId = initResponse.sessionId;
  await fetchBackend(url, { jsonrpc: "2.0", method: "notifications/initialized" }, sessionId);

  backend = {
    url,
    sessionId,
    ready: true,
    initializeResult: initResponse.body.result
  };
  return backend;
}

async function ensureBackend() {
  if (backend?.ready) {
    return backend;
  }
  return await connectBackend();
}

function clearBackend(reason) {
  if (backend?.ready) {
    log(`Backend cleared: ${reason}`);
  }
  backend = null;
}

async function forwardToBackend(request) {
  const active = await ensureBackend();
  const response = await fetchBackend(active.url, request, active.sessionId);
  active.sessionId = response.sessionId || active.sessionId;
  if (response.body?.error) {
    throw new Error(response.body.error.message || JSON.stringify(response.body.error));
  }
  return response.body?.result;
}

async function listWithStatus(method, fallbackKey) {
  try {
    const result = await forwardToBackend({ jsonrpc: "2.0", id: nextBackendId++, method });
    if (method === "tools/list") {
      return {
        ...result,
        tools: [statusTool(), ...(result?.tools ?? [])]
      };
    }
    return result;
  } catch (error) {
    clearBackend(error.message);
    if (method === "tools/list") {
      return { tools: [statusTool()] };
    }
    return { [fallbackKey]: [] };
  }
}

async function handleRequest(message) {
  const { id, method, params } = message;
  try {
    switch (method) {
      case "initialize":
        initialized = true;
        respond(id, {
          protocolVersion,
          capabilities: {
            tools: { listChanged: true },
            resources: { listChanged: true, subscribe: true }
          },
          serverInfo: {
            name: `ccs-mcp-${serverName}-resilient-shim`,
            version: "0.1.0"
          },
          instructions: [
            `Resilient bridge for the CCS ${serverName} MCP backend.`,
            "The bridge waits for CCS and reconnects without opening, closing, restarting, or killing the IDE."
          ].join("\n")
        });
        break;

      case "ping":
        respond(id, {});
        break;

      case "tools/list":
        respond(id, await listWithStatus("tools/list", "tools"));
        break;

      case "tools/call":
        if (params?.name === "ccsMcpStatus") {
          respond(id, toolResult(JSON.stringify(statusPayload(), null, 2)));
          break;
        }
        respond(id, await forwardToBackend({ jsonrpc: "2.0", id: nextBackendId++, method, params }));
        break;

      case "resources/list":
        respond(id, await listWithStatus("resources/list", "resources"));
        break;

      case "resources/templates/list":
        respond(id, await listWithStatus("resources/templates/list", "resourceTemplates"));
        break;

      case "resources/read":
      case "resources/subscribe":
      case "resources/unsubscribe":
        respond(id, await forwardToBackend({ jsonrpc: "2.0", id: nextBackendId++, method, params }));
        break;

      default:
        respond(id, await forwardToBackend({ jsonrpc: "2.0", id: nextBackendId++, method, params }));
        break;
    }
  } catch (error) {
    clearBackend(error.message);
    if (method === "tools/call") {
      respond(id, toolResult(`${unavailableText()}\n\n${error.message}`, true));
      return;
    }
    respondError(id, -32000, error.message || String(error));
  }
}

async function handleNotification(message) {
  if (message.method === "notifications/initialized") {
    sawInitializedNotification = true;
    return;
  }
  // Backend requests use shim-local ids, so a client cancellation id cannot
  // be forwarded verbatim. The backend request remains bounded by its timeout.
  if (message.method === "$/cancelRequest") {
    return;
  }
  try {
    await forwardToBackend(message);
  } catch (error) {
    clearBackend(error.message);
  }
}

async function handleMessage(message) {
  if (message.id !== undefined) {
    await handleRequest(message);
  } else {
    await handleNotification(message);
  }
}

async function pollBackendPresence() {
  try {
    await connectBackend();
    if (!backendPresent) {
      backendPresent = true;
      if (initialized && sawInitializedNotification) {
        notify("notifications/tools/list_changed");
        notify("notifications/resources/list_changed");
      }
    }
  } catch (error) {
    clearBackend(error.message);
    if (backendPresent) {
      backendPresent = false;
      if (initialized && sawInitializedNotification) {
        notify("notifications/tools/list_changed");
        notify("notifications/resources/list_changed");
      }
    }
  }
}

const lines = readline.createInterface({
  input: process.stdin,
  crlfDelay: Infinity
});

lines.on("line", line => {
  if (line.trim().length === 0) {
    return;
  }
  try {
    const message = JSON.parse(line);
    handleMessage(message).catch(error => {
      log(`Unhandled message error: ${error.message}`);
    });
  } catch (error) {
    log(`Invalid JSON-RPC message: ${error.message}`);
  }
});

lines.on("close", () => process.exit(0));
process.on("SIGINT", () => process.exit(0));
process.on("SIGTERM", () => process.exit(0));

setInterval(() => {
  pollBackendPresence().catch(error => log(`poll failed: ${error.message}`));
}, pollMs).unref();

pollBackendPresence().catch(error => log(`initial poll failed: ${error.message}`));
