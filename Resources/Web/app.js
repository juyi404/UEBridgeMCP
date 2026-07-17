(() => {
  "use strict";

  const elements = Object.freeze({
    chatView: document.querySelector("#chat-view"), settingsView: document.querySelector("#settings-view"),
    pageTitle: document.querySelector("#page-title"), connectionLine: document.querySelector("#connection-line"),
    readyPill: document.querySelector("#ready-pill"), threadList: document.querySelector("#thread-list"),
    threadCount: document.querySelector("#thread-count"), messageList: document.querySelector("#message-list"),
    chatError: document.querySelector("#chat-error"), composer: document.querySelector("#composer"),
    messageInput: document.querySelector("#message-input"), modelSelect: document.querySelector("#model-select"),
    send: document.querySelector("#send"), settingsContent: document.querySelector("#settings-content"),
    approvalModal: document.querySelector("#approval-modal"), approvalTitle: document.querySelector("#approval-title"),
    approvalText: document.querySelector("#approval-text"), navChat: document.querySelector("#nav-chat"),
    navSettings: document.querySelector("#nav-settings")
  });

  let route = "chat";
  let state = null;
  let polling = false;
  let previousConversationSignature = "";

  const escapeHtml = (value = "") => String(value).replace(/[&<>"']/g, char => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"})[char]);
  const bridge = () => window.ue && window.ue.worlddata;
  async function invoke(method, ...args) {
    const api = bridge();
    if (!api || typeof api[method] !== "function") throw new Error("Unreal bridge is not ready");
    return await api[method](...args);
  }

  function setRoute(next) {
    route = next;
    const settings = route === "settings";
    elements.chatView.classList.toggle("hidden", settings);
    elements.settingsView.classList.toggle("hidden", !settings);
    elements.navChat.classList.toggle("active", !settings);
    elements.navSettings.classList.toggle("active", settings);
    elements.pageTitle.textContent = settings ? "设置" : "Codex 会话";
    if (state) render(state);
  }

  function connectionLabel(connection) {
    if (connection.state === "ready") return "Codex 与 WorldData MCP 已就绪";
    if (connection.state === "degraded" || connection.state === "fatal") return connection.statusText || "连接异常";
    return connection.statusText || "正在初始化 Agent Host…";
  }

  function relativeTime(timestamp) {
    if (!timestamp) return "";
    const seconds = Math.max(0, Math.floor(Date.now() / 1000 - timestamp));
    if (seconds < 60) return "刚刚";
    if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时`;
    return new Date(timestamp * 1000).toLocaleDateString("zh-CN", {month:"numeric", day:"numeric"});
  }

  function renderConnection(next) {
    const connection = next.connection;
    elements.connectionLine.className = `connection-line ${connection.state === "ready" ? "ready" : (connection.state === "fatal" || connection.state === "degraded" ? "error" : "")}`;
    elements.connectionLine.innerHTML = `<span class="dot"></span><span>${escapeHtml(connectionLabel(connection))}</span>`;
    const ready = connection.state === "ready";
    const failed = connection.state === "fatal" || connection.state === "degraded";
    elements.readyPill.className = `pill ${ready ? "ready" : (failed ? "error" : "neutral")}`;
    elements.readyPill.textContent = ready ? "● 已就绪" : (failed ? "● 需要处理" : "● 连接中");
  }

  function renderThreads(next) {
    elements.threadCount.textContent = `${next.threads.length} 个会话`;
    elements.threadList.innerHTML = next.threads.map(thread => {
      const title = thread.title || thread.preview || "新对话";
      return `<button class="thread-item ${thread.id === next.activeThreadId ? "active" : ""}" data-thread-id="${escapeHtml(thread.id)}"><span class="thread-title">${escapeHtml(title)}</span><span class="thread-meta"><span>${escapeHtml(thread.status || "")}</span><span>${relativeTime(thread.updatedAt)}</span></span></button>`;
    }).join("") || `<div class="thread-meta" style="padding:12px">暂无本项目会话</div>`;
  }

  function renderModels(next) {
    const current = elements.modelSelect.value || next.selectedModel;
    const models = next.connection.models || [];
    elements.modelSelect.innerHTML = models.map(model => `<option value="${escapeHtml(model.id)}" ${model.id === current || (!current && model.isDefault) ? "selected" : ""}>${escapeHtml(model.displayName || model.id)}</option>`).join("");
  }

  function renderConversation(next) {
    const signature = JSON.stringify((next.conversation || []).map(item => [item.id, item.status, item.text && item.text.length]));
    if (signature === previousConversationSignature) return;
    const wasNearBottom = elements.messageList.scrollHeight - elements.messageList.scrollTop - elements.messageList.clientHeight < 100;
    previousConversationSignature = signature;
    if (!next.conversation.length) {
      elements.messageList.innerHTML = `<div class="empty-state"><strong>今天我能帮您做什么？</strong><span>连接完成后，可直接操作当前 Unreal 项目。</span></div>`;
      return;
    }
    elements.messageList.innerHTML = next.conversation.map(item => {
      if (item.kind === "tool") return `<div class="tool-card ${escapeHtml(item.status)}"><span class="tool-state"></span><strong>${escapeHtml(item.toolName || "工具调用")}</strong><span>${escapeHtml(item.status || item.text || "")}</span></div>`;
      const role = item.role === "user" ? "user" : "assistant";
      return `<article class="message ${role}"><div class="message-label">${role === "user" ? "你" : "CODEX"}</div>${escapeHtml(item.text)}</article>`;
    }).join("");
    if (wasNearBottom) elements.messageList.scrollTop = elements.messageList.scrollHeight;
  }

  function renderSettings(next) {
    const runtime = next.runtime;
    const connection = next.connection;
    const configured = runtime.configured && runtime.verified;
    elements.settingsContent.innerHTML = `
      <article class="settings-card">
        <div class="settings-card-head"><div><h3>Codex 后端运行时</h3><p>自动发现或安装受支持的原生 Codex，生成协议 schema，并按 SHA-256 固定到当前项目。</p></div><span class="pill ${configured ? "ready" : "neutral"}">${configured ? "已配置" : "未配置"}</span></div>
        <div class="status-box"><div class="status-copy"><strong>${configured ? "配置已保存，后续启动自动使用" : "点击一次即可完成发现、复制、校验与连接"}</strong><code>${escapeHtml(runtime.manifestPath || "尚未生成运行时清单")}</code></div><button id="configure-runtime" class="send-button" ${next.configuring ? "disabled" : ""}>${next.configuring ? "正在配置…" : (configured ? "重新校验配置" : "一键自动配置")}</button></div>
        <div class="hash-grid"><div class="hash-cell"><span>Agent Host · ${escapeHtml(runtime.hostVersion || "待配置")}</span><code>${escapeHtml(runtime.hostSha256 || "—")}</code></div><div class="hash-cell"><span>Codex · ${escapeHtml(runtime.codexVersion || "待配置")}</span><code>${escapeHtml(runtime.codexSha256 || "—")}</code></div></div>
      </article>
      <article class="settings-card">
        <div class="settings-card-head"><div><h3>WorldData MCP</h3><p>每个 Codex 线程自动注入本地 MCP 地址和临时认证头；密钥不会进入页面、配置文件或日志。</p></div><span class="pill ${connection.mcpConnected ? "ready" : "neutral"}">${connection.mcpConnected ? "线程已连接" : "等待会话"}</span></div>
        <div class="status-box"><div class="status-copy"><strong>${escapeHtml(connectionLabel(connection))}</strong><code>Agent Host 协议 v1 · 本地 JSONL · 线程级 MCP</code></div><button id="refresh-settings" class="button">刷新状态</button></div>
      </article>
      <article class="settings-card">
        <div class="settings-card-head"><div><h3>账户与模型</h3><p>账户状态和模型列表直接来自 Codex app-server，不在面板中保存登录凭据或硬编码模型。</p></div><span class="pill ${connection.authenticated ? "ready" : "error"}">${connection.authenticated ? "已认证" : "需要登录 Codex"}</span></div>
        <div class="status-box"><div class="status-copy"><strong>${(connection.models || []).length} 个可用模型</strong><code>${escapeHtml((connection.models || []).map(model => model.displayName || model.id).join(" · ") || "连接后读取")}</code></div></div>
      </article>`;
    document.querySelector("#configure-runtime")?.addEventListener("click", () => invoke("configureruntime").catch(console.error));
    document.querySelector("#refresh-settings")?.addEventListener("click", () => invoke("refreshthreads").catch(console.error));
  }

  function renderApproval(next) {
    const approval = next.approval || {};
    elements.approvalModal.classList.toggle("hidden", !approval.pending);
    if (!approval.pending) return;
    elements.approvalTitle.textContent = approval.toolName ? `允许 ${approval.toolName}？` : "Codex 请求执行操作";
    elements.approvalText.textContent = approval.text || "此操作需要编辑器确认。";
  }

  function render(next) {
    renderConnection(next);
    renderThreads(next);
    renderModels(next);
    renderConversation(next);
    if (route === "settings") renderSettings(next);
    const ready = next.connection.state === "ready";
    elements.messageInput.disabled = !ready || next.busy;
    elements.send.disabled = !ready || next.busy;
    elements.send.textContent = next.busy ? "处理中…" : "发送";
    elements.chatError.classList.toggle("hidden", !next.error?.present);
    elements.chatError.textContent = next.error?.present ? `${next.error.code ? `[${next.error.code}] ` : ""}${next.error.message}` : "";
    renderApproval(next);
  }

  async function poll() {
    if (polling) return;
    polling = true;
    try {
      const raw = await invoke("getstate");
      const next = typeof raw === "string" ? JSON.parse(raw) : raw;
      state = next;
      render(next);
    } catch (error) {
      elements.connectionLine.innerHTML = `<span class="dot"></span><span>正在等待 Unreal 接口…</span>`;
    } finally { polling = false; }
  }

  document.querySelectorAll("[data-route]").forEach(button => button.addEventListener("click", () => setRoute(button.dataset.route)));
  document.querySelector("#top-settings").addEventListener("click", () => setRoute("settings"));
  document.querySelector("#back-chat").addEventListener("click", () => setRoute("chat"));
  document.querySelector("#refresh").addEventListener("click", () => invoke("refreshthreads").catch(console.error));
  document.querySelector("#new-chat").addEventListener("click", async () => { await invoke("newconversation"); setRoute("chat"); elements.messageInput.focus(); });
  elements.threadList.addEventListener("click", async event => {
    const button = event.target.closest("[data-thread-id]");
    if (!button) return;
    setRoute("chat");
    await invoke("resumethread", button.dataset.threadId, elements.modelSelect.value || "");
  });
  elements.composer.addEventListener("submit", async event => {
    event.preventDefault();
    const text = elements.messageInput.value.trim();
    if (!text || !state || state.busy) return;
    elements.messageInput.value = "";
    await invoke("sendmessage", state.activeThreadId || "", text, elements.modelSelect.value || "");
  });
  elements.messageInput.addEventListener("keydown", event => {
    if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); elements.composer.requestSubmit(); }
  });
  document.querySelector("#deny").addEventListener("click", () => state?.approval?.requestId && invoke("resolveapproval", state.approval.requestId, false));
  document.querySelector("#approve").addEventListener("click", () => state?.approval?.requestId && invoke("resolveapproval", state.approval.requestId, true));

  setInterval(poll, 600);
  poll();
  window.WorldDataUI = Object.freeze({ refresh: poll, openSettings: () => setRoute("settings"), openChat: () => setRoute("chat") });
})();
