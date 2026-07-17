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
    navSettings: document.querySelector("#nav-settings"), composerFooter: document.querySelector("#composer-footer"),
    composerStatus: document.querySelector("#composer-status")
  });

  let route = "chat";
  let state = null;
  let polling = false;
  let previousConversationSignature = "";

  const escapeHtml = (value = "") => String(value).replace(/[&<>"']/g, char => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"})[char]);
  function renderInlineMarkdown(value) {
    return escapeHtml(value)
      .replace(/`([^`\n]+)`/g, "<code>$1</code>")
      .replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>");
  }

  function renderMarkdown(value = "") {
    const lines = String(value).replace(/\r\n?/g, "\n").split("\n");
    const output = [];
    let code = null;
    let list = null;
    const closeList = () => {
      if (!list) return;
      output.push(`</${list}>`);
      list = null;
    };

    for (const line of lines) {
      if (/^```/.test(line)) {
        closeList();
        if (code) {
          output.push(`<pre><code>${escapeHtml(code.join("\n"))}</code></pre>`);
          code = null;
        } else {
          code = [];
        }
        continue;
      }
      if (code) {
        code.push(line);
        continue;
      }

      const heading = line.match(/^(#{1,4})\s+(.+)$/);
      if (heading) {
        closeList();
        const level = Math.min(heading[1].length + 2, 6);
        output.push(`<h${level}>${renderInlineMarkdown(heading[2])}</h${level}>`);
        continue;
      }
      const unordered = line.match(/^\s*[-*]\s+(.+)$/);
      const ordered = line.match(/^\s*\d+[.)]\s+(.+)$/);
      if (unordered || ordered) {
        const nextList = unordered ? "ul" : "ol";
        if (list !== nextList) {
          closeList();
          list = nextList;
          output.push(`<${list}>`);
        }
        output.push(`<li>${renderInlineMarkdown((unordered || ordered)[1])}</li>`);
        continue;
      }
      closeList();
      if (line.trim()) output.push(`<p>${renderInlineMarkdown(line)}</p>`);
    }
    closeList();
    if (code) output.push(`<pre><code>${escapeHtml(code.join("\n"))}</code></pre>`);
    return output.join("");
  }

  function threadStatusLabel(status) {
    const labels = Object.freeze({ loaded: "已加载", active: "进行中", idle: "空闲", systemError: "异常" });
    return labels[status] || "";
  }
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
    elements.composerFooter.classList.toggle("hidden", settings);
    elements.navChat.setAttribute("aria-current", settings ? "false" : "page");
    elements.navSettings.setAttribute("aria-current", settings ? "page" : "false");
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

  function threadTimestamp(thread) {
    return Number(thread.recencyAt || thread.updatedAt || thread.createdAt || 0);
  }

  function compareThreadsByRecency(left, right) {
    return threadTimestamp(right) - threadTimestamp(left)
      || Number(right.updatedAt || 0) - Number(left.updatedAt || 0)
      || String(left.id || "").localeCompare(String(right.id || ""));
  }

  function renderConnection(next) {
    const connection = next.connection;
    elements.connectionLine.className = `top-server-connection ${connection.state === "ready" ? "" : "is-offline"}`;
    elements.connectionLine.textContent = connectionLabel(connection);
    const ready = connection.state === "ready";
    const failed = connection.state === "fatal" || connection.state === "degraded";
    elements.readyPill.className = `status ${failed ? "status--error" : (!ready ? "status--processing" : "")}`;
    elements.readyPill.textContent = ready ? "已就绪" : (failed ? "需要处理" : "连接中");
    elements.composerStatus.classList.toggle("is-offline", !ready);
    elements.composerStatus.textContent = ready ? "Codex 与 MCP 已连接" : connectionLabel(connection);
  }

  function renderThreads(next) {
    const orderedThreads = [...next.threads].sort(compareThreadsByRecency);
    elements.threadCount.textContent = `${next.threads.length} 个会话`;
    elements.threadList.innerHTML = orderedThreads.map(thread => {
      const title = thread.title || thread.preview || "新对话";
      const status = threadStatusLabel(thread.status);
      const tooltip = status ? `${title} · ${status}` : title;
      return `<button class="conversation" type="button" aria-current="${thread.id === next.activeThreadId ? "page" : "false"}" data-thread-id="${escapeHtml(thread.id)}" title="${escapeHtml(tooltip)}"><span class="conversation__title">${escapeHtml(title)}</span><span class="conversation__age">${relativeTime(threadTimestamp(thread))}</span></button>`;
    }).join("") || `<div class="conversation-empty">暂无本项目会话</div>`;
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
      if (item.kind === "tool") return `<article class="message message--tool ${escapeHtml(item.status)}"><span class="message__role">TOOL · ${escapeHtml(item.toolName || "工具调用")}</span><div class="message__body"><span class="tool-state"></span>${escapeHtml(item.status || item.text || "")}</div></article>`;
      const role = item.role === "user" ? "user" : "assistant";
      const body = role === "user"
        ? `<div class="message-body">${escapeHtml(item.text)}</div>`
        : `<div class="message-markdown">${renderMarkdown(item.text)}</div>`;
      return `<article class="message message--${role}"><span class="message__role">${role === "user" ? "你" : "CODEX"}</span>${body}</article>`;
    }).join("");
    if (wasNearBottom) elements.messageList.scrollTop = elements.messageList.scrollHeight;
  }

  function renderSettings(next) {
    const runtime = next.runtime;
    const connection = next.connection;
    const configured = runtime.configured && runtime.verified;
    elements.settingsContent.innerHTML = `
      <article class="settings-card settings-card--wide">
        <div class="settings-card__head"><div><h3 class="settings-card__title">Codex 后端运行时</h3><p class="settings-card__description">自动发现原生 Codex、复制到托管目录、生成协议 schema，并按 SHA-256 固定到当前项目。</p></div><span class="server-state ${configured ? "" : "is-stopped"}">${configured ? "已配置" : "未配置"}</span></div>
        <div class="runtime-config-row"><div><strong>${configured ? "配置已保存，后续启动会自动使用" : "点击一次完成发现、复制、校验与连接"}</strong><p>${escapeHtml(runtime.manifestPath || "尚未生成运行时清单")}</p></div><button id="configure-runtime" class="button button--primary" type="button" ${next.configuring ? "disabled" : ""}>${next.configuring ? "正在配置…" : (configured ? "重新校验配置" : "一键自动配置")}</button></div>
        <div class="runtime-grid"><div class="runtime-item"><span>Agent Host · ${escapeHtml(runtime.hostVersion || "待配置")}</span><code>${escapeHtml(runtime.hostSha256 || "—")}</code></div><div class="runtime-item"><span>Codex · ${escapeHtml(runtime.codexVersion || "待配置")}</span><code>${escapeHtml(runtime.codexSha256 || "—")}</code></div></div>
      </article>
      <article class="settings-card">
        <div class="settings-card__head"><div><h3 class="settings-card__title">WorldData MCP</h3><p class="settings-card__description">每个 Codex 线程自动注入本地服务和临时认证头。</p></div><span class="server-state ${connection.mcpConnected ? "" : "is-stopped"}">${connection.mcpConnected ? "线程已连接" : "等待会话"}</span></div>
        <div class="client-banner"><span class="client-banner__mark">MCP</span><span>${escapeHtml(connectionLabel(connection))}</span></div>
        <div class="settings-actions"><button id="refresh-settings" class="button button--quiet" type="button">刷新状态</button></div>
      </article>
      <article class="settings-card">
        <div class="settings-card__head"><div><h3 class="settings-card__title">账户与模型</h3><p class="settings-card__description">状态直接来自 Codex app-server，不保存登录凭据，也不硬编码模型。</p></div><span class="server-state ${connection.authenticated ? "" : "is-stopped"}">${connection.authenticated ? "已认证" : "需要登录"}</span></div>
        <div class="security-option"><div class="security-option__copy"><span class="security-option__title">${(connection.models || []).length} 个可用模型</span><span class="security-option__help">连接后实时读取当前账户可用模型。</span></div></div>
        <div class="chips">${(connection.models || []).map(model => `<span class="chip">${escapeHtml(model.displayName || model.id)}</span>`).join("") || `<span class="chip">等待连接</span>`}</div>
      </article>
      <article class="settings-card settings-card--wide">
        <div class="settings-card__head"><div><h3 class="settings-card__title">安全连接</h3><p class="settings-card__description">浏览器只接收稳定状态 JSON；MCP 密钥仅存在于编辑器内存和受信任子进程环境。</p></div><span class="server-state">本地保护</span></div>
        <div class="security-grid"><div class="security-option"><div class="security-option__copy"><span class="security-option__title">Agent Host IPC v1</span><span class="security-option__help">有界 JSONL 帧、协议版本校验和明确错误代码。</span></div><span class="chip">STDIO</span></div><div class="security-option"><div class="security-option__copy"><span class="security-option__title">线程级 MCP</span><span class="security-option__help">临时认证头不会进入 HTML、配置文件或诊断日志。</span></div><span class="chip">EPHEMERAL</span></div></div>
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
      elements.connectionLine.className = "top-server-connection is-offline";
      elements.connectionLine.textContent = "正在等待 Unreal 接口…";
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
