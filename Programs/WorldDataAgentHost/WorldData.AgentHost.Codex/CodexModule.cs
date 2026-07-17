using WorldData.AgentHost.Contracts;

namespace WorldData.AgentHost.Codex;

internal sealed class CodexModule : ICodexModule
{
    public ICodexClient CreateClient() => new CodexAppServerClient();
}
