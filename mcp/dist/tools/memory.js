import { z } from "zod";
import { get, getRaw, post } from "../bridge.js";
function json(data) {
    return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}
export function registerMemoryTools(server) {
    server.tool("mem_dump", "Hex dump of raw memory at an address", {
        addr: z.string().describe("Address (hex, e.g. '0x7ffa5500')"),
        size: z.string().default("256").describe("Number of bytes to dump (decimal)"),
    }, async ({ addr, size }) => json(await get("/api/memdump", { addr, size })));
    server.tool("mem_scan", "Scan method body for CMP instructions (proto field number discovery)", {
        ns: z.string().describe("Namespace"),
        name: z.string().describe("Class name"),
        method: z.string().describe("Method name"),
    }, async ({ ns, name, method }) => json(await get("/api/memscan", { ns, name, method })));
    server.tool("method_info", "Dump the MethodInfo struct for a method (address, invoker, params, flags)", {
        ns: z.string().describe("Namespace"),
        name: z.string().describe("Class name"),
        method: z.string().describe("Method name"),
    }, async ({ ns, name, method }) => json(await get("/api/methodinfo", { ns, name, method })));
    server.tool("vtable", "List all vtable entries for a class", {
        ns: z.string().describe("Namespace"),
        name: z.string().describe("Class name"),
    }, async ({ ns, name }) => json(await get("/api/vtable", { ns, name })));
    server.tool("find_pe", "Scan process memory for PE files (MZ headers)", {}, async () => json(await get("/api/findpe")));
    server.tool("dump_pe", "Dump raw PE bytes at address (returns base64-encoded data)", { addr: z.string().describe("Address of PE header (hex)") }, async ({ addr }) => json(await getRaw("/api/dumppe", { addr })));
    server.tool("rc4_debug", "Get RC4 encryption debug log. Shows S-box state, plaintext, ciphertext for each RC4.Encrypt call. Hook must be installed first via rc4_hook.", {
        clear: z.boolean().optional().describe("Clear log after reading (default false)"),
    }, async ({ clear }) => {
        const params = {};
        if (clear)
            params.clear = "1";
        return json(await get("/api/rc4debug", params));
    });
    server.tool("rc4_hook", "Install or uninstall the RC4.Encrypt Detours hook for debugging. Once installed, all RC4.Encrypt calls are logged with full S-box state snapshots.", {
        action: z.enum(["install", "uninstall"]).describe("install or uninstall the hook"),
    }, async ({ action }) => json(await post(`/api/rc4hook?action=${action}`, {})));
    server.tool("price_hook", "Manage the UploadHandlerRaw hook. Actions: install, uninstall, enable, disable, set (field override), remove (field override), clear (all overrides), list.", {
        action: z.enum(["install", "uninstall", "enable", "disable", "set", "remove", "clear", "list"]).describe("Action to perform"),
        field: z.string().optional().describe("JSON field name (for set/remove, e.g. 'goods_price', 'goods_id')"),
        value: z.string().optional().describe("Replacement value (for set, e.g. '6', 'diamond_60')"),
    }, async ({ action, field, value }) => {
        const parts = [`action=${action}`];
        if (field !== undefined)
            parts.push(`field=${encodeURIComponent(field)}`);
        if (value !== undefined)
            parts.push(`value=${encodeURIComponent(value)}`);
        return json(await post(`/api/pricehook?${parts.join("&")}`, {}));
    });
    server.tool("price_hook_log", "Get the price hook intercept log. Shows all UploadHandlerRaw bodies seen, with original and modified content.", {
        clear: z.boolean().optional().describe("Clear log after reading"),
    }, async ({ clear }) => {
        const params = {};
        if (clear)
            params.clear = "1";
        return json(await get("/api/pricehook", params));
    });
}
