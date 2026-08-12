/**
 * Small, shared handoff for every AI client connected to the Unreal bridge.
 * This intentionally stores decisions and recent work, not transcripts.
 */
import fs from "node:fs";
import path from "node:path";

const MAX_ACTIVITY_LINES = 36;
const MAX_NOTE_CHARS = 1400;

function pathsFor(status) {
  const projectDir = status?.projectDir || status?.projectPath
    ? (status.projectDir || path.dirname(status.projectPath))
    : process.env.UNREAL_PROJECT_DIR;
  if (!projectDir) return null;
  return {
    handoff: path.join(projectDir, "Docs", "AI_HANDOFF.md"),
    archive: path.join(projectDir, "Docs", "AI_HANDOFF_ARCHIVE.md"),
    agent: path.join(projectDir, "Saved", "UnrealClaude", "mcp-agent-session.json"),
  };
}

function ensureHandoff(filePath) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  if (!fs.existsSync(filePath)) {
    fs.writeFileSync(filePath, [
      "# AI Project Handoff",
      "",
      "> Maintained by the Unreal MCP bridge. Keep this compact: decisions, current work, and handoff notes — not chat transcripts.",
      "",
      "## Current summary",
      "- No summary has been recorded yet.",
      "",
      "## Recent bridge activity",
      "- Handoff created.",
      "",
    ].join("\n"), "utf8");
  }
}

function sanitize(value, limit = MAX_NOTE_CHARS) {
  return String(value || "").replace(/\s+/g, " ").trim().slice(0, limit);
}

function splitHandoff(text) {
  const marker = "## Recent bridge activity";
  const index = text.indexOf(marker);
  if (index < 0) return { summary: text.trim(), activity: [] };
  const summary = text.slice(0, index).trimEnd();
  const activity = text.slice(index + marker.length).trim().split("\n").filter(Boolean);
  return { summary, activity };
}

function writeHandoff(filePath, summary, activity) {
  fs.writeFileSync(filePath, `${summary}\n\n## Recent bridge activity\n${activity.slice(-MAX_ACTIVITY_LINES).join("\n")}\n`, "utf8");
}

export function getProjectMemory(status, maxChars = 6000) {
  const paths = pathsFor(status);
  if (!paths) return { success: false, error: "Unreal did not report a project directory yet." };
  ensureHandoff(paths.handoff);
  const content = fs.readFileSync(paths.handoff, "utf8");
  return { success: true, path: paths.handoff, content: content.slice(0, Math.max(500, Math.min(Number(maxChars) || 6000, 12000))) };
}

export function appendProjectNote(status, note, label = "Note") {
  const paths = pathsFor(status);
  if (!paths) return { success: false, error: "Unreal did not report a project directory yet." };
  ensureHandoff(paths.handoff);
  const existing = fs.readFileSync(paths.handoff, "utf8");
  const { summary, activity } = splitHandoff(existing);
  const clean = sanitize(note);
  if (!clean) return { success: false, error: "A note is required." };
  activity.push(`- ${new Date().toISOString().slice(0, 16).replace("T", " ")} — **${sanitize(label, 80)}:** ${clean}`);
  // Preserve the older compact entries before retaining the active window.
  // This keeps the working handoff small without losing the project trail.
  if (activity.length > MAX_ACTIVITY_LINES) {
    const archived = activity.slice(0, -MAX_ACTIVITY_LINES).join("\n");
    fs.mkdirSync(path.dirname(paths.archive), { recursive: true });
    fs.appendFileSync(
      paths.archive,
      `\n\n---\nArchived bridge activity ${new Date().toISOString()}\n\n${archived}\n`,
      "utf8",
    );
  }
  writeHandoff(paths.handoff, summary, activity.slice(-MAX_ACTIVITY_LINES));
  return { success: true, path: paths.handoff };
}

export function setProjectSummary(status, summaryText) {
  const paths = pathsFor(status);
  if (!paths) return { success: false, error: "Unreal did not report a project directory yet." };
  ensureHandoff(paths.handoff);
  const existing = fs.readFileSync(paths.handoff, "utf8");
  const { activity } = splitHandoff(existing);
  const summary = sanitize(summaryText, 7000);
  if (!summary) return { success: false, error: "A summary is required." };
  const header = "# AI Project Handoff\n\n> Maintained by the Unreal MCP bridge. Keep this compact: decisions, current work, and handoff notes — not chat transcripts.\n\n## Current summary\n";
  writeHandoff(paths.handoff, `${header}${summary}`, activity);
  return { success: true, path: paths.handoff };
}

export function compactProjectMemory(status) {
  const paths = pathsFor(status);
  if (!paths) return { success: false, error: "Unreal did not report a project directory yet." };
  ensureHandoff(paths.handoff);
  const existing = fs.readFileSync(paths.handoff, "utf8");
  const { summary, activity } = splitHandoff(existing);
  if (activity.length > MAX_ACTIVITY_LINES) {
    fs.mkdirSync(path.dirname(paths.archive), { recursive: true });
    fs.appendFileSync(paths.archive, `\n\n---\nArchived ${new Date().toISOString()}\n\n${existing}`, "utf8");
  }
  writeHandoff(paths.handoff, summary, activity);
  return { success: true, path: paths.handoff, retainedActivityLines: Math.min(activity.length, MAX_ACTIVITY_LINES) };
}

export function recordToolActivity(status, toolName, args, result) {
  if (!result?.success || toolName === "project_memory") return;
  const mutations = new Set(["spawn_actor", "move_actor", "delete_actors", "set_property", "open_level", "blueprint_modify", "anim_blueprint_modify", "character", "enhanced_input", "material", "asset", "sequencer", "world_builder", "performance", "narrative_trigger"]);
  if (!mutations.has(toolName)) return;
  const operation = sanitize(args?.operation || args?.action || "change", 80);
  appendProjectNote(status, `${toolName} · ${operation}`, "Bridge activity");
}

export function registerAgent(status, details = {}) {
  const paths = pathsFor(status);
  if (!paths) return { success: false, error: "Unreal did not report a project directory yet." };
  fs.mkdirSync(path.dirname(paths.agent), { recursive: true });
  const payload = {
    client: sanitize(details.client || "External MCP client", 80),
    model: sanitize(details.model || "Not reported", 120),
    usage_remaining: sanitize(details.usage_remaining || "Not provided by client", 120),
    note: sanitize(details.note || "", 300),
    updated_at: new Date().toISOString(),
  };
  fs.writeFileSync(paths.agent, `${JSON.stringify(payload, null, 2)}\n`, "utf8");
  return { success: true, ...payload };
}

export function getAgentStatus(status) {
  const paths = pathsFor(status);
  if (!paths || !fs.existsSync(paths.agent)) return { registered: false };
  try { return { registered: true, ...JSON.parse(fs.readFileSync(paths.agent, "utf8")) }; }
  catch { return { registered: false }; }
}
