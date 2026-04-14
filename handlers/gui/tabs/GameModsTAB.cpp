#include "pch-il2cpp.h"

#include "gui/tabs/GameModsTAB.h"
#include <imgui/imgui.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <Windows.h>

#include "Il2CppResolver.h"

// --- Speed state ---
static std::atomic<float> s_speedTarget{1.0f};  // current desired speed
static std::atomic<bool> s_speedThreadRunning{false};
static std::string s_speedStatus;

static void CallResetDeltaTime(float value) {
	Il2CppClass* bmClass = Resolver::FindClass("Battle", "BattleMgr");
	if (!bmClass) { s_speedStatus = "BattleMgr not found"; return; }

	FieldInfo* insField = il2cpp_class_get_field_from_name(bmClass, "__ins");
	if (!insField) { s_speedStatus = "BattleMgr._ins not found"; return; }

	Il2CppObject* ins = nullptr;
	il2cpp_field_static_get_value(insField, &ins);
	if (!ins || !Resolver::Protection::IsValidIl2CppObject(ins)) {
		s_speedStatus = "BattleMgr._ins is null";
		return;
	}

	const MethodInfo* resetDT = il2cpp_class_get_method_from_name(bmClass, "ResetDeltaTime", 1);
	if (!resetDT) { s_speedStatus = "ResetDeltaTime not found"; return; }

	void* params[] = { &value };
	Resolver::Protection::SafeRuntimeInvoke(resetDT, ins, params);
}

// Background thread: re-applies current speed every 500ms
static void SpeedThreadFunc() {
	il2cpp_thread_attach(il2cpp_domain_get());
	s_speedThreadRunning.store(true);
	while (true) {
		float val = s_speedTarget.load();
		Resolver::Protection::safe_call([&]() {
			CallResetDeltaTime(val);
		});
		Sleep(500);
	}
}

static void SetSpeed(float value) {
	s_speedTarget.store(value);
	s_speedStatus = "x" + std::to_string((int)value);
	// Also apply immediately (don't wait for thread tick)
	Resolver::Protection::safe_call([&]() {
		CallResetDeltaTime(value);
	});
}

// --- MaxSpeed state ---
static bool s_maxSpeedInitDone = false; // one-time CheckWhitelistMod field override
static bool s_cctorDone = false;        // one-time DebugMod .cctor call
static std::string s_statusMsg;

static void DoMaxSpeed() {
	s_statusMsg.clear();

	// Step A: One-time — unlock time scale limits via CheckWhitelistMod._inst
	if (!s_maxSpeedInitDone) {
		Il2CppClass* cwmClass = Resolver::FindClass("Game.Hotfix", "CheckCheatMod");
		if (!cwmClass) { s_statusMsg = "CheckWhitelistMod class not found"; return; }

		FieldInfo* instField = il2cpp_class_get_field_from_name(cwmClass, "_inst");
		if (!instField) { s_statusMsg = "_inst field not found"; return; }

		Il2CppObject* inst = nullptr;
		il2cpp_field_static_get_value(instField, &inst);
		if (!inst || !Resolver::Protection::IsValidIl2CppObject(inst)) {
			s_statusMsg = "_inst is null or invalid";
			return;
		}

		FieldInfo* diffOff = il2cpp_class_get_field_from_name(cwmClass, "_diffOff");
		FieldInfo* maxTS = il2cpp_class_get_field_from_name(cwmClass, "_maxTimeScale");
		if (!diffOff || !maxTS) { s_statusMsg = "_diffOff or _maxTimeScale not found"; return; }

		float bigVal = 99999999.0f;
		il2cpp_field_set_value(inst, diffOff, &bigVal);
		il2cpp_field_set_value(inst, maxTS, &bigVal);

		s_maxSpeedInitDone = true;
	}

	// Step B: DebugMod — call .cctor once, then DoTimeCmd(2) x50
	Il2CppClass* dmClass = Resolver::FindClass("Game.Hotfix", "DebugMod");
	if (!dmClass) { s_statusMsg = "DebugMod class not found"; return; }

	// Call .cctor once (static constructor / initialization)
	if (!s_cctorDone) {
		const MethodInfo* cctor = il2cpp_class_get_method_from_name(dmClass, ".cctor", 0);
		if (cctor) {
			Resolver::Protection::SafeRuntimeInvoke(cctor, nullptr, nullptr);
		}
		s_cctorDone = true;
	}

	// Get static Inst field
	FieldInfo* instField = il2cpp_class_get_field_from_name(dmClass, "Inst");
	if (!instField) { s_statusMsg = "DebugMod.Inst field not found"; return; }

	Il2CppObject* debugInst = nullptr;
	il2cpp_field_static_get_value(instField, &debugInst);
	if (!debugInst || !Resolver::Protection::IsValidIl2CppObject(debugInst)) {
		s_statusMsg = "DebugMod.Inst is null or invalid";
		return;
	}

	// Get DoTimeCmd method (1 param: EnumMainCityCmdType)
	const MethodInfo* doTimeCmd = il2cpp_class_get_method_from_name(dmClass, "DoTimeCmd", 1);
	if (!doTimeCmd) { s_statusMsg = "DoTimeCmd method not found"; return; }

	// Call DoTimeCmd(2) x50
	int enumVal = 2;
	for (int i = 0; i < 500; i++) {
		void* params[] = { &enumVal };
		Resolver::Protection::SafeRuntimeInvoke(doTimeCmd, debugInst, params);
	}

	s_statusMsg = "OK";
}

// --- Skip Level ---
static std::atomic<int> s_skipDungeonId{1000306};
static std::string s_skipStatus;
static std::mutex s_skipMutex;
static std::atomic<bool> s_skipInProgress{false};
static bool s_dryRun = false;

// Persistent pinned arrays — survive GC, safe for async encryption inside SendRaw
static Il2CppArray* s_pkt1Array = nullptr;
static Il2CppArray* s_pkt2Array = nullptr;
static uint32_t s_pkt1GcHandle = 0;
static uint32_t s_pkt2GcHandle = 0;

static void SetSkipStatus(const std::string& msg) {
	std::lock_guard<std::mutex> lk(s_skipMutex);
	s_skipStatus = msg;
}

static std::string HexDump(const uint8_t* data, size_t len) {
	std::ostringstream oss;
	for (size_t i = 0; i < len; i += 16) {
		oss << std::setfill('0') << std::setw(4) << std::hex << i << ": ";
		for (size_t j = 0; j < 16; j++) {
			if (i + j < len)
				oss << std::setfill('0') << std::setw(2) << std::hex << (int)data[i + j] << " ";
			else
				oss << "   ";
			if (j == 7) oss << " ";
		}
		oss << " |";
		for (size_t j = 0; j < 16 && i + j < len; j++) {
			uint8_t c = data[i + j];
			oss << (char)(c >= 32 && c < 127 ? c : '.');
		}
		oss << "|" << std::endl;
	}
	return oss.str();
}

static std::string VarintHex(const std::vector<uint8_t>& v) {
	std::ostringstream oss;
	for (size_t i = 0; i < v.size(); i++) {
		if (i > 0) oss << " ";
		oss << std::setfill('0') << std::setw(2) << std::hex << std::uppercase << (int)v[i];
	}
	return oss.str();
}

// Encode uint32 as protobuf varint
static std::vector<uint8_t> EncodeVarint(uint32_t val) {
	std::vector<uint8_t> out;
	while (val > 0x7F) {
		out.push_back((uint8_t)(val & 0x7F) | 0x80);
		val >>= 7;
	}
	out.push_back((uint8_t)val);
	return out;
}

static void DoSkipLevelThread(int dungeonId, bool dryRun) {
	il2cpp_thread_attach(il2cpp_domain_get());

	std::ofstream logFile;
	if (dryRun) {
		logFile.open("d:\\dp\\skip_debug.txt", std::ios::trunc);
		logFile << "========================================\n";
		logFile << "  SKIP LEVEL DRY RUN\n";
		logFile << "========================================\n\n";
		logFile << "Dungeon ID: " << std::dec << dungeonId << "\n";
	}

	SetSkipStatus(dryRun ? "Dry run..." : "Sending packet 1...");

	// Find Netmod._inst and SendRaw (shared for both packets)
	Il2CppClass* nmClass = Resolver::FindClass("GameEngine", "NetMod");
	if (!nmClass) { SetSkipStatus("Netmod not found"); s_skipInProgress = false; return; }

	FieldInfo* instField = il2cpp_class_get_field_from_name(nmClass, "_inst");
	if (!instField) { SetSkipStatus("_inst not found"); s_skipInProgress = false; return; }

	Il2CppObject* inst = nullptr;
	il2cpp_field_static_get_value(instField, &inst);
	if (!inst || !Resolver::Protection::IsValidIl2CppObject(inst)) {
		SetSkipStatus("_inst is null"); s_skipInProgress = false; return;
	}

	const MethodInfo* sendRaw = il2cpp_class_get_method_from_name(nmClass, "SendRaw", 3);
	if (!sendRaw) { SetSkipStatus("SendRaw not found"); s_skipInProgress = false; return; }

	Il2CppClass* byteClass = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
	if (!byteClass) { SetSkipStatus("System.Byte not found"); s_skipInProgress = false; return; }

	// Allocate persistent pinned arrays once — they survive GC and stay valid
	// for async encryption inside SendRaw
	if (!s_pkt1Array) {
		s_pkt1Array = il2cpp_array_new(byteClass, 65535);
		if (s_pkt1Array) s_pkt1GcHandle = il2cpp_gchandle_new((Il2CppObject*)s_pkt1Array, true);
	}
	if (!s_pkt2Array) {
		s_pkt2Array = il2cpp_array_new(byteClass, 65535);
		if (s_pkt2Array) s_pkt2GcHandle = il2cpp_gchandle_new((Il2CppObject*)s_pkt2Array, true);
	}
	if (!s_pkt1Array || !s_pkt2Array) {
		SetSkipStatus("Failed to allocate pinned arrays"); s_skipInProgress = false; return;
	}

	auto varint = EncodeVarint((uint32_t)dungeonId);

	if (dryRun) {
		logFile << "Varint encoding: " << VarintHex(varint) << " (" << varint.size() << " bytes)\n\n";
		logFile << "--- Resolved IL2CPP Objects ---\n";
		logFile << "  NetMod class:    0x" << std::hex << (uintptr_t)nmClass << "\n";
		logFile << "  _inst field:     0x" << std::hex << (uintptr_t)instField << "\n";
		logFile << "  inst object:     0x" << std::hex << (uintptr_t)inst << "\n";
		logFile << "  SendRaw method:  0x" << std::hex << (uintptr_t)sendRaw << "\n";
		logFile << "  Byte class:      0x" << std::hex << (uintptr_t)byteClass << "\n\n";

		// Log method signature and param types
		logFile << "--- SendRaw Method Signature ---\n";
		logFile << "  Name: " << (sendRaw->name ? sendRaw->name : "???") << "\n";
		logFile << "  Param count: " << std::dec << il2cpp_method_get_param_count(sendRaw) << "\n";
		for (uint32_t pi = 0; pi < il2cpp_method_get_param_count(sendRaw); pi++) {
			const Il2CppType* pType = il2cpp_method_get_param(sendRaw, pi);
			const char* pName = il2cpp_method_get_param_name(sendRaw, pi);
			Il2CppClass* pKlass = pType ? il2cpp_class_from_type(pType) : nullptr;
			logFile << "  param[" << pi << "]: "
				<< (pName ? pName : "???")
				<< " type=" << (pType ? std::to_string(pType->type) : "null")
				<< " class=" << (pKlass && pKlass->name ? pKlass->name : "???") << "\n";
		}
		logFile << "\n";
		logFile << "--- Pinned Arrays ---\n";
		logFile << "  pkt1Array: 0x" << std::hex << (uintptr_t)s_pkt1Array << " (gchandle=" << std::dec << s_pkt1GcHandle << ")\n";
		logFile << "  pkt2Array: 0x" << std::hex << (uintptr_t)s_pkt2Array << " (gchandle=" << std::dec << s_pkt2GcHandle << ")\n\n";
	}

	// --- Packet 1 (skip packet) ---
	std::vector<uint8_t> pkt1;
	{
		static const uint8_t prefix[] = {
			0x00, 0x00, 0x00, 0x55, 0xB5, 0xBC, 0xF5, 0xCA,
			0x00, 0x00, 0x02, 0x99, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x0F, 0xA1, 0x08
		};
		static const uint8_t suffix[] = {
			0x18, 0x00, 0x22, 0x00,
			0x28, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
			0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
			0x38, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
			0x40, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
			0x48, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
			0x50, 0x01
		};

		pkt1.insert(pkt1.end(), prefix, prefix + sizeof(prefix));
		pkt1.insert(pkt1.end(), varint.begin(), varint.end());
		pkt1.push_back(0x10);
		pkt1.insert(pkt1.end(), varint.begin(), varint.end());
		pkt1.insert(pkt1.end(), suffix, suffix + sizeof(suffix));

		int32_t totalLen = (int32_t)pkt1.size();

		if (dryRun) {
			logFile << "========================================\n";
			logFile << "  PACKET 1\n";
			logFile << "========================================\n";
			logFile << "  msgId:    " << std::dec << 4001 << "\n";
			logFile << "  dataLen:  " << std::dec << totalLen << "\n";
			logFile << "  arrayLen: 65535\n";
			logFile << "  Call:     SendRaw(4001, Il2CppArray[65535], " << totalLen << ")\n\n";
			logFile << "  Packet hex dump:\n" << HexDump(pkt1.data(), pkt1.size()) << "\n";
		} else {
			memcpy((uint8_t*)s_pkt1Array + sizeof(Il2CppArray), pkt1.data(), totalLen);

			int32_t msgId = 4001;
			void* params[] = { &msgId, s_pkt1Array, &totalLen };
			Resolver::Protection::SafeRuntimeInvoke(sendRaw, inst, params);
		}
	}

	if (!dryRun) {
		SetSkipStatus("Packet 1 sent, waiting 1s...");
		Sleep(100);

		// Re-validate inst after sleep — game state may have changed
		inst = nullptr;
		il2cpp_field_static_get_value(instField, &inst);
		if (!inst || !Resolver::Protection::IsValidIl2CppObject(inst)) {
			SetSkipStatus("_inst invalidated after pkt1"); s_skipInProgress = false; return;
		}
	}

	// --- Packet 2 (confirm packet, base dungeon 1000721 replaced with dungeonId) ---
	std::vector<uint8_t> pkt2;
	{
		// Bytes 4-20 of the base template (after 4-byte BE length prefix)
		// Bytes 0-3 (packet bytes 4-7) are randomized each send
		static uint8_t prefix2_mid[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x99,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xA3,
			0x08
		};
		unsigned int randomToken = rand() * rand();
		memcpy(prefix2_mid, &randomToken, sizeof(randomToken));

		// 40 bytes — everything after 08+varint2 inside the inner message
		static const uint8_t innerSuffix[] = {
			0x10, 0x01, 0x1A, 0x24,
			0x20, 0x00, 0x00, 0x00, 0xC6, 0x09, 0x00, 0x00,
			0x08, 0xAA, 0x14, 0x12, 0x13, 0x0A, 0x11, 0x11,
			0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C,
			0xE2, 0xB8, 0x37, 0x68, 0x72, 0x8E, 0x70, 0x01,
			0x20, 0x00, 0x28, 0x02
		};

		uint32_t innerLen = 1 + (uint32_t)varint.size() + (uint32_t)sizeof(innerSuffix);
		auto innerLenVarint = EncodeVarint(innerLen);

		// Build body (everything after the 4-byte BE length prefix)
		std::vector<uint8_t> body;
		body.insert(body.end(), prefix2_mid, prefix2_mid + sizeof(prefix2_mid));
		body.insert(body.end(), varint.begin(), varint.end());
		body.push_back(0x12);
		body.insert(body.end(), innerLenVarint.begin(), innerLenVarint.end());
		body.push_back(0x08);
		body.insert(body.end(), varint.begin(), varint.end());
		body.insert(body.end(), innerSuffix, innerSuffix + sizeof(innerSuffix));

		// Prepend 4-byte big-endian length prefix
		uint32_t bodyLen = (uint32_t)body.size();
		pkt2.push_back((uint8_t)(bodyLen >> 24));
		pkt2.push_back((uint8_t)(bodyLen >> 16));
		pkt2.push_back((uint8_t)(bodyLen >> 8));
		pkt2.push_back((uint8_t)(bodyLen));
		pkt2.insert(pkt2.end(), body.begin(), body.end());

		int32_t totalLen = (int32_t)pkt2.size();

		if (dryRun) {
			logFile << "========================================\n";
			logFile << "  PACKET 2 (sent 1s after packet 1)\n";
			logFile << "========================================\n";
			logFile << "  msgId:    " << std::dec << 4003 << "\n";
			logFile << "  dataLen:  " << std::dec << totalLen << "\n";
			logFile << "  arrayLen: 65535\n";
			logFile << "  innerLen: " << std::dec << innerLen << " (varint: " << VarintHex(innerLenVarint) << ")\n";
			logFile << "  bodyLen:  " << std::dec << bodyLen << " (BE prefix: "
			        << std::hex << std::setfill('0') << std::setw(2) << (int)(uint8_t)(bodyLen >> 24) << " "
			        << std::setw(2) << (int)(uint8_t)(bodyLen >> 16) << " "
			        << std::setw(2) << (int)(uint8_t)(bodyLen >> 8) << " "
			        << std::setw(2) << (int)(uint8_t)(bodyLen) << ")\n";
			logFile << "  Call:     SendRaw(4003, Il2CppArray[65535], " << std::dec << totalLen << ")\n\n";
			logFile << "  Packet hex dump:\n" << HexDump(pkt2.data(), pkt2.size()) << "\n";
		} else {
			memcpy((uint8_t*)s_pkt2Array + sizeof(Il2CppArray), pkt2.data(), totalLen);

			int32_t msgId = 4003;
			void* params[] = { &msgId, s_pkt2Array, &totalLen };
			Resolver::Protection::SafeRuntimeInvoke(sendRaw, inst, params);
		}
	}

	if (dryRun) {
		logFile << "========================================\n";
		logFile << "  SUMMARY\n";
		logFile << "========================================\n";
		logFile << "  1) SendRaw(4001, byte[65535], " << std::dec << pkt1.size() << ")  -- skip request\n";
		logFile << "  2) Sleep(1000ms)\n";
		logFile << "  3) SendRaw(4003, byte[65535], " << std::dec << pkt2.size() << ")  -- confirm\n";
		logFile << "  4) s_skipDungeonId++ (" << dungeonId << " -> " << (dungeonId + 1) << ")\n";
		logFile.close();

		SetSkipStatus("Dry run saved to skip_debug.txt");
		s_skipInProgress.store(false);
		return;
	}

	int newId = s_skipDungeonId.fetch_add(1) + 1;
	SetSkipStatus("OK (id=" + std::to_string(dungeonId) + " -> " + std::to_string(newId) + ")");
	s_skipInProgress.store(false);
}

// --- Skill Spam ---
static std::atomic<bool> s_skillSpamActive{false};
static std::atomic<bool> s_skillSpamThreadRunning{false};
static std::string s_skillSpamStatus;
static std::mutex s_skillSpamMutex;

static void SetSkillSpamStatus(const std::string& msg) {
	std::lock_guard<std::mutex> lk(s_skillSpamMutex);
	s_skillSpamStatus = msg;
}

static void SkillSpamThreadFunc() {
	il2cpp_thread_attach(il2cpp_domain_get());
	s_skillSpamThreadRunning.store(true);

	static const int32_t skillIds[] = { 36, 96, 56, 120, 76, 142 };
	static const int NUM_SKILLS = 6;

	// Resolve once
	Il2CppClass* bmClass = nullptr;
	FieldInfo* insField = nullptr;
	const MethodInfo* playSkill = nullptr;

	Resolver::Protection::safe_call([&]() {
		bmClass = Resolver::FindClass("Battle", "BattleMgr");
		if (!bmClass) { SetSkillSpamStatus("BattleMgr not found"); return; }
		insField = il2cpp_class_get_field_from_name(bmClass, "__ins");
		if (!insField) { SetSkillSpamStatus("__ins not found"); return; }
		playSkill = il2cpp_class_get_method_from_name(bmClass, "PlayManualSkill", 7);
		if (!playSkill) { SetSkillSpamStatus("PlayManualSkill not found"); return; }
	});

	if (!bmClass || !insField || !playSkill) {
		s_skillSpamActive.store(false);
		s_skillSpamThreadRunning.store(false);
		return;
	}

	// Per-skill cooldown tracking (GetTickCount64)
	uint64_t lastFired[NUM_SKILLS] = {};

	int32_t uid = 0;
	bool isClick = true;
	int32_t targetUID = -1;
	struct { float x, y, z; } pos = {0.0f, 0.0f, 0.0f};
	bool success = true;
	int32_t other = -1;

	while (s_skillSpamActive.load()) {
		Il2CppObject* ins = nullptr;
		Resolver::Protection::safe_call([&]() {
			il2cpp_field_static_get_value(insField, &ins);
		});
		if (!ins || !Resolver::Protection::IsValidIl2CppObject(ins)) {
			SetSkillSpamStatus("__ins is null");
			Sleep(250);
			continue;
		}

		uint64_t now = GetTickCount64();
		bool fired = false;
		for (int i = 0; i < NUM_SKILLS && s_skillSpamActive.load(); i++) {
			if (now - lastFired[i] >= 3000) {
				int32_t skillUID = skillIds[i];
				void* params[] = { &uid, &skillUID, &isClick, &targetUID, &pos, &success, &other };
				Resolver::Protection::safe_call([&]() {
					Resolver::Protection::SafeRuntimeInvoke(playSkill, ins, params);
				});
				lastFired[i] = GetTickCount64();
				fired = true;
				Sleep(250);
			}
		}

		if (fired) SetSkillSpamStatus("OK");
		else Sleep(50); // nothing ready, check again soon
	}

	SetSkillSpamStatus("Stopped");
	s_skillSpamThreadRunning.store(false);
}

// Hotkey polling thread — runs forever, checks F1-F5 via GetAsyncKeyState
static std::atomic<bool> s_hotkeyThreadStarted{false};

static void HotkeyThreadFunc() {
	il2cpp_thread_attach(il2cpp_domain_get());
	while (true) {
		if (GetAsyncKeyState(VK_F1) & 1) SetSpeed(1.0f);
		if (GetAsyncKeyState(VK_F2) & 1) SetSpeed(25.0f);
		if (GetAsyncKeyState(VK_F3) & 1) SetSpeed(50.0f);
		if (GetAsyncKeyState(VK_F4) & 1) SetSpeed(75.0f);
		if (GetAsyncKeyState(VK_F5) & 1) SetSpeed(100.0f);
		Sleep(50);
	}
}

void GameModsTAB::Tick() {
	// Start hotkey + speed threads once
	if (!s_hotkeyThreadStarted.exchange(true)) {
		std::thread(HotkeyThreadFunc).detach();
		std::thread(SpeedThreadFunc).detach();
	}
}

void GameModsTAB::Render() {
	ImGui::Spacing();
	ImGui::Text("Game Mods");
	ImGui::Separator();
	ImGui::Spacing();

	// --- MaxSpeed button ---
	if (ImGui::Button("MaxSpeed", ImVec2(120, 30))) {
		Resolver::Protection::safe_call([&]() {
			DoMaxSpeed();
		});
		if (s_statusMsg.empty()) s_statusMsg = "Unknown error";
	}
	if (!s_statusMsg.empty()) {
		ImGui::SameLine();
		if (s_statusMsg == "OK")
			ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Status: %s", s_statusMsg.c_str());
		else
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Status: %s", s_statusMsg.c_str());
	}

	ImGui::Spacing();

	// --- Speed buttons (F1-F5) ---
	ImGui::Text("Battle Speed:");
	ImGui::SameLine();
	if (!s_speedStatus.empty())
		ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(%s)", s_speedStatus.c_str());

	struct SpeedOpt { const char* label; float value; int fKey; };
	SpeedOpt opts[] = {
		{"x1 [F1]",   1.0f,   0},
		{"x25 [F2]",  25.0f,  0},
		{"x50 [F3]",  50.0f,  0},
		{"x75 [F4]",  75.0f,  0},
		{"x100 [F5]", 100.0f, 0},
	};
	float cur = s_speedTarget.load();
	for (int i = 0; i < 5; i++) {
		if (i > 0) ImGui::SameLine();
		bool active = (cur == opts[i].value);
		if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.9f, 1.0f));
		if (ImGui::Button(opts[i].label, ImVec2(85, 28)))
			SetSpeed(opts[i].value);
		if (active) ImGui::PopStyleColor();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// --- Skip Level ---
	ImGui::Text("Skip Level");
	ImGui::SetNextItemWidth(150);
	int localId = s_skipDungeonId.load();
	if (ImGui::InputInt("Dungeon ID", &localId))
		s_skipDungeonId.store(localId);
	ImGui::SameLine();
	if (ImGui::Button(s_skipInProgress.load() ? "Skipping..." : "Skip", ImVec2(90, 0))) {
		if (!s_skipInProgress.exchange(true)) {
			int id = s_skipDungeonId.load();
			bool dry = s_dryRun;
			std::thread([id, dry]() {
				Resolver::Protection::safe_call([&]() {
					DoSkipLevelThread(id, dry);
				});
				if (s_skipInProgress.load()) {
					SetSkipStatus("Exception during skip");
					s_skipInProgress.store(false);
				}
			}).detach();
		}
	}
	ImGui::SameLine();
	ImGui::Checkbox("Dry Run", &s_dryRun);
	std::string statusCopy;
	{
		std::lock_guard<std::mutex> lk(s_skipMutex);
		statusCopy = s_skipStatus;
	}
	if (!statusCopy.empty()) {
		ImGui::SameLine();
		if (statusCopy.rfind("OK", 0) == 0)
			ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", statusCopy.c_str());
		else if (s_skipInProgress.load())
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", statusCopy.c_str());
		else
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", statusCopy.c_str());
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// --- Skill Spam ---
	ImGui::Text("Skill Spam");
	bool spamActive = s_skillSpamActive.load();
	if (spamActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
	if (ImGui::Button(spamActive ? "Stop Spam" : "Skill Spam", ImVec2(120, 28))) {
		if (spamActive) {
			s_skillSpamActive.store(false);
		} else {
			s_skillSpamActive.store(true);
			std::thread(SkillSpamThreadFunc).detach();
		}
	}
	if (spamActive) ImGui::PopStyleColor();
	{
		std::lock_guard<std::mutex> lk(s_skillSpamMutex);
		if (!s_skillSpamStatus.empty()) {
			ImGui::SameLine();
			if (s_skillSpamStatus.rfind("OK", 0) == 0)
				ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", s_skillSpamStatus.c_str());
			else if (s_skillSpamActive.load())
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", s_skillSpamStatus.c_str());
			else
				ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_skillSpamStatus.c_str());
		}
	}
}
