#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace PacketCapture {

    struct CapturedPacket {
        enum Dir : uint8_t { SEND = 0, RECV = 1 };
        Dir dir;
        int32_t msgId;
        int32_t len;         // transport length (Send) or Length field (Recv)
        int32_t dataLen;     // same as len for Send, DataLength for Recv
        std::vector<uint8_t> data;
        uint64_t timestamp;  // GetTickCount64
        uint32_t seq;        // auto-increment
    };

    // Enable/disable hooks
    bool Enable();
    void Disable();
    bool IsEnabled();

    // Drain all pending packets (thread-safe, swap-and-return)
    std::vector<CapturedPacket> Drain();

    // Queue a raw packet to be sent into the game (thread-safe, processed on attached thread)
    // data = full transport frame (20-byte header + proto payload)
    void QueueSend(int32_t msgId, const uint8_t* data, int32_t len);

    // Process queued sends — call from an IL2CPP-attached thread only
    void ProcessSendQueue();

    // Queue a recv simulation (as if server sent it) — thread-safe
    // data = raw proto bytes (no transport frame)
    void QueueRecv(int32_t msgId, const uint8_t* data, int32_t len);

    // Process queued recv injections — call from an IL2CPP-attached thread only
    void ProcessRecvQueue();

    // Stats
    uint32_t GetSendCount();
    uint32_t GetRecvCount();
}
