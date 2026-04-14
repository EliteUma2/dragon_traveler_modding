import { z } from "zod";
const BOT_BASE = process.env.BOT_API_URL || "http://localhost:9999";
async function botGet(path, params) {
    const url = new URL(path, BOT_BASE);
    if (params)
        for (const [k, v] of Object.entries(params))
            if (v !== undefined)
                url.searchParams.set(k, v);
    const res = await fetch(url.toString(), { signal: AbortSignal.timeout(15000) });
    if (!res.ok) {
        const body = await res.text().catch(() => "");
        throw new Error(`HTTP ${res.status}: ${res.statusText}${body ? " — " + body.slice(0, 200) : ""}`);
    }
    const ct = res.headers.get("content-type") || "";
    return ct.includes("application/json") ? res.json() : res.text();
}
async function botPost(path, body) {
    const res = await fetch(new URL(path, BOT_BASE).toString(), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body ?? {}),
        signal: AbortSignal.timeout(30000),
    });
    if (!res.ok) {
        const text = await res.text().catch(() => "");
        throw new Error(`HTTP ${res.status}: ${res.statusText}${text ? " — " + text.slice(0, 200) : ""}`);
    }
    const ct = res.headers.get("content-type") || "";
    return ct.includes("application/json") ? res.json() : res.text();
}
async function botDelete(path) {
    const res = await fetch(new URL(path, BOT_BASE).toString(), {
        method: "DELETE",
        signal: AbortSignal.timeout(15000),
    });
    if (!res.ok)
        throw new Error(`HTTP ${res.status}: ${res.statusText}`);
    const ct = res.headers.get("content-type") || "";
    return ct.includes("application/json") ? res.json() : res.text();
}
function json(data) {
    return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}
export function registerBotTools(server) {
    // ── Account Management ──────────────────────────────────────────────────────
    server.tool("bot_status", "Get status of all bot accounts and proxy info", {}, async () => json(await botGet("/api/status")));
    server.tool("bot_add_account", "Add a bot account", {
        email: z.string().describe("Account email"),
        password: z.string().describe("Account password"),
        serverId: z.string().optional().describe("Server ID"),
    }, async ({ email, password, serverId }) => json(await botPost("/api/accounts", { email, password, serverId })));
    server.tool("bot_remove_account", "Remove a bot account by ID", { id: z.number().describe("Account ID") }, async ({ id }) => json(await botDelete(`/api/accounts/${id}`)));
    server.tool("bot_start", "Start a bot account (connect + login + claims)", { id: z.number().describe("Account ID") }, async ({ id }) => json(await botPost(`/api/accounts/${id}/start`)));
    server.tool("bot_start_bot", "Start a bot account and immediately begin auto-play (connect + login + auto-play to chapter 3+)", { id: z.number().describe("Account ID") }, async ({ id }) => json(await botPost(`/api/accounts/${id}/start-bot`)));
    server.tool("bot_stop", "Stop a bot account", { id: z.number().describe("Account ID") }, async ({ id }) => json(await botPost(`/api/accounts/${id}/stop`)));
    server.tool("bot_start_all", "Start all bot accounts", {}, async () => json(await botPost("/api/start-all")));
    server.tool("bot_stop_all", "Stop all bot accounts", {}, async () => json(await botPost("/api/stop-all")));
    server.tool("bot_logs", "Get recent logs for a bot account", {
        id: z.number().describe("Account ID"),
        limit: z.number().optional().describe("Max log lines (default 100)"),
    }, async ({ id, limit }) => json(await botGet(`/api/accounts/${id}/logs`, { limit: String(limit ?? 100) })));
    server.tool("bot_load_file", "Load accounts from a text file", { path: z.string().describe("Path to accounts.txt file") }, async ({ path }) => json(await botPost("/api/load-file", { path })));
    server.tool("bot_register", "Register new game account(s)", {
        count: z.number().optional().describe("Number of accounts to create (default 1, max 50)"),
        serverId: z.string().optional().describe("Server ID"),
        password: z.string().optional().describe("Password for new accounts"),
    }, async ({ count, serverId, password }) => json(await botPost("/api/register", { count, serverId, password })));
    // ── Packet Send/Receive ─────────────────────────────────────────────────────
    server.tool("bot_send", "Send a game packet from a specific bot account", {
        id: z.number().describe("Account ID"),
        name: z.string().describe("Message name (e.g. CSReqEnterDungeon)"),
        data: z.record(z.any()).optional().describe("Packet data object"),
    }, async ({ id, name, data }) => json(await botPost("/api/send", { id, name, data: data ?? {} })));
    server.tool("bot_send_all", "Send a game packet from all bot accounts", {
        name: z.string().describe("Message name"),
        data: z.record(z.any()).optional().describe("Packet data object"),
    }, async ({ name, data }) => json(await botPost("/api/send-all", { name, data: data ?? {} })));
    // ── Packet Inspection ───────────────────────────────────────────────────────
    server.tool("bot_packets", "Get recent packets from bot accounts and/or MITM proxy. Packets are already decoded with message names and proto fields.", {
        since: z.number().optional().describe("Sequence number to poll from (0 = all)"),
        limit: z.number().optional().describe("Max packets (default 200, max 500)"),
    }, async ({ since, limit }) => {
        const params = {};
        if (since !== undefined)
            params.since = String(since);
        if (limit !== undefined)
            params.limit = String(limit);
        json(await botGet("/api/packets", params));
        return json(await botGet("/api/packets", params));
    });
    // ── Hero / Game State ───────────────────────────────────────────────────────
    server.tool("bot_heroes", "Get hero roster, equipment, items, currencies, formation, and next dungeon enemy info for a bot account", { id: z.number().describe("Account ID") }, async ({ id }) => json(await botGet(`/api/accounts/${id}/heroes`)));
    server.tool("bot_servers", "Get available game servers for an account", { id: z.number().describe("Account ID") }, async ({ id }) => json(await botGet(`/api/accounts/${id}/servers`)));
    server.tool("bot_change_server", "Change the game server for an account", {
        id: z.number().describe("Account ID"),
        serverId: z.string().describe("New server ID"),
    }, async ({ id, serverId }) => json(await botPost(`/api/accounts/${id}/server`, { serverId })));
    // ── Autoplay ────────────────────────────────────────────────────────────────
    server.tool("bot_autoplay", "Start, stop, or configure autoplay for a bot account", {
        id: z.number().describe("Account ID"),
        action: z.enum(["start", "stop", "configure"]).describe("Action"),
        settings: z.record(z.any()).optional().describe("Autoplay settings (for start/configure)"),
    }, async ({ id, action, settings }) => json(await botPost(`/api/accounts/${id}/autoplay`, { action, settings })));
    // ── MITM Proxy ──────────────────────────────────────────────────────────────
    server.tool("bot_proxy_inject", "Inject a packet through the MITM proxy (to server or to client)", {
        direction: z.enum(["C2S", "S2C"]).describe("C2S = send to server, S2C = send to client"),
        name: z.string().describe("Message name (e.g. CSReqEnterDungeon)"),
        data: z.record(z.any()).optional().describe("Packet data object"),
    }, async ({ direction, name, data }) => json(await botPost("/api/proxy/inject", { direction, name, data: data ?? {} })));
    server.tool("bot_proxy_rules", "List all MITM proxy interception rules", {}, async () => json(await botGet("/api/proxy/rules")));
    server.tool("bot_proxy_add_rule", "Add a MITM proxy rule (drop or modify packets)", {
        packet: z.string().describe("Message name to match, or '*' for all"),
        direction: z.enum(["C2S", "S2C"]).optional().describe("Direction filter"),
        action: z.enum(["drop", "modify"]).describe("Action: drop or modify"),
        mods: z.array(z.object({
            path: z.string().describe("Dot-path to field (e.g. 'battleEnd.result')"),
            value: z.any().describe("New value"),
        })).optional().describe("Modifications (for action=modify)"),
        label: z.string().optional().describe("Human-readable label"),
    }, async ({ packet, direction, action, mods, label }) => json(await botPost("/api/proxy/rules", { packet, direction, action, mods, label })));
    server.tool("bot_proxy_remove_rule", "Remove a MITM proxy rule by ID", { id: z.number().describe("Rule ID") }, async ({ id }) => json(await botDelete(`/api/proxy/rules/${id}`)));
    server.tool("bot_proxy_target", "Get or set the MITM proxy target server", {
        host: z.string().optional().describe("New target host (omit to just read current)"),
        port: z.number().optional().describe("New target port"),
    }, async ({ host, port }) => {
        if (host && port)
            return json(await botPost("/api/proxy/target", { host, port }));
        return json(await botGet("/api/proxy/target"));
    });
    server.tool("bot_proxy_target_server", "Resolve a game server ID to IP:port and set as proxy target (uses account login data)", {
        accountId: z.number().describe("Account ID to use for server resolution"),
        serverId: z.string().describe("Game server ID to connect to"),
    }, async ({ accountId, serverId }) => json(await botPost("/api/proxy/target-server", { accountId, serverId })));
    // ── Game Schema ─────────────────────────────────────────────────────────────
    server.tool("bot_messages", "List all known message names and IDs (for autocomplete/lookup)", {}, async () => json(await botGet("/api/messages")));
    server.tool("bot_schema", "Get the proto schema for a message (fields, sub-messages, enums)", { name: z.string().describe("Message name (e.g. CSReqEnterDungeon)") }, async ({ name }) => json(await botGet("/api/schema", { name })));
    // ── GameDB ──────────────────────────────────────────────────────────────────
    server.tool("bot_gamedb_resolve", "Resolve game IDs to human-readable names (heroes, items, equipment, currencies)", {
        heroes: z.string().optional().describe("Comma-separated hero TIDs"),
        items: z.string().optional().describe("Comma-separated item IDs"),
        equips: z.string().optional().describe("Comma-separated equipment template IDs"),
        currencies: z.string().optional().describe("Comma-separated currency IDs"),
    }, async ({ heroes, items, equips, currencies }) => {
        const params = {};
        if (heroes)
            params.heroes = heroes;
        if (items)
            params.items = items;
        if (equips)
            params.equips = equips;
        if (currencies)
            params.currencies = currencies;
        return json(await botGet("/api/gamedb/resolve", params));
    });
    server.tool("bot_gamedb_query", "Run a SELECT query against the game SQLite databases", {
        table: z.string().describe("Table name (determines which .db file)"),
        sql: z.string().describe("SELECT SQL query"),
    }, async ({ table, sql }) => json(await botGet("/api/gamedb/query", { table, sql })));
    server.tool("bot_gamedb_tables", "List game database tables, optionally filtered by name pattern", { q: z.string().optional().describe("Search pattern") }, async ({ q }) => json(await botGet("/api/gamedb/tables", q ? { q } : {})));
}
