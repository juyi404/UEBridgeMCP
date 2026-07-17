import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const testsDirectory = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(testsDirectory, "../../..");
const webRoot = resolve(projectRoot, "Plugins/UEBridgeMCP/Resources/Web");
const html = readFileSync(resolve(webRoot, "index.html"), "utf8");
const script = readFileSync(resolve(webRoot, "app.js"), "utf8");
const layout = readFileSync(resolve(webRoot, "layout.css"), "utf8");
const messageStyles = readFileSync(resolve(webRoot, "message.css"), "utf8");
const failures = [];
const requiredResources = ["index.html", "console.css", "layout.css", "message.css", "app.js"];

const section = id => html.match(new RegExp(`<section\\s+id=["']${id}["'][\\s\\S]*?<\\/section>`))?.[0] ?? "";
const chat = section("chat-view");
const settings = section("settings-view");

if (!chat) failures.push("chat-view section is missing");
if (!settings) failures.push("settings-view section is missing");
if (/<form\b|<textarea\b|id=["']composer["']/.test(settings)) {
  failures.push("settings-view must not contain conversation controls");
}
if ((html.match(/<form\s+id=["']composer["']/g) || []).length !== 1
    || (html.match(/<textarea\s+id=["']message-input["']/g) || []).length !== 1
    || !/<footer\s+id=["']composer-footer["']/.test(html)) {
  failures.push("the shared chat composer must exist exactly once outside settings");
}
if (!/connect-src\s+'none'/.test(html)) failures.push("HTML CSP must prohibit network access");
for (const resource of requiredResources) {
  if (!existsSync(resolve(webRoot, resource))) failures.push(`required Web resource is missing: ${resource}`);
}
for (const stylesheet of ["console.css", "layout.css", "message.css"]) {
  if (!new RegExp(`<link\\s+rel=["']stylesheet["']\\s+href=["']${stylesheet}["']`).test(html)) {
    failures.push(`index.html must load ${stylesheet}`);
  }
}
if (!/<script\s+src=["']app\.js["']/.test(html)) failures.push("index.html must load app.js");
try {
  new Function(script);
} catch (error) {
  failures.push(`app.js has invalid JavaScript syntax: ${error.message}`);
}
if (!/window\.ue\s*&&\s*window\.ue\.worlddata/.test(script)) failures.push("UI must use the narrow Unreal bridge");
if (!/classList\.toggle\(["']hidden["'],\s*settings\)/.test(script)
    || !/classList\.toggle\(["']hidden["'],\s*!settings\)/.test(script)
    || !/composerFooter\.classList\.toggle\(["']hidden["'],\s*settings\)/.test(script)) {
  failures.push("chat/settings route isolation is missing");
}
if (/WORLDDATA_MCP_TOKEN|accessToken|authorization|bearer/i.test(`${html}\n${script}`)) {
  failures.push("browser resources must not reference credentials");
}
if (!/const\s+escapeHtml\s*=/.test(script) || !/renderMarkdown\(item\.text\)/.test(script)) {
  failures.push("assistant rendering must pass through the local safe Markdown renderer");
}
if (!/Array\.isArray\(next\.tools\)/.test(script)
    || !/tool-registry/.test(script)
    || !/已注册 MCP 工具/.test(script)
    || !/previousSettingsSignature/.test(script)) {
  failures.push("settings must render the live registered MCP tool list");
}
if (!/\[\.\.\.next\.threads\]\.sort\(compareThreadsByRecency\)/.test(script)
    || !/thread\.recencyAt\s*\|\|\s*thread\.updatedAt\s*\|\|\s*thread\.createdAt/.test(script)
    || !/relativeTime\(threadTimestamp\(thread\)\)/.test(script)) {
  failures.push("thread history must be defensively sorted by effective recency");
}
if (!/<link\s+rel=["']stylesheet["']\s+href=["']layout\.css["']/.test(html)
    || !/\.app\s*\{[\s\S]*?width:\s*100%[\s\S]*?height:\s*100%/.test(layout)
    || !/#chat-view\s+\.messages\s*\{[\s\S]*?flex:\s*1\s+1\s+auto[\s\S]*?overflow-y:\s*auto/.test(layout)) {
  failures.push("embedded layout must fill the Unreal viewport and keep messages independently scrollable");
}
if (!/function\s+conversationSignature\s*\(next\)[\s\S]*?item\.toolName[\s\S]*?item\.text/.test(script)
    || !/function\s+renderToolBatch\s*\(items\)[\s\S]*?tool-batch/.test(script)
    || !/function\s+renderActivityBatch\s*\(items\)[\s\S]*?activity-batch/.test(script)
    || !/while\s*\(index\s*<\s*items\.length\s*&&\s*items\[index\]\.kind\s*===\s*["']tool["']\)/.test(script)
    || !/items\[index\]\.kind\s*===\s*["']activity["']/.test(script)
    || !/function\s+renderToolMessage\s*\(item\)[\s\S]*?tool-detail/.test(script)) {
  failures.push("conversation rendering must compact tool events and render non-tool activity separately");
}
if (!/accepted\s*===\s*false[\s\S]*?await\s+poll\(\)/.test(script)
    || !/if\s*\(sent\)\s*elements\.messageInput\.value\s*=\s*["']{2}/.test(script)) {
  failures.push("the composer must retain its draft when the bridge rejects a send");
}
if (!/\.tool-batch\s*\{[\s\S]*?width:\s*fit-content[\s\S]*?max-width:\s*min\(460px,\s*100%\)/.test(messageStyles)
    || !/\.tool-batch__summary\s*\{[\s\S]*?min-height:\s*30px/.test(messageStyles)
    || !/\.activity-batch\s*\{[\s\S]*?width:\s*fit-content/.test(messageStyles)) {
  failures.push("tool batches must use a compact collapsed summary instead of wide individual cards");
}
if (!/@media\s*\(max-width:\s*820px\)[\s\S]*?height:\s*100%[\s\S]*?overflow:\s*hidden[\s\S]*?\.app\s*\{[\s\S]*?display:\s*grid[\s\S]*?grid-template-columns/.test(layout)) {
  failures.push("narrow embedded panels must retain a fixed work area instead of falling back to document scrolling");
}

console.log(JSON.stringify({ ok: failures.length === 0, checks: 18, failures }));
process.exit(failures.length === 0 ? 0 : 1);
