#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const pluginRoot = path.join(repoRoot, "Plugins", "UEBridgeMCP");
const sourceRoot = path.join(pluginRoot, "Source", "UEBridgeMCPEditor");
const privateRoot = path.join(sourceRoot, "Private");

const allowedDispatchAliases = new Set([
  "get_level_actors",
  "get_project_info",
  "search_assets",
]);

const mutatingNamePattern =
  /^(add|attach|bind|build|compile|connect|create|delete|disconnect|duplicate|execute|focus|import|link|load|play|rebuild|regenerate|remove|rename|reparent|redo|save|set|spawn|stop|undo)_/;

const sensitiveHumanConfirmationPattern =
  /^(execute_python|execute_console_command|write_file|delete_file|rename_file|set_object_property|set_widget_property)$/;

const issues = [];
const warnings = [];

function fail(message) {
  issues.push(message);
}

function warn(message) {
  warnings.push(message);
}

function readText(file) {
  return fs.readFileSync(file, "utf8");
}

function walk(dir, out = []) {
  if (!fs.existsSync(dir)) {
    return out;
  }
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const fullPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      walk(fullPath, out);
    } else {
      out.push(fullPath);
    }
  }
  return out;
}

function extractBalancedJsonObject(text, start) {
  let depth = 0;
  let inString = false;
  let escaped = false;
  for (let index = start; index < text.length; index += 1) {
    const ch = text[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (inString) {
      if (ch === "\\") {
        escaped = true;
      } else if (ch === "\"") {
        inString = false;
      }
      continue;
    }
    if (ch === "\"") {
      inString = true;
      continue;
    }
    if (ch === "{") {
      depth += 1;
    } else if (ch === "}") {
      depth -= 1;
      if (depth === 0) {
        return text.slice(start, index + 1);
      }
    }
  }
  return null;
}

function extractToolDefinitions(files) {
  const tools = new Map();
  const duplicates = [];
  for (const file of files) {
    const text = readText(file);
    let searchFrom = 0;
    while (true) {
      const start = text.indexOf("{\"name\"", searchFrom);
      if (start === -1) {
        break;
      }
      searchFrom = start + 7;
      const jsonText = extractBalancedJsonObject(text, start);
      if (!jsonText) {
        fail(`Could not parse balanced JSON object in ${file}`);
        continue;
      }
      try {
        const parsed = JSON.parse(jsonText);
        if (typeof parsed.name !== "string" || !parsed.inputSchema) {
          continue;
        }
        if (tools.has(parsed.name)) {
          duplicates.push(parsed.name);
          continue;
        }
        tools.set(parsed.name, { tool: parsed, file });
      } catch (error) {
        fail(`Invalid tool JSON in ${file}: ${error.message}`);
      }
    }
  }
  for (const name of duplicates) {
    fail(`Duplicate advertised tool definition: ${name}`);
  }
  return tools;
}

function addDispatchTarget(targets, name, call, acceptsArgs, file) {
  const handlerName = call ? call.split("::").at(-1) : "";
  const qualifier = call && call.includes("::") ? call.slice(0, -(handlerName.length + 2)) : "";
  const next = { name, call, handlerName, qualifier, acceptsArgs, file };
  const existing = targets.get(name);
  if (existing && (existing.call !== next.call || existing.acceptsArgs !== next.acceptsArgs)) {
    fail(`Dispatch branch has conflicting handlers for ${name}: ${existing.call || "<no-arg>"} and ${next.call || "<no-arg>"}`);
    return;
  }
  targets.set(name, next);
}

function extractDispatchTargets(files) {
  const targets = new Map();
  const branchPattern = /if\s*\(\s*ToolName\s*==\s*TEXT\("([^"]+)"\)\s*\)\s*\{\s*OutResult\s*=\s*([A-Za-z_][\w:]*)\s*\(([^)]*)\)/g;
  const localToolMapPattern = /\{\s*TEXT\("([^"]+)"\)\s*,\s*\[\]\([^)]*\)\s*\{\s*return\s+([A-Za-z_][\w:]*)\s*\(([^)]*)\)\s*;\s*\}\s*\}/g;
  for (const file of files) {
    const text = readText(file);
    for (const match of text.matchAll(branchPattern)) {
      addDispatchTarget(targets, match[1], match[2], match[3].trim() === "Args", file);
    }
    if (text.includes("FLocalToolHandler")) {
      for (const match of text.matchAll(localToolMapPattern)) {
        const acceptsArgs = match[3].trim() === "A";
        addDispatchTarget(targets, match[1], match[2], acceptsArgs, file);
      }
    }
  }
  return targets;
}

function findBalancedCppBlockEnd(text, start) {
  let depth = 0;
  let inString = false;
  let inChar = false;
  let escaped = false;
  let inLineComment = false;
  let inBlockComment = false;
  for (let index = start; index < text.length; index += 1) {
    const ch = text[index];
    const next = text[index + 1] || "";

    if (inLineComment) {
      if (ch === "\n") {
        inLineComment = false;
      }
      continue;
    }
    if (inBlockComment) {
      if (ch === "*" && next === "/") {
        inBlockComment = false;
        index += 1;
      }
      continue;
    }
    if (escaped) {
      escaped = false;
      continue;
    }
    if (inString) {
      if (ch === "\\") {
        escaped = true;
      } else if (ch === "\"") {
        inString = false;
      }
      continue;
    }
    if (inChar) {
      if (ch === "\\") {
        escaped = true;
      } else if (ch === "'") {
        inChar = false;
      }
      continue;
    }
    if (ch === "/" && next === "/") {
      inLineComment = true;
      index += 1;
      continue;
    }
    if (ch === "/" && next === "*") {
      inBlockComment = true;
      index += 1;
      continue;
    }
    if (ch === "\"") {
      inString = true;
      continue;
    }
    if (ch === "'") {
      inChar = true;
      continue;
    }
    if (ch === "{") {
      depth += 1;
    } else if (ch === "}") {
      depth -= 1;
      if (depth === 0) {
        return index;
      }
    }
  }
  return -1;
}

function namespaceAt(text, index) {
  const namespaces = [];
  const namespacePattern = /namespace\s+([A-Za-z_]\w*)\s*\{/g;
  for (const match of text.matchAll(namespacePattern)) {
    const openBrace = text.indexOf("{", match.index);
    if (openBrace < 0 || openBrace > index) {
      continue;
    }
    const closeBrace = findBalancedCppBlockEnd(text, openBrace);
    if (closeBrace > index) {
      namespaces.push({ name: match[1], openBrace });
    }
  }
  return namespaces
    .sort((left, right) => left.openBrace - right.openBrace)
    .map((entry) => entry.name)
    .join("::");
}

function extractHandlers(files) {
  const handlers = [];
  const handlerPattern = /\bFString\s+([A-Za-z_]\w*)\s*\(\s*const\s+TSharedPtr\s*<\s*FJsonObject\s*>\s*&\s*Args\s*\)\s*\{/g;
  for (const file of files) {
    const text = readText(file);
    for (const match of text.matchAll(handlerPattern)) {
      const openBrace = text.indexOf("{", match.index);
      const closeBrace = findBalancedCppBlockEnd(text, openBrace);
      if (closeBrace < 0) {
        fail(`Could not parse handler body for ${match[1]} in ${file}`);
        continue;
      }
      const namespaceName = namespaceAt(text, match.index);
      const qualifiedName = namespaceName ? `${namespaceName}::${match[1]}` : match[1];
      handlers.push({
        name: match[1],
        namespaceName,
        qualifiedName,
        file,
        body: text.slice(openBrace, closeBrace + 1),
      });
    }
  }
  return handlers;
}

function resolveHandler(target, handlers) {
  if (!target.acceptsArgs) {
    return null;
  }
  const candidates = handlers.filter((handler) => handler.name === target.handlerName);
  const exact = candidates.filter((handler) => handler.qualifiedName === target.call);
  if (exact.length === 1) {
    return exact[0];
  }
  if (target.qualifier) {
    const qualified = candidates.filter((handler) =>
      handler.qualifiedName.endsWith(target.call) || target.call.endsWith(handler.qualifiedName));
    if (qualified.length === 1) {
      return qualified[0];
    }
  }
  const sameFile = candidates.filter((handler) => handler.file === target.file);
  if (sameFile.length === 1) {
    return sameFile[0];
  }
  if (candidates.length === 1) {
    return candidates[0];
  }
  return null;
}

function addFieldRead(reads, field, kind) {
  const kinds = reads.get(field) || new Set();
  kinds.add(kind);
  reads.set(field, kinds);
}

function fieldReads(body) {
  const reads = new Map();
  const patterns = [
    { kind: "string", regex: /Args\s*->\s*(?:TryGetStringField|GetStringField)\s*\(\s*TEXT\("([^"]+)"\)/g },
    { kind: "number", regex: /Args\s*->\s*(?:TryGetNumberField|GetNumberField|GetIntegerField)\s*\(\s*TEXT\("([^"]+)"\)/g },
    { kind: "boolean", regex: /Args\s*->\s*(?:TryGetBoolField|GetBoolField)\s*\(\s*TEXT\("([^"]+)"\)/g },
    { kind: "array", regex: /Args\s*->\s*(?:TryGetArrayField|GetArrayField)\s*\(\s*TEXT\("([^"]+)"\)/g },
    { kind: "object", regex: /Args\s*->\s*(?:TryGetObjectField|GetObjectField)\s*\(\s*TEXT\("([^"]+)"\)/g },
    { kind: "any", regex: /Args\s*->\s*(?:TryGetField|GetField|HasField)\s*\(\s*TEXT\("([^"]+)"\)/g },
  ];
  for (const { kind, regex } of patterns) {
    for (const match of body.matchAll(regex)) {
      addFieldRead(reads, match[1], kind);
    }
  }
  return reads;
}

function schemaTypes(schema, out = new Set()) {
  if (!schema || typeof schema !== "object") {
    return out;
  }
  if (typeof schema.type === "string") {
    out.add(schema.type);
  } else if (Array.isArray(schema.type)) {
    for (const type of schema.type) {
      if (typeof type === "string") {
        out.add(type);
      }
    }
  }
  for (const keyword of ["oneOf", "anyOf", "allOf"]) {
    if (Array.isArray(schema[keyword])) {
      for (const item of schema[keyword]) {
        schemaTypes(item, out);
      }
    }
  }
  return out;
}

function schemaAllowsKind(schema, kind) {
  if (kind === "any") {
    return true;
  }
  const types = schemaTypes(schema);
  if (types.size === 0) {
    return true;
  }
  if (kind === "number") {
    return types.has("number") || types.has("integer");
  }
  return types.has(kind);
}

function formatSchemaTypes(schema) {
  const types = [...schemaTypes(schema)].sort();
  return types.length > 0 ? types.join("|") : "<unspecified>";
}

function rel(file) {
  return path.relative(repoRoot, file).replaceAll(path.sep, "/");
}

function auditToolContracts(tools, dispatchTargets, handlers) {
  let checked = 0;
  let argReads = 0;

  for (const [name, { tool }] of tools) {
    const target = dispatchTargets.get(name);
    if (!target) {
      continue;
    }

    const inputSchema = tool.inputSchema && typeof tool.inputSchema === "object" ? tool.inputSchema : {};
    const properties = inputSchema.properties && typeof inputSchema.properties === "object" ? inputSchema.properties : {};
    const propertyCount = Object.keys(properties).length;

    if (!target.acceptsArgs) {
      if (propertyCount > 0) {
        fail(`Tool schema declares parameters but dispatch does not pass Args to a handler: ${name} (${rel(target.file)})`);
      }
      continue;
    }

    const handler = resolveHandler(target, handlers);
    if (!handler) {
      fail(`Could not resolve Args handler for tool contract audit: ${name} -> ${target.call} (${rel(target.file)})`);
      continue;
    }

    checked += 1;
    const reads = fieldReads(handler.body);
    for (const [field, kinds] of reads) {
      for (const kind of kinds) {
        argReads += 1;
        const property = properties[field];
        if (!property) {
          fail(`Tool schema is missing handler argument: ${name}.${field} read as ${kind} in ${handler.qualifiedName} (${rel(handler.file)})`);
        } else if (!schemaAllowsKind(property, kind)) {
          fail(`Tool schema type mismatch: ${name}.${field} read as ${kind} in ${handler.qualifiedName}, schema allows ${formatSchemaTypes(property)} (${rel(handler.file)})`);
        }
      }
    }
  }

  return { checked, argReads };
}

function requiresTrusted(tool) {
  const annotations = tool.annotations;
  if (!annotations || typeof annotations.readOnlyHint !== "boolean") {
    return true;
  }
  return annotations.readOnlyHint !== true || annotations.destructiveHint === true || annotations.openWorldHint === true;
}

function requiresHumanConfirmation(name, tool) {
  return tool.annotations?.destructiveHint === true || sensitiveHumanConfirmationPattern.test(name);
}

function getRiskLevel(name, tool) {
  const annotations = tool.annotations || {};
  if (requiresHumanConfirmation(name, tool) || annotations.destructiveHint === true) {
    return "high";
  }
  if (annotations.readOnlyHint !== true || annotations.openWorldHint === true) {
    return "medium";
  }
  return "low";
}

function extractHumanConfirmationPolicy() {
  const internalFile = path.join(privateRoot, "WorldDataMCPServerInternal.cpp");
  const text = readText(internalFile);
  const match = text.match(/static\s+const\s+TSet<FString>\s+Names\s*=\s*\{([\s\S]*?)\};/);
  if (!match) {
    fail("Could not find GetHumanConfirmationToolNames policy table in WorldDataMCPServerInternal.cpp");
    return new Set();
  }

  return new Set([...match[1].matchAll(/TEXT\("([^"]+)"\)/g)].map((entry) => entry[1]));
}

function auditHumanConfirmationPolicy(tools) {
  const policy = extractHumanConfirmationPolicy();

  let requiredCount = 0;
  for (const [name, { tool, file }] of tools) {
    if (requiresHumanConfirmation(name, tool)) {
      requiredCount += 1;
      if (!policy.has(name)) {
        fail(`High-risk tool is missing server-side human confirmation gate: ${name} (${file})`);
      }
    }
  }

  for (const name of policy) {
    if (!tools.has(name)) {
      fail(`Human confirmation policy references a non-advertised tool: ${name}`);
    }
  }

  return requiredCount;
}

function auditRiskPolicy(tools) {
  const counts = { high: 0, medium: 0, low: 0 };
  for (const [name, { tool }] of tools) {
    const riskLevel = getRiskLevel(name, tool);
    counts[riskLevel] += 1;
  }

  const expectedRisk = {
    create_level_sequence: "medium",
    set_pcg_component_graph: "medium",
    remove_pcg_node: "high",
    set_widget_property: "high",
  };
  for (const [name, expected] of Object.entries(expectedRisk)) {
    const entry = tools.get(name);
    if (!entry) {
      fail(`Risk policy smoke test references a non-advertised tool: ${name}`);
      continue;
    }
    const actual = getRiskLevel(name, entry.tool);
    if (actual !== expected) {
      fail(`Risk policy classified ${name} as ${actual}, expected ${expected}`);
    }
  }

  const internalFile = path.join(privateRoot, "WorldDataMCPServerInternal.cpp");
  const internalText = readText(internalFile);
  if (!/ApplyRiskLevelPolicy/.test(internalText) || !/worldDataRiskLevel/.test(internalText)) {
    fail("WorldDataMCPServerInternal.cpp must inject centralized worldDataRiskLevel annotations");
  }

  return counts;
}

function auditTools(tools, dispatchNames) {
  for (const name of tools.keys()) {
    if (!dispatchNames.has(name)) {
      fail(`Advertised tool has no dispatch branch: ${name}`);
    }
  }

  for (const name of dispatchNames) {
    if (!tools.has(name) && !allowedDispatchAliases.has(name)) {
      fail(`Dispatch branch is not advertised as a tool: ${name}`);
    }
  }

  let trustedCount = 0;
  for (const [name, { tool, file }] of tools) {
    const annotations = tool.annotations;
    if (!annotations) {
      fail(`Tool is missing annotations and will be trusted-only by default: ${name} (${file})`);
      continue;
    }
    if (typeof annotations.readOnlyHint !== "boolean") {
      fail(`Tool annotations.readOnlyHint must be a boolean: ${name} (${file})`);
    }
    if (requiresTrusted(tool)) {
      trustedCount += 1;
    } else if (mutatingNamePattern.test(name)) {
      fail(`Mutating-looking tool is marked read-only and not open-world: ${name} (${file})`);
    }
  }

  return trustedCount;
}

function auditAcpPermissionScope() {
  const acpCpp = path.join(privateRoot, "WorldDataCodexACPClient.cpp");
  const acpHeader = path.join(privateRoot, "WorldDataCodexACPClient.h");
  const text = readText(acpCpp) + "\n" + readText(acpHeader);
  if (/bTrustedMcpToolAccessForPrompt/.test(text)) {
    fail("ACP trusted MCP authorization must not be stored as a prompt-wide boolean");
  }
  if (!/ApprovedWorldDataMcpToolCalls/.test(text)) {
    fail("ACP trusted MCP authorization must be single-use and auditable");
  }
}

function auditSecurityConfig() {
  const configPaths = [
    path.join(repoRoot, ".mcp.json"),
    path.join(repoRoot, ".cursor", "mcp.json"),
  ];
  for (const file of configPaths) {
    if (!fs.existsSync(file)) {
      continue;
    }
    const text = readText(file);
    if (/X-WorldData-MCP-Token|accessToken/i.test(text)) {
      fail(`Project-root MCP config must not contain trusted token material: ${file}`);
    }
  }
}

function auditModuleBoundary() {
  const sourceNwiro = path.join(privateRoot, "Nwiro");
  if (fs.existsSync(sourceNwiro)) {
    fail(`Legacy Nwiro sources must stay outside UEBridgeMCPEditor Source: ${sourceNwiro}`);
  }

  const buildCs = path.join(sourceRoot, "UEBridgeMCPEditor.Build.cs");
  const buildText = readText(buildCs);
  if (/"Private"\s*,\s*"Nwiro"|Private[\\/]+Nwiro|UE_MCP_BRIDGE_API/.test(buildText)) {
    fail("UEBridgeMCPEditor.Build.cs still references legacy Nwiro include paths or API defines");
  }
  if (/"StructUtils"/.test(buildText)) {
    fail("UEBridgeMCPEditor.Build.cs should not depend on the deprecated StructUtils plugin module directly");
  }

  const uplugin = path.join(pluginRoot, "UEBridgeMCP.uplugin");
  const upluginText = readText(uplugin);
  if (/"Name"\s*:\s*"StructUtils"/.test(upluginText)) {
    fail("UEBridgeMCP.uplugin should not explicitly enable the deprecated StructUtils plugin");
  }
}

function auditSize(cppFiles) {
  const lineCount = cppFiles.reduce((sum, file) => sum + readText(file).split(/\r?\n/).length, 0);
  if (lineCount > 25000) {
    warn(`UEBridgeMCPEditor source line count is ${lineCount}, above the 25000 soft budget`);
  }
  return lineCount;
}

const sourceFiles = walk(sourceRoot).filter((file) => /\.(cpp|h|cs)$/.test(file));
const cppFiles = sourceFiles.filter((file) => /\.(cpp|h)$/.test(file));
const toolRoots = [
  privateRoot,
  path.join(pluginRoot, "Source", "UEBridgeMCPPCGTools", "Private"),
];
const toolSourceFiles = toolRoots.flatMap((root) => walk(root)).filter((file) => /\.(cpp|h)$/.test(file));

const tools = extractToolDefinitions(toolSourceFiles);
const dispatchTargets = extractDispatchTargets(toolSourceFiles);
const dispatchNames = new Set(dispatchTargets.keys());
const trustedCount = auditTools(tools, dispatchNames);
const contractStats = auditToolContracts(tools, dispatchTargets, extractHandlers(toolSourceFiles));
const humanConfirmationCount = auditHumanConfirmationPolicy(tools);
const riskCounts = auditRiskPolicy(tools);
auditAcpPermissionScope();
auditSecurityConfig();
auditModuleBoundary();
const sourceLines = auditSize(sourceFiles);

for (const message of warnings) {
  console.warn(`[warn] ${message}`);
}

if (issues.length > 0) {
  for (const message of issues) {
    console.error(`[fail] ${message}`);
  }
  process.exit(1);
}

console.log("UEBridgeMCP audit passed");
console.log(`tools=${tools.size} dispatch=${dispatchNames.size} trustedOnly=${trustedCount} humanConfirmed=${humanConfirmationCount} riskHigh=${riskCounts.high} riskMedium=${riskCounts.medium} riskLow=${riskCounts.low} contracts=${contractStats.checked} argReads=${contractStats.argReads} sourceFiles=${sourceFiles.length} sourceLines=${sourceLines}`);
