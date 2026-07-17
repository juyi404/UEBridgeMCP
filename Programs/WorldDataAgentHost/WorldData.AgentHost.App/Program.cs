using WorldData.AgentHost.App;
using WorldData.AgentHost.Codex;

if (args.Contains("--version", StringComparer.Ordinal))
{
    Console.WriteLine("worlddata-agent-host 1.0.0");
    return 0;
}

using var shutdown = new CancellationTokenSource();
Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    shutdown.Cancel();
};

await using var application = new AgentHostApplication(new CodexModule());
return await application.RunAsync(Console.OpenStandardInput(), Console.OpenStandardOutput(), shutdown.Token);
