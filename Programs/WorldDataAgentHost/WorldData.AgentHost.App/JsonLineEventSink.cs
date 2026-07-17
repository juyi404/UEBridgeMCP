using System.Text;
using System.Text.Json;
using WorldData.AgentHost.Contracts;

namespace WorldData.AgentHost.App;

internal sealed class JsonLineEventSink(Stream output, JsonSerializerOptions options) : IHostEventSink
{
    private readonly SemaphoreSlim writeLock = new(1, 1);
    private readonly StreamWriter writer = new(output, new UTF8Encoding(false), 16 * 1024, leaveOpen: true)
    {
        AutoFlush = false,
        NewLine = "\n"
    };

    public async ValueTask WriteAsync(HostEventEnvelope message, CancellationToken cancellationToken)
    {
        var json = JsonSerializer.Serialize(message, options);
        if (Encoding.UTF8.GetByteCount(json) > AgentHostProtocol.MaximumFrameBytes)
        {
            throw new InvalidDataException("IPC event exceeds the 1 MiB limit.");
        }
        await writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await writer.WriteLineAsync(json.AsMemory(), cancellationToken).ConfigureAwait(false);
            await writer.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            writeLock.Release();
        }
    }
}
