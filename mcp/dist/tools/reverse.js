import { z } from "zod";
import { get } from "../bridge.js";
function json(data) {
    return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}
export function registerReverseTools(server) {
    server.tool("list_modules", "List all loaded modules (DLLs) with base address, size, and path", {}, async () => json(await get("/api/modules")));
    server.tool("addr_to_module", "Resolve an address to its containing module + offset from base", { addr: z.string().describe("Address (hex, e.g. '0x7ffa55131f30')") }, async ({ addr }) => json(await get("/api/module", { addr })));
    server.tool("list_exports", "List PE export table entries (name, ordinal, RVA, VA) for a module", { module: z.string().describe("Module name (e.g. 'GameAssembly.dll')") }, async ({ module: mod }) => json(await get("/api/exports", { module: mod })));
    server.tool("list_imports", "List PE import table entries (module, function, IAT address) for a module", { module: z.string().describe("Module name") }, async ({ module: mod }) => json(await get("/api/imports", { module: mod })));
    server.tool("list_threads", "List all threads with ID, start address, containing module, and state", {}, async () => json(await get("/api/threads")));
    server.tool("memory_regions", "Walk virtual memory regions (base, size, state, protect, type)", {
        addr: z.string().optional().describe("Start address (hex, default: 0x0)"),
        size: z.string().optional().describe("Range size to scan (hex)"),
    }, async ({ addr, size }) => {
        const params = {};
        if (addr)
            params.addr = addr;
        if (size)
            params.size = size;
        return json(await get("/api/regions", params));
    });
    server.tool("symbol_lookup", "DbgHelp symbol lookup: resolve address to function name, displacement, source line", { addr: z.string().describe("Address (hex)") }, async ({ addr }) => json(await get("/api/symbol", { addr })));
    server.tool("find_strings", "Scan a memory region for ASCII and UTF-16 strings", {
        addr: z.string().describe("Start address (hex)"),
        size: z.string().describe("Region size in bytes (decimal)"),
        minlen: z.string().default("4").describe("Minimum string length"),
    }, async ({ addr, size, minlen }) => json(await get("/api/strings", { addr, size, minlen })));
    server.tool("pattern_scan", "IDA-style signature scan within a module (e.g. '48 89 5C 24 ?? 48 89 74')", {
        module: z.string().describe("Module name (e.g. 'GameAssembly.dll')"),
        sig: z.string().describe("Byte pattern with ?? wildcards"),
    }, async ({ module: mod, sig }) => json(await get("/api/pattern", { module: mod, sig })));
    server.tool("find_xrefs", "Find code references (CALL, JMP, LEA) to a target address", {
        addr: z.string().describe("Target address (hex)"),
        module: z.string().optional().describe("Module to scan (default: GameAssembly.dll)"),
    }, async ({ addr, module: mod }) => {
        const params = { addr };
        if (mod)
            params.module = mod;
        return json(await get("/api/xrefs", params));
    });
    server.tool("process_info", "Get process info: PID, memory stats, handle count, uptime", {}, async () => json(await get("/api/processinfo")));
}
