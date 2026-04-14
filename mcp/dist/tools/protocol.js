import { z } from "zod";
import { get } from "../bridge.js";
function json(data) {
    return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}
export function registerProtocolTools(server) {
    server.tool("msg_defs", "Get all msgId→className mappings", {}, async () => json(await get("/api/msgdefs")));
    server.tool("msg_schema", "Get proto schema for a single message class", { name: z.string().describe("Message class name (e.g. 'CSLogin', 'SCGuildExplore')") }, async ({ name }) => json(await get("/api/msgschema", { name })));
    server.tool("msg_dump", "Bulk dump of all message definitions, schemas, and enums", {}, async () => json(await get("/api/msgdump")));
    server.tool("proto_map", "Get proto field→tag number mappings for a message (or all messages if name omitted)", { name: z.string().optional().describe("Message class name (optional, omit for all)") }, async ({ name }) => json(await get("/api/protomap", name ? { name } : {})));
    server.tool("proto_tags", "Discover proto tag numbers via IL2CPP analysis", {
        ns: z.string().describe("Namespace"),
        name: z.string().describe("Class name"),
    }, async ({ ns, name }) => json(await get("/api/prototags", { ns, name })));
    server.tool("decode_packet", "Decode raw proto bytes into JSON using the game's MergeFrom deserializer", {
        msgId: z.string().describe("Message ID (decimal)"),
        hex: z.string().describe("Hex-encoded proto data"),
    }, async ({ msgId, hex }) => json(await get("/api/decode", { msgId, hex })));
}
