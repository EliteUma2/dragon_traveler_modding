import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerReflectionTools } from "./tools/reflection.js";
import { registerProtocolTools } from "./tools/protocol.js";
import { registerMemoryTools } from "./tools/memory.js";
import { registerPacketTools } from "./tools/packet.js";
import { registerInstanceTools } from "./tools/instance.js";
import { registerReverseTools } from "./tools/reverse.js";
import { registerSocketTools } from "./tools/socket.js";
import { registerBotTools } from "./tools/bot.js";
const server = new McpServer({
    name: "game-reflection",
    version: "1.0.0",
});
registerReflectionTools(server);
registerProtocolTools(server);
registerMemoryTools(server);
registerPacketTools(server);
registerInstanceTools(server);
registerReverseTools(server);
registerSocketTools(server);
registerBotTools(server);
const transport = new StdioServerTransport();
await server.connect(transport);
