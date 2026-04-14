import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { get } from "../bridge.js";

function json(data: any): { content: { type: "text"; text: string }[] } {
  return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}

export function registerReflectionTools(server: McpServer) {
  server.tool(
    "list_assemblies",
    "List all loaded IL2CPP assemblies with class counts",
    {},
    async () => json(await get("/api/assemblies"))
  );

  server.tool(
    "list_classes",
    "List classes in a namespace (name, fieldCount, methodCount)",
    { ns: z.string().describe("Namespace (e.g. 'Game', 'GameEngine')") },
    async ({ ns }) => json(await get("/api/classes", { ns }))
  );

  server.tool(
    "get_class",
    "Full class info: fields, methods, properties, base class, interfaces",
    {
      ns: z.string().describe("Namespace"),
      name: z.string().describe("Class name"),
    },
    async ({ ns, name }) => json(await get("/api/class", { ns, name }))
  );

  server.tool(
    "search",
    "Search classes and methods by substring",
    { query: z.string().describe("Search term") },
    async ({ query }) => json(await get("/api/search", { q: query }))
  );

  server.tool(
    "read_static",
    "Read a static field value (int/float/string/pointer)",
    {
      ns: z.string().describe("Namespace"),
      name: z.string().describe("Class name"),
      field: z.string().describe("Static field name"),
    },
    async ({ ns, name, field }) => json(await get("/api/static", { ns, name, field }))
  );
}
