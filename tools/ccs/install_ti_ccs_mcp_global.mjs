#!/usr/bin/env node
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const sourcePlugin = path.join(here, "ti-ccs-mcp");
const home = os.homedir();
const targetPlugin = path.join(home, "plugins", "ti-ccs-mcp");
const marketplacePath = path.join(home, ".agents", "plugins", "marketplace.json");
const shimPath = path.join(targetPlugin, "scripts", "ccs-mcp-shim.mjs");

const ccsServers = {
  "ccs-project": "project",
  "ccs-debug": "debug",
  "ccs-sysconfig": "sysconfig",
  "ccs-serial": "serial"
};

function mcpConfigFor(kind) {
  return {
    command: "node",
    args: [shimPath, kind],
    startup_timeout_sec: 120
  };
}

async function readJson(file, fallback) {
  try {
    return JSON.parse(await fs.readFile(file, "utf8"));
  } catch (error) {
    if (error.code === "ENOENT") {
      return fallback;
    }
    throw error;
  }
}

async function writeJson(file, data) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  await fs.writeFile(file, `${JSON.stringify(data, null, 2)}\n`);
}

async function copyPlugin() {
  await fs.mkdir(path.dirname(targetPlugin), { recursive: true });
  await fs.cp(sourcePlugin, targetPlugin, {
    recursive: true,
    force: true,
    dereference: false,
    verbatimSymlinks: true
  });
}

async function updateMarketplace() {
  const marketplace = await readJson(marketplacePath, {
    name: "personal",
    interface: { displayName: "Personal" },
    plugins: []
  });

  marketplace.name ||= "personal";
  marketplace.interface ||= { displayName: "Personal" };
  marketplace.interface.displayName ||= "Personal";
  marketplace.plugins = Array.isArray(marketplace.plugins) ? marketplace.plugins : [];

  const entry = {
    name: "ti-ccs-mcp",
    source: {
      source: "local",
      path: "./plugins/ti-ccs-mcp"
    },
    policy: {
      installation: "AVAILABLE",
      authentication: "ON_INSTALL"
    },
    category: "Developer Tools"
  };

  const existing = marketplace.plugins.findIndex(plugin => plugin?.name === "ti-ccs-mcp");
  if (existing >= 0) {
    marketplace.plugins[existing] = entry;
  } else {
    marketplace.plugins.push(entry);
  }

  await writeJson(marketplacePath, marketplace);
}

async function updateClaudeConfig(file) {
  const config = await readJson(file, {});
  config.mcpServers ||= {};
  for (const [server, kind] of Object.entries(ccsServers)) {
    config.mcpServers[server] = mcpConfigFor(kind);
  }
  await writeJson(file, config);
}

async function main() {
  await copyPlugin();
  await updateMarketplace();
  await updateClaudeConfig(path.join(home, ".claude.json"));
  await updateClaudeConfig(path.join(home, "Library", "Application Support", "Claude", "claude_desktop_config.json"));

  console.log(`Installed plugin source: ${targetPlugin}`);
  console.log(`Updated marketplace: ${marketplacePath}`);
  console.log("Updated Claude MCP configs:");
  console.log(`- ${path.join(home, ".claude.json")}`);
  console.log(`- ${path.join(home, "Library", "Application Support", "Claude", "claude_desktop_config.json")}`);
}

main().catch(error => {
  console.error(error.stack || error.message || String(error));
  process.exit(1);
});
