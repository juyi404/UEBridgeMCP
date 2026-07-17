import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const testsDirectory = dirname(fileURLToPath(import.meta.url));
const hostRoot = resolve(testsDirectory, "..");
const projectRoot = resolve(hostRoot, "../..");
const pluginRoot = join(projectRoot, "Plugins", "UEBridgeMCP");
const read = (...segments) => readFileSync(join(...segments), "utf8");
const failures = [];
const expect = (condition, message) => {
  if (!condition) failures.push(message);
};

const descriptor = JSON.parse(read(pluginRoot, "UEBridgeMCP.uplugin"));
expect(Array.isArray(descriptor.SupportedTargetPlatforms)
  && descriptor.SupportedTargetPlatforms.length === 1
  && descriptor.SupportedTargetPlatforms[0] === "Win64", "UEBridgeMCP must explicitly declare Win64 support only");

const clientSource = read(pluginRoot, "Source", "WorldDataAgentClient", "Private", "WorldDataAgentClientModule.cpp");
expect(/const FTCHARToUTF8 Utf8Json\(\*Json\);/.test(clientSource), "UE client must measure outbound frames as UTF-8 bytes");
expect(/Utf8Json\.Length\(\) > WorldDataAgentProtocol::MaximumOutboundPayloadBytes/.test(clientSource), "UE client must reserve a byte below the Host frame limit");
expect(!/Json\.Len\(\) >/.test(clientSource), "UE client must not use UTF-16 character counts for frame limits");

const hostProcessSource = read(pluginRoot, "Source", "WorldDataAgentClient", "Private", "WorldDataAgentHostProcess.cpp");
expect(!/SetEnvironmentVar|GetEnvironmentVariable|SecretEnvironmentVariable|SecretValue/.test(hostProcessSource), "UE process launcher must not mutate the editor environment for credentials");
expect(/Converted\.Length\(\) > WorldDataAgentProtocol::MaximumOutboundPayloadBytes/.test(hostProcessSource), "host process must defend the byte limit before writing a frame");

const schema = JSON.parse(read(hostRoot, "protocol", "worlddata-agent-host-v2.schema.json"));
expect(schema.$defs.command.properties.protocolVersion.const === 2, "IPC schema must require protocol v2");
expect(/string\? McpToken/.test(read(hostRoot, "WorldData.AgentHost.App", "AgentHostApplication.cs")), "connect must define the private MCP credential");

const codexSource = read(hostRoot, "WorldData.AgentHost.Codex", "CodexAppServerClient.cs");
expect(/options\.McpToken/.test(codexSource), "Codex child must receive the credential from the private handshake");
expect(!/Environment\.SetEnvironmentVariable/.test(codexSource), "Agent Host must not mutate its inherited global credential environment");
expect(/startOptions = options with \{ McpToken = string\.Empty \};/.test(codexSource), "Agent Host must not retain the handshake credential after startup");
const jobSource = read(hostRoot, "WorldData.AgentHost.Codex", "WindowsChildProcessJob.cs");
expect(/JobObjectLimitKillOnJobClose/.test(jobSource) && /AssignProcessToJobObject/.test(jobSource), "Codex child must be bound to a kill-on-close Host job");

const serverSource = read(pluginRoot, "Source", "UEBridgeMCPCore", "Private", "WorldDataMCPServer.cpp");
const governanceSource = read(pluginRoot, "Source", "UEBridgeMCPCore", "Private", "WorldDataMCPToolGovernance.cpp");
expect(!/FMD5|MakeNonSecretHash/.test(serverSource), "approval audit must not use MD5 or value-derived hashes");
expect(/MakeArgumentShapeFingerprint\(ExecutionArguments\)/.test(serverSource), "approval audit must derive its summary from argument names and types only");
expect(/changeSummaryFingerprint/.test(governanceSource) && !/changeSummaryHash/.test(governanceSource), "audit output must identify the redacted summary as a fingerprint");

console.log(JSON.stringify({ ok: failures.length === 0, checks: 14, failures }));
process.exit(failures.length === 0 ? 0 : 1);
