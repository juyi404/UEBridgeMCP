import { spawn } from "node:child_process";

const [hostExecutable, codexExecutable, projectRoot, mcpUrl = "http://127.0.0.1:16722/mcp"] = process.argv.slice(2);
if (!hostExecutable || !codexExecutable || !projectRoot) {
  console.error("usage: node protocol-probe.mjs <host> <codex> <project-root> [mcp-url]");
  process.exit(2);
}

const mcpToken = process.env.WORLDDATA_MCP_TOKEN ?? "protocol-probe-secret";
const childEnvironment = { ...process.env };
delete childEnvironment.WORLDDATA_MCP_TOKEN;

const child = spawn(hostExecutable, [], {
  cwd: projectRoot,
  env: childEnvironment,
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true,
});

const seen = [];
let normalizedThreadStatuses = true;
let threadRecencyFields = true;
let sortedByRecency = true;
let ownedThreadSources = true;
let listedThreadCount = 0;
let stdout = "";
let stderr = "";
let finished = false;
let timeout;
let sessionId = "";
let lastSequence = 0;

function send(value) {
  child.stdin.write(`${JSON.stringify(value)}\n`);
}

function finish(code) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  console.log(JSON.stringify({ code, seen, listedThreadCount, stderrBytes: Buffer.byteLength(stderr) }));
  process.exit(code);
}

child.stdout.on("data", (chunk) => {
  stdout += chunk.toString("utf8");
  let newline;
  while ((newline = stdout.indexOf("\n")) >= 0) {
    const line = stdout.slice(0, newline).trim();
    stdout = stdout.slice(newline + 1);
    if (!line) continue;
    const message = JSON.parse(line);
    if (message.protocolVersion !== 2 || typeof message.sessionId !== "string" || message.sequence <= lastSequence) {
      console.error("Agent Host v2 event envelope is invalid or out of order.");
      child.kill();
      finish(1);
      return;
    }
    sessionId ||= message.sessionId;
    if (sessionId !== message.sessionId) {
      console.error("Agent Host changed session id during a process lifetime.");
      child.kill();
      finish(1);
      return;
    }
    lastSequence = message.sequence;
    seen.push(message.type);
    if (message.type === "error") {
      console.error(JSON.stringify(message.payload));
      child.kill();
      finish(1);
    }
    if (message.type === "connect.completed") {
      send({ protocolVersion: 2, id: "list-threads", type: "listThreads", payload: { limit: 10 } });
    }
    if (message.type === "threads.listed") {
      const threads = Array.isArray(message.payload?.threads) ? message.payload.threads : [];
      listedThreadCount = threads.length;
      const effectiveRecencyAt = thread => Number(thread.recencyAt || thread.updatedAt || thread.createdAt || 0);
      normalizedThreadStatuses = threads.every(thread => {
        if (typeof thread.status !== "string") return false;
        const status = thread.status.trim();
        return !status.startsWith("{") && !status.startsWith("[");
      });
      if (!normalizedThreadStatuses) {
        console.error("Agent Host leaked a version-specific Codex thread status object.");
        child.kill();
        finish(1);
        return;
      }
      threadRecencyFields = threads.every(thread => typeof thread.recencyAt === "number");
      ownedThreadSources = threads.every(thread => thread.threadSource === "worlddata-unreal-agent");
      sortedByRecency = threads.every((thread, index) => index === 0
        || effectiveRecencyAt(threads[index - 1]) >= effectiveRecencyAt(thread));
      if (!threadRecencyFields || !sortedByRecency || !ownedThreadSources) {
        console.error("Agent Host returned unowned or incorrectly ordered thread summaries.");
        child.kill();
        finish(1);
        return;
      }
      send({ protocolVersion: 2, id: "shutdown", type: "shutdown", payload: {} });
    }
  }
});

child.stderr.on("data", (chunk) => {
  stderr += chunk.toString("utf8");
});

child.on("exit", (code) => {
  const ok = code === 0
    && seen.includes("host.started")
    && seen.includes("connect.completed")
    && seen.includes("threads.listed")
    && seen.includes("host.stopping")
    && normalizedThreadStatuses
    && threadRecencyFields
    && sortedByRecency
    && ownedThreadSources;
  finish(ok ? 0 : 1);
});

send({
  protocolVersion: 2,
  id: "connect",
  type: "connect",
  payload: {
    codexExecutable,
    projectRoot,
    mcpUrl,
    mcpToken,
    clientVersion: "0.3.0",
  },
});

timeout = setTimeout(() => {
  child.kill();
  finish(124);
}, 30_000);
