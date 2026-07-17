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
  let previousSettingsSignature = "";
  let actionError = "";
  let pendingAction = "";

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
    const labels = Object.freeze({ draft: "草稿", creating: "创建中", loading: "加载中", running: "生成中", approval: "待确认", active: "活跃", loaded: "已加载", idle: "空闲", completed: "已完成", failed: "失败", systemError: "异常" });
    return labels[status] || "";
  }

  function toolStatusLabel(status) {
    const labels = Object.freeze({ running: "执行中", completed: "已完成", failed: "失败", pending: "等待中" });
    return labels[status] || status || "等待中";
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

  function toolRiskLabel(risk) {
    const labels = Object.freeze({
      read_only: "只读",
      workspace_change: "修改工作区",
      destructive: "破坏性操作",
      arbitrary_code: "任意代码"
    });
    return labels[risk] || "未知风险";
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
      return `<button class="conversation" type="button" aria-current="${thread.id === next.activeThreadId ? "page" : "false"}" data-thread-id="${escapeHtml(thread.id)}" title="${escapeHtml(tooltip)}"><span class="conversation__title">${escapeHtml(title)}</span><span class="conversation__age">${escapeHtml(status || relativeTime(threadTimestamp(thread)))}</span></button>`;
    }).join("") || `<div class="conversation-empty">暂无本项目会话</div>`;
  }

  function renderModels(next) {
    const current = elements.modelSelect.value || next.selectedModel;
    const models = next.connection.models || [];
    elements.modelSelect.innerHTML = models.map(model => `<option value="${escapeHtml(model.id)}" ${model.id === current || (!current && model.isDefault) ? "selected" : ""}>${escapeHtml(model.displayName || model.id)}</option>`).join("");
  }

  function conversationSignature(next) {
    return JSON.stringify([
      next.activeThreadId,
      ...(next.conversation || []).map(item => [
        item.id, item.turnId, item.kind, item.role, item.toolName, item.status, item.text
      ])
    ]);
  }

  function renderToolMessage(item) {
    const status = item.status || "pending";
    const detail = item.text && item.text !== status ? `<code class="tool-detail">${escapeHtml(item.text)}</code>` : "";
    return `<li class="tool-batch__item ${escapeHtml(status)}"><code class="tool-batch__name">${escapeHtml(item.toolName || "工具调用")}</code><span class="tool-status">${escapeHtml(toolStatusLabel(status))}</span>${detail}</li>`;
  }

  function renderToolBatch(items) {
    const failed = items.some(item => item.status === "failed");
    const running = items.some(item => item.status === "running" || item.status === "pending");
    const status = failed ? "failed" : (running ? "running" : "completed");
    const summary = failed
      ? `后台处理 · ${items.length} 项中有失败`
      : (running ? `后台处理 · ${items.length} 项进行中` : `后台处理 · ${items.length} 项已完成`);
    return `<details class="tool-batch ${status}"${status === "completed" ? "" : " open"}>
      <summary class="tool-batch__summary"><span class="tool-state" aria-hidden="true"></span><span class="tool-batch__label">${summary}</span><span class="tool-batch__toggle">详情</span></summary>
      <ul class="tool-batch__list">${items.map(renderToolMessage).join("")}</ul>
    </details>`;
  }

  function renderActivityBatch(items) {
    const running = items.some(item => item.status === "running" || item.status === "pending");
    const summary = running ? `正在处理 · ${items.length} 项活动` : `处理完成 · ${items.length} 项活动`;
    return `<details class="activity-batch ${running ? "running" : "completed"}"${running ? " open" : ""}>
      <summary class="activity-batch__summary"><span class="activity-batch__state" aria-hidden="true"></span><span>${summary}</span><span class="activity-batch__toggle">详情</span></summary>
      <ul class="activity-batch__list">${items.map(item => `<li>${escapeHtml(item.text || "正在处理")}</li>`).join("")}</ul>
    </details>`;
  }

  function renderConversationItems(items) {
    const output = [];
    for (let index = 0; index < items.length;) {
      const item = items[index];
      if (item.kind === "tool") {
        const batch = [];
        while (index < items.length && items[index].kind === "tool") batch.push(items[index++]);
        output.push(renderToolBatch(batch));
        continue;
      }
      if (item.kind === "activity") {
        const batch = [];
        while (index < items.length && items[index].kind === "activity") batch.push(items[index++]);
        output.push(renderActivityBatch(batch));
        continue;
      }
      const role = item.role === "user" ? "user" : "assistant";
      const body = role === "user"
        ? `<div class="message-body">${escapeHtml(item.text)}</div>`
        : `<div class="message-markdown">${renderMarkdown(item.text)}</div>`;
      output.push(`<article class="message message--${role}"><span class="message__role">${role === "user" ? "你" : "CODEX"}</span>${body}</article>`);
      index += 1;
    }
    return output.join("");
  }

  function renderConversation(next) {
    const signature = conversationSignature(next);
    if (signature === previousConversationSignature) return;
    const wasNearBottom = elements.messageList.scrollHeight - elements.messageList.scrollTop - elements.messageList.clientHeight < 100;
    const previousScrollTop = elements.messageList.scrollTop;
    previousConversationSignature = signature;
    if (!next.conversation.length) {
      elements.messageList.innerHTML = `<div class="empty-state"><strong>今天我能帮您做什么？</strong><span>连接完成后，可直接操作当前 Unreal 项目。</span></div>`;
      return;
    }
    elements.messageList.innerHTML = renderConversationItems(next.conversation);
    if (wasNearBottom) elements.messageList.scrollTop = elements.messageList.scrollHeight;
    else elements.messageList.scrollTop = previousScrollTop;
  }

  function renderSettings(next) {
    const runtime = next.runtime;
    const connection = next.connection;
    const configured = runtime.configured && runtime.verified;
    const tools = Array.isArray(next.tools) ? next.tools : [];
    const settingsSignature = JSON.stringify({
      configuring: next.configuring,
      runtime,
      connection: {
        state: connection.state,
        statusText: connection.statusText,
        mcpConnected: connection.mcpConnected,
        mcpToolCount: connection.mcpToolCount,
        mcpStatus: connection.mcpStatus,
        authenticated: connection.authenticated,
        models: connection.models
      },
      tools
    });
    if (settingsSignature === previousSettingsSignature) return;
    previousSettingsSignature = settingsSignature;
    const toolRegistry = tools.length
      ? `<div class="tool-registry" role="list">${tools.map(tool => {
          const capabilities = Array.isArray(tool.requiredCapabilities) ? tool.requiredCapabilities.filter(Boolean) : [];
          const risk = tool.risk || "unknown";
          return `<article class="tool-row" role="listitem">
            <div class="tool-row__top"><code class="tool-row__name">${escapeHtml(tool.name || "unnamed_tool")}</code><span class="tool-risk tool-risk--${escapeHtml(risk)}">${escapeHtml(toolRiskLabel(risk))}</span></div>
            <div class="tool-row__meta"><span>${escapeHtml(tool.provider || "unknown provider")}</span><span>${tool.requiresApproval ? "需要审批" : "无需审批"}</span>${tool.audited ? "<span>已审计</span>" : ""}</div>
            ${capabilities.length ? `<div class="tool-row__capabilities">${capabilities.map(capability => `<span class="chip">${escapeHtml(capability)}</span>`).join("")}</div>` : ""}
          </article>`;
        }).join("")}</div>`
      : `<p class="tool-registry-empty">尚未检测到已注册的 MCP 工具。请确认 UEBridgeMCPTools 已完成加载。</p>`;
    elements.settingsContent.innerHTML = `
      <article class="settings-card settings-card--wide">
        <div class="settings-card__head"><div><h3 class="settings-card__title">Codex 后端运行时</h3><p class="settings-card__description">自动发现原生 Codex、复制到托管目录、生成协议 schema，并按 SHA-256 固定到当前项目。</p></div><span class="server-state ${configured ? "" : "is-stopped"}">${configured ? "已配置" : "未配置"}</span></div>
        <div class="runtime-config-row"><div><strong>${configured ? "配置已保存，后续启动会自动使用" : "点击一次完成发现、复制、校验与连接"}</strong><p>${escapeHtml(runtime.manifestPath || "尚未生成运行时清单")}</p></div><button id="configure-runtime" class="button button--primary" type="button" ${next.configuring ? "disabled" : ""}>${next.configuring ? "正在配置…" : (configured ? "重新校验配置" : "一键自动配置")}</button></div>
        <div class="runtime-grid"><div class="runtime-item"><span>Agent Host · ${escapeHtml(runtime.hostVersion || "待配置")}</span><code>${escapeHtml(runtime.hostSha256 || "—")}</code></div><div class="runtime-item"><span>Codex · ${escapeHtml(runtime.codexVersion || "待配置")}</span><code>${escapeHtml(runtime.codexSha256 || "—")}</code></div></div>
      </article>
      <article class="settings-card">
        <div class="settings-card__head"><div><h3 class="settings-card__title">WorldData MCP</h3><p class="settings-card__description">每个 Codex 线程自动注入本地服务和临时认证头。</p></div><span class="server-state ${connection.mcpConnected ? "" : "is-stopped"}">${connection.mcpConnected ? "线程已连接" : "等待会话"}</span></div>
        <div class="client-banner"><span class="client-banner__mark">MCP</span><span>${escapeHtml(connectionLabel(connection))}${connection.mcpStatus ? ` · ${escapeHtml(connection.mcpStatus)}` : ""} · ${Number(connection.mcpToolCount || 0)} 工具</span></div>
        <div class="settings-actions"><button id="refresh-settings" class="button button--quiet" type="button">刷新会话</button></div>
      </article>
      <article class="settings-card settings-card--wide">
        <div class="settings-card__head"><div><h3 class="settings-card__title">已注册 MCP 工具</h3><p class="settings-card__description">此列表直接来自 Unreal 当前运行中的工具注册表；名称、提供方、风险级别和审批要求会随模块加载状态更新。</p></div><span class="server-state ${tools.length ? "" : "is-stopped"}">${tools.length} 个工具</span></div>
        ${toolRegistry}
      </article>
      <article class="settings-card">
        <div class="settings-card__head"><div><h3 class="settings-card__title">账户与模型</h3><p class="settings-card__description">状态直接来自 Codex app-server，不保存登录凭据，也不硬编码模型。</p></div><span class="server-state ${connection.authenticated ? "" : "is-stopped"}">${connection.authenticated ? "已认证" : "需要登录"}</span></div>
        <div class="security-option"><div class="security-option__copy"><span class="security-option__title">${(connection.models || []).length} 个可用模型</span><span class="security-option__help">连接后实时读取当前账户可用模型。</span></div></div>
        <div class="chips">${(connection.models || []).map(model => `<span class="chip">${escapeHtml(model.displayName || model.id)}</span>`).join("") || `<span class="chip">等待连接</span>`}</div>
      </article>
      <article class="settings-card settings-card--wide">
        <div class="settings-card__head"><div><h3 class="settings-card__title">安全连接</h3><p class="settings-card__description">浏览器只接收稳定状态 JSON；MCP 密钥仅存在于编辑器内存和受信任子进程环境。</p></div><span class="server-state">本地保护</span></div>
        <div class="security-grid"><div class="security-option"><div class="security-option__copy"><span class="security-option__title">Agent Host IPC v2</span><span class="security-option__help">有界 JSONL 帧、会话序号、协议版本校验和明确错误代码。</span></div><span class="chip">STDIO</span></div><div class="security-option"><div class="security-option__copy"><span class="security-option__title">线程级 MCP</span><span class="security-option__help">临时认证头不会进入 HTML、配置文件或诊断日志。</span></div><span class="chip">EPHEMERAL</span></div></div>
      </article>`;
    document.querySelector("#configure-runtime")?.addEventListener("click", () => { void runAction("配置运行时", "configure", () => invoke("configureruntime")); });
    document.querySelector("#refresh-settings")?.addEventListener("click", () => { void refreshThreads(); });
  }

  function renderApproval(next) {
    const approval = next.approval || {};
    elements.approvalModal.classList.toggle("hidden", !approval.pending);
    if (!approval.pending) return;
    elements.approvalTitle.textContent = approval.toolName ? `允许 ${approval.toolName}？` : "Codex 请求执行操作";
    elements.approvalText.textContent = approval.text || "此操作需要编辑器确认。";
  }

  function renderError(next) {
    const backendError = next?.error?.present
      ? `${next.error.code ? `[${next.error.code}] ` : ""}${next.error.message}`
      : "";
    const message = backendError || actionError;
    elements.chatError.classList.toggle("hidden", !message);
    elements.chatError.textContent = message;
  }

  function describeActionError(action, error) {
    const detail = error instanceof Error && error.message ? error.message : "请检查 Unreal 连接后重试。";
    return `${action}失败：${detail}`;
  }

  async function runAction(action, key, operation) {
    if (pendingAction) return false;
    pendingAction = key;
    actionError = "";
    renderError(state);
    try {
      await operation();
      await poll();
      return true;
    } catch (error) {
      actionError = describeActionError(action, error);
      renderError(state);
      console.error(error);
      return false;
    } finally {
      pendingAction = "";
      if (state) render(state);
    }
  }

  async function refreshThreads() {
    await runAction("刷新会话", "refresh", () => invoke("refreshthreads"));
  }

  function render(next) {
    renderConnection(next);
    renderThreads(next);
    renderModels(next);
    renderConversation(next);
    if (route === "settings") renderSettings(next);
    const ready = next.connection.state === "ready";
    elements.messageInput.disabled = !ready || next.busy || pendingAction === "send";
    elements.send.disabled = !ready || next.busy || pendingAction === "send";
    elements.send.textContent = next.busy ? "处理中…" : "发送";
    renderError(next);
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
  document.querySelector("#refresh").addEventListener("click", () => { void refreshThreads(); });
  document.querySelector("#new-chat").addEventListener("click", async () => {
    const created = await runAction("新建会话", "new-chat", () => invoke("newconversation"));
    if (!created) return;
    setRoute("chat");
    elements.messageInput.focus();
  });
  elements.threadList.addEventListener("click", async event => {
    const button = event.target.closest("[data-thread-id]");
    if (!button) return;
    const resumed = await runAction("加载会话", "resume", () => invoke("resumethread", button.dataset.threadId, elements.modelSelect.value || ""));
    if (resumed) setRoute("chat");
  });
  elements.composer.addEventListener("submit", async event => {
    event.preventDefault();
    const draft = elements.messageInput.value;
    const text = draft.trim();
    if (!text || !state || state.busy || pendingAction) return;
    const sent = await runAction("发送消息", "send", async () => {
      const accepted = await invoke("sendmessage", state.activeThreadId || "", text, elements.modelSelect.value || "");
      if (accepted === false) {
        await poll();
        throw new Error("请求未被 Agent Host 接受。");
      }
    });
    if (sent) elements.messageInput.value = "";
  });
  elements.messageInput.addEventListener("keydown", event => {
    if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); elements.composer.requestSubmit(); }
  });
  document.querySelector("#deny").addEventListener("click", () => state?.approval?.requestId && void runAction("拒绝操作", "approval", () => invoke("resolveapproval", state.approval.requestId, false)));
  document.querySelector("#approve").addEventListener("click", () => state?.approval?.requestId && void runAction("允许操作", "approval", () => invoke("resolveapproval", state.approval.requestId, true)));

  setInterval(poll, 600);
  poll();
  window.WorldDataUI = Object.freeze({ refresh: poll, openSettings: () => setRoute("settings"), openChat: () => setRoute("chat") });
})();
