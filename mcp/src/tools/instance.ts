import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { get, post } from "../bridge.js";

function json(data: any): { content: { type: "text"; text: string }[] } {
  return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}

export function registerInstanceTools(server: McpServer) {
  server.tool(
    "find_instances",
    "Find live instances of a class (FindObjectsOfType) with addresses and field values",
    {
      ns: z.string().describe("Namespace"),
      name: z.string().describe("Class name"),
      limit: z.string().default("50").describe("Max results"),
    },
    async ({ ns, name, limit }) => json(await get("/api/instances", { ns, name, limit }))
  );

  server.tool(
    "read_field",
    "Read a field value from a live object at an address",
    {
      addr: z.string().describe("Object address (hex, e.g. '0x1234abcd')"),
      field: z.string().describe("Field name"),
    },
    async ({ addr, field }) => json(await get("/api/readfield", { addr, field }))
  );

  server.tool(
    "write_field",
    "Write a field value on a live object",
    {
      addr: z.string().describe("Object address (hex)"),
      field: z.string().describe("Field name"),
      value: z.string().describe("New value (string representation)"),
    },
    async ({ addr, field, value }) => json(await post("/api/writefield", { addr, field, value }))
  );

  server.tool(
    "write_static",
    "Write a static field value on a class",
    {
      ns: z.string().describe("Namespace"),
      name: z.string().describe("Class name"),
      field: z.string().describe("Field name"),
      value: z.string().describe("New value"),
    },
    async ({ ns, name, field, value }) => json(await post("/api/writestatic", { ns, name: name, field, value }))
  );

  server.tool(
    "invoke_method",
    "Invoke a method at runtime on a class or instance",
    {
      ns: z.string().describe("Namespace"),
      name: z.string().describe("Class name"),
      method: z.string().describe("Method name"),
      args: z.array(z.string()).default([]).describe("Method arguments as strings"),
      instance: z.string().optional().describe("Instance address (hex) for instance methods, omit for static"),
    },
    async ({ ns, name, method, args, instance }) =>
      json(await post("/api/invoke", { ns, class: name, method, args, instance }))
  );
}
