# WorldData Agent Host

`WorldData.AgentHost.App` is the only executable composition root. It exposes
WorldData Agent Host IPC v1 on stdin/stdout and keeps the version-specific Codex
app-server JSON-RPC protocol inside `WorldData.AgentHost.Codex`.

The MCP credential is inherited through the private child-process environment.
It is never part of the versioned JSONL protocol or command-line arguments.

- `Contracts`: the only public .NET assembly surface; DTOs and interfaces only.
- `Codex`: internal app-server transport and protocol implementation, visible only to the composition root.
- `App`: internal command routing and dependency composition.
- `protocol`: the stable, versioned UE-to-host wire contract.

The Unreal side follows the same boundary: implementation modules expose one
`I...Module` header from `Public/`; concrete gateways, processes, runtime
installers, diagnostics, security storage, the web bridge, and widgets remain in
`Private/`.

Build a self-contained Windows runtime with:

```powershell
dotnet publish .\WorldData.AgentHost.App\WorldData.AgentHost.App.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=false
```

Run the transport/auth/model-list handshake probe with:

```powershell
node .\tests\protocol-probe.mjs <host-exe> <codex-exe> <project-root>
```

Run deterministic fault and module-boundary checks with:

```powershell
node .\tests\fault-probe.mjs <host-exe> <project-root>
node .\tests\verify-module-boundaries.mjs
node .\tests\verify-ui-contract.mjs
```
