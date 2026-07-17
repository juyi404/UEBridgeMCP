using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using WorldData.AgentHost.Contracts;

namespace WorldData.AgentHost.App;

internal sealed class AgentHostApplication(ICodexModule codexModule) : IAgentHostApplication, IAsyncDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        WriteIndented = false
    };

    private readonly ICodexClient codex = codexModule.CreateClient();
    private readonly object operationLock = new();
    private readonly Dictionary<string, Task<string>> createOperations = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Task<string>> turnOperations = new(StringComparer.Ordinal);
    private IHostEventSink? sink;
    private bool connected;

    public async Task<int> RunAsync(Stream input, Stream output, CancellationToken cancellationToken)
    {
        const string capabilities = "sequenced-events,item-semantics,mcp-readiness,operation-dedup";
        sink = new JsonLineEventSink(output, JsonOptions, Guid.NewGuid().ToString("N"));
        codex.EventReceived += ForwardCodexEventAsync;
        var reader = new BoundedJsonLineReader(input, AgentHostProtocol.MaximumFrameBytes);

        await sink.WriteAsync(new HostEventEnvelope(
            AgentHostProtocol.CurrentVersion,
            "host.started",
            null,
            new
            {
                hostVersion = typeof(AgentHostApplication).Assembly.GetName().Version?.ToString() ?? "2.0.0",
                minimumProtocolVersion = AgentHostProtocol.MinimumVersion,
                maximumProtocolVersion = AgentHostProtocol.CurrentVersion,
                capabilities = capabilities.Split(',', StringSplitOptions.RemoveEmptyEntries)
            }), cancellationToken);

        var pendingCommands = new List<Task>();

        while (!cancellationToken.IsCancellationRequested)
        {
            string? line;
            try
            {
                line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (FrameTooLargeException)
            {
                await WriteErrorAsync(null, "host.frame_too_large", "IPC frame exceeds the 1 MiB limit.", false, cancellationToken);
                continue;
            }
            if (line is null) break;
            if (string.IsNullOrWhiteSpace(line)) continue;

            HostCommandEnvelope? command;
            try
            {
                command = JsonSerializer.Deserialize<HostCommandEnvelope>(line, JsonOptions);
            }
            catch (JsonException)
            {
                await WriteErrorAsync(null, "host.invalid_json", "IPC command is not valid JSON.", false, cancellationToken);
                continue;
            }
            if (command is null || string.IsNullOrWhiteSpace(command.Id) || string.IsNullOrWhiteSpace(command.Type)
                || command.Id.Length > 128 || command.Type.Length > 128 || command.Payload.ValueKind != JsonValueKind.Object)
            {
                await WriteErrorAsync(command?.Id, "host.invalid_command", "IPC command requires id and type.", false, cancellationToken);
                continue;
            }
            if (command.ProtocolVersion < AgentHostProtocol.MinimumVersion || command.ProtocolVersion > AgentHostProtocol.CurrentVersion)
            {
                await WriteErrorAsync(command.Id, "host.protocol_mismatch", $"Unsupported protocol version {command.ProtocolVersion}.", false, cancellationToken);
                continue;
            }

            if (string.Equals(command.Type, "shutdown", StringComparison.Ordinal))
            {
                await ExecuteCommandAsync(command, cancellationToken).ConfigureAwait(false);
                break;
            }

            // Codex JSON-RPC is multiplexed. Do not let a slow history read or
            // MCP startup wait block interruption or approval decisions.
            pendingCommands.Add(ExecuteCommandAsync(command, cancellationToken));
            pendingCommands.RemoveAll(task => task.IsCompleted);
        }
        if (pendingCommands.Count > 0) await Task.WhenAll(pendingCommands).ConfigureAwait(false);
        return 0;
    }

    private async Task ExecuteCommandAsync(HostCommandEnvelope command, CancellationToken cancellationToken)
    {
        try
        {
            await HandleCommandAsync(command, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            await WriteErrorAsync(command.Id, ErrorCode(exception), SafeMessage(exception), IsRetryable(exception), cancellationToken);
        }
    }

    private async Task<bool> HandleCommandAsync(HostCommandEnvelope command, CancellationToken cancellationToken)
    {
        switch (command.Type)
        {
            case "connect":
            {
                var request = command.Payload.Deserialize<ConnectRequest>(JsonOptions)
                    ?? throw new InvalidDataException("connect payload is required.");
                var info = await codex.StartAsync(new CodexStartOptions(
                    Require(request.CodexExecutable, "codexExecutable"),
                    Require(request.ProjectRoot, "projectRoot"),
                    Require(request.McpUrl, "mcpUrl"),
                    ResolveMcpToken(request.McpToken),
                    Require(request.ClientVersion, "clientVersion")), cancellationToken);
                connected = true;
                await WriteResultAsync(command.Id, "connect.completed", new
                {
                    state = info.Authenticated ? "ready" : "checkingAuth",
                    info.Authenticated,
                    info.UserAgent,
                    account = info.Account,
                    models = info.Models
                }, cancellationToken);
                return false;
            }
            case "createThread":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<CreateThreadRequest>(JsonOptions)
                    ?? throw new InvalidDataException("createThread payload is required.");
                var clientConversationId = Require(request.ClientConversationId, "clientConversationId");
                var threadId = await StartOperationOnceAsync(createOperations, clientConversationId, () => codex.CreateThreadAsync(new CreateThreadOptions(
                    clientConversationId,
                    Require(request.WorkingDirectory, "workingDirectory"),
                    request.Model,
                    request.ApprovalPolicy,
                    request.SandboxMode,
                    request.Ephemeral), cancellationToken)).ConfigureAwait(false);
                var mcpStatus = await codex.WaitForMcpReadyAsync(threadId, cancellationToken).ConfigureAwait(false);
                await WriteResultAsync(command.Id, "thread.created", new { threadId, mcp = mcpStatus }, cancellationToken);
                return false;
            }
            case "listThreads":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<ListThreadsRequest>(JsonOptions)
                    ?? throw new InvalidDataException("listThreads payload is required.");
                var page = await codex.ListThreadsAsync(request.Cursor, request.Limit <= 0 ? 50 : request.Limit, cancellationToken);
                await WriteResultAsync(command.Id, "threads.listed", new
                {
                    threads = page.Threads,
                    page.NextCursor
                }, cancellationToken);
                return false;
            }
            case "resumeThread":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<ResumeThreadRequest>(JsonOptions)
                    ?? throw new InvalidDataException("resumeThread payload is required.");
                var snapshot = await codex.ResumeThreadAsync(new ResumeThreadOptions(
                    Require(request.ThreadId, "threadId"),
                    Require(request.WorkingDirectory, "workingDirectory"),
                    request.Model,
                    request.ApprovalPolicy,
                    request.SandboxMode), cancellationToken);
                var mcpStatus = await codex.WaitForMcpReadyAsync(snapshot.Thread.Id, cancellationToken).ConfigureAwait(false);
                await WriteResultAsync(command.Id, "thread.resumed", new
                {
                    threadId = snapshot.Thread.Id,
                    thread = snapshot.Thread,
                    items = snapshot.Items,
                    mcp = mcpStatus
                }, cancellationToken);
                return false;
            }
            case "readThread":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<ReadThreadRequest>(JsonOptions)
                    ?? throw new InvalidDataException("readThread payload is required.");
                var snapshot = await codex.ReadThreadAsync(Require(request.ThreadId, "threadId"), cancellationToken);
                await WriteResultAsync(command.Id, "thread.loaded", new
                {
                    threadId = snapshot.Thread.Id,
                    thread = snapshot.Thread,
                    items = snapshot.Items
                }, cancellationToken);
                return false;
            }
            case "sendTurn":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<SendTurnRequest>(JsonOptions)
                    ?? throw new InvalidDataException("sendTurn payload is required.");
                var threadId = Require(request.ThreadId, "threadId");
                var clientTurnId = Require(request.ClientTurnId, "clientTurnId");
                var turnId = await StartOperationOnceAsync(turnOperations, $"{threadId}:{clientTurnId}", () => codex.StartTurnAsync(new TurnOptions(
                    threadId,
                    clientTurnId,
                    Require(request.Text, "text")), cancellationToken)).ConfigureAwait(false);
                await WriteResultAsync(command.Id, "turn.accepted", new { request.ThreadId, turnId }, cancellationToken);
                return false;
            }
            case "interruptTurn":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<InterruptTurnRequest>(JsonOptions)
                    ?? throw new InvalidDataException("interruptTurn payload is required.");
                await codex.InterruptTurnAsync(
                    Require(request.ThreadId, "threadId"),
                    Require(request.TurnId, "turnId"),
                    cancellationToken);
                await WriteResultAsync(command.Id, "turn.interrupted", new { request.ThreadId, request.TurnId }, cancellationToken);
                return false;
            }
            case "resolveApproval":
            {
                EnsureConnected();
                var request = command.Payload.Deserialize<ResolveApprovalRequest>(JsonOptions)
                    ?? throw new InvalidDataException("resolveApproval payload is required.");
                await codex.ResolveApprovalAsync(
                    new ApprovalDecision(Require(request.RequestId, "requestId"), request.Approved),
                    cancellationToken);
                await WriteResultAsync(command.Id, "approval.resolved", new { request.RequestId, request.Approved }, cancellationToken);
                return false;
            }
            case "shutdown":
                await WriteResultAsync(command.Id, "host.stopping", new { }, cancellationToken);
                return true;
            default:
                throw new InvalidDataException($"Unknown IPC command '{command.Type}'.");
        }
    }

    private ValueTask ForwardCodexEventAsync(CodexEvent value)
    {
        if (sink is null) return ValueTask.CompletedTask;
        var payload = new
        {
            value.ThreadId,
            value.TurnId,
            value.ItemId,
            value.Text,
            value.ToolName,
            value.RequestId,
            value.ErrorCode,
            value.Retryable,
            value.Detail,
            value.ItemKind,
            value.ItemRole,
            value.ItemStatus,
            value.McpConnected,
            value.McpToolCount
        };
        return sink.WriteAsync(new HostEventEnvelope(AgentHostProtocol.CurrentVersion, value.Type, null, payload), CancellationToken.None);
    }

    private ValueTask WriteResultAsync(string requestId, string type, object payload, CancellationToken cancellationToken)
        => sink!.WriteAsync(new HostEventEnvelope(AgentHostProtocol.CurrentVersion, type, requestId, payload), cancellationToken);

    private ValueTask WriteErrorAsync(string? requestId, string code, string message, bool retryable, CancellationToken cancellationToken)
        => sink!.WriteAsync(new HostEventEnvelope(AgentHostProtocol.CurrentVersion, "error", requestId, new
        {
            code,
            message,
            component = "WorldDataAgentHost",
            retryable
        }), cancellationToken);

    private void EnsureConnected()
    {
        if (!connected) throw new HostNotConnectedException();
    }

    private static string ResolveMcpToken(string? suppliedToken)
    {
        if (!string.IsNullOrWhiteSpace(suppliedToken)) return suppliedToken;

        // Compatibility for an already-built UE client during the protocol v1
        // migration. Current clients provide this through their private stdio
        // channel; the Host never writes or changes the parent environment.
        return Require(Environment.GetEnvironmentVariable("WORLDDATA_MCP_TOKEN"), "mcpToken");
    }

    private static string Require(string? value, string fieldName)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new InvalidDataException($"'{fieldName}' must be a non-empty string.");
        }
        return value;
    }

    private static string ErrorCode(Exception exception) => exception switch
    {
        FileNotFoundException => "runtime.not_found",
        UnauthorizedAccessException => "runtime.access_denied",
        TimeoutException => "codex.timeout",
        HostNotConnectedException => "host.not_connected",
        InvalidDataException => "host.invalid_command",
        _ => "host.operation_failed"
    };

    private static bool IsRetryable(Exception exception)
        => exception is IOException or TimeoutException or InvalidOperationException;

    private static string SafeMessage(Exception exception)
    {
        var message = exception.Message;
        if (message.Length > 2048) message = message[..2048];
        foreach (var key in new[] { "authorization", "bearer", "access_token", "api_key", "token", "secret" })
        {
            var index = message.IndexOf(key, StringComparison.OrdinalIgnoreCase);
            if (index >= 0) return $"{message[..index]}{key}=[REDACTED]";
        }
        return message;
    }

    private Task<string> StartOperationOnceAsync(
        Dictionary<string, Task<string>> operations,
        string key,
        Func<Task<string>> start)
    {
        lock (operationLock)
        {
            if (operations.TryGetValue(key, out var existing)) return existing;
            var operation = start();
            operations[key] = operation;
            _ = operation.ContinueWith(_ =>
            {
                if (!operation.IsFaulted && !operation.IsCanceled) return;
                lock (operationLock)
                {
                    if (operations.TryGetValue(key, out var candidate) && ReferenceEquals(candidate, operation)) operations.Remove(key);
                }
            }, TaskScheduler.Default);
            return operation;
        }
    }

    public async ValueTask DisposeAsync()
    {
        codex.EventReceived -= ForwardCodexEventAsync;
        await codex.DisposeAsync();
    }

    private sealed record ConnectRequest(
        string CodexExecutable,
        string ProjectRoot,
        string McpUrl,
        string? McpToken,
        string ClientVersion);

    private sealed record CreateThreadRequest(
        string ClientConversationId,
        string WorkingDirectory,
        string? Model,
        string? ApprovalPolicy,
        string? SandboxMode,
        bool Ephemeral = false);

    private sealed record ListThreadsRequest(string? Cursor, int Limit);

    private sealed record ResumeThreadRequest(
        string ThreadId,
        string WorkingDirectory,
        string? Model,
        string? ApprovalPolicy,
        string? SandboxMode);

    private sealed record ReadThreadRequest(string ThreadId);

    private sealed record SendTurnRequest(string ThreadId, string ClientTurnId, string Text);
    private sealed record InterruptTurnRequest(string ThreadId, string TurnId);
    private sealed record ResolveApprovalRequest(string RequestId, bool Approved);

    private sealed class HostNotConnectedException()
        : InvalidOperationException("Agent Host is not connected to Codex.");
}
