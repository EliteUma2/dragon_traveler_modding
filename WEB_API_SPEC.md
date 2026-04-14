# WEB Api Feature — Specification & Progress

## Status: Phase 1 — Skeleton + Static Serving

## Architecture
- **WebServer** (`handlers/web/WebServer.h/.cpp`) — WinSock2 HTTP + WebSocket
- **PacketCapture** (`handlers/web/PacketCapture.h/.cpp`) — Detours hooks on SendRaw & OnReceiveMsg
- **ReflectionApi** (`handlers/web/ReflectionApi.h/.cpp`) — IL2CPP REST routes
- **WebApiTAB** (`handlers/gui/tabs/WebApiTAB.h/.cpp`) — ImGui tab
- **Web UI** (`web/`) — HTML + JS + CSS

## Key Technical Details

### SendRaw Hook
- Namespace: `GameEngine`, Class: `NetMod`, Method: `SendRaw`
- IL2CPP sig: `void(Il2CppObject* self, int32_t msgId, Il2CppArray* buf, int32_t len, const MethodInfo*)`
- Buffer data at `(uint8_t*)buf + 32` (Il2CppArray header size on x64)
- Array always 65535 bytes, `len` is meaningful length
- This is a transport frame (BE frame: 4B len + 4B token + 4B route + 4B flags + 4B msgId + proto payload)

### OnReceiveMsg Hook
- Namespace: `Game`, Class: `MessageMod`, Method: `OnReceiveMsg`
- IL2CPP sig: `void(Il2CppObject* self, Il2CppObject* recvData, const MethodInfo*)`
- recvData is `ReceiveMsgData` with fields: `Data` (Byte[]), `Length`, `DataLength`, `MsgId`
- `Data` is raw protobuf (no transport frame)
- Read fields via memory offset: `*(T*)((uint8_t*)obj + field->offset)`

### Detours Pattern (from InitHooks.cpp)
```cpp
DetourTransactionBegin();
DetourUpdateThread(GetCurrentThread());
DetourAttach(&(PVOID&)Original, Hook);
DetourTransactionCommit();
```

### WebSocket Handshake
- SHA-1 via Windows CryptoAPI (`CryptCreateHash` + `CALG_SHA1`)
- Key: SHA1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") → Base64

### WS Protocol (JSON)
- Server→Client: `{"type":"packet","dir":"send|recv","msgId":N,"len":N,"data":"hex","seq":N,"ts":N}`
- Client→Server: `{"type":"send","msgId":N,"data":"hex"}`

### Existing Patterns to Reuse
- `Resolver::FindClass(ns, name)` — find IL2CPP class
- `il2cpp_class_get_method_from_name(klass, name, argc)` — get method
- `method->methodPointer` — native function address for Detours
- `il2cpp_array_new(byteClass, 65535)` — allocate managed array
- `il2cpp_gchandle_new(obj, true)` — pin array for GC safety
- `Resolver::Protection::SafeRuntimeInvoke(method, obj, params)` — safe invoke

## Phase Checklist

- [x] Phase 1: Skeleton + Static Serving
- [x] Phase 2: WebSocket
- [x] Phase 3: SendRaw Hook
- [x] Phase 4: OnReceiveMsg Hook
- [x] Phase 5: Packet Detail + Protocol Editor
- [x] Phase 6: Packet Send
- [x] Phase 7: Protocol Editor Integration
- [x] Phase 8: Reflection API

## Files Created
- `handlers/web/WebServer.h` + `.cpp` — HTTP + WebSocket server (WinSock2)
- `handlers/web/PacketCapture.h` + `.cpp` — Detours hooks on SendRaw & OnReceiveMsg
- `handlers/web/ReflectionApi.h` + `.cpp` — IL2CPP reflection REST routes
- `handlers/gui/tabs/WebApiTAB.h` + `.cpp` — ImGui tab with enable/disable
- `web/index.html` — Packet monitor page
- `web/app.js` — WS client, packet list, detail, send UI
- `web/app.css` — Dark theme styles
- `web/protocol-editor.js` — Copied from user's packet modifier
- `web/protocol-editor.css` — Copied from user's packet modifier

## Files Modified
- `handlers/gui/menu.cpp` — Added tab 3 (WEB Api)
- `il2cpp-dll-injection.vcxproj` — Added all new .cpp/.h entries

## API Routes
- `GET /api/assemblies` — List all loaded assemblies
- `GET /api/classes?ns=X&image=Y` — List classes in namespace/image
- `GET /api/class?ns=X&name=Y` — Full class info (fields, methods, properties)
- `GET /api/search?q=X` — Search classes/methods by name
- `GET /api/static?ns=X&name=Y&field=Z` — Read static field value
