using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using WorldData.AgentHost.Contracts;

namespace WorldData.AgentHost.Codex;

internal sealed class CodexAppServerClient : ICodexClient
{
    private const string McpTokenEnvironmentVariable = "WORLDDATA_MCP_TOKEN";
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonElement>> pending = new();
    private readonly ConcurrentDictionary<string, PendingServerRequest> pendingApprovals = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, McpConnectionInfo> threadMcpStatus = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, TaskCompletionSource<McpConnectionInfo>> mcpReadiness = new(StringComparer.Ordinal);
    private readonly SemaphoreSlim writeLock = new(1, 1);
    private readonly CancellationTokenSource lifetime = new();
    private Process? process;
    private WindowsChildProcessJob? childProcessJob;
    private Task? stdoutTask;
    private Task? stderrTask;
    private long nextRpcId;
    private CodexStartOptions? startOptions;

    public event Func<CodexEvent, ValueTask>? EventReceived;

    public async Task<CodexConnectionInfo> StartAsync(CodexStartOptions options, CancellationToken cancellationToken)
    {
        if (process is { HasExited: false })
        {
            throw new InvalidOperationException("Codex app-server is already running.");
        }
        if (string.IsNullOrWhiteSpace(options.Executable) || !Path.IsPathFullyQualified(options.Executable) || !File.Exists(options.Executable))
        {
            throw new FileNotFoundException("Codex executable must be an existing absolute path.", options.Executable);
        }

        if (string.IsNullOrWhiteSpace(options.McpToken))
        {
            throw new InvalidOperationException("The private MCP credential is unavailable.");
        }

        // Keep the credential only in the child Codex process environment. The
        // retained options are deliberately scrubbed once process startup begins.
        startOptions = options with { McpToken = string.Empty };
        var startInfo = new ProcessStartInfo
        {
            FileName = options.Executable,
            Arguments = "app-server --listen stdio://",
            WorkingDirectory = options.ProjectRoot,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardInputEncoding = new UTF8Encoding(false),
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false)
        };
        startInfo.Environment[McpTokenEnvironmentVariable] = options.McpToken;

        process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        process.Exited += (_, _) => OnCodexExited();
        if (!process.Start())
        {
            throw new InvalidOperationException("Failed to launch Codex app-server.");
        }
        childProcessJob = WindowsChildProcessJob.TryAttach(process);
        if (OperatingSystem.IsWindows() && childProcessJob is null)
        {
            process.Kill(entireProcessTree: true);
            process.Dispose();
            process = null;
            throw new InvalidOperationException("Failed to assign Codex app-server to the Host lifecycle job.");
        }

        stdoutTask = ReadStdoutAsync(process.StandardOutput, lifetime.Token);
        stderrTask = ReadStderrAsync(process.StandardError, lifetime.Token);

        var initialize = await SendRequestAsync("initialize", new JsonObject
        {
            ["clientInfo"] = new JsonObject
            {
                ["name"] = "worlddata_unreal_agent_host",
                ["title"] = "WorldData Unreal Agent Host",
                ["version"] = options.ClientVersion
            }
        }, cancellationToken).ConfigureAwait(false);
        await SendNotificationAsync("initialized", new JsonObject(), cancellationToken).ConfigureAwait(false);

        var account = await SendRequestAsync("account/read", new JsonObject
        {
            ["refreshToken"] = false
        }, cancellationToken).ConfigureAwait(false);
        var models = await SendRequestAsync("model/list", new JsonObject
        {
            ["includeHidden"] = false
        }, cancellationToken).ConfigureAwait(false);

        var userAgent = initialize.TryGetProperty("userAgent", out var userAgentElement)
            ? userAgentElement.GetString() ?? string.Empty
            : string.Empty;
        var authenticated = account.ValueKind == JsonValueKind.Object
            && account.TryGetProperty("account", out var accountElement)
            && accountElement.ValueKind is not JsonValueKind.Null and not JsonValueKind.Undefined;

        return new CodexConnectionInfo(userAgent, authenticated, account.Clone(), models.Clone());
    }

    public async Task<ThreadListPage> ListThreadsAsync(string? cursor, int limit, CancellationToken cancellationToken)
    {
        EnsureStarted();
        const int maximumPagesToScan = 20;
        var requestedLimit = Math.Clamp(limit, 1, 100);
        var threads = new List<ThreadSummary>();
        var pageCursor = cursor;
        string? nextCursor = null;
        for (var pageIndex = 0; pageIndex < maximumPagesToScan && threads.Count < requestedLimit; pageIndex++)
        {
            var parameters = new JsonObject
            {
                ["cwd"] = startOptions!.ProjectRoot,
                ["limit"] = requestedLimit - threads.Count,
                ["archived"] = false,
                // Current Codex app-server accepts created_at/updated_at only;
                // Host still performs its own stable recency ordering afterwards.
                ["sortKey"] = "updated_at",
                ["sortDirection"] = "desc"
            };
            if (!string.IsNullOrWhiteSpace(pageCursor)) parameters["cursor"] = pageCursor;

            var result = await SendRequestAsync("thread/list", parameters, cancellationToken).ConfigureAwait(false);
            if (result.ValueKind == JsonValueKind.Object
                && result.TryGetProperty("data", out var data)
                && data.ValueKind == JsonValueKind.Array)
            {
                foreach (var thread in data.EnumerateArray())
                {
                    if (thread.ValueKind == JsonValueKind.Object
                        && string.Equals(ReadString(thread, "threadSource"), AgentHostProtocol.ThreadSource, StringComparison.Ordinal))
                    {
                        threads.Add(ParseThreadSummary(thread));
                    }
                }
            }
            nextCursor = result.ValueKind == JsonValueKind.Object ? ReadString(result, "nextCursor") : null;
            if (string.IsNullOrWhiteSpace(nextCursor)) break;
            pageCursor = nextCursor;
        }
        threads.Sort(CompareThreadRecency);
        return new ThreadListPage(threads, nextCursor);
    }

    public async Task<string> CreateThreadAsync(CreateThreadOptions options, CancellationToken cancellationToken)
    {
        EnsureStarted();
        var parameters = new JsonObject
        {
            ["cwd"] = options.WorkingDirectory,
            ["config"] = CreateThreadConfig(),
            ["ephemeral"] = options.Ephemeral,
            ["threadSource"] = AgentHostProtocol.ThreadSource
        };
        if (!string.IsNullOrWhiteSpace(options.Model)) parameters["model"] = options.Model;
        if (!string.IsNullOrWhiteSpace(options.ApprovalPolicy)) parameters["approvalPolicy"] = options.ApprovalPolicy;
        if (!string.IsNullOrWhiteSpace(options.SandboxMode)) parameters["sandbox"] = options.SandboxMode;

        var result = await SendRequestAsync("thread/start", parameters, cancellationToken).ConfigureAwait(false);
        if (!result.TryGetProperty("thread", out var thread)
            || !thread.TryGetProperty("id", out var id)
            || string.IsNullOrWhiteSpace(id.GetString()))
        {
            throw new InvalidDataException("Codex thread/start did not return thread.id.");
        }
        var threadId = id.GetString()!;
        threadMcpStatus.TryAdd(threadId, new McpConnectionInfo(false, "worlddata", 0, "WorldData MCP is starting."));
        mcpReadiness.TryAdd(threadId, NewMcpReadiness());
        return threadId;
    }

    public async Task<ThreadSnapshot> ResumeThreadAsync(ResumeThreadOptions options, CancellationToken cancellationToken)
    {
        EnsureStarted();
        var parameters = new JsonObject
        {
            ["threadId"] = options.ThreadId,
            ["cwd"] = options.WorkingDirectory,
            ["config"] = CreateThreadConfig()
        };
        if (!string.IsNullOrWhiteSpace(options.Model)) parameters["model"] = options.Model;
        if (!string.IsNullOrWhiteSpace(options.ApprovalPolicy)) parameters["approvalPolicy"] = options.ApprovalPolicy;
        if (!string.IsNullOrWhiteSpace(options.SandboxMode)) parameters["sandbox"] = options.SandboxMode;

        var result = await SendRequestAsync("thread/resume", parameters, cancellationToken).ConfigureAwait(false);
        var snapshot = ParseThreadResponse(result, "thread/resume");
        threadMcpStatus.TryAdd(options.ThreadId, new McpConnectionInfo(false, "worlddata", 0, "WorldData MCP is starting."));
        mcpReadiness.TryAdd(options.ThreadId, NewMcpReadiness());
        return snapshot;
    }

    public async Task<ThreadSnapshot> ReadThreadAsync(string threadId, CancellationToken cancellationToken)
    {
        EnsureStarted();
        var result = await SendRequestAsync("thread/read", new JsonObject
        {
            ["threadId"] = threadId,
            ["includeTurns"] = true
        }, cancellationToken).ConfigureAwait(false);
        return ParseThreadResponse(result, "thread/read");
    }

    public async Task<string> StartTurnAsync(TurnOptions options, CancellationToken cancellationToken)
    {
        EnsureStarted();
        var result = await SendRequestAsync("turn/start", new JsonObject
        {
            ["threadId"] = options.ThreadId,
            ["clientUserMessageId"] = options.ClientTurnId,
            ["input"] = new JsonArray
            {
                new JsonObject
                {
                    ["type"] = "text",
                    ["text"] = options.Text
                }
            }
        }, cancellationToken).ConfigureAwait(false);

        if (result.TryGetProperty("turn", out var turn)
            && turn.TryGetProperty("id", out var id)
            && !string.IsNullOrWhiteSpace(id.GetString()))
        {
            return id.GetString()!;
        }
        return options.ClientTurnId;
    }

    public async Task InterruptTurnAsync(string threadId, string turnId, CancellationToken cancellationToken)
    {
        await SendRequestAsync("turn/interrupt", new JsonObject
        {
            ["threadId"] = threadId,
            ["turnId"] = turnId
        }, cancellationToken).ConfigureAwait(false);
    }

    public async Task ResolveApprovalAsync(ApprovalDecision decision, CancellationToken cancellationToken)
    {
        if (!pendingApprovals.TryRemove(decision.RequestId, out var request))
        {
            throw new KeyNotFoundException("Approval request is no longer pending.");
        }

        JsonNode result = request.Method switch
        {
            "item/permissions/requestApproval" => new JsonObject
            {
                ["permissions"] = decision.Approved
                    ? RequestedPermissions(request.Parameters)
                    : new JsonObject(),
                ["scope"] = "turn"
            },
            _ => new JsonObject
            {
                ["decision"] = decision.Approved ? "accept" : "decline"
            }
        };
        await SendResponseAsync(decision.RequestId, result, cancellationToken).ConfigureAwait(false);
    }

    public async Task<McpConnectionInfo> GetMcpStatusAsync(string threadId, CancellationToken cancellationToken)
    {
        threadMcpStatus.TryGetValue(threadId, out var startupStatus);
        var result = await SendRequestAsync("mcpServerStatus/list", new JsonObject
        {
            ["detail"] = "toolsAndAuthOnly"
        }, cancellationToken).ConfigureAwait(false);
        if (result.ValueKind != JsonValueKind.Object
            || !result.TryGetProperty("data", out var data)
            || data.ValueKind != JsonValueKind.Array)
        {
            return new McpConnectionInfo(false, "worlddata", 0, "Codex returned an invalid MCP status response.");
        }

        foreach (var server in data.EnumerateArray())
        {
            if (server.ValueKind != JsonValueKind.Object
                || !server.TryGetProperty("name", out var nameElement)
                || !string.Equals(nameElement.GetString(), "worlddata", StringComparison.Ordinal))
            {
                continue;
            }
            var toolCount = server.TryGetProperty("tools", out var tools) && tools.ValueKind == JsonValueKind.Object
                ? tools.EnumerateObject().Count()
                : 0;
            var hasServerInfo = server.TryGetProperty("serverInfo", out var serverInfo)
                && serverInfo.ValueKind == JsonValueKind.Object;
            return new McpConnectionInfo(
                hasServerInfo && toolCount > 0,
                "worlddata",
                toolCount,
                hasServerInfo && toolCount > 0 ? null : "WorldData MCP did not publish a usable tool inventory.");
        }
        return startupStatus
            ?? new McpConnectionInfo(false, "worlddata", 0, "WorldData MCP did not report thread startup status.");
    }

    public async Task<McpConnectionInfo> WaitForMcpReadyAsync(string threadId, CancellationToken cancellationToken)
    {
        if (threadMcpStatus.TryGetValue(threadId, out var existing) && existing.Connected) return existing;
        var readiness = mcpReadiness.GetOrAdd(threadId, _ => NewMcpReadiness());
        try
        {
            return await readiness.Task.WaitAsync(TimeSpan.FromSeconds(15), cancellationToken).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            var timeout = new McpConnectionInfo(false, "worlddata", 0, "WorldData MCP startup timed out after 15 seconds.");
            threadMcpStatus[threadId] = timeout;
            return timeout;
        }
    }

    private JsonObject CreateThreadConfig()
        => new()
        {
            ["mcp_servers"] = new JsonObject
            {
                ["worlddata"] = new JsonObject
                {
                    ["url"] = startOptions!.McpUrl,
                    ["env_http_headers"] = new JsonObject
                    {
                        ["X-WorldData-MCP-Token"] = McpTokenEnvironmentVariable
                    },
                    ["required"] = true,
                    ["startup_timeout_sec"] = 15,
                    ["tool_timeout_sec"] = 120
                }
            }
        };

    private static ThreadSnapshot ParseThreadResponse(JsonElement result, string method)
    {
        if (result.ValueKind != JsonValueKind.Object
            || !result.TryGetProperty("thread", out var thread)
            || thread.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException($"Codex {method} did not return a thread object.");
        }

        var items = new List<ConversationItem>();
        if (thread.TryGetProperty("turns", out var turns) && turns.ValueKind == JsonValueKind.Array)
        {
            foreach (var turn in turns.EnumerateArray())
            {
                var turnId = ReadString(turn, "id") ?? string.Empty;
                if (!turn.TryGetProperty("items", out var turnItems) || turnItems.ValueKind != JsonValueKind.Array) continue;
                foreach (var item in turnItems.EnumerateArray())
                {
                    if (TryParseConversationItem(item, turnId, out var parsed)) items.Add(parsed);
                }
            }
        }
        return new ThreadSnapshot(ParseThreadSummary(thread), items);
    }

    private static ThreadSummary ParseThreadSummary(JsonElement thread)
    {
        var id = ReadString(thread, "id") ?? throw new InvalidDataException("Codex thread is missing id.");
        var preview = ReadString(thread, "preview") ?? string.Empty;
        var title = ReadString(thread, "name");
        return new ThreadSummary(
            id,
            string.IsNullOrWhiteSpace(title) ? preview : title,
            preview,
            ReadString(thread, "cwd") ?? string.Empty,
            ReadInt64(thread, "createdAt"),
            ReadInt64(thread, "updatedAt"),
            ReadInt64(thread, "recencyAt"),
            ReadString(thread, "threadSource") ?? string.Empty,
            ReadValue(thread, "status"));
    }

    private static int CompareThreadRecency(ThreadSummary left, ThreadSummary right)
    {
        var byRecency = EffectiveRecencyAt(right).CompareTo(EffectiveRecencyAt(left));
        if (byRecency != 0) return byRecency;
        var byUpdated = right.UpdatedAt.CompareTo(left.UpdatedAt);
        return byUpdated != 0 ? byUpdated : string.CompareOrdinal(left.Id, right.Id);
    }

    private static long EffectiveRecencyAt(ThreadSummary thread) =>
        thread.RecencyAt > 0 ? thread.RecencyAt :
        thread.UpdatedAt > 0 ? thread.UpdatedAt : thread.CreatedAt;

    private static bool TryParseConversationItem(JsonElement item, string turnId, out ConversationItem parsed)
    {
        parsed = default!;
        if (item.ValueKind != JsonValueKind.Object) return false;
        var id = ReadString(item, "id") ?? string.Empty;
        var type = ReadString(item, "type") ?? string.Empty;
        switch (type)
        {
            case "userMessage":
            {
                var text = new StringBuilder();
                if (item.TryGetProperty("content", out var content) && content.ValueKind == JsonValueKind.Array)
                {
                    foreach (var input in content.EnumerateArray())
                    {
                        if (ReadString(input, "type") == "text") text.Append(ReadString(input, "text"));
                    }
                }
                parsed = new ConversationItem(id, turnId, "message", "user", text.ToString(), null, null);
                return true;
            }
            case "agentMessage":
                parsed = new ConversationItem(id, turnId, "message", "assistant", ReadString(item, "text") ?? string.Empty, null, null);
                return true;
            case "mcpToolCall":
            {
                var server = ReadString(item, "server");
                var tool = ReadString(item, "tool");
                var toolName = string.Join('.', new[] { server, tool }.Where(value => !string.IsNullOrWhiteSpace(value)));
                var status = ReadValue(item, "status");
                parsed = new ConversationItem(id, turnId, "tool", "tool", status, toolName, status);
                return true;
            }
            case "commandExecution":
                parsed = new ConversationItem(id, turnId, "tool", "tool", ReadString(item, "command") ?? string.Empty, type, ReadValue(item, "status"));
                return true;
            case "fileChange":
                parsed = new ConversationItem(id, turnId, "tool", "tool", "File changes", type, ReadValue(item, "status"));
                return true;
            case "reasoning":
                parsed = new ConversationItem(id, turnId, "activity", "assistant", "正在处理", "reasoning", ReadValue(item, "status"));
                return true;
            default:
                return false;
        }
    }

    private static long ReadInt64(JsonElement value, string name)
        => value.ValueKind == JsonValueKind.Object
            && value.TryGetProperty(name, out var property)
            && property.TryGetInt64(out var result)
                ? result
                : 0;

    private static string ReadValue(JsonElement value, string name)
    {
        if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty(name, out var property))
        {
            return string.Empty;
        }

        if (property.ValueKind == JsonValueKind.String)
        {
            return property.GetString() ?? string.Empty;
        }

        // Recent app-server versions represent lifecycle state as a tagged
        // object (for example { "type": "notLoaded" }). The stable Host
        // contract exposes the tag, never Codex's version-specific JSON.
        if (property.ValueKind == JsonValueKind.Object)
        {
            foreach (var discriminator in new[] { "type", "status", "state" })
            {
                if (property.TryGetProperty(discriminator, out var tag)
                    && tag.ValueKind == JsonValueKind.String)
                {
                    return tag.GetString() ?? string.Empty;
                }
            }
            return string.Empty;
        }

        return property.ValueKind is JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False
            ? property.ToString()
            : string.Empty;
    }

    private async Task<JsonElement> SendRequestAsync(string method, JsonNode? parameters, CancellationToken cancellationToken)
    {
        EnsureStarted();
        var id = Interlocked.Increment(ref nextRpcId);
        var completion = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!pending.TryAdd(id, completion)) throw new InvalidOperationException("Duplicate JSON-RPC request id.");
        try
        {
            await WriteAsync(new JsonObject
            {
                ["id"] = id,
                ["method"] = method,
                ["params"] = parameters ?? new JsonObject()
            }, cancellationToken).ConfigureAwait(false);
            return await completion.Task.WaitAsync(TimeSpan.FromSeconds(45), cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            pending.TryRemove(id, out _);
        }
    }

    private Task SendNotificationAsync(string method, JsonNode? parameters, CancellationToken cancellationToken)
        => WriteAsync(new JsonObject
        {
            ["method"] = method,
            ["params"] = parameters ?? new JsonObject()
        }, cancellationToken);

    private Task SendResponseAsync(string requestId, JsonNode result, CancellationToken cancellationToken)
    {
        JsonNode id = long.TryParse(requestId, out var numericId)
            ? JsonValue.Create(numericId)!
            : JsonValue.Create(requestId)!;
        return WriteAsync(new JsonObject
        {
            ["id"] = id,
            ["result"] = result
        }, cancellationToken);
    }

    private async Task WriteAsync(JsonObject message, CancellationToken cancellationToken)
    {
        EnsureStarted();
        var line = message.ToJsonString(JsonOptions.Compact);
        if (Encoding.UTF8.GetByteCount(line) > AgentHostProtocol.MaximumFrameBytes)
        {
            throw new InvalidDataException("Codex JSON-RPC frame exceeds the configured limit.");
        }
        await writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await process!.StandardInput.WriteLineAsync(line.AsMemory(), cancellationToken).ConfigureAwait(false);
            await process.StandardInput.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            writeLock.Release();
        }
    }

    private async Task ReadStdoutAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
            if (line is null) break;
            if (string.IsNullOrWhiteSpace(line)) continue;
            if (Encoding.UTF8.GetByteCount(line) > AgentHostProtocol.MaximumFrameBytes)
            {
                await EmitAsync(new CodexEvent("error", ErrorCode: "codex.frame_too_large", Text: "Codex emitted an oversized frame.")).ConfigureAwait(false);
                continue;
            }
            try
            {
                using var document = JsonDocument.Parse(line);
                await ProcessMessageAsync(document.RootElement.Clone()).ConfigureAwait(false);
            }
            catch (JsonException)
            {
                await EmitAsync(new CodexEvent("diagnostic", ErrorCode: "codex.non_json_stdout", Text: "Codex emitted a non-JSON stdout frame.")).ConfigureAwait(false);
            }
        }
    }

    private async Task ProcessMessageAsync(JsonElement message)
    {
        if (message.TryGetProperty("id", out var idElement) && !message.TryGetProperty("method", out _))
        {
            if (idElement.ValueKind == JsonValueKind.Number && idElement.TryGetInt64(out var id) && pending.TryGetValue(id, out var completion))
            {
                if (message.TryGetProperty("error", out var error))
                {
                    var code = error.TryGetProperty("code", out var codeElement) ? codeElement.ToString() : "codex.rpc_error";
                    var text = error.TryGetProperty("message", out var messageElement) ? messageElement.GetString() : "Codex JSON-RPC request failed.";
                    completion.TrySetException(new CodexRpcException(code, text ?? "Codex JSON-RPC request failed."));
                }
                else if (message.TryGetProperty("result", out var result))
                {
                    completion.TrySetResult(result.Clone());
                }
            }
            return;
        }

        if (!message.TryGetProperty("method", out var methodElement)) return;
        var method = methodElement.GetString() ?? string.Empty;
        var parameters = message.TryGetProperty("params", out var paramsElement) ? paramsElement.Clone() : default;
        if (message.TryGetProperty("id", out var serverRequestId))
        {
            var requestId = serverRequestId.ToString();
            if (IsApprovalMethod(method))
            {
                pendingApprovals[requestId] = new PendingServerRequest(method, parameters);
                await EmitAsync(new CodexEvent(
                    "approval.requested",
                    ThreadId: ReadString(parameters, "threadId"),
                    TurnId: ReadString(parameters, "turnId"),
                    ItemId: ReadString(parameters, "itemId"),
                    Text: ReadString(parameters, "reason") ?? ReadString(parameters, "command"),
                    ToolName: ApprovalKind(method),
                    RequestId: requestId,
                    Detail: parameters.ValueKind == JsonValueKind.Undefined ? null : parameters)).ConfigureAwait(false);
            }
            else
            {
                await SendErrorResponseAsync(requestId, -32601, $"Unsupported Codex server request '{method}'.", CancellationToken.None).ConfigureAwait(false);
                await EmitAsync(new CodexEvent("diagnostic", ErrorCode: "codex.unsupported_server_request", Text: method)).ConfigureAwait(false);
            }
            return;
        }

        await TranslateNotificationAsync(method, parameters).ConfigureAwait(false);
    }

    private ValueTask TranslateNotificationAsync(string method, JsonElement parameters)
    {
        string? GetString(string name)
            => parameters.ValueKind == JsonValueKind.Object && parameters.TryGetProperty(name, out var value) ? value.GetString() : null;

        if (method == "mcpServer/startupStatus/updated")
        {
            var name = GetString("name") ?? string.Empty;
            var threadId = GetString("threadId") ?? string.Empty;
            var status = GetString("status") ?? string.Empty;
            var error = GetString("error");
            if (name == "worlddata" && !string.IsNullOrWhiteSpace(threadId))
            {
                var connection = new McpConnectionInfo(
                    status == "ready",
                    name,
                    0,
                    status == "ready" ? null : error ?? $"WorldData MCP startup state is '{status}'.");
                threadMcpStatus[threadId] = connection;
                var readiness = mcpReadiness.GetOrAdd(threadId, _ => NewMcpReadiness());
                if (connection.Connected || !string.Equals(status, "starting", StringComparison.OrdinalIgnoreCase)) readiness.TrySetResult(connection);
            }
            return EmitAsync(new CodexEvent(
                "mcp.status",
                ThreadId: threadId,
                Text: status,
                ToolName: name,
                ErrorCode: error,
                Detail: parameters,
                McpConnected: status == "ready",
                McpToolCount: 0));
        }

        return method switch
        {
            "item/agentMessage/delta" => EmitAsync(new CodexEvent(
                "message.delta",
                ThreadId: GetString("threadId"),
                TurnId: GetString("turnId"),
                ItemId: GetString("itemId"),
                Text: GetString("delta"),
                Detail: parameters)),
            "turn/started" => EmitAsync(new CodexEvent(
                "turn.started",
                ThreadId: GetString("threadId"),
                TurnId: ReadNestedId(parameters, "turn"),
                Detail: parameters)),
            "turn/completed" => EmitAsync(new CodexEvent(
                "turn.completed",
                ThreadId: GetString("threadId"),
                TurnId: ReadNestedId(parameters, "turn"),
                Detail: parameters)),
            "item/started" => ItemEvent("item.started", parameters),
            "item/completed" => ItemEvent("item.completed", parameters),
            _ => ValueTask.CompletedTask
        };
    }

    private ValueTask ItemEvent(string type, JsonElement parameters)
    {
        var item = parameters.ValueKind == JsonValueKind.Object
            && parameters.TryGetProperty("item", out var itemElement)
                ? itemElement
                : default;
        var itemType = ReadString(item, "type");
        var server = ReadString(item, "server");
        var tool = ReadString(item, "tool");
        var toolName = itemType == "mcpToolCall"
            ? string.Join('.', new[] { server, tool }.Where(value => !string.IsNullOrWhiteSpace(value)))
            : itemType;
        var (kind, role) = itemType switch
        {
            "mcpToolCall" or "commandExecution" or "fileChange" => ("tool", "tool"),
            "userMessage" => ("message", "user"),
            "agentMessage" => ("message", "assistant"),
            "reasoning" => ("activity", "assistant"),
            _ => ("activity", "assistant")
        };
        var status = ReadValue(item, "status");
        return EmitAsync(new CodexEvent(
            type,
            ThreadId: ReadString(parameters, "threadId"),
            TurnId: ReadString(parameters, "turnId"),
            ItemId: ReadString(item, "id"),
            Text: ItemDisplayText(item, itemType),
            ToolName: toolName,
            Detail: parameters,
            ItemKind: kind,
            ItemRole: role,
            ItemStatus: string.IsNullOrWhiteSpace(status) ? (type == "item.completed" ? "completed" : "running") : status));
    }

    private static string? ItemDisplayText(JsonElement item, string? itemType) => itemType switch
    {
        "commandExecution" => ReadString(item, "command"),
        "fileChange" => "File changes",
        "reasoning" => "正在处理",
        _ => ReadString(item, "text") ?? ReadValue(item, "status")
    };

    private static TaskCompletionSource<McpConnectionInfo> NewMcpReadiness()
        => new(TaskCreationOptions.RunContinuationsAsynchronously);

    private void OnCodexExited()
    {
        if (lifetime.IsCancellationRequested) return;
        foreach (var completion in pending.Values)
        {
            completion.TrySetException(new IOException("Codex app-server exited unexpectedly."));
        }
        _ = EmitAsync(new CodexEvent(
            "codex.exited",
            Text: "Codex app-server exited unexpectedly.",
            ErrorCode: "codex.exited",
            Retryable: true));
    }

    private static string? ReadNestedId(JsonElement value, string objectName)
    {
        return value.ValueKind == JsonValueKind.Object
            && value.TryGetProperty(objectName, out var nested)
            && nested.ValueKind == JsonValueKind.Object
            && nested.TryGetProperty("id", out var id)
                ? id.GetString()
                : null;
    }

    private static string? ReadString(JsonElement value, string name)
        => value.ValueKind == JsonValueKind.Object
            && value.TryGetProperty(name, out var property)
            && property.ValueKind == JsonValueKind.String
                ? property.GetString()
                : null;

    private static bool IsApprovalMethod(string method)
        => method is "item/commandExecution/requestApproval"
            or "item/fileChange/requestApproval"
            or "item/permissions/requestApproval";

    private static string ApprovalKind(string method) => method switch
    {
        "item/commandExecution/requestApproval" => "commandExecution",
        "item/fileChange/requestApproval" => "fileChange",
        "item/permissions/requestApproval" => "permissions",
        _ => "unknown"
    };

    private static JsonNode RequestedPermissions(JsonElement parameters)
    {
        if (parameters.ValueKind == JsonValueKind.Object
            && parameters.TryGetProperty("permissions", out var permissions)
            && permissions.ValueKind == JsonValueKind.Object)
        {
            return JsonNode.Parse(permissions.GetRawText()) ?? new JsonObject();
        }
        return new JsonObject();
    }

    private Task SendErrorResponseAsync(string requestId, int code, string message, CancellationToken cancellationToken)
    {
        JsonNode id = long.TryParse(requestId, out var numericId)
            ? JsonValue.Create(numericId)!
            : JsonValue.Create(requestId)!;
        return WriteAsync(new JsonObject
        {
            ["id"] = id,
            ["error"] = new JsonObject
            {
                ["code"] = code,
                ["message"] = message
            }
        }, cancellationToken);
    }

    private async Task ReadStderrAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
            if (line is null) break;
            if (!string.IsNullOrWhiteSpace(line))
            {
                await EmitAsync(new CodexEvent("diagnostic", ErrorCode: "codex.stderr", Text: Redact(line))).ConfigureAwait(false);
            }
        }
    }

    private static string Redact(string text)
    {
        if (text.Length > 4096) text = text[..4096];
        foreach (var key in new[] { "authorization", "bearer", "access_token", "api_key", "token", "secret" })
        {
            var index = text.IndexOf(key, StringComparison.OrdinalIgnoreCase);
            if (index >= 0) return $"{text[..index]}{key}=[REDACTED]";
        }
        return text;
    }

    private ValueTask EmitAsync(CodexEvent value)
        => EventReceived is { } handler ? handler(value) : ValueTask.CompletedTask;

    private void EnsureStarted()
    {
        if (process is null || process.HasExited)
        {
            throw new InvalidOperationException("Codex app-server is not running.");
        }
    }

    public async ValueTask DisposeAsync()
    {
        lifetime.Cancel();
        foreach (var completion in pending.Values)
        {
            completion.TrySetCanceled();
        }
        if (process is { HasExited: false })
        {
            process.StandardInput.Close();
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            try { await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) { process.Kill(entireProcessTree: true); }
        }
        if (stdoutTask is not null) await IgnoreCancellation(stdoutTask).ConfigureAwait(false);
        if (stderrTask is not null) await IgnoreCancellation(stderrTask).ConfigureAwait(false);
        process?.Dispose();
        childProcessJob?.Dispose();
        childProcessJob = null;
        writeLock.Dispose();
        lifetime.Dispose();
    }

    private static async Task IgnoreCancellation(Task task)
    {
        try { await task.ConfigureAwait(false); }
        catch (OperationCanceledException) { }
    }

    private sealed record PendingServerRequest(string Method, JsonElement Parameters);
}

internal sealed class CodexRpcException(string code, string message) : Exception(message)
{
    public string Code { get; } = code;
}

internal static class JsonOptions
{
    public static readonly JsonSerializerOptions Compact = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = false
    };
}
