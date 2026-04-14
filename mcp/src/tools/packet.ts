import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { post, get } from "../bridge.js";

function json(data: any): { content: { type: "text"; text: string }[] } {
  return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}

// Trim large decoded objects to keep output reasonable
function trimDecoded(obj: any, maxDepth = 3, depth = 0): any {
  if (obj === null || obj === undefined || typeof obj !== "object") return obj;
  if (Array.isArray(obj)) {
    if (obj.length > 10) {
      const trimmed = obj.slice(0, 10).map(v => trimDecoded(v, maxDepth, depth + 1));
      trimmed.push(`... ${obj.length - 10} more items`);
      return trimmed;
    }
    return obj.map(v => trimDecoded(v, maxDepth, depth + 1));
  }
  if (depth >= maxDepth) {
    const keys = Object.keys(obj);
    if (keys.length > 5) return `{${keys.length} fields: ${keys.slice(0, 5).join(", ")}...}`;
    return obj;
  }
  const out: any = {};
  const keys = Object.keys(obj);
  for (const k of keys) {
    // Skip 'has*' fields that are just booleans echoing presence
    if (k.startsWith("has") && typeof obj[k] === "boolean" && obj[k] === true) continue;
    if (k.startsWith("has") && typeof obj[k] === "boolean" && obj[k] === false) continue;
    out[k] = trimDecoded(obj[k], maxDepth, depth + 1);
  }
  return out;
}

// Cache msgId→className mapping
let msgDefsCache: Record<string, string> | null = null;
let msgDefsCacheTime = 0;

async function getMsgDefs(): Promise<Record<string, string>> {
  const now = Date.now();
  if (msgDefsCache && now - msgDefsCacheTime < 60000) return msgDefsCache;
  try {
    msgDefsCache = await get("/api/msgdefs") as Record<string, string>;
    msgDefsCacheTime = now;
  } catch {
    if (!msgDefsCache) msgDefsCache = {};
  }
  return msgDefsCache!;
}

async function tryDecode(msgId: number, hexData: string): Promise<any> {
  try {
    return await get("/api/decode", { msgId: String(msgId), data: hexData });
  } catch {
    return null;
  }
}

export function registerPacketTools(server: McpServer) {
  server.tool(
    "send_packet",
    "Send a raw packet to the game (msgId + hex-encoded proto data)",
    {
      msgId: z.number().describe("Message ID"),
      data: z.string().describe("Hex-encoded packet data"),
    },
    async ({ msgId, data }) => json(await post("/api/send", { msgId, data }))
  );

  server.tool(
    "capture_packets",
    "Poll recently captured packets with auto-decode. Returns packets with message names and decoded proto fields. Use 'since' for incremental polling. Recv packets are decoded using the game's own deserializer; send packets include raw hex (they have a 20-byte transport header).",
    {
      since: z.number().optional().describe("Sequence number to start after (0 = all available)"),
      limit: z.number().optional().describe("Max packets to return (default 100, max 1000)"),
      decode: z.boolean().optional().describe("Auto-decode recv packets (default true)"),
      filter: z.string().optional().describe("Filter by message name substring (case-insensitive), e.g. 'Battle' or 'Tower'"),
      msgId: z.number().optional().describe("Filter by exact message ID"),
      dir: z.enum(["send", "recv"]).optional().describe("Filter by direction: 'send' or 'recv'"),
      exclude: z.string().optional().describe("Exclude packets matching name substring (case-insensitive), e.g. 'Ping,Pong' to skip heartbeat"),
    },
    async ({ since, limit, decode: doDecode, filter, msgId: filterMsgId, dir: filterDir, exclude }) => {
      const params: Record<string, string> = {};
      if (since !== undefined) params.since = String(since);
      params.limit = String(limit ?? 100);

      const [packets, defs] = await Promise.all([
        get("/api/packets", params) as Promise<any[]>,
        getMsgDefs(),
      ]);

      const shouldDecode = doDecode !== false;

      // Enrich packets with names and optionally decode
      const results: any[] = [];
      const decodePromises: { idx: number; promise: Promise<any> }[] = [];

      for (const pkt of packets) {
        const name = defs[String(pkt.msgId)] || `unknown_${pkt.msgId}`;

        // Apply filters
        if (filterMsgId !== undefined && pkt.msgId !== filterMsgId) continue;
        if (filterDir && pkt.dir !== filterDir) continue;
        if (filter && !name.toLowerCase().includes(filter.toLowerCase())) continue;
        if (exclude) {
          const excludeParts = exclude.toLowerCase().split(",").map((s: string) => s.trim());
          if (excludeParts.some(ex => name.toLowerCase().includes(ex))) continue;
        }

        const entry: any = {
          seq: pkt.seq,
          dir: pkt.dir,
          msgId: pkt.msgId,
          name,
          len: pkt.dataLen,
          ts: pkt.ts,
        };

        if (pkt.dir === "recv" && shouldDecode && pkt.data) {
          // Queue decode for recv packets (raw proto)
          const idx = results.length;
          results.push(entry);
          decodePromises.push({ idx, promise: tryDecode(pkt.msgId, pkt.data) });
        } else {
          // Send packets: include raw hex (has 20-byte transport header)
          entry.data = pkt.data;
          results.push(entry);
        }
      }

      // Run decodes in parallel (batches of 10 to avoid overwhelming)
      for (let i = 0; i < decodePromises.length; i += 10) {
        const batch = decodePromises.slice(i, i + 10);
        const decoded = await Promise.all(batch.map(b => b.promise));
        for (let j = 0; j < batch.length; j++) {
          const d = decoded[j];
          if (d && d.fields) {
            results[batch[j].idx].decoded = trimDecoded(d.fields);
          } else if (d) {
            results[batch[j].idx].decoded = trimDecoded(d);
          } else {
            results[batch[j].idx].data = packets.find(
              (p: any) => p.seq === results[batch[j].idx].seq
            )?.data;
          }
        }
      }

      return json(results);
    }
  );
}
