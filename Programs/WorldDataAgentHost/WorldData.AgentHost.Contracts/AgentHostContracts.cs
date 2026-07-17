using System.Text.Json;

namespace WorldData.AgentHost.Contracts;

public static class AgentHostProtocol
{
    public const int CurrentVersion = 2;
    public const int MinimumVersion = 2;
    public const int MaximumFrameBytes = 1024 * 1024;
    public const string ThreadSource = "worlddata-unreal-agent";
}

public sealed record HostCommandEnvelope(
    int ProtocolVersion,
    string Id,
    string Type,
    JsonElement Payload);

public sealed record HostEventEnvelope(
    int ProtocolVersion,
    string Type,
    string? RequestId,
    object? Payload,
    string? SessionId = null,
    long Sequence = 0);

public sealed record CodexStartOptions(
    string Executable,
    string ProjectRoot,
    string McpUrl,
    string McpToken,
    string ClientVersion);

public sealed record CodexConnectionInfo(
    string UserAgent,
    bool Authenticated,
    JsonElement Account,
    JsonElement Models);

public sealed record McpConnectionInfo(
    bool Connected,
    string ServerName,
    int ToolCount,
    string? Error);

public sealed record CreateThreadOptions(
    string ClientConversationId,
    string WorkingDirectory,
    string? Model,
    string? ApprovalPolicy,
    string? SandboxMode,
    bool Ephemeral);

public sealed record ResumeThreadOptions(
    string ThreadId,
    string WorkingDirectory,
    string? Model,
    string? ApprovalPolicy,
    string? SandboxMode);

public sealed record ThreadSummary(
    string Id,
    string Title,
    string Preview,
    string WorkingDirectory,
    long CreatedAt,
    long UpdatedAt,
    long RecencyAt,
    string ThreadSource,
    string Status);

public sealed record ConversationItem(
    string Id,
    string TurnId,
    string Kind,
    string Role,
    string Text,
    string? ToolName,
    string? Status);

public sealed record ThreadSnapshot(
    ThreadSummary Thread,
    IReadOnlyList<ConversationItem> Items);

public sealed record ThreadListPage(
    IReadOnlyList<ThreadSummary> Threads,
    string? NextCursor);

public sealed record TurnOptions(
    string ThreadId,
    string ClientTurnId,
    string Text);

public sealed record ApprovalDecision(
    string RequestId,
    bool Approved);

public sealed record CodexEvent(
    string Type,
    string? ThreadId = null,
    string? TurnId = null,
    string? ItemId = null,
    string? Text = null,
    string? ToolName = null,
    string? RequestId = null,
    string? ErrorCode = null,
    bool Retryable = false,
    JsonElement? Detail = null,
    string? ItemKind = null,
    string? ItemRole = null,
    string? ItemStatus = null,
    bool? McpConnected = null,
    int? McpToolCount = null);

public interface IHostEventSink
{
    ValueTask WriteAsync(HostEventEnvelope message, CancellationToken cancellationToken);
}

public interface ICodexClient : IAsyncDisposable
{
    event Func<CodexEvent, ValueTask>? EventReceived;

    Task<CodexConnectionInfo> StartAsync(CodexStartOptions options, CancellationToken cancellationToken);
    Task<ThreadListPage> ListThreadsAsync(string? cursor, int limit, CancellationToken cancellationToken);
    Task<string> CreateThreadAsync(CreateThreadOptions options, CancellationToken cancellationToken);
    Task<ThreadSnapshot> ResumeThreadAsync(ResumeThreadOptions options, CancellationToken cancellationToken);
    Task<ThreadSnapshot> ReadThreadAsync(string threadId, CancellationToken cancellationToken);
    Task<string> StartTurnAsync(TurnOptions options, CancellationToken cancellationToken);
    Task InterruptTurnAsync(string threadId, string turnId, CancellationToken cancellationToken);
    Task ResolveApprovalAsync(ApprovalDecision decision, CancellationToken cancellationToken);
    Task<McpConnectionInfo> GetMcpStatusAsync(string threadId, CancellationToken cancellationToken);
    Task<McpConnectionInfo> WaitForMcpReadyAsync(string threadId, CancellationToken cancellationToken);
}

public interface ICodexModule
{
    ICodexClient CreateClient();
}

public interface IAgentHostApplication
{
    Task<int> RunAsync(Stream input, Stream output, CancellationToken cancellationToken);
}
