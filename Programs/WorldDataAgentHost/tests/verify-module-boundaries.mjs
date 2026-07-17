import { existsSync, readFileSync, readdirSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const testsDirectory = dirname(fileURLToPath(import.meta.url));
const hostRoot = resolve(testsDirectory, "..");
const projectRoot = resolve(hostRoot, "../..");
const standalonePluginSource = join(projectRoot, "Source");
const embeddedPluginSource = join(projectRoot, "Plugins", "UEBridgeMCP", "Source");
const pluginSource = existsSync(standalonePluginSource)
  ? standalonePluginSource
  : embeddedPluginSource;
const schemaPath = join(hostRoot, "protocol", "worlddata-agent-host-v1.schema.json");
const applicationPath = join(hostRoot, "WorldData.AgentHost.App", "AgentHostApplication.cs");

const implementationModules = [
  "WorldDataAgentSecurity",
  "WorldDataAgentDiagnostics",
  "WorldDataAgentRuntime",
  "WorldDataAgentClient",
  "WorldDataAgentBootstrap",
  "WorldDataAgentUI",
];

const failures = [];
for (const moduleName of implementationModules) {
  const publicDirectory = join(pluginSource, moduleName, "Public");
  for (const entry of readdirSync(publicDirectory, { withFileTypes: true })) {
    if (!entry.isFile()) continue;
    if (!/^I[A-Za-z0-9_]+\.h$/.test(entry.name)) {
      failures.push(`${moduleName}/Public exposes non-interface file ${entry.name}`);
      continue;
    }
    const source = readFileSync(join(publicDirectory, entry.name), "utf8");
    if (!/class\s+(?:[A-Z0-9_]+_API\s+)?I[A-Za-z0-9_]+/.test(source)) {
      failures.push(`${moduleName}/Public/${entry.name} does not declare an interface class`);
    }
  }
}

const schema = JSON.parse(readFileSync(schemaPath, "utf8"));
const schemaCommands = [...schema.$defs.commandType.enum].sort();
const applicationSource = readFileSync(applicationPath, "utf8");
const routedCommands = [...applicationSource.matchAll(/case\s+"([A-Za-z0-9]+)"\s*:/g)]
  .map(match => match[1])
  .sort();
if (JSON.stringify(schemaCommands) !== JSON.stringify(routedCommands)) {
  failures.push(`schema commands ${schemaCommands.join(",")} do not match host routes ${routedCommands.join(",")}`);
}

for (const relative of ["WorldData.AgentHost.App", "WorldData.AgentHost.Codex"]) {
  const directory = join(hostRoot, relative);
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    if (!entry.isFile() || !entry.name.endsWith(".cs")) continue;
    const source = readFileSync(join(directory, entry.name), "utf8");
    if (/^public\s+(?:sealed\s+|static\s+|abstract\s+)?(?:class|record|interface|struct|enum)\s+/m.test(source)) {
      failures.push(`${relative}/${entry.name} exposes a public implementation type`);
    }
  }
}

console.log(JSON.stringify({
  ok: failures.length === 0,
  checkedModules: implementationModules,
  commandCount: schemaCommands.length,
  failures,
}));
process.exit(failures.length === 0 ? 0 : 1);
