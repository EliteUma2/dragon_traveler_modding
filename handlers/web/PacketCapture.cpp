#include "pch-il2cpp.h"
#include "PacketCapture.h"
#include "Il2CppResolver.h"

#include <detours/detours.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <iostream>

namespace PacketCapture {

// ─── State ──────────────────────────────────────────────────────────────────

static std::atomic<bool> s_enabled{false};
static std::atomic<uint32_t> s_seqCounter{0};
static std::atomic<uint32_t> s_sendCount{0};
static std::atomic<uint32_t> s_recvCount{0};

static std::mutex s_queueMutex;
static std::vector<CapturedPacket> s_queue;
static Il2CppClass* s_recvDataClass = nullptr;

// ─── SendRaw hook ───────────────────────────────────────────────────────────
// GameEngine::NetMod::SendRaw(int32 msgId, byte[] buffer, int32 len)
// IL2CPP compiled: void(Il2CppObject* self, int32_t msgId, Il2CppArray* buf, int32_t len, const MethodInfo* method)

typedef void (*SendRaw_t)(Il2CppObject*, int32_t, Il2CppArray*, int32_t, const MethodInfo*);
static SendRaw_t s_sendRawOriginal = nullptr;
static const MethodInfo* s_sendRawMethod = nullptr;

// C++ impl — uses objects with destructors (CapturedPacket, lock_guard)
static void SendRaw_CaptureImpl(int32_t msgId, Il2CppArray* buf, int32_t len) {
    uint8_t* data = (uint8_t*)buf + sizeof(Il2CppArray); // +32 on x64
    CapturedPacket pkt;
    pkt.dir = CapturedPacket::SEND;
    pkt.msgId = msgId;
    pkt.len = len;
    pkt.dataLen = len;
    pkt.timestamp = GetTickCount64();
    pkt.seq = s_seqCounter.fetch_add(1);
    int32_t copyLen = (len > 65535) ? 65535 : len;
    pkt.data.assign(data, data + copyLen);
    s_sendCount.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(s_queueMutex);
        s_queue.push_back(std::move(pkt));
    }
}

// SEH wrapper — no C++ objects with destructors allowed
static void SendRaw_CaptureGuarded(int32_t msgId, Il2CppArray* buf, int32_t len) {
    __try { SendRaw_CaptureImpl(msgId, buf, len); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* snapshot failed, don't crash */ }
}

static void SendRaw_Hook(Il2CppObject* self, int32_t msgId, Il2CppArray* buf, int32_t len, const MethodInfo* method) {
    if (s_enabled.load() && buf && len > 0) {
        SendRaw_CaptureGuarded(msgId, buf, len);
    }
    s_sendRawOriginal(self, msgId, buf, len, method);
}

// ─── OnReceiveMsg hook ──────────────────────────────────────────────────────
// Game::MessageMod::OnReceiveMsg(ReceiveMsgData recvData)
// IL2CPP compiled: void(Il2CppObject* self, Il2CppObject* recvData, const MethodInfo* method)

typedef void (*OnReceiveMsg_t)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
static OnReceiveMsg_t s_onReceiveMsgOriginal = nullptr;
static const MethodInfo* s_onReceiveMsgMethod = nullptr;

// Cached field offsets for ReceiveMsgData
static size_t s_recvMsgId_offset = 0;
static size_t s_recvLength_offset = 0;
static size_t s_recvDataLength_offset = 0;
static size_t s_recvData_offset = 0;
static bool s_recvOffsetsResolved = false;

// C++ impl — uses objects with destructors
static void OnReceiveMsg_CaptureImpl(Il2CppObject* recvData) {
    // Validate the object is actually a ReceiveMsgData instance
    if (s_recvDataClass && il2cpp_object_get_class(recvData) != s_recvDataClass) return;

    int32_t msgId      = *(int32_t*)((uint8_t*)recvData + s_recvMsgId_offset);
    int32_t length     = *(int32_t*)((uint8_t*)recvData + s_recvLength_offset);
    int32_t dataLength = *(int32_t*)((uint8_t*)recvData + s_recvDataLength_offset);
    Il2CppArray* dataArr = *(Il2CppArray**)((uint8_t*)recvData + s_recvData_offset);

    // Validate: msgId must be positive, dataLength in sane range, array must exist
    if (!dataArr || msgId <= 0 || dataLength <= 0 || dataLength > 0x100000) return;

    // Cross-check against the array's actual allocated length
    il2cpp_array_size_t arrLen = dataArr->max_length;
    if (arrLen <= 0 || dataLength > (int32_t)arrLen) return;

    uint8_t* rawData = (uint8_t*)dataArr + sizeof(Il2CppArray);
    int32_t copyLen = (dataLength > 65535) ? 65535 : dataLength;

    CapturedPacket pkt;
    pkt.dir = CapturedPacket::RECV;
    pkt.msgId = msgId;
    pkt.len = length;
    pkt.dataLen = dataLength;
    pkt.timestamp = GetTickCount64();
    pkt.seq = s_seqCounter.fetch_add(1);
    pkt.data.assign(rawData, rawData + copyLen);
    s_recvCount.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(s_queueMutex);
        s_queue.push_back(std::move(pkt));
    }
}

// SEH wrapper — no C++ objects with destructors allowed
static void OnReceiveMsg_CaptureGuarded(Il2CppObject* recvData) {
    __try { OnReceiveMsg_CaptureImpl(recvData); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* snapshot failed */ }
}

// Cache MessageMod instance from hook for recv injection
static Il2CppObject* s_msgModInstance = nullptr;

static void OnReceiveMsg_Hook(Il2CppObject* self, Il2CppObject* recvData, const MethodInfo* method) {
    s_msgModInstance = self; // always cache latest instance
    if (s_enabled.load() && recvData && s_recvOffsetsResolved) {
        OnReceiveMsg_CaptureGuarded(recvData);
    }
    s_onReceiveMsgOriginal(self, recvData, method);
}

// ─── Send queue (browser → game) ─────────────────────────────────────────────
// Sends are queued from any thread, processed on an IL2CPP-attached thread.

struct SendRequest {
    int32_t msgId;
    std::vector<uint8_t> data;
};

static std::mutex s_sendQueueMutex;
static std::vector<SendRequest> s_sendQueue;

static Il2CppArray* s_sendArray = nullptr;
static uint32_t     s_sendGcHandle = 0;
static FieldInfo*   s_netModInstField = nullptr;

void QueueSend(int32_t msgId, const uint8_t* data, int32_t len) {
    if (len <= 0 || len > 65535 || !data) return;
    SendRequest req;
    req.msgId = msgId;
    req.data.assign(data, data + len);
    {
        std::lock_guard<std::mutex> lk(s_sendQueueMutex);
        s_sendQueue.push_back(std::move(req));
    }
}

// Must be called from an IL2CPP-attached thread (e.g. broadcast thread)
void ProcessSendQueue() {
    std::vector<SendRequest> pending;
    {
        std::lock_guard<std::mutex> lk(s_sendQueueMutex);
        pending.swap(s_sendQueue);
    }
    if (pending.empty()) return;
    if (!s_sendRawMethod || !s_netModInstField) return;

    Resolver::Protection::safe_call([&]() {
        // Get NetMod._inst
        Il2CppObject* inst = nullptr;
        il2cpp_field_static_get_value(s_netModInstField, &inst);
        if (!inst) return;

        // Allocate pinned array once
        if (!s_sendArray) {
            Il2CppClass* byteClass = il2cpp_class_from_name(
                il2cpp_get_corlib(), "System", "Byte");
            s_sendArray = il2cpp_array_new(byteClass, 65535);
            if (s_sendArray)
                s_sendGcHandle = il2cpp_gchandle_new((Il2CppObject*)s_sendArray, true);
        }
        if (!s_sendArray) return;

        for (auto& req : pending) {
            int32_t len = (int32_t)req.data.size();
            memcpy((uint8_t*)s_sendArray + sizeof(Il2CppArray), req.data.data(), len);

            int32_t localMsgId = req.msgId;
            int32_t localLen = len;
            void* params[] = { &localMsgId, s_sendArray, &localLen };
            Resolver::Protection::SafeRuntimeInvoke(s_sendRawMethod, inst, params);
        }
    });
}

// ─── Recv queue (browser → game, simulate server message) ───────────────────
// Injects a message as if the server sent it, by calling OnReceiveMsg directly.

struct RecvRequest {
    int32_t msgId;
    std::vector<uint8_t> data; // raw proto bytes (no transport frame)
};

static std::mutex s_recvQueueMutex;
static std::vector<RecvRequest> s_recvQueue;

void QueueRecv(int32_t msgId, const uint8_t* data, int32_t len) {
    if (len <= 0 || len > 65535 || !data) return;
    RecvRequest req;
    req.msgId = msgId;
    req.data.assign(data, data + len);
    {
        std::lock_guard<std::mutex> lk(s_recvQueueMutex);
        s_recvQueue.push_back(std::move(req));
    }
}

// C++ impl — uses objects with destructors
static void ProcessRecvQueueImpl() {
    std::vector<RecvRequest> pending;
    {
        std::lock_guard<std::mutex> lk(s_recvQueueMutex);
        pending.swap(s_recvQueue);
    }
    if (pending.empty()) return;
    if (!s_onReceiveMsgOriginal || !s_recvOffsetsResolved || !s_msgModInstance) return;
    if (!s_recvDataClass) {
        s_recvDataClass = Resolver::FindClass("GameEngine", "ReceiveMsgData");
        if (!s_recvDataClass) return;
    }

    Il2CppClass* byteClass = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");

    for (auto& req : pending) {
        Il2CppObject* recvData = il2cpp_object_new(s_recvDataClass);
        if (!recvData) continue;

        Il2CppArray* dataArr = il2cpp_array_new(byteClass, req.data.size());
        if (!dataArr) continue;
        memcpy((uint8_t*)dataArr + sizeof(Il2CppArray), req.data.data(), req.data.size());

        int32_t dataLen = (int32_t)req.data.size();
        *(int32_t*)((uint8_t*)recvData + s_recvMsgId_offset) = req.msgId;
        *(int32_t*)((uint8_t*)recvData + s_recvLength_offset) = dataLen;
        *(int32_t*)((uint8_t*)recvData + s_recvDataLength_offset) = dataLen;
        *(Il2CppArray**)((uint8_t*)recvData + s_recvData_offset) = dataArr;

        s_onReceiveMsgOriginal(s_msgModInstance, recvData, s_onReceiveMsgMethod);
    }
}

// SEH wrapper
static void ProcessRecvQueueGuarded() {
    __try { ProcessRecvQueueImpl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* failed */ }
}

void ProcessRecvQueue() {
    ProcessRecvQueueGuarded();
}

// ─── Enable / Disable ───────────────────────────────────────────────────────

bool Enable() {
    if (s_enabled.load()) return true;

    // ── Resolve SendRaw ──
    Il2CppClass* netModClass = Resolver::FindClass("GameEngine", "NetMod");
    if (!netModClass) {
        std::cout << "[WebApi] NetMod class not found" << std::endl;
        return false;
    }

    s_netModInstField = il2cpp_class_get_field_from_name(netModClass, "_inst");

    s_sendRawMethod = il2cpp_class_get_method_from_name(netModClass, "SendRaw", 3);
    if (!s_sendRawMethod || !s_sendRawMethod->methodPointer) {
        std::cout << "[WebApi] SendRaw method not found" << std::endl;
        return false;
    }

    s_sendRawOriginal = (SendRaw_t)s_sendRawMethod->methodPointer;

    // ── Resolve OnReceiveMsg ──
    Il2CppClass* msgModClass = Resolver::FindClass("Game", "MessageMod");
    if (!msgModClass) {
        std::cout << "[WebApi] MessageMod class not found" << std::endl;
        return false;
    }

    s_onReceiveMsgMethod = il2cpp_class_get_method_from_name(msgModClass, "OnReceiveMsg", 1);
    if (!s_onReceiveMsgMethod || !s_onReceiveMsgMethod->methodPointer) {
        std::cout << "[WebApi] OnReceiveMsg method not found" << std::endl;
        return false;
    }

    s_onReceiveMsgOriginal = (OnReceiveMsg_t)s_onReceiveMsgMethod->methodPointer;

    // ── Resolve ReceiveMsgData field offsets ──
    s_recvDataClass = Resolver::FindClass("GameEngine", "ReceiveMsgData");
    Il2CppClass* recvDataClass = s_recvDataClass;
    if (recvDataClass) {
        FieldInfo* f;
        f = il2cpp_class_get_field_from_name(recvDataClass, "MsgId");
        if (f) s_recvMsgId_offset = f->offset;
        f = il2cpp_class_get_field_from_name(recvDataClass, "Length");
        if (f) s_recvLength_offset = f->offset;
        f = il2cpp_class_get_field_from_name(recvDataClass, "DataLength");
        if (f) s_recvDataLength_offset = f->offset;
        f = il2cpp_class_get_field_from_name(recvDataClass, "Data");
        if (f) s_recvData_offset = f->offset;

        s_recvOffsetsResolved = (s_recvMsgId_offset && s_recvLength_offset &&
                                  s_recvDataLength_offset && s_recvData_offset);
        if (!s_recvOffsetsResolved) {
            std::cout << "[WebApi] ReceiveMsgData field offsets incomplete" << std::endl;
        }
    } else {
        std::cout << "[WebApi] ReceiveMsgData class not found" << std::endl;
        s_recvOffsetsResolved = false;
    }

    // ── Install Detours ──
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    LONG err1 = DetourAttach(&(PVOID&)s_sendRawOriginal, SendRaw_Hook);
    if (err1 != NO_ERROR) {
        std::cout << "[WebApi] DetourAttach SendRaw failed: " << err1 << std::endl;
        DetourTransactionAbort();
        return false;
    }

    LONG err2 = NO_ERROR;
    if (s_recvOffsetsResolved) {
        err2 = DetourAttach(&(PVOID&)s_onReceiveMsgOriginal, OnReceiveMsg_Hook);
        if (err2 != NO_ERROR) {
            std::cout << "[WebApi] DetourAttach OnReceiveMsg failed: " << err2 << std::endl;
            // Continue with just SendRaw
        }
    }

    LONG commit = DetourTransactionCommit();
    if (commit != NO_ERROR) {
        std::cout << "[WebApi] DetourTransactionCommit failed: " << commit << std::endl;
        return false;
    }

    s_enabled.store(true);
    std::cout << "[WebApi] PacketCapture enabled (Send: OK, Recv: "
              << (s_recvOffsetsResolved && err2 == NO_ERROR ? "OK" : "SKIP") << ")" << std::endl;
    return true;
}

void Disable() {
    if (!s_enabled.load()) return;
    s_enabled.store(false);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (s_sendRawOriginal) {
        DetourDetach(&(PVOID&)s_sendRawOriginal, SendRaw_Hook);
    }
    if (s_onReceiveMsgOriginal && s_recvOffsetsResolved) {
        DetourDetach(&(PVOID&)s_onReceiveMsgOriginal, OnReceiveMsg_Hook);
    }

    DetourTransactionCommit();

    // Clear queue
    {
        std::lock_guard<std::mutex> lk(s_queueMutex);
        s_queue.clear();
    }

    s_sendRawOriginal = nullptr;
    s_onReceiveMsgOriginal = nullptr;
    s_sendCount.store(0);
    s_recvCount.store(0);
    s_seqCounter.store(0);

    // Free pinned GC handle to prevent leak on re-enable
    if (s_sendGcHandle) {
        il2cpp_gchandle_free(s_sendGcHandle);
        s_sendGcHandle = 0;
    }
    s_sendArray = nullptr;

    std::cout << "[WebApi] PacketCapture disabled" << std::endl;
}

bool IsEnabled() {
    return s_enabled.load();
}

std::vector<CapturedPacket> Drain() {
    std::vector<CapturedPacket> out;
    {
        std::lock_guard<std::mutex> lk(s_queueMutex);
        out.swap(s_queue);
    }
    return out;
}

uint32_t GetSendCount() { return s_sendCount.load(); }
uint32_t GetRecvCount() { return s_recvCount.load(); }

} // namespace PacketCapture
