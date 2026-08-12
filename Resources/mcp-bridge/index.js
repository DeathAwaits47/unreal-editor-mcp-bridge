#!/usr/bin/env node

/**
 * UE5 MCP Server
 *
 * Bridges MCP-compatible AI clients to Unreal Engine 5's editor via HTTP REST API.
 * The UnrealClaude plugin runs an HTTP server (default port 3000) with editor manipulation tools.
 *
 * Environment Variables:
 *   UNREAL_MCP_URL - Base URL for Unreal MCP server (default: http://localhost:3000)
 *   MCP_REQUEST_TIMEOUT_MS - HTTP request timeout in milliseconds (default: 30000)
 *   INJECT_CONTEXT - Enable automatic context injection on tool calls (default: false)
 *   MCP_TOOL_CACHE_TTL_MS - TTL for tool list cache in milliseconds (default: 300000)
 *   MCP_MAX_RESPONSE_CHARS - Bound ordinary text tool output (default: 12000; 0 disables)
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

// Dynamic context loader for UE 5.7 API documentation
import {
  getContextForTool,
  getContextForQuery,
  listCategories,
  getCategoryInfo,
  loadContextForCategory,
  listSections,
  getSectionByHeading,
  getSectionsForQuery,
} from "./context-loader.js";

// Pure routing logic for unreal_get_ue_context (extracted for testability)
import { resolveUeContextRequest, resolveProjectContextRequest } from "./context-handler.js";

// Extracted library functions
import {
  log,
  fetchUnrealTools as _fetchUnrealTools,
  executeUnrealTool as _executeUnrealTool,
  executeUnrealToolAsync as _executeUnrealToolAsync,
  checkUnrealConnection as _checkUnrealConnection,
  convertToMCPSchema,
  convertAnnotations,
  formatToolResponse as _formatToolResponse,
} from "./lib.js";

// Tool router for mega-tool collapsing
import {
  classifyTool,
  resolveUnrealTool,
  categorizeToolForStatus,
  ROUTER_TOOL_SCHEMA,
  COMPACT_ROUTER_TOOL_SCHEMA,
} from "./tool-router.js";
import {
  getProjectMemory,
  appendProjectNote,
  setProjectSummary,
  compactProjectMemory,
  recordToolActivity,
  registerAgent,
  getAgentStatus,
} from "./project-memory.js";

// Configuration with defaults
const CONFIG = {
  unrealMcpUrl: process.env.UNREAL_MCP_URL || "http://localhost:3000",
  requestTimeoutMs: parseInt(process.env.MCP_REQUEST_TIMEOUT_MS, 10) || 30000,
  injectContext: process.env.INJECT_CONTEXT === "true",
  asyncEnabled: process.env.MCP_ASYNC_ENABLED !== "false",
  asyncTimeoutMs: parseInt(process.env.MCP_ASYNC_TIMEOUT_MS, 10) || 300000,
  pollIntervalMs: parseInt(process.env.MCP_POLL_INTERVAL_MS, 10) || 2000,
  toolCacheTtlMs: parseInt(process.env.MCP_TOOL_CACHE_TTL_MS, 10) || 300000,
  maxResponseChars: parseInt(process.env.MCP_MAX_RESPONSE_CHARS, 10) || 12000,
  // compact keeps the schema surface small. Set balanced only for clients that need all direct tools.
  toolProfile: (process.env.UNREAL_MCP_TOOL_PROFILE || "compact").toLowerCase(),
};

// Bind CONFIG values to library functions for convenience
const fetchUnrealTools = () => _fetchUnrealTools(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs);
const executeUnrealTool = (toolName, args) => _executeUnrealTool(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs, toolName, args);
const checkUnrealConnection = () => _checkUnrealConnection(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs);
const formatToolResponse = (toolName, result) => {
  const response = _formatToolResponse(toolName, result, CONFIG.injectContext ? getContextForTool : null);
  // Large raw asset/actor lists are the biggest avoidable context drain. Keep image
  // payloads intact, but bound text responses; the client can always run a narrower query.
  if (process.env.MCP_MAX_RESPONSE_CHARS === "0") return response;
  for (const block of response.content || []) {
    if (block.type === "text" && typeof block.text === "string" && block.text.length > CONFIG.maxResponseChars) {
      block.text = `${block.text.slice(0, CONFIG.maxResponseChars)}\n\n[Output truncated to preserve context. Run a narrower query for the remaining details.]`;
    }
  }
  return response;
};

// Create the MCP server
const server = new Server(
  {
    name: "ue5-mcp-server",
    version: "1.4.1",
  },
  {
    capabilities: {
      tools: {},
    },
  }
);

// Cache for tools with TTL (avoids re-fetching the full tool list on every list_tools call)
let toolCache = { tools: [], timestamp: 0 };
let statusCache = { value: null, timestamp: 0 };
const STATUS_CACHE_TTL_MS = 60000;

async function getConnectionStatus() {
  const age = Date.now() - statusCache.timestamp;
  if (statusCache.value && age < STATUS_CACHE_TTL_MS) return statusCache.value;
  const status = await checkUnrealConnection();
  statusCache = { value: status, timestamp: Date.now() };
  return status;
}

function isToolExposed(toolName) {
  if (CONFIG.toolProfile === "balanced") return classifyTool(toolName) === "simple";
  return new Set(["capture_viewport", "asset_search", "get_output_log"]).has(toolName);
}

const PROJECT_MEMORY_SCHEMA = {
  name: "unreal_project_memory",
  description: "Shared, compact project handoff for every AI using this bridge. Read this before broad project exploration. Actions: read, note, set_summary, compact, register_agent, agent_status. It stores decisions and recent bridge activity, not full chat history.",
  inputSchema: {
    type: "object",
    properties: {
      action: { type: "string", enum: ["read", "note", "set_summary", "compact", "register_agent", "agent_status"], description: "read is the default. Use set_summary only for a short, durable milestone handoff." },
      text: { type: "string", description: "Required for note and set_summary." },
      max_chars: { type: "number", description: "Maximum characters returned by read (default 6000; max 12000)." },
      client: { type: "string", description: "For register_agent, e.g. Codex or Claude Code." },
      model: { type: "string", description: "For register_agent. Only report a model name the client actually knows." },
      usage_remaining: { type: "string", description: "Optional. Only report it if the client exposes it; otherwise omit." },
      note: { type: "string", description: "Optional status note for register_agent." },
    },
  },
  annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false, openWorldHint: false },
};

// Handle list_tools request
server.setRequestHandler(ListToolsRequestSchema, async () => {
  const status = await getConnectionStatus();

  if (!status.connected) {
    log.info("Unreal not connected", { reason: status.reason });
    return {
      tools: [
        {
          name: "unreal_status",
          description: "Check if Unreal Editor is running with the plugin. Currently: NOT CONNECTED. Please start Unreal Editor with the plugin enabled.",
          inputSchema: {
            type: "object",
            properties: {},
          },
        },
      ],
    };
  }

  let unrealTools;
  const cacheAge = Date.now() - toolCache.timestamp;
  if (toolCache.tools.length > 0 && cacheAge < CONFIG.toolCacheTtlMs) {
    unrealTools = toolCache.tools;
    log.debug("Using cached tool list", { ageMs: cacheAge });
  } else {
    unrealTools = await fetchUnrealTools();
    toolCache = { tools: unrealTools, timestamp: Date.now() };
  }

  const mcpTools = [];

  // 1. Status first
  mcpTools.push({
    name: "unreal_status",
    description: `Check Unreal Editor connection status. Currently: CONNECTED to ${status.projectName || "Unknown Project"} (${status.engineVersion || "Unknown"})`,
    inputSchema: { type: "object", properties: {} },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  });

  // 2. Direct tools. Compact mode intentionally exposes only frequent read/debug tools.
  for (const tool of toolCache.tools) {
    if (isToolExposed(tool.name)) {
      mcpTools.push({
        name: `unreal_${tool.name}`,
        description: tool.description,
        inputSchema: convertToMCPSchema(tool.parameters, true),
        annotations: convertAnnotations(tool.annotations),
      });
    }
  }

  // 3. Router tool. The compact profile avoids repeating a giant operation catalogue in every client context.
  mcpTools.push(CONFIG.toolProfile === "balanced" ? ROUTER_TOOL_SCHEMA : COMPACT_ROUTER_TOOL_SCHEMA);

  // 4. Context tool (section-aware)
  mcpTools.push({
    name: "unreal_get_ue_context",
    description: [
      "Get UE 5.7 API documentation. By default returns only the most relevant SECTIONS for your query (saves tokens).",
      `Categories: ${listCategories().join(", ")}.`,
      'Modes: "sections" (default — targeted by query), "outline" (TOC of headings), "full" (entire file, use sparingly).',
    ].join(" "),
    inputSchema: {
      type: "object",
      properties: {
        query: {
          type: "string",
          description: "Natural-language query — returns up to max_sections most relevant sections across matching categories.",
        },
        category: {
          type: "string",
          description: `Restrict search to one category: ${listCategories().join(", ")}. With mode="full" returns the entire file.`,
        },
        section: {
          type: "string",
          description: 'Exact section heading to retrieve (e.g., "Core Classes"). Use after an outline call.',
        },
        mode: {
          type: "string",
          enum: ["sections", "outline", "full"],
          description: 'sections=targeted (default), outline=list headings only, full=entire file.',
        },
        max_sections: {
          type: "number",
          description: "Max sections to return in sections mode (default 3, max 8).",
        },
      },
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  });

  // 5. Shared handoff is the normal, compact way to restore project context.
  mcpTools.push(PROJECT_MEMORY_SCHEMA);

  // Full project inventory is useful but large, so keep it out of the compact profile.
  if (CONFIG.toolProfile === "balanced") {
    mcpTools.push({
      name: "unreal_get_project_context",
      description: "Get full project context: C++ class list, source structure, level actors, asset counts. Call when you need project-specific details.",
      inputSchema: { type: "object", properties: {} },
      annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    });
  }

  log.info("Tools listed", { profile: CONFIG.toolProfile, exposed: mcpTools.length, cached: toolCache.tools.length, connected: true });
  return { tools: mcpTools };
});

// Handle call_tool request
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  if (name === "unreal_project_memory") {
    const status = await getConnectionStatus();
    if (!status.connected) {
      return { content: [{ type: "text", text: "Unreal is not connected, so the project handoff location is unavailable." }], isError: true };
    }
    const action = args?.action || "read";
    let result;
    if (action === "read") result = getProjectMemory(status, args?.max_chars);
    else if (action === "note") result = appendProjectNote(status, args?.text, "AI note");
    else if (action === "set_summary") result = setProjectSummary(status, args?.text);
    else if (action === "compact") result = compactProjectMemory(status);
    else if (action === "register_agent") result = registerAgent(status, args);
    else if (action === "agent_status") result = getAgentStatus(status);
    else result = { success: false, error: `Unknown project memory action: ${action}` };
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }], isError: !result.success && action !== "agent_status" };
  }

  // Handle UE context request (section-aware, token-efficient)
  if (name === "unreal_get_ue_context") {
    const response = resolveUeContextRequest(args, {
      listCategories,
      listSections,
      getSectionByHeading,
      loadContextForCategory,
      getSectionsForQuery,
      getCategoryInfo,
    });
    log.info("UE context resolved", { mode: (args || {}).mode, category: (args || {}).category, hasQuery: !!(args || {}).query });
    return response;
  }

  // Handle project context request (on-demand, avoids bloating system prompt)
  if (name === "unreal_get_project_context") {
    const response = await resolveProjectContextRequest({
      checkConnection: checkUnrealConnection,
      fetchImpl: fetch,
      url: CONFIG.unrealMcpUrl,
      timeoutMs: CONFIG.requestTimeoutMs,
    });
    log.info("Project context resolved", { isError: !!response.isError });
    return response;
  }

  // Handle status check (lightweight — uses cached tools, no context probe)
  if (name === "unreal_status") {
    const status = await getConnectionStatus();
    if (status.connected) {
      // Use cached tool list instead of re-fetching
      const unrealTools = toolCache.tools;
      const categories = {};

      for (const tool of unrealTools) {
        const category = categorizeToolForStatus(tool.name);
        categories[category] = (categories[category] || 0) + 1;
      }

      const contextCategories = listCategories();
      const directCount = unrealTools.filter(t => isToolExposed(t.name)).length;

      const response = {
        connected: true,
        project: status.projectName,
        engine: status.engineVersion,
        context_categories: contextCategories.length,
        tool_summary: categories,
        total_tools: unrealTools.length,
        tool_profile: CONFIG.toolProfile,
        exposed_tools: directCount + (CONFIG.toolProfile === "balanced" ? 5 : 4),
        external_agent: getAgentStatus(status),
        message: "Unreal Editor connected. Use unreal_project_memory for the compact shared handoff.",
      };

      return {
        content: [
          {
            type: "text",
            text: JSON.stringify(response, null, 2),
          },
        ],
      };
    } else {
      return {
        content: [
          {
            type: "text",
            text: JSON.stringify({
              connected: false,
              reason: status.reason,
              message: "Unreal Editor is not running or the plugin is not enabled. Please start Unreal Editor with the plugin.",
            }, null, 2),
          },
        ],
        isError: true,
      };
    }
  }

  // Router tool — dispatches to underlying mega-tool
  if (name === "unreal_ue") {
    const { domain, operation, params: routerParams } = args || {};

    if (!domain || !operation) {
      return {
        content: [{ type: "text", text: "Error: unreal_ue requires 'domain' and 'operation' parameters." }],
        isError: true,
      };
    }

    const targetTool = resolveUnrealTool(domain, operation);
    if (!targetTool) {
      return {
        content: [{
          type: "text",
          text: `Error: Unknown domain "${domain}". Valid domains: blueprint, anim, character, enhanced_input, material, asset, sequencer, world, performance, narrative`,
        }],
        isError: true,
      };
    }

    const unrealArgs = { operation, ...(routerParams || {}) };

    log.info("Router dispatch", { domain, operation, targetTool });

    const progressToken = request.params._meta?.progressToken;
    const onProgress = progressToken
      ? ({ progress, total, message }) => {
          server.notification({
            method: "notifications/progress",
            params: { progressToken, progress, total: total || 0, message },
          });
        }
      : undefined;

    let result;
    if (CONFIG.asyncEnabled) {
      result = await _executeUnrealToolAsync(
        CONFIG.unrealMcpUrl,
        CONFIG.requestTimeoutMs,
        targetTool,
        unrealArgs,
        { onProgress, pollIntervalMs: CONFIG.pollIntervalMs, asyncTimeoutMs: CONFIG.asyncTimeoutMs }
      );
    } else {
      result = await executeUnrealTool(targetTool, unrealArgs);
    }

    if (result.success) recordToolActivity(await getConnectionStatus(), targetTool, unrealArgs, result);
    return formatToolResponse(targetTool, result);
  }

  // Strip "unreal_" prefix to get actual tool name
  if (!name.startsWith("unreal_")) {
    return {
      content: [
        {
          type: "text",
          text: `Unknown tool: ${name}`,
        },
      ],
      isError: true,
    };
  }

  const toolName = name.substring(7);

  // Tools excluded from auto-async: task_* tools and read-only tools
  const isTaskTool = toolName.startsWith("task_");
  const cachedTool = toolCache.tools.find(t => t.name === toolName);
  const isReadOnly = cachedTool?.annotations?.readOnlyHint === true;

  let result;
  if (CONFIG.asyncEnabled && !isTaskTool && !isReadOnly) {
    const progressToken = request.params._meta?.progressToken;
    const onProgress = progressToken
      ? ({ progress, total, message }) => {
          server.notification({
            method: "notifications/progress",
            params: { progressToken, progress, total: total || 0, message },
          });
        }
      : undefined;

    result = await _executeUnrealToolAsync(
      CONFIG.unrealMcpUrl,
      CONFIG.requestTimeoutMs,
      toolName,
      args,
      {
        onProgress,
        pollIntervalMs: CONFIG.pollIntervalMs,
        asyncTimeoutMs: CONFIG.asyncTimeoutMs,
      }
    );
  } else {
    result = await executeUnrealTool(toolName, args);
  }

  const response = formatToolResponse(toolName, result);
  if (result.success) recordToolActivity(await getConnectionStatus(), toolName, args, result);
  if (CONFIG.injectContext && result.success) {
    log.debug("Injected context for tool", { tool: toolName });
  }
  return response;
});

// Start the server
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);

  const categories = listCategories();
  const testContext = loadContextForCategory("animation");
  const contextStatus = testContext ? `OK (${categories.length} categories loaded)` : "FAILED";

  log.info("UE5 MCP Server started", {
    version: "1.4.1",
    unrealUrl: CONFIG.unrealMcpUrl,
    timeoutMs: CONFIG.requestTimeoutMs,
    asyncEnabled: CONFIG.asyncEnabled,
    asyncTimeoutMs: CONFIG.asyncTimeoutMs,
    pollIntervalMs: CONFIG.pollIntervalMs,
    contextInjection: CONFIG.injectContext,
    contextSystem: contextStatus,
    contextCategories: categories,
  });
}

main().catch((error) => {
  log.error("Fatal error", { error: error.message, stack: error.stack });
  process.exit(1);
});
