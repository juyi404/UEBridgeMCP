import { spawn } from "node:child_process";

const [hostExecutable, projectRoot = process.cwd()] = process.argv.slice(2);
if (!hostExecutable) {
  console.error("usage: node fault-probe.mjs <host> [project-root]");
  process.exit(2);
}

const childEnvironment = { ...process.env };
delete childEnvironment.WORLDDATA_MCP_TOKEN;

const child = spawn(hostExecutable, [], {
  cwd: projectRoot,
  env: childEnvironment,
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true,
});

const observed = [];
let stdout = "";
let stderr = "";
let started = false;
let finished = false;
let sessionId = "";
let lastSequence = 0;
let v2EnvelopeValid = true;

function send(value) {
  child.stdin.write(`${JSON.stringify(value)}\n`);
}

function runCases() {
  child.stdin.write("this-is-not-json\n");
  send({ protocolVersion: 999, id: "version", type: "shutdown", payload: {} });
  send({ protocolVersion: 2, id: "not-connected", type: "listThreads", payload: { limit: 1 } });
  send({ protocolVersion: 2, id: "unknown", type: "notACommand", payload: {} });
  send({
    protocolVersion: 2,
    id: "missing-token",
    type: "connect",
    payload: {
      codexExecutable: "C:\\missing-codex.exe",
      projectRoot: process.cwd(),
      mcpUrl: "http://127.0.0.1:1/mcp",
      clientVersion: "fault-probe",
    },
  });
  send({
    protocolVersion: 2,
    id: "explicit-token",
    type: "connect",
    payload: {
      codexExecutable: "C:\\missing-codex.exe",
      projectRoot: process.cwd(),
      mcpUrl: "http://127.0.0.1:1/mcp",
      mcpToken: "fault-probe-token",
      clientVersion: "fault-probe",
    },
  });
  child.stdin.write(`${"x".repeat(1024 * 1024 + 1)}\n`);
  send({ protocolVersion: 2, id: "shutdown", type: "shutdown", payload: {} });
}

function finish(code, reason = "") {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  console.log(JSON.stringify({ code, reason, observed, stderrBytes: Buffer.byteLength(stderr) }));
  process.exit(code);
}

child.stdout.on("data", chunk => {
  stdout += chunk.toString("utf8");
  let newline;
  while ((newline = stdout.indexOf("\n")) >= 0) {
    const line = stdout.slice(0, newline).trim();
    stdout = stdout.slice(newline + 1);
    if (!line) continue;
    const message = JSON.parse(line);
    if (message.protocolVersion !== 2
      || typeof message.sessionId !== "string"
      || !message.sessionId
      || !Number.isInteger(message.sequence)
      || message.sequence <= lastSequence
      || (sessionId && sessionId !== message.sessionId)) {
      v2EnvelopeValid = false;
    }
    sessionId ||= message.sessionId;
    lastSequence = message.sequence;
    observed.push({ type: message.type, requestId: message.requestId ?? null, code: message.payload?.code ?? null });
    if (message.type === "host.started" && !started) {
      started = true;
      runCases();
    }
  }
});

child.stderr.on("data", chunk => { stderr += chunk.toString("utf8"); });

child.on("exit", code => {
  const errors = observed.filter(item => item.type === "error");
  const expected = [
    [null, "host.invalid_json"],
    ["version", "host.protocol_mismatch"],
    ["not-connected", "host.not_connected"],
    ["unknown", "host.invalid_command"],
    ["missing-token", "host.invalid_command"],
    ["explicit-token", "runtime.not_found"],
    [null, "host.frame_too_large"],
  ];
  const matches = expected.every(([requestId, errorCode]) =>
    errors.some(item => item.requestId === requestId && item.code === errorCode));
  const stopped = observed.some(item => item.type === "host.stopping" && item.requestId === "shutdown");
  finish(code === 0 && matches && stopped && v2EnvelopeValid && stderr.length === 0 ? 0 : 1,
    matches && stopped && v2EnvelopeValid ? "" : "fault contract mismatch");
});

const timeout = setTimeout(() => {
  child.kill();
  finish(124, "timeout");
}, 15_000);
