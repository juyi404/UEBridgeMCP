using System.Buffers;
using System.Text;

namespace WorldData.AgentHost.App;

/// <summary>Reads a UTF-8 JSONL frame without allocating an unbounded line.</summary>
internal sealed class BoundedJsonLineReader(Stream input, int maximumFrameBytes)
{
    private readonly byte[] buffer = new byte[16 * 1024];
    private int position;
    private int available;

    public async Task<string?> ReadLineAsync(CancellationToken cancellationToken)
    {
        var line = new ArrayBufferWriter<byte>(Math.Min(maximumFrameBytes, 16 * 1024));
        var sawData = false;
        while (true)
        {
            if (position == available)
            {
                available = await input.ReadAsync(buffer.AsMemory(), cancellationToken).ConfigureAwait(false);
                position = 0;
                if (available == 0) return sawData ? Decode(line.WrittenSpan) : null;
            }

            var segmentStart = position;
            while (position < available && buffer[position] != (byte)'\n') position++;
            var segmentLength = position - segmentStart;
            if (line.WrittenCount + segmentLength > maximumFrameBytes)
            {
                if (position < available) position++;
                else await DrainToLineEndAsync(cancellationToken).ConfigureAwait(false);
                throw new FrameTooLargeException();
            }
            if (segmentLength > 0)
            {
                line.Write(buffer.AsSpan(segmentStart, segmentLength));
                sawData = true;
            }
            if (position < available)
            {
                position++;
                return Decode(line.WrittenSpan);
            }
        }
    }

    private async Task DrainToLineEndAsync(CancellationToken cancellationToken)
    {
        while (true)
        {
            if (position == available)
            {
                available = await input.ReadAsync(buffer.AsMemory(), cancellationToken).ConfigureAwait(false);
                position = 0;
                if (available == 0) return;
            }
            while (position < available)
            {
                if (buffer[position++] == (byte)'\n') return;
            }
        }
    }

    private static string Decode(ReadOnlySpan<byte> bytes)
    {
        if (!bytes.IsEmpty && bytes[^1] == (byte)'\r') bytes = bytes[..^1];
        return Encoding.UTF8.GetString(bytes);
    }
}

internal sealed class FrameTooLargeException : Exception
{
}
