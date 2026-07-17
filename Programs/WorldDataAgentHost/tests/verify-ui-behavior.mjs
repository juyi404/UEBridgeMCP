import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const testsDirectory = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(testsDirectory, "../../..");
const standaloneWebRoot = resolve(projectRoot, "Resources/Web");
const embeddedWebRoot = resolve(projectRoot, "Plugins/UEBridgeMCP/Resources/Web");
const webRoot = existsSync(standaloneWebRoot) ? standaloneWebRoot : embeddedWebRoot;
const script = readFileSync(resolve(webRoot, "app.js"), "utf8");
const failures = [];

class FakeElement {
  constructor() {
    this.className = "";
    this.innerHTML = "";
    this.textContent = "";
    this.value = "";
    this.disabled = false;
    this.scrollHeight = 100;
    this.scrollTop = 0;
    this.clientHeight = 100;
    this.listeners = new Map();
    this.attributes = new Map();
    this.classList = { toggle: () => {} };
  }

  addEventListener(type, listener) { this.listeners.set(type, listener); }
  setAttribute(name, value) { this.attributes.set(name, value); }
  focus() {}
  requestSubmit() { return this.listeners.get("submit")?.({ preventDefault() {} }); }
}

const ids = [
  "chat-view", "settings-view", "page-title", "connection-line", "ready-pill", "thread-list", "thread-count",
  "message-list", "chat-error", "composer", "message-input", "model-select", "send", "settings-content",
  "approval-modal", "approval-title", "approval-text", "nav-chat", "nav-settings", "composer-footer",
  "composer-status", "top-settings", "back-chat", "refresh", "new-chat", "deny", "approve"
];
const elements = Object.fromEntries(ids.map(id => [id, new FakeElement()]));
elements["model-select"].value = "gpt-5";
const timers = [];
const hostConsole = console;

const state = {
  activeThreadId: "thread-1",
  selectedModel: "gpt-5",
  busy: false,
  configuring: false,
  connection: { state: "ready", statusText: "ready", authenticated: true, mcpConnected: true, models: [{ id: "gpt-5", displayName: "GPT-5", isDefault: true }] },
  runtime: { configured: true, verified: true },
  tools: [],
  threads: [{ id: "thread-1", title: "Regression", status: "running", recencyAt: 1 }],
  conversation: [
    { id: "tool-1", turnId: "turn-1", kind: "tool", role: "tool", toolName: "commandExecution", status: "completed", text: "Get-ChildItem" },
    { id: "assistant-1", turnId: "turn-1", kind: "message", role: "assistant", text: "alpha" }
  ],
  approval: { pending: false },
  error: { present: false, code: "", message: "" }
};

globalThis.document = {
  querySelector(selector) {
    if (!selector.startsWith("#")) return null;
    return elements[selector.slice(1)] ?? null;
  },
  querySelectorAll(selector) {
    return selector === "[data-route]" ? [elements["nav-chat"], elements["nav-settings"]] : [];
  }
};
globalThis.window = {
  ue: {
    worlddata: {
      getstate: async () => JSON.stringify(state),
      sendmessage: async () => false,
      refreshthreads: async () => true,
      newconversation: async () => true,
      resumethread: async () => true,
      configureruntime: async () => true,
      resolveapproval: async () => true
    }
  }
};
globalThis.setInterval = callback => { timers.push(callback); return timers.length; };
globalThis.console = { error() {}, log: hostConsole.log, warn() {}, info() {} };

new Function(script)();
const settle = async () => { await Promise.resolve(); await Promise.resolve(); await Promise.resolve(); };
await settle();

if (!elements["message-list"].innerHTML.includes("Get-ChildItem")
    || !elements["message-list"].innerHTML.includes("tool-status")) {
  failures.push("tool details and status must render independently");
}

state.conversation[1].text = "bravo";
await timers[0]?.();
await settle();
if (!elements["message-list"].innerHTML.includes("bravo")) {
  failures.push("same-length conversation updates must rerender");
}

elements["message-input"].value = "保留这条草稿";
await elements.composer.requestSubmit();
await settle();
if (elements["message-input"].value !== "保留这条草稿") {
  failures.push("a rejected send must retain the draft");
}
if (!elements["chat-error"].textContent.includes("发送消息失败")) {
  failures.push("a rejected send must be visible in the chat error area");
}

hostConsole.log(JSON.stringify({ ok: failures.length === 0, checks: 4, failures }));
process.exit(failures.length === 0 ? 0 : 1);
