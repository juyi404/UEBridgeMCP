import { spawn } from "node:child_process";

const [hostExecutable, codexExecutable, projectRoot, mcpUrl] = process.argv.slice(2);
if (!hostExecutable || !codexExecutable || !projectRoot || !mcpUrl || !process.env.WORLDDATA_MCP_TOKEN) {
  console.error("usage: set WORLDDATA_MCP_TOKEN and pass <host> <codex> <project-root> <mcp-url>");
  process.exit(2);
}

const child = spawn(hostExecutable, [], {
  cwd: projectRoot,
  env: process.env,
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true,
});

const seen = [];
let stdout = "";
let stderrBytes = 0;
let responseText = "";
let mcpConnected = false;
let mcpError = "";
let toolObserved = false;
let finished = false;
let timeout;
const diagnostics = [];

function send(id, type, payload) {
  child.stdin.write(`${JSON.stringify({ protocolVersion: 1, id, type, payload })}\n`);
}

function finish(code) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  console.log(JSON.stringify({
    code,
    seen,
    mcpConnected,
    mcpError,
    toolObserved,
    responseContainsProjectName: responseText.includes("CollectWorldData"),
    stderrBytes,
    diagnostics: diagnostics.slice(-12),
  }));
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
    seen.push(message.type);
    if (message.type === "error") {
      console.error(JSON.stringify({
        code: message.payload?.code,
        component: message.payload?.component,
        message: message.payload?.message,
      }));
      child.kill();
      finish(1);
    } else if (message.type === "diagnostic") {
      diagnostics.push({ code: message.payload?.errorCode, text: message.payload?.text });
    } else if (message.type === "connect.completed") {
      send("create-thread", "createThread", {
        clientConversationId: crypto.randomUUID(),
        workingDirectory: projectRoot,
        approvalPolicy: "never",
        sandboxMode: "read-only",
        ephemeral: true,
      });
    } else if (message.type === "thread.created") {
      mcpConnected = message.payload?.mcp?.connected === true;
      mcpError = message.payload?.mcp?.error ?? "";
      if (!mcpConnected) {
        child.kill();
        finish(1);
        return;
      }
      send("send-turn", "sendTurn", {
        threadId: message.payload.threadId,
        clientTurnId: crypto.randomUUID(),
        text: "Call the WorldData MCP tool worlddata.get_project_info exactly once, then reply with only the projectName.",
      });
    } else if (message.type === "message.delta") {
      responseText += message.payload?.text ?? "";
    } else if (message.type === "item.started" || message.type === "item.completed") {
      toolObserved ||= (message.payload?.toolName ?? "").includes("worlddata.get_project_info");
    } else if (message.type === "turn.completed") {
      send("shutdown", "shutdown", {});
    }
  }
});

child.stderr.on("data", (chunk) => {
  stderrBytes += chunk.length;
});

child.on("exit", (code) => {
  const ok = code === 0 && mcpConnected && toolObserved && responseText.includes("CollectWorldData");
  finish(ok ? 0 : 1);
});

send("connect", "connect", {
  codexExecutable,
  projectRoot,
  mcpUrl,
  clientVersion: "0.3.0",
});

timeout = setTimeout(() => {
  child.kill();
  finish(124);
}, 120_000);
