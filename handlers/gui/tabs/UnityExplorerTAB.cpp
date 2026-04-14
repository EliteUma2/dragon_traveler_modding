#include "pch-il2cpp.h"

#define IMGUI_DEFINE_MATH_OPERATORS

#include <iostream>
#include <vector> 
#include <map>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "gui/tabs/UnityExplorerTAB.h"
#include "gui/GUITheme.h" 
#include <helpers.h>
#include <algorithm>
#include <functional>
#include <stack>
#include <set>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <hooks/DirectX.h>

#include "Il2CppResolver.h"

struct HierarchyNode {
	Il2CppObject* ptr;
	std::string name;
	bool isActive;
	std::vector<HierarchyNode> children;
};

struct InspectorHistoryItem {
	Il2CppObject* ptr;
	bool isClassView;
};

static std::stack<InspectorHistoryItem> inspectionHistory;

static Il2CppObject* selectedGameObject = nullptr;
static bool isRawClassView = false; //
static int explorerMode = 0; // 0: Hierarchy, 1: Class Search

static std::map<std::string, std::vector<HierarchyNode>> cachedTree;
static char searchFilter[128] = "";
static bool needsRefresh = true;
static bool showInactive = false;

static char classSearchFilter[128] = "";
static std::vector<Il2CppClass*> searchResults;

// Force-cast: override class detection for a specific object
static Il2CppClass* forceCastClass = nullptr;
static Il2CppObject* forceCastObj = nullptr;

static std::map<const MethodInfo*, std::vector<std::string>> methodParamBuffers;
static std::map<const MethodInfo*, std::string> methodLastResults;
static std::map<const MethodInfo*, Il2CppObject*> methodLastReturnObj; // for SZARRAY/object returns

// Cached enum name resolution map (declared early for Reset() access)
static std::map<Il2CppClass*, std::map<int32_t, std::string>> enumNameCache;

// Find References state
struct RefResult {
	Il2CppClass* ownerClass;
	FieldInfo* field;
	bool isStatic;
	std::string ownerName;
	std::string fieldName;
	std::string fieldTypeName;
};
static std::vector<RefResult> refResults;
static bool showRefResults = false;
static Il2CppClass* lastRefSearchClass = nullptr;
static void ScanReferences(Il2CppClass* targetClass);

// --- Collection Instance Search ---
struct CollSearchFieldInfo {
	FieldInfo* field = nullptr;           // for raw fields
	const MethodInfo* getter = nullptr;   // for properties (get_Xxx)
	bool isProp = false;
	std::string name;
	std::string typeName;
	int typeEnum = 0;
};
struct CollSearchCollectionInfo {
	FieldInfo* field = nullptr;
	std::string name;
	bool isList = false; // true = List<T>, false = SZARRAY
	Il2CppClass* elemClass = nullptr;
};
struct CollSearchResult {
	Il2CppObject* obj = nullptr;
	std::string preview;
};
static Il2CppClass* g_collSearchClass = nullptr;
static std::vector<CollSearchCollectionInfo> g_collSearchCollections;
static std::vector<CollSearchFieldInfo> g_collSearchFields;
static int g_collSearchCollIdx = 0;
static int g_collSearchFieldIdx = 0;
static char g_collSearchValue[256] = {};
static int g_collSearchMode = 0; // 0=contains, 1=exact
static std::vector<CollSearchResult> g_collSearchResults;
static bool g_collSearchDone = false;

// Inspector member search filter (case-insensitive, applies to fields/properties/methods)
static char memberSearchFilter[128] = "";

static bool MatchesMemberFilter(const char* name) {
	if (!memberSearchFilter[0]) return true;
	if (!name) return false;
	std::string lower_name(name);
	std::string lower_filter(memberSearchFilter);
	std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
	std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
	return lower_name.find(lower_filter) != std::string::npos;
}

// --- Watchlist ---
struct WatchEntry {
	Il2CppObject* obj;      // nullptr for static
	FieldInfo* field;
	bool isStatic;
	std::string label;      // "Namespace.Class::fieldName"
};
static std::vector<WatchEntry> watchList;

// --- Static Field Scanner ---
struct StaticFieldResult {
	Il2CppClass* klass;
	FieldInfo* field;
	std::string className;
	std::string fieldName;
	std::string typeName;
};
static std::vector<StaticFieldResult> staticScanResults;
static bool showStaticScan = false;
static char staticScanFilter[128] = "";

// --- Value Scanner (Cheat Engine-like) ---
struct ValueScanResult {
	std::string path;        // e.g. "GameManager._instance.heroes[42].hp"
	std::string valueStr;    // display value
	std::string prevStr;     // previous scan value (for next scan display)
	double numericVal;       // raw numeric for int/float comparison
	Il2CppObject* obj;       // owning object (for re-read / inspect)
	FieldInfo* field;        // field on obj
	int arrayIndex;          // -1 if not an array element
	Il2CppObject* arrayObj;  // array parent (if arrayIndex >= 0)
};
enum ValueScanType { VSCAN_INT = 0, VSCAN_FLOAT, VSCAN_BOOL, VSCAN_STRING };
enum ValueScanMode { VMODE_EXACT = 0, VMODE_BETWEEN, VMODE_NOT_EQUAL, VMODE_GREATER, VMODE_LESS, VMODE_CONTAINS };
// Next-scan comparison modes
enum NextScanMode { NSCAN_EXACT = 0, NSCAN_CHANGED, NSCAN_UNCHANGED, NSCAN_INCREASED, NSCAN_DECREASED, NSCAN_CONTAINS };
static std::vector<ValueScanResult> valueScanResults;
static char vscanValue1[128] = "";       // primary value / min
static char vscanValue2[128] = "";       // max (for between)
static int vscanType = VSCAN_INT;
static int vscanMode = VMODE_EXACT;
static int nscanMode = NSCAN_CHANGED;    // next scan comparison
static std::atomic<bool> vscanRunning{false};
static bool vscanDone = false;
static int vscanCount = 0;               // 0 = no scan yet, 1+ = scan #
static std::atomic<int64_t> vscanTotalScanned{0};
static std::atomic<int> vscanProgress{0};    // current assembly index
static std::atomic<int> vscanProgressMax{1}; // total assemblies
static float vscanFloatEpsilon = 0.01f;  // tolerance for float comparison
static const size_t VSCAN_MAX_RESULTS = 2000;
static std::string vscanAbortReason;     // why scan stopped early (if at all)
static void RunValueScan();
static void RunNextScan();

// --- Field Change Tracker (snapshot + diff) ---
struct FieldSnapshot {
	FieldInfo* field;
	std::string name;
	std::string value;
};
static std::vector<FieldSnapshot> snapshot;
static Il2CppObject* snapshotObj = nullptr;
static Il2CppClass* snapshotKlass = nullptr;
static bool hasSnapshot = false;
static std::map<FieldInfo*, std::string> snapshotMap; // for fast lookup

// Property snapshot (tracks property values alongside fields)
struct PropertySnapshot {
	const PropertyInfo* prop;
	std::string name;
	std::string value;
};
static std::vector<PropertySnapshot> propSnapshot;
static std::map<const PropertyInfo*, std::string> propSnapshotMap;

// Global search (declared early for Reset; struct defined before Render)
static char globalSearchBuf[128] = "";

// --- Deep capture snapshot for captured params ---
struct SnapNode {
	std::string label;       // field/element name
	std::string typeName;    // type string
	std::string value;       // display value for primitives/strings
	std::vector<uint8_t> rawBytes; // raw hex data for byte[]
	std::vector<SnapNode> children;
};
struct CapturedParamSnapshot {
	std::string paramName;
	SnapNode root;
};
static std::vector<CapturedParamSnapshot> capturedSnapshots;
static bool showCapturedSnapshotWindow = false;

// One-shot method param capture via INT3 breakpoint + VEH
struct CapturedArg {
	uint64_t regVal;   // integer register value
	uint64_t xmmLow;   // low 64 bits of XMM register (for floats)
};
struct OneShotCapture {
	const MethodInfo* method = nullptr;
	void* targetAddr = nullptr;
	uint8_t savedByte = 0;
	volatile bool active = false;
	volatile bool captured = false;
	CapturedArg args[20]; // up to 20 args (this + params + MethodInfo*)
	int totalArgCount = 0; // including 'this', excluding MethodInfo*
	Il2CppClass* filterClass = nullptr; // class filter — only capture when 'this' is this class
	bool isStatic = false;
	// Pre-computed array param info for immediate VEH-time snapshot
	struct ArrayParamInfo {
		int paramIdx = -1;    // 0-based param index (not counting 'this')
		int argIdx = -1;      // index into args[] (with 'this' offset)
		uint32_t elemSize = 1;
	};
	ArrayParamInfo arrayParams[8];
	int numArrayParams = 0;
	volatile bool pendingRearm = false; // single-step re-arm after class filter skip
};
static OneShotCapture g_capture = {};
static PVOID g_vehHandle = nullptr;
static void SetOneShotCapture(const MethodInfo* method, Il2CppClass* fromClass = nullptr);
static void CancelOneShotCapture();

// --- Captured buffer snapshots for array/byte[] params ---
// When a capture fires and a param is an array of byte-like elements,
// we copy up to 64KB into a static buffer so the data survives and can be edited + replayed.
static constexpr size_t CAPTURE_BUF_MAX = 65536; // 64KB per param
struct CapturedBuffer {
	bool valid = false;
	int paramIdx = -1;                    // which param this belongs to (0-based, not counting 'this')
	uint32_t elemSize = 0;                // element size in bytes
	uint32_t arrayLen = 0;                // element count in original array
	uint32_t byteLen = 0;                 // bytes copied (min of actual size, CAPTURE_BUF_MAX)
	uint8_t data[CAPTURE_BUF_MAX] = {};   // the editable snapshot
	void* originalArrayPtr = nullptr;     // the Il2CppArray* it came from (for reference)
	Il2CppArray* clonedArray = nullptr;   // GC-allocated clone for replay (has our edited data)
};
static constexpr int MAX_CAPTURE_BUFS = 8; // up to 8 array params
static CapturedBuffer g_capturedBufs[MAX_CAPTURE_BUFS];
static int g_capturedBufCount = 0;
static bool g_capturedBufDone = false; // true after snapshot attempt (even if no arrays found)

// Safe array snapshot for VEH context — separate function so __try/__except compiles
// (can't mix __try with C++ objects that have destructors in same function)
static bool TrySnapshotArray(uint64_t arrPtr, uint32_t elemSize, CapturedBuffer& cb, int paramIdx) {
	__try {
		Il2CppArray* arr = (Il2CppArray*)arrPtr;
		uint32_t arrLen = (uint32_t)arr->max_length;
		if (arrLen == 0 || arrLen > 0x1000000) return false; // sanity: max 16M elements
		uint32_t totalBytes = arrLen * elemSize;
		uint32_t copyBytes = (totalBytes > CAPTURE_BUF_MAX) ? (uint32_t)CAPTURE_BUF_MAX : totalBytes;
		void* srcData = (void*)((uintptr_t)arr + sizeof(Il2CppArray));
		memcpy(cb.data, srcData, copyBytes);
		cb.valid = true;
		cb.paramIdx = paramIdx;
		cb.elemSize = elemSize;
		cb.arrayLen = arrLen;
		cb.byteLen = copyBytes;
		cb.originalArrayPtr = (void*)arr;
		cb.clonedArray = nullptr;
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Scratch buffer for hook array snapshots — SEH-safe, copies into HookCapturedBuffer afterward
static uint8_t g_hookSnapScratch[CAPTURE_BUF_MAX];

// SEH-safe array snapshot to scratch buffer (no C++ objects = safe with __try/__except)
static bool TrySnapshotArrayRaw(uint64_t arrPtr, uint32_t elemSize,
	uint32_t& outArrLen, uint32_t& outByteLen) {
	__try {
		Il2CppArray* arr = (Il2CppArray*)arrPtr;
		uint32_t arrLen = (uint32_t)arr->max_length;
		if (arrLen == 0 || arrLen > 0x1000000) return false;
		uint32_t totalBytes = arrLen * elemSize;
		uint32_t copyBytes = (totalBytes > CAPTURE_BUF_MAX) ? (uint32_t)CAPTURE_BUF_MAX : totalBytes;
		void* srcData = (void*)((uintptr_t)arr + sizeof(Il2CppArray));
		memcpy(g_hookSnapScratch, srcData, copyBytes);
		outArrLen = arrLen;
		outByteLen = copyBytes;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// SEH-safe stack walk for VEH — no C++ objects, writes to plain arrays
static int CaptureStackWalkSafe(CONTEXT* exCtx, void* bpAddr, uintptr_t* outFrames, int maxFrames) {
	__try {
		CONTEXT ctxCopy = *exCtx;
		ctxCopy.Rip = (DWORD64)bpAddr;
		int count = 0;
		for (int sf = 0; sf < maxFrames; sf++) {
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION rtFunc = RtlLookupFunctionEntry(ctxCopy.Rip, &imageBase, NULL);
			if (!rtFunc) {
				if (ctxCopy.Rsp) {
					uintptr_t retAddr = *(uintptr_t*)ctxCopy.Rsp;
					if (retAddr) outFrames[count++] = retAddr;
				}
				break;
			}
			outFrames[count++] = (uintptr_t)ctxCopy.Rip;
			PVOID handlerData = nullptr;
			DWORD64 establisherFrame = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctxCopy.Rip, rtFunc,
				&ctxCopy, &handlerData, &establisherFrame, NULL);
			if (!ctxCopy.Rip) break;
		}
		return count;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// --- Multi-Hook System (persistent INT3 hooks with call logging + stack trace) ---

// Per-call array buffer snapshot (heap-allocated, sized to actual data)
struct HookCapturedBuffer {
	bool valid = false;
	int paramIdx = -1;
	uint32_t elemSize = 0;
	uint32_t arrayLen = 0;
	uint32_t byteLen = 0;
	std::vector<uint8_t> data;        // heap-allocated snapshot (up to CAPTURE_BUF_MAX)
	void* originalArrayPtr = nullptr;
	Il2CppArray* clonedArray = nullptr; // GC clone for replay (lazily created)
};

struct HookCallRecord {
	CapturedArg args[20];
	int totalArgCount = 0;
	uint64_t timestamp = 0;
	std::vector<uintptr_t> stack;
	std::vector<std::string> stackResolved;
	std::vector<CapturedParamSnapshot> snapshots; // deep snapshot at capture time
	std::vector<HookCapturedBuffer> buffers;      // array param snapshots for replay
};

struct HookEntry {
	uint32_t id = 0;
	const MethodInfo* method = nullptr;
	void* targetAddr = nullptr;
	uint8_t savedByte = 0;
	std::atomic<bool> paused{ false };
	bool isStatic = false;
	int paramCount = 0;
	int totalArgCount = 0;
	std::string displayName;
	Il2CppClass* filterClass = nullptr; // only record calls where this == filterClass or subclass
	// Pre-computed array param info for VEH-time snapshot
	struct ArrayParamInfo {
		int paramIdx = -1;
		int argIdx = -1;
		uint32_t elemSize = 1;
	};
	ArrayParamInfo arrayParams[8];
	int numArrayParams = 0;
	bool customPass = false; // "Custom Pass" — modify buffer before original call
	static constexpr size_t MAX_HOOK_LOG = 100;
	std::vector<HookCallRecord> callLog;
	std::atomic<uint64_t> totalCallCount{ 0 };
	std::mutex logMutex;
};

// Shared INT3 breakpoint state — one per unique address, shared by all hooks at that address
struct BreakpointSlot {
	void* addr = nullptr;
	uint8_t savedByte = 0;
	std::atomic<bool> armed{ false };
	std::atomic<bool> singleStepping{ false };
	DWORD singleStepThreadId = 0;
	std::vector<std::shared_ptr<HookEntry>> hooks; // all hooks at this address
};

static std::mutex g_hooksMutex;
static std::map<void*, std::shared_ptr<BreakpointSlot>> g_slots; // addr → breakpoint slot
static std::vector<std::shared_ptr<HookEntry>> g_hookList;        // ordered list for UI
static uint32_t g_nextHookId = 1;

static std::mutex g_singleStepMutex;
static std::map<DWORD, std::shared_ptr<BreakpointSlot>> g_pendingSingleStep;

// Stack trace resolution map (built lazily once)
static std::vector<std::pair<uintptr_t, const MethodInfo*>> g_addrToMethod;
static std::atomic<bool> g_addrMapBuilt{ false };
static std::mutex g_addrMapMutex;

// Custom Pass — deferred log from VEH (no file I/O in exception context)
struct CustomPassLogEntry {
	uint8_t original[64];
	uint8_t modified[64];
	int32_t msgId;
	int32_t len;
};
static std::mutex g_customPassLogMutex;
static std::vector<CustomPassLogEntry> g_customPassPendingLogs;

// Hooks tab UI state
static std::shared_ptr<HookEntry> g_selectedHook = nullptr;
static int g_selectedCallIdx = -1;
static HookCallRecord g_selectedCallRecord;  // stable copy of selected call
static bool g_hasSelectedCall = false;
static uint64_t g_selectedCallTimestamp = 0; // used to identify the selected call

static std::shared_ptr<HookEntry> AddPersistentHook(const MethodInfo* method, Il2CppClass* fromClass, void* overrideAddr = nullptr);
static void RemovePersistentHook(std::shared_ptr<HookEntry> hook);
static void TogglePauseHook(std::shared_ptr<HookEntry> hook);
static void RemoveAllHooks();
static void BuildAddrToMethodMap();
static std::string ResolveAddress(uintptr_t addr);
static void ResolveCallStack(HookCallRecord& rec);
static void DrawHookDetail(std::shared_ptr<HookEntry> hook);

// Forward declarations for new tools
static std::string ReadFieldValueAsString(Il2CppObject* obj, FieldInfo* field);
static std::string ReadPropertyValueAsString(Il2CppObject* obj, const PropertyInfo* prop);
static void ScanStaticFields();
static void DrawSnapNode(const SnapNode& node, int id);

void UnityExplorerTAB::Reset()
{
	cachedTree.clear();
	searchResults.clear();
	enumNameCache.clear();

	methodParamBuffers.clear();
	methodLastResults.clear();
	methodLastReturnObj.clear();
	refResults.clear();
	showRefResults = false;
	lastRefSearchClass = nullptr;
	memberSearchFilter[0] = '\0';
	CancelOneShotCapture();
	g_capturedBufCount = 0;
	g_capturedBufDone = false;
	for (int i = 0; i < MAX_CAPTURE_BUFS; i++) {
		g_capturedBufs[i].valid = false;
		g_capturedBufs[i].clonedArray = nullptr;
		g_capturedBufs[i].originalArrayPtr = nullptr;
	}
	RemoveAllHooks();
	g_selectedHook = nullptr;
	g_selectedCallIdx = -1;
	g_hasSelectedCall = false;

	watchList.clear();
	staticScanResults.clear();
	showStaticScan = false;
	staticScanFilter[0] = '\0';
	valueScanResults.clear();
	vscanValue1[0] = '\0';
	vscanValue2[0] = '\0';
	vscanType = VSCAN_INT;
	vscanMode = VMODE_EXACT;
	nscanMode = NSCAN_CHANGED;
	vscanRunning = false;
	vscanCount = 0;
	vscanDone = false;
	vscanTotalScanned = 0;
	snapshot.clear();
	snapshotMap.clear();
	propSnapshot.clear();
	propSnapshotMap.clear();
	snapshotObj = nullptr;
	snapshotKlass = nullptr;
	hasSnapshot = false;
	globalSearchBuf[0] = '\0';
	capturedSnapshots.clear();
	showCapturedSnapshotWindow = false;

	while (!inspectionHistory.empty()) inspectionHistory.pop();

	selectedGameObject = nullptr;
	isRawClassView = false;
	forceCastClass = nullptr;
	forceCastObj = nullptr;
	needsRefresh = true;

	memset(searchFilter, 0, sizeof(searchFilter));
	memset(classSearchFilter, 0, sizeof(classSearchFilter));
}

HierarchyNode BuildCache(Il2CppObject* go, int depth = 0) {

	if (depth > 50 || !Resolver::Protection::IsAlive(go)) return { nullptr, "Dead/Too Deep", false, {} };

	static Il2CppClass* goClass = Resolver::FindClass("UnityEngine", "GameObject");
	static Il2CppClass* trClass = Resolver::FindClass("UnityEngine", "Transform");

	static const MethodInfo* getName = il2cpp_class_get_method_from_name(goClass, "get_name", 0);
	static const MethodInfo* getActive = il2cpp_class_get_method_from_name(goClass, "get_activeInHierarchy", 0);
	static const MethodInfo* getTransform = il2cpp_class_get_method_from_name(goClass, "get_transform", 0);
	static const MethodInfo* getChildCount = il2cpp_class_get_method_from_name(trClass, "get_childCount", 0);
	static const MethodInfo* getChild = il2cpp_class_get_method_from_name(trClass, "GetChild", 1);
	static const MethodInfo* getGO = il2cpp_class_get_method_from_name(trClass, "get_gameObject", 0);

	HierarchyNode node;
	node.ptr = go;

	Il2CppString* nStr = (Il2CppString*)Resolver::Protection::SafeRuntimeInvoke(getName, go, nullptr);
	node.name = nStr ? il2cppi_to_string(nStr) : "Unnamed";

	Il2CppObject* resActive = Resolver::Protection::SafeRuntimeInvoke(getActive, go, nullptr);
	node.isActive = resActive ? *static_cast<bool*>(il2cpp_object_unbox(resActive)) : true;

	Il2CppObject* transform = Resolver::Protection::SafeRuntimeInvoke(getTransform, go, nullptr);
	if (transform) {
		Il2CppObject* resCount = Resolver::Protection::SafeRuntimeInvoke(getChildCount, transform, nullptr);
		int count = resCount ? *static_cast<int*>(il2cpp_object_unbox(resCount)) : 0;

		for (int i = 0; i < count; i++) {
			void* p[] = { &i };
			Il2CppObject* childTr = Resolver::Protection::SafeRuntimeInvoke(getChild, transform, p);
			if (childTr) {
				Il2CppObject* childGO = Resolver::Protection::SafeRuntimeInvoke(getGO, childTr, nullptr);
				if (childGO && Resolver::Protection::IsAlive(childGO)) {
					node.children.push_back(BuildCache(childGO, depth + 1));
				}
			}
		}
	}
	return node;
}

void DrawCachedNode(const HierarchyNode& node) {
	if (!node.isActive && !showInactive) return;

	if (strlen(searchFilter) > 0) {
		std::string nName = node.name;
		std::string sFilter = searchFilter;
		std::transform(nName.begin(), nName.end(), nName.begin(), ::tolower);
		std::transform(sFilter.begin(), sFilter.end(), sFilter.begin(), ::tolower);
		if (nName.find(sFilter) == std::string::npos && node.children.empty()) return;
	}

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
	if (selectedGameObject == node.ptr) flags |= ImGuiTreeNodeFlags_Selected;

	if (!node.isActive) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

	ImGui::PushID(node.ptr);
	bool opened = ImGui::TreeNodeEx("##node", flags, "%s", node.name.c_str());
	ImGui::PopID();

	if (ImGui::IsItemClicked()) {
		if (Resolver::Protection::IsAlive(node.ptr)) {
			UnityExplorerTAB::Helpers::InspectObject(node.ptr);
		}
		else {
			needsRefresh = true;
		}
	}

	if (opened) {
		for (const auto& child : node.children) {
			DrawCachedNode(child);
		}
		ImGui::TreePop();
	}

	if (!node.isActive) ImGui::PopStyleColor();
}

// Global search results
struct GlobalSearchResult {
	enum Type { CLASS, FIELD, METHOD };
	Type type;
	Il2CppClass* klass;
	std::string className;
	std::string memberName;  // empty for class results
	std::string typeName;    // field type or method return type
};
static std::vector<GlobalSearchResult> globalSearchResults;
static bool globalSearchActive = false;

static void DoGlobalSearch() {
	globalSearchResults.clear();
	globalSearchActive = true;
	std::string filter(globalSearchBuf);
	if (filter.empty()) { globalSearchActive = false; return; }
	std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

	auto domain = il2cpp_domain_get();
	size_t asmCount = 0;
	const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);
	if (!assemblies) return;

	const size_t MAX = 200;
	for (size_t a = 0; a < asmCount && globalSearchResults.size() < MAX; a++) {
		const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[a]);
		if (!image) continue;
		size_t classCount = il2cpp_image_get_class_count(image);
		for (size_t c = 0; c < classCount && globalSearchResults.size() < MAX; c++) {
			Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, c);
			if (!klass) continue;

			Resolver::Protection::safe_call([&]() {
				const char* cName = il2cpp_class_get_name(klass);
				const char* cNs = il2cpp_class_get_namespace(klass);
				std::string fullName = (cNs && cNs[0]) ? (std::string(cNs) + "." + cName) : cName;
				std::string lFull = fullName;
				std::transform(lFull.begin(), lFull.end(), lFull.begin(), ::tolower);

				// Match class name
				if (lFull.find(filter) != std::string::npos) {
					GlobalSearchResult r; r.type = GlobalSearchResult::CLASS; r.klass = klass; r.className = fullName;
					globalSearchResults.push_back(r);
				}

				// Match fields
				void* fIter = nullptr;
				while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
					if (globalSearchResults.size() >= MAX) break;
					const char* fn = il2cpp_field_get_name(f);
					if (!fn) continue;
					std::string lfn(fn);
					std::transform(lfn.begin(), lfn.end(), lfn.begin(), ::tolower);
					if (lfn.find(filter) != std::string::npos) {
						const Il2CppType* ft = il2cpp_field_get_type(f);
						char* tn = ft ? il2cpp_type_get_name(ft) : nullptr;
						GlobalSearchResult r; r.type = GlobalSearchResult::FIELD; r.klass = klass; r.className = fullName; r.memberName = fn; r.typeName = tn ? tn : "?";
						globalSearchResults.push_back(r);
						if (tn) il2cpp_free(tn);
					}
				}

				// Match methods (name or param names)
				void* mIter = nullptr;
				while (const MethodInfo* m = il2cpp_class_get_methods(klass, &mIter)) {
					if (globalSearchResults.size() >= MAX) break;
					const char* mn = m->name;
					if (!mn || strstr(mn, "get_") || strstr(mn, "set_") || strstr(mn, ".ctor")) continue;
					bool matched = false;
					std::string lmn(mn);
					std::transform(lmn.begin(), lmn.end(), lmn.begin(), ::tolower);
					if (lmn.find(filter) != std::string::npos) matched = true;
					// Also check param names and param type names
					if (!matched) {
						int pc = il2cpp_method_get_param_count(m);
						for (int pi = 0; pi < pc && !matched; pi++) {
							const char* pn = il2cpp_method_get_param_name(m, pi);
							if (pn) {
								std::string lpn(pn);
								std::transform(lpn.begin(), lpn.end(), lpn.begin(), ::tolower);
								if (lpn.find(filter) != std::string::npos) matched = true;
							}
						}
					}
					if (matched) {
						GlobalSearchResult r; r.type = GlobalSearchResult::METHOD; r.klass = klass; r.className = fullName; r.memberName = mn;
						globalSearchResults.push_back(r);
					}
				}
			});
		}
	}
}

void UnityExplorerTAB::Render() {
	il2cpp_thread_attach(il2cpp_domain_get());

	// Top bar: Refresh + Global Search
	if (ImGui::Button("Refresh")) needsRefresh = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60);
	bool searchEnter = ImGui::InputTextWithHint("##globalSearch", "Search all classes, fields, methods...", globalSearchBuf, 128, ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if (ImGui::Button("Go##gs") || searchEnter) DoGlobalSearch();

	// Global search results dropdown
	if (globalSearchActive && !globalSearchResults.empty()) {
		float panelH = (std::min)(200.0f, (float)globalSearchResults.size() * 20.0f + 10.0f);
		ImGui::BeginChild("GlobalResults", ImVec2(0, panelH), true);
		for (size_t i = 0; i < globalSearchResults.size(); i++) {
			auto& r = globalSearchResults[i];
			ImGui::PushID((int)i);
			if (r.type == GlobalSearchResult::CLASS) {
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[C]");
				ImGui::SameLine();
				if (ImGui::Selectable(r.className.c_str(), false)) {
					Helpers::InspectClass(r.klass);
					globalSearchActive = false;
				}
			}
			else if (r.type == GlobalSearchResult::FIELD) {
				ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "[F]");
				ImGui::SameLine();
				ImGui::TextDisabled("%s", r.typeName.c_str());
				ImGui::SameLine();
				char label[512];
				sprintf_s(label, "%s.%s", r.className.c_str(), r.memberName.c_str());
				if (ImGui::Selectable(label, false)) {
					Helpers::InspectClass(r.klass);
					// Pre-fill member filter so the field is visible
					strncpy_s(memberSearchFilter, r.memberName.c_str(), sizeof(memberSearchFilter) - 1);
					globalSearchActive = false;
				}
			}
			else {
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "[M]");
				ImGui::SameLine();
				char label[512];
				sprintf_s(label, "%s.%s()", r.className.c_str(), r.memberName.c_str());
				if (ImGui::Selectable(label, false)) {
					Helpers::InspectClass(r.klass);
					strncpy_s(memberSearchFilter, r.memberName.c_str(), sizeof(memberSearchFilter) - 1);
					globalSearchActive = false;
				}
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
	}
	else if (globalSearchActive && globalSearchResults.empty()) {
		ImGui::TextDisabled("No results found.");
		globalSearchActive = false;
	}

	// Mode tabs
	ImGui::Text("Mode:"); ImGui::SameLine();
	ImGui::RadioButton("Hierarchy", &explorerMode, 0); ImGui::SameLine();
	ImGui::RadioButton("Class Browser", &explorerMode, 1); ImGui::SameLine();
	ImGui::RadioButton("Statics", &explorerMode, 2); ImGui::SameLine();
	ImGui::RadioButton("Watchlist", &explorerMode, 3); ImGui::SameLine();
	ImGui::RadioButton("Value Scan", &explorerMode, 4); ImGui::SameLine();
	ImGui::RadioButton("Hooks", &explorerMode, 5);

	ImGui::Separator();

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 4.0f));
	ImGui::Columns(2, "UnityExplorerLayout", true);
	ImGui::PopStyleVar();

	ImGui::BeginChild("LeftPanelView");

	if (explorerMode == 0) {
		ImGui::Checkbox("Show Inactive", &showInactive);
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##hSearch", "Filter objects...", searchFilter, 128);
		ImGui::Separator();

		if (needsRefresh) {
			cachedTree.clear();
			static Il2CppClass* resClass = Resolver::FindClass("UnityEngine", "Resources");
			static Il2CppClass* goClass = Resolver::FindClass("UnityEngine", "GameObject");
			static const MethodInfo* findObjects = il2cpp_class_get_method_from_name(resClass, "FindObjectsOfTypeAll", 1);

			Il2CppReflectionType* goType = (Il2CppReflectionType*)il2cpp_type_get_object(il2cpp_class_get_type(goClass));
			void* params[] = { goType };
			Il2CppArray* all = (Il2CppArray*)Resolver::Protection::SafeRuntimeInvoke(findObjects, nullptr, params);

			if (all) {
				static const MethodInfo* getTransform = il2cpp_class_get_method_from_name(goClass, "get_transform", 0);
				static Il2CppClass* trClass = Resolver::FindClass("UnityEngine", "Transform");
				static const MethodInfo* getParent = il2cpp_class_get_method_from_name(trClass, "get_parent", 0);

				for (uint32_t i = 0; i < il2cpp_array_length(all); i++) {
					Il2CppObject* go = GET_ARRAY_ELEMENT(all, i);
					if (!Resolver::Protection::IsAlive(go)) continue;

					Il2CppObject* tr = Resolver::Protection::SafeRuntimeInvoke(getTransform, go, nullptr);
					Il2CppObject* parent = tr ? Resolver::Protection::SafeRuntimeInvoke(getParent, tr, nullptr) : nullptr;

					if (!parent) {
						std::string sName = Resolver::Helpers::GetSceneName(go);
						cachedTree[sName].push_back(BuildCache(go));
					}
				}
			}
			needsRefresh = false;
		}

		for (auto const& [sceneName, nodes] : cachedTree) {
			if (ImGui::TreeNodeEx(sceneName.c_str(), ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
				for (const auto& node : nodes) DrawCachedNode(node);
				ImGui::TreePop();
			}
		}
	}
	else if (explorerMode == 1) {
		ImGui::SetNextItemWidth(-50);
		ImGui::InputTextWithHint("##cSearch", "Search Assembly...", classSearchFilter, 128);
		ImGui::SameLine();

		if (ImGui::Button("Go")) {
			searchResults.clear();
			std::string filter = classSearchFilter;
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			auto domain = il2cpp_domain_get();
			size_t assembly_count = 0;
			const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &assembly_count);

			if (assemblies) {
				for (size_t i = 0; i < assembly_count; ++i) {
					const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
					if (!image) continue;

					size_t class_count = il2cpp_image_get_class_count(image);
					for (size_t j = 0; j < class_count; ++j) {
						Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, j);
						if (!klass) continue;

						std::string name = il2cpp_class_get_name(klass);
						std::string lowName = name;
						std::transform(lowName.begin(), lowName.end(), lowName.begin(), ::tolower);

						if (lowName.find(filter) != std::string::npos) {
							searchResults.push_back(klass);
						}
					}
				}
			}
		}

		ImGui::Separator();
		for (auto klass : searchResults) {
			std::string name = il2cpp_class_get_name(klass);
			std::string ns = il2cpp_class_get_namespace(klass);
			if (ImGui::Selectable((ns + "::" + name).c_str())) {
				Helpers::InspectClass(klass);
			}
		}
	}
	else if (explorerMode == 2) {
		// --- Static Field Scanner ---
		ImGui::SetNextItemWidth(-60);
		ImGui::InputTextWithHint("##staticFilter", "Filter statics...", staticScanFilter, 128);
		ImGui::SameLine();
		if (ImGui::Button("Scan")) ScanStaticFields();

		if (showStaticScan) {
			ImGui::Text("%d non-null statics found.", (int)staticScanResults.size());
			ImGui::Separator();
			for (size_t i = 0; i < staticScanResults.size(); i++) {
				auto& r = staticScanResults[i];
				ImGui::PushID((int)i);
				ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", r.typeName.c_str());
				ImGui::SameLine();
				char label[512];
				sprintf_s(label, "%s.%s", r.className.c_str(), r.fieldName.c_str());
				if (ImGui::Selectable(label, false)) {
					// Read the value and inspect it
					Il2CppObject* val = nullptr;
					Resolver::Protection::safe_call([&]() {
						il2cpp_field_static_get_value(r.field, &val);
					});
					if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
						Helpers::InspectObject(val);
					}
					else {
						Helpers::InspectClass(r.klass);
					}
				}
				ImGui::PopID();
			}
		}
	}
	else if (explorerMode == 3) {
		// --- Watchlist ---
		if (watchList.empty()) {
			ImGui::TextDisabled("No watched fields.");
			ImGui::TextDisabled("Pin fields with +W in the inspector.");
		}
		else {
			ImGui::Text("Watching %d field(s)", (int)watchList.size());
			if (ImGui::Button("Clear All")) watchList.clear();
			ImGui::Separator();

			int removeIdx = -1;
			for (size_t i = 0; i < watchList.size(); i++) {
				auto& w = watchList[i];
				ImGui::PushID((int)i);

				// Remove button
				if (ImGui::SmallButton("X")) removeIdx = (int)i;
				ImGui::SameLine();

				// Read current value
				std::string val = ReadFieldValueAsString(w.obj, w.field);
				ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", w.label.c_str());
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "= %s", val.c_str());

				ImGui::PopID();
			}
			if (removeIdx >= 0) watchList.erase(watchList.begin() + removeIdx);
		}
	}
	else if (explorerMode == 4) {
		// --- Value Scanner (Cheat Engine-like) ---
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Value Scanner");
		if (vscanCount > 0) {
			ImGui::SameLine();
			ImGui::TextDisabled("(Scan #%d, %d results)", vscanCount, (int)valueScanResults.size());
		}
		ImGui::Separator();

		// Type selector (locked after first scan — must New Scan to change)
		if (vscanCount == 0) {
			ImGui::Text("Type:"); ImGui::SameLine();
			ImGui::RadioButton("Int##vt", &vscanType, VSCAN_INT); ImGui::SameLine();
			ImGui::RadioButton("Float##vt", &vscanType, VSCAN_FLOAT); ImGui::SameLine();
			ImGui::RadioButton("Bool##vt", &vscanType, VSCAN_BOOL); ImGui::SameLine();
			ImGui::RadioButton("String##vt", &vscanType, VSCAN_STRING);
		}
		else {
			const char* typeNames[] = { "Int", "Float", "Bool", "String" };
			ImGui::TextDisabled("Type: %s (locked)", typeNames[vscanType]);
		}

		// First scan: value comparison mode
		if (vscanCount == 0) {
			ImGui::Text("Mode:"); ImGui::SameLine();
			ImGui::RadioButton("==##vm", &vscanMode, VMODE_EXACT); ImGui::SameLine();
			ImGui::RadioButton("!=##vm", &vscanMode, VMODE_NOT_EQUAL); ImGui::SameLine();
			if (vscanType == VSCAN_STRING) {
				ImGui::RadioButton("Contains##vm", &vscanMode, VMODE_CONTAINS);
			}
			else if (vscanType != VSCAN_BOOL) {
				ImGui::RadioButton(">##vm", &vscanMode, VMODE_GREATER); ImGui::SameLine();
				ImGui::RadioButton("<##vm", &vscanMode, VMODE_LESS); ImGui::SameLine();
				ImGui::RadioButton("A..B##vm", &vscanMode, VMODE_BETWEEN);
			}
		}
		else {
			// Next scan: comparison mode against previous values
			ImGui::Text("Compare:"); ImGui::SameLine();
			ImGui::RadioButton("Exact##ns", &nscanMode, NSCAN_EXACT); ImGui::SameLine();
			ImGui::RadioButton("Changed##ns", &nscanMode, NSCAN_CHANGED); ImGui::SameLine();
			ImGui::RadioButton("Unchanged##ns", &nscanMode, NSCAN_UNCHANGED);
			if (vscanType == VSCAN_INT || vscanType == VSCAN_FLOAT) {
				ImGui::SameLine();
				ImGui::RadioButton("Increased##ns", &nscanMode, NSCAN_INCREASED); ImGui::SameLine();
				ImGui::RadioButton("Decreased##ns", &nscanMode, NSCAN_DECREASED);
			}
			if (vscanType == VSCAN_STRING) {
				ImGui::SameLine();
				ImGui::RadioButton("Contains##ns", &nscanMode, NSCAN_CONTAINS);
			}
		}

		// Value input (shown for first scan always, for next scan only on Exact mode)
		bool showValueInput = (vscanCount == 0) || (nscanMode == NSCAN_EXACT) || (nscanMode == NSCAN_CONTAINS);
		if (showValueInput) {
			if (vscanType == VSCAN_BOOL) {
				ImGui::Text("Value:"); ImGui::SameLine();
				static int boolChoice = 1;
				ImGui::RadioButton("true##vb", &boolChoice, 1); ImGui::SameLine();
				ImGui::RadioButton("false##vb", &boolChoice, 0);
				strcpy_s(vscanValue1, boolChoice ? "true" : "false");
			}
			else {
				const char* hint1 = (vscanType == VSCAN_STRING) ? "Search text..." :
					(vscanCount == 0 && vscanMode == VMODE_BETWEEN) ? "Min value..." : "Value...";
				ImGui::SetNextItemWidth((vscanCount == 0 && vscanMode == VMODE_BETWEEN) ? ImGui::GetContentRegionAvail().x * 0.45f : -1);
				ImGui::InputTextWithHint("##vsval1", hint1, vscanValue1, sizeof(vscanValue1));
				if (vscanCount == 0 && vscanMode == VMODE_BETWEEN) {
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1);
					ImGui::InputTextWithHint("##vsval2", "Max value...", vscanValue2, sizeof(vscanValue2));
				}
			}
		}

		// Float epsilon
		if (vscanType == VSCAN_FLOAT) {
			ImGui::SetNextItemWidth(100);
			ImGui::DragFloat("Tolerance", &vscanFloatEpsilon, 0.001f, 0.0001f, 10.0f, "%.4f");
		}

		// Scan buttons (disabled while running)
		bool scanning = vscanRunning.load();
		float btnWidth = (vscanCount > 0 && !scanning) ? ImGui::GetContentRegionAvail().x * 0.5f - 2 : -1;

		if (scanning) {
			// Progress bar while scanning
			float progress = (vscanProgressMax > 0) ? (float)vscanProgress.load() / (float)vscanProgressMax.load() : 0.0f;
			ImGui::ProgressBar(progress, ImVec2(-1, 24));
			ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scanning... %lld fields checked", vscanTotalScanned.load());
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
			if (ImGui::Button("Cancel##vscan", ImVec2(-1, 22))) vscanRunning = false;
			ImGui::PopStyleColor();
		}
		else {
			if (vscanCount > 0) {
				// New Scan (reset)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
				if (ImGui::Button("New Scan", ImVec2(btnWidth, 28))) {
					valueScanResults.clear();
					vscanDone = false;
					vscanCount = 0;
					vscanTotalScanned = 0;
				}
				ImGui::PopStyleColor();
				ImGui::SameLine();
			}

			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
			const char* scanLabel = (vscanCount == 0) ? "First Scan" : "Next Scan";
			if (ImGui::Button(scanLabel, ImVec2(-1, 28))) {
				// Launch scan on background thread
				if (vscanCount == 0) {
					valueScanResults.clear();
					vscanTotalScanned = 0;
					vscanDone = false;
					vscanRunning = true;
					std::thread([]() { RunValueScan(); }).detach();
				}
				else {
					vscanDone = false;
					vscanRunning = true;
					std::thread([]() { RunNextScan(); }).detach();
				}
			}
			ImGui::PopStyleColor();
		}

		// Results
		ImGui::Separator();
		if (vscanDone && !vscanRunning.load()) {
			ImGui::Text("%d results", (int)valueScanResults.size());
			if (vscanCount == 1) {
				ImGui::SameLine();
				ImGui::TextDisabled("(%lld scanned)", vscanTotalScanned.load());
			}
			if (valueScanResults.size() >= VSCAN_MAX_RESULTS) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1, 1, 0, 1), "(capped)");
			}
			if (!vscanAbortReason.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "(%s)", vscanAbortReason.c_str());
			}

			if (!valueScanResults.empty()) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Copy All##vs")) {
					std::string dump = "=== Value Scan Results (Scan #" + std::to_string(vscanCount) + ") ===\n";
					for (auto& r : valueScanResults) {
						dump += r.path + " = " + r.valueStr;
						if (!r.prevStr.empty()) dump += " (was: " + r.prevStr + ")";
						dump += "\n";
					}
					Resolver::Helpers::CopyToClipboard(dump.c_str());
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear##vs")) {
					valueScanResults.clear();
					vscanDone = false;
					vscanCount = 0;
				}
			}

			ImGui::Separator();
			ImGui::BeginChild("VScanResults");
			// Clipper for fast rendering of many results
			ImGuiListClipper clipper;
			clipper.Begin((int)valueScanResults.size());
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
					auto& r = valueScanResults[i];
					ImGui::PushID(i);

					// Value column (current value)
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "%s", r.valueStr.c_str());

					// Show previous value if we have one (from next scan)
					if (!r.prevStr.empty() && r.prevStr != r.valueStr) {
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(was %s)", r.prevStr.c_str());
					}
					ImGui::SameLine();

					// Copy button
					if (ImGui::SmallButton("Cp")) {
						std::string v = r.valueStr;
						if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
							v = v.substr(1, v.size() - 2);
						Resolver::Helpers::CopyToClipboard(v.c_str());
					}
					ImGui::SameLine();

					// Navigate: inspect the owning object
					if (r.obj && Resolver::Protection::IsValidIl2CppObject(r.obj)) {
						if (ImGui::SmallButton("Go")) {
							Helpers::InspectObject(r.obj);
							// Pre-fill member filter with field name
							if (r.field) {
								const char* fn = il2cpp_field_get_name(r.field);
								if (fn) strncpy_s(memberSearchFilter, fn, sizeof(memberSearchFilter) - 1);
							}
						}
						ImGui::SameLine();
					}

					// Path
					ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", r.path.c_str());

					ImGui::PopID();
				}
			}
			clipper.End();
			ImGui::EndChild();
		}
		else if (vscanRunning.load()) {
			// Progress shown above in button area
		}
		else {
			ImGui::TextDisabled("Enter a value and click Scan.");
			ImGui::TextDisabled("Scans all static roots -> objects -> arrays -> collections.");
		}
	}
	else if (explorerMode == 5) {
		// --- Hooks Manager ---
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Method Hooks");
		ImGui::SameLine();
		ImGui::TextDisabled("(%d active)", (int)g_hookList.size());
		ImGui::Separator();

		// Toolbar
		bool noHooks = g_hookList.empty();
		if (noHooks) ImGui::BeginDisabled();
		if (ImGui::Button("Remove All")) RemoveAllHooks();
		ImGui::SameLine();
		if (ImGui::Button("Pause All")) {
			for (auto& h : g_hookList) if (!h->paused.load()) TogglePauseHook(h);
		}
		ImGui::SameLine();
		if (ImGui::Button("Resume All")) {
			for (auto& h : g_hookList) if (h->paused.load()) TogglePauseHook(h);
		}
		if (noHooks) ImGui::EndDisabled();

		ImGui::Separator();

		if (noHooks) {
			ImGui::TextDisabled("No hooks active.");
			ImGui::TextDisabled("Use the 'Hook' button on any method in the inspector.");
		}

		// Hook list
		int removeIdx = -1;
		for (size_t i = 0; i < g_hookList.size(); i++) {
			auto& h = g_hookList[i];
			ImGui::PushID((int)i);

			// Status indicator
			bool hp = h->paused.load();
			if (hp)
				ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[P]");
			else
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[A]");
			ImGui::SameLine();

			// Selectable hook name
			bool selected = (g_selectedHook == h);
			char label[512];
			sprintf_s(label, "%s (%llu calls)##hook%d",
				h->displayName.c_str(), h->totalCallCount.load(), h->id);
			if (ImGui::Selectable(label, selected, 0, ImVec2(ImGui::GetContentRegionAvail().x - 90, 0))) {
				g_selectedHook = h;
				// Auto-select latest call and copy it
				std::lock_guard<std::mutex> lock(h->logMutex);
				if (!h->callLog.empty()) {
					g_selectedCallIdx = (int)h->callLog.size() - 1;
					g_selectedCallRecord = h->callLog.back();
					g_selectedCallTimestamp = g_selectedCallRecord.timestamp;
					g_hasSelectedCall = true;
				} else {
					g_selectedCallIdx = -1;
					g_hasSelectedCall = false;
				}
			}

			// Pause/Remove on same line
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
			if (ImGui::SmallButton(hp ? "Resume" : "Pause")) TogglePauseHook(h);
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
			if (ImGui::SmallButton("X")) removeIdx = (int)i;
			ImGui::PopStyleColor();

			ImGui::PopID();
		}
		if (removeIdx >= 0 && removeIdx < (int)g_hookList.size())
			RemovePersistentHook(g_hookList[removeIdx]);
	}
	ImGui::EndChild();

	ImGui::NextColumn();

	ImGui::BeginChild("InspectorView");
	if (explorerMode == 5 && g_selectedHook) {
		DrawHookDetail(g_selectedHook);
	}
	else {
		Helpers::DrawInspector();
	}
	ImGui::EndChild();

	ImGui::Columns(1);

	// --- Captured Param Snapshot Window ---
	if (showCapturedSnapshotWindow && !capturedSnapshots.empty()) {
		ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Captured Snapshot", &showCapturedSnapshotWindow)) {
			if (ImGui::Button("Copy All")) {
				std::string dump = "=== Captured Param Snapshot ===\n";
				// Simple text dump (flat)
				std::function<void(const SnapNode&, int)> dumpNode = [&](const SnapNode& n, int indent) {
					std::string pad(indent * 2, ' ');
					if (!n.rawBytes.empty()) {
						dump += pad + n.label + " (" + n.typeName + ") = " + n.value + " [hex data]\n";
					}
					else {
						dump += pad + n.label + " (" + n.typeName + ") = " + n.value + "\n";
					}
					for (auto& c : n.children) dumpNode(c, indent + 1);
				};
				for (auto& ps : capturedSnapshots) dumpNode(ps.root, 0);
				Resolver::Helpers::CopyToClipboard(dump.c_str());
			}
			ImGui::SameLine();
			if (ImGui::Button("Close")) showCapturedSnapshotWindow = false;
			ImGui::Separator();

			for (size_t si = 0; si < capturedSnapshots.size(); si++) {
				DrawSnapNode(capturedSnapshots[si].root, (int)(si + 80000));
			}
		}
		ImGui::End();
	}
}

// --- Collection Instance Search ---
static void CollSearchBuildFields(Il2CppClass* elemClass); // forward decl
static void CollSearchDetectCollections(Il2CppClass* klass) {
	g_collSearchCollections.clear();
	g_collSearchFields.clear();
	g_collSearchClass = klass;
	g_collSearchCollIdx = 0;
	g_collSearchFieldIdx = 0;
	g_collSearchResults.clear();
	g_collSearchDone = false;

	Resolver::Protection::safe_call([&]() {
		il2cpp_runtime_class_init(klass);

		// Scan static fields for arrays/lists
		void* fIter = nullptr;
		while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
			const Il2CppType* fType = il2cpp_field_get_type(f);
			if (!fType) continue;
			if (!(fType->attrs & 0x0010)) continue; // static only
			const char* fname = il2cpp_field_get_name(f);
			if (!fname) continue;

			if (fType->type == IL2CPP_TYPE_SZARRAY) {
				// Static array field
				Il2CppClass* fClass = il2cpp_class_from_type(fType);
				Il2CppClass* elemClass = fClass ? il2cpp_class_get_element_class(fClass) : nullptr;
				if (elemClass && !il2cpp_class_is_valuetype(elemClass)) {
					CollSearchCollectionInfo ci;
					ci.field = f;
					ci.name = fname;
					ci.isList = false;
					ci.elemClass = elemClass;
					g_collSearchCollections.push_back(std::move(ci));
				}
			}
			else if (fType->type == IL2CPP_TYPE_GENERICINST) {
				// Could be List<T> — check for get_Item + get_Count
				Il2CppObject* val = nullptr;
				il2cpp_field_static_get_value(f, &val);
				if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
					Il2CppClass* valClass = il2cpp_object_get_class(val);
					if (!valClass) continue;
					const MethodInfo* getCount = il2cpp_class_get_method_from_name(valClass, "get_Count", 0);
					const MethodInfo* getItem = il2cpp_class_get_method_from_name(valClass, "get_Item", 1);
					if (getCount && getItem) {
						// Determine element type from get_Item return type
						const Il2CppType* retType = il2cpp_method_get_return_type(getItem);
						Il2CppClass* elemClass = retType ? il2cpp_class_from_type(retType) : nullptr;
						if (elemClass && !il2cpp_class_is_valuetype(elemClass)) {
							CollSearchCollectionInfo ci;
							ci.field = f;
							ci.name = fname;
							ci.isList = true;
							ci.elemClass = elemClass;
							g_collSearchCollections.push_back(std::move(ci));
						}
					}
				}
			}
		}

		// Build searchable fields from element class of first collection
		Il2CppClass* fieldSource = klass;
		if (!g_collSearchCollections.empty())
			fieldSource = g_collSearchCollections[0].elemClass;

		CollSearchBuildFields(fieldSource);
	});
}

// Shared helper: populate g_collSearchFields from a class (fields + properties)
static void CollSearchBuildFields(Il2CppClass* elemClass) {
	g_collSearchFields.clear();
	if (!elemClass) return;

	auto isSearchableType = [](int te) {
		return te == IL2CPP_TYPE_STRING || te == IL2CPP_TYPE_I4 || te == IL2CPP_TYPE_U4 ||
			te == IL2CPP_TYPE_I8 || te == IL2CPP_TYPE_U8 || te == IL2CPP_TYPE_R4 ||
			te == IL2CPP_TYPE_R8 || te == IL2CPP_TYPE_BOOLEAN || te == IL2CPP_TYPE_I2 ||
			te == IL2CPP_TYPE_U2 || te == IL2CPP_TYPE_I1 || te == IL2CPP_TYPE_U1 ||
			te == IL2CPP_TYPE_VALUETYPE;
	};

	std::unordered_set<std::string> seen; // avoid duplicates between fields and properties

	Resolver::Protection::safe_call([&]() {
		Il2CppClass* stopClass = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Object");
		Il2CppClass* cur = elemClass;

		while (cur && cur != stopClass) {
			// Properties first — these trigger lazy loading
			void* pIter = nullptr;
			while (const PropertyInfo* p = il2cpp_class_get_properties(cur, &pIter)) {
				if (!p->get) continue; // needs a getter
				if (!p->name) continue;
				// Skip static properties
				if (p->get->flags & 0x0010) continue;
				// Skip indexed properties (get_Item etc)
				if (il2cpp_method_get_param_count(p->get) > 0) continue;

				const Il2CppType* retType = p->get->return_type;
				if (!retType) continue;
				int te = retType->type;
				if (!isSearchableType(te)) continue;

				std::string pname(p->name);
				if (seen.count(pname)) continue;
				seen.insert(pname);

				CollSearchFieldInfo fi;
				fi.getter = p->get;
				fi.isProp = true;
				fi.name = pname;
				Il2CppClass* ftClass = il2cpp_class_from_type(retType);
				fi.typeName = ftClass ? il2cpp_class_get_name(ftClass) : "?";
				fi.typeEnum = te;
				g_collSearchFields.push_back(std::move(fi));
			}

			// Raw fields (only those not already covered by a property)
			void* fIter = nullptr;
			while (FieldInfo* f = il2cpp_class_get_fields(cur, &fIter)) {
				const Il2CppType* fType = il2cpp_field_get_type(f);
				if (!fType) continue;
				if (fType->attrs & 0x0010) continue; // skip static
				const char* fname = il2cpp_field_get_name(f);
				if (!fname) continue;
				int te = fType->type;
				if (!isSearchableType(te)) continue;

				std::string fn(fname);
				// Skip compiler-generated backing fields if we already have the property
				// e.g. "<Name>k__BackingField" → property "Name" already listed
				if (fn.size() > 2 && fn[0] == '<') continue;
				if (seen.count(fn)) continue;
				seen.insert(fn);

				CollSearchFieldInfo fi;
				fi.field = f;
				fi.name = fn;
				Il2CppClass* ftClass = il2cpp_class_from_type(fType);
				fi.typeName = ftClass ? il2cpp_class_get_name(ftClass) : "?";
				fi.typeEnum = te;
				g_collSearchFields.push_back(std::move(fi));
			}

			cur = il2cpp_class_get_parent(cur);
		}
	});
}

static std::string CollSearchReadValue(Il2CppObject* obj, const CollSearchFieldInfo& fi) {
	std::string result;
	Resolver::Protection::safe_call([&]() {
		if (fi.isProp && fi.getter) {
			// Property: invoke getter — triggers lazy loading / deserialization
			Il2CppObject* ret = Resolver::Protection::SafeRuntimeInvoke(fi.getter, obj, nullptr);
			if (!ret) return;
			if (fi.typeEnum == IL2CPP_TYPE_STRING) {
				result = il2cppi_to_string((Il2CppString*)ret);
				return;
			}
			// Value types come back boxed from runtime_invoke
			void* raw = il2cpp_object_unbox(ret);
			if (!raw) return;
			switch (fi.typeEnum) {
			case IL2CPP_TYPE_BOOLEAN: result = (*(bool*)raw) ? "true" : "false"; break;
			case IL2CPP_TYPE_I4: result = std::to_string(*(int32_t*)raw); break;
			case IL2CPP_TYPE_U4: result = std::to_string(*(uint32_t*)raw); break;
			case IL2CPP_TYPE_I8: result = std::to_string(*(int64_t*)raw); break;
			case IL2CPP_TYPE_U8: result = std::to_string(*(uint64_t*)raw); break;
			case IL2CPP_TYPE_R4: { char b[32]; sprintf_s(b, "%.4f", *(float*)raw); result = b; break; }
			case IL2CPP_TYPE_R8: { char b[32]; sprintf_s(b, "%.6f", *(double*)raw); result = b; break; }
			case IL2CPP_TYPE_I1: result = std::to_string(*(int8_t*)raw); break;
			case IL2CPP_TYPE_U1: result = std::to_string(*(uint8_t*)raw); break;
			case IL2CPP_TYPE_I2: result = std::to_string(*(int16_t*)raw); break;
			case IL2CPP_TYPE_U2: result = std::to_string(*(uint16_t*)raw); break;
			case IL2CPP_TYPE_VALUETYPE: result = std::to_string(*(int32_t*)raw); break;
			default: break;
			}
		}
		else if (fi.field) {
			// Raw field read
			switch (fi.typeEnum) {
			case IL2CPP_TYPE_STRING: {
				Il2CppString* s = nullptr;
				il2cpp_field_get_value(obj, fi.field, &s);
				if (s) result = il2cppi_to_string(s);
				break;
			}
			case IL2CPP_TYPE_BOOLEAN: { bool v = false; il2cpp_field_get_value(obj, fi.field, &v); result = v ? "true" : "false"; break; }
			case IL2CPP_TYPE_I4: { int32_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_U4: { uint32_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_I8: { int64_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_U8: { uint64_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_R4: { float v = 0; il2cpp_field_get_value(obj, fi.field, &v); char b[32]; sprintf_s(b, "%.4f", v); result = b; break; }
			case IL2CPP_TYPE_R8: { double v = 0; il2cpp_field_get_value(obj, fi.field, &v); char b[32]; sprintf_s(b, "%.6f", v); result = b; break; }
			case IL2CPP_TYPE_I1: { int8_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_U1: { uint8_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_I2: { int16_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_U2: { uint16_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v); break; }
			case IL2CPP_TYPE_VALUETYPE: {
				const Il2CppType* fType = il2cpp_field_get_type(fi.field);
				Il2CppClass* fClass = fType ? il2cpp_class_from_type(fType) : nullptr;
				if (fClass && il2cpp_class_is_enum(fClass)) {
					int32_t v = 0; il2cpp_field_get_value(obj, fi.field, &v); result = std::to_string(v);
				}
				break;
			}
			default: break;
			}
		}
	});
	return result;
}

static void CollSearchExecute() {
	g_collSearchResults.clear();
	g_collSearchDone = true;

	if (g_collSearchCollIdx < 0 || g_collSearchCollIdx >= (int)g_collSearchCollections.size()) return;
	if (g_collSearchFieldIdx < 0 || g_collSearchFieldIdx >= (int)g_collSearchFields.size()) return;

	auto& coll = g_collSearchCollections[g_collSearchCollIdx];
	auto& searchField = g_collSearchFields[g_collSearchFieldIdx];
	std::string target(g_collSearchValue);

	// Pre-lowercase target for case-insensitive matching
	std::string targetLower = target;
	for (auto& c : targetLower) c = (char)tolower((unsigned char)c);

	Resolver::Protection::safe_call([&]() {
		std::vector<Il2CppObject*> elements;

		if (!coll.isList) {
			// SZARRAY
			Il2CppArray* arr = nullptr;
			il2cpp_field_static_get_value(coll.field, &arr);
			if (!arr) return;
			uint32_t len = il2cpp_array_length(arr);
			uint32_t cap = len > 10000 ? 10000 : len;
			elements.reserve(cap);
			for (uint32_t i = 0; i < cap; i++) {
				Il2CppObject* elem = GET_ARRAY_ELEMENT(arr, i);
				if (elem) elements.push_back(elem);
			}
		}
		else {
			// List<T>
			Il2CppObject* listObj = nullptr;
			il2cpp_field_static_get_value(coll.field, &listObj);
			if (!listObj || !Resolver::Protection::IsValidIl2CppObject(listObj)) return;
			Il2CppClass* listClass = il2cpp_object_get_class(listObj);
			const MethodInfo* getCount = il2cpp_class_get_method_from_name(listClass, "get_Count", 0);
			const MethodInfo* getItem = il2cpp_class_get_method_from_name(listClass, "get_Item", 1);
			if (!getCount || !getItem) return;

			Il2CppObject* countObj = Resolver::Protection::SafeRuntimeInvoke(getCount, listObj, nullptr);
			if (!countObj) return;
			int count = *(int*)il2cpp_object_unbox(countObj);
			int cap = count > 10000 ? 10000 : count;
			elements.reserve(cap);

			Il2CppClass* int32Class = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Int32");
			for (int i = 0; i < cap; i++) {
				Il2CppObject* boxedIdx = il2cpp_value_box(int32Class, &i);
				void* params[] = { boxedIdx };
				Il2CppObject* elem = Resolver::Protection::SafeRuntimeInvoke(getItem, listObj, params);
				if (elem && Resolver::Protection::IsValidIl2CppObject(elem))
					elements.push_back(elem);
			}
		}

		// Warm up: invoke all 0-param instance property getters to trigger lazy loading
		// This mimics what "Inspect" does — many Unity objects lazily populate fields
		{
			std::vector<const MethodInfo*> warmUpGetters;
			Il2CppClass* walkClass = coll.elemClass;
			while (walkClass) {
				void* propIter = nullptr;
				while (const PropertyInfo* prop = il2cpp_class_get_properties(walkClass, &propIter)) {
					const MethodInfo* getter = il2cpp_property_get_get_method((PropertyInfo*)prop);
					if (!getter) continue;
					if (il2cpp_method_get_param_count(getter) != 0) continue;
					if (getter->flags & 0x0010) continue; // skip static
					warmUpGetters.push_back(getter);
				}
				walkClass = il2cpp_class_get_parent(walkClass);
				if (walkClass && std::string(il2cpp_class_get_name(walkClass)) == "Object"
					&& std::string(il2cpp_class_get_namespace(walkClass)) == "System")
					break;
			}

			for (auto* elem : elements) {
				for (auto* getter : warmUpGetters) {
					Resolver::Protection::SafeRuntimeInvoke(getter, elem, nullptr);
				}
			}
		}

		// Search through elements
		for (auto* elem : elements) {
			std::string fieldVal = CollSearchReadValue(elem, searchField);
			if (fieldVal.empty() && !targetLower.empty()) continue;

			// Match
			bool match = false;
			if (targetLower.empty()) {
				match = true; // show all if empty search
			}
			else {
				std::string valLower = fieldVal;
				for (auto& c : valLower) c = (char)tolower((unsigned char)c);
				if (g_collSearchMode == 0) // contains
					match = valLower.find(targetLower) != std::string::npos;
				else // exact
					match = valLower == targetLower;
			}

			if (match) {
				CollSearchResult r;
				r.obj = elem;
				r.preview = searchField.name + "=" + fieldVal;
				g_collSearchResults.push_back(std::move(r));
				if (g_collSearchResults.size() >= 500) break; // cap results
			}
		}
	});
}

static void DrawCollectionSearch(Il2CppClass* klass) {
	if (!ImGui::CollapsingHeader("Search Instances")) return;

	ImGui::Indent();

	// Detect collections when class changes
	static int lastCollIdx = -1;
	if (klass != g_collSearchClass) {
		CollSearchDetectCollections(klass);
		lastCollIdx = -1;
	}

	if (g_collSearchCollections.empty()) {
		ImGui::TextDisabled("No static array/list collections found in this class.");
		// Allow re-scan with a button (in case class wasn't initialized)
		if (ImGui::SmallButton("Rescan")) CollSearchDetectCollections(klass);
		ImGui::Unindent();
		return;
	}

	// Update fields list when collection selection changes
	if (lastCollIdx != g_collSearchCollIdx && g_collSearchCollIdx >= 0 &&
		g_collSearchCollIdx < (int)g_collSearchCollections.size()) {
		lastCollIdx = g_collSearchCollIdx;
		g_collSearchFieldIdx = 0;
		CollSearchBuildFields(g_collSearchCollections[g_collSearchCollIdx].elemClass);
	}

	// Collection dropdown
	{
		std::string collPreview = g_collSearchCollIdx < (int)g_collSearchCollections.size()
			? g_collSearchCollections[g_collSearchCollIdx].name
			+ (g_collSearchCollections[g_collSearchCollIdx].isList ? " (List)" : " (Array)")
			: "Select...";
		ImGui::SetNextItemWidth(200);
		if (ImGui::BeginCombo("Collection", collPreview.c_str())) {
			for (int i = 0; i < (int)g_collSearchCollections.size(); i++) {
				auto& ci = g_collSearchCollections[i];
				std::string label = ci.name + (ci.isList ? " (List<" : " (") +
					(ci.elemClass ? il2cpp_class_get_name(ci.elemClass) : "?") +
					(ci.isList ? ">)" : "[])");
				if (ImGui::Selectable(label.c_str(), i == g_collSearchCollIdx))
					g_collSearchCollIdx = i;
			}
			ImGui::EndCombo();
		}
	}

	// Field dropdown
	if (!g_collSearchFields.empty()) {
		std::string fieldPreview = g_collSearchFieldIdx < (int)g_collSearchFields.size()
			? g_collSearchFields[g_collSearchFieldIdx].name + " (" + g_collSearchFields[g_collSearchFieldIdx].typeName + ")"
			: "Select...";
		ImGui::SameLine();
		ImGui::SetNextItemWidth(200);
		if (ImGui::BeginCombo("Field", fieldPreview.c_str())) {
			for (int i = 0; i < (int)g_collSearchFields.size(); i++) {
				auto& fi = g_collSearchFields[i];
				std::string label = fi.name + " (" + fi.typeName + ")";
				if (ImGui::Selectable(label.c_str(), i == g_collSearchFieldIdx))
					g_collSearchFieldIdx = i;
			}
			ImGui::EndCombo();
		}
	}
	else {
		ImGui::TextDisabled("No searchable instance fields found.");
		ImGui::Unindent();
		return;
	}

	// Search value + mode + button
	ImGui::SetNextItemWidth(200);
	bool enterPressed = ImGui::InputText("Value##collsearch", g_collSearchValue, sizeof(g_collSearchValue),
		ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(80);
	ImGui::Combo("##collmode", &g_collSearchMode, "Contains\0Exact\0");
	ImGui::SameLine();
	if (ImGui::Button("Search##coll") || enterPressed) {
		CollSearchExecute();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Rescan##coll")) CollSearchDetectCollections(klass);

	// Results
	if (g_collSearchDone) {
		ImGui::Text("%d results", (int)g_collSearchResults.size());
		if (g_collSearchResults.size() >= 500)
			ImGui::SameLine(), ImGui::TextColored(ImVec4(1, 1, 0, 1), "(capped at 500)");

		ImGui::BeginChild("CollSearchResults", ImVec2(0, 200), true);
		for (int i = 0; i < (int)g_collSearchResults.size(); i++) {
			auto& r = g_collSearchResults[i];
			ImGui::PushID(i);
			if (ImGui::SmallButton("Inspect"))
				UnityExplorerTAB::Helpers::InspectObject(r.obj);
			ImGui::SameLine();
			ImGui::Text("[%d] %s  (0x%p)", i, r.preview.c_str(), r.obj);
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	ImGui::Unindent();
}

void UnityExplorerTAB::Helpers::DrawInspector()
{
	if (!selectedGameObject) {
		ImGui::TextDisabled("Select an object to inspect.");
		return;
	}

	// For raw class view or force-cast, skip IsValidIl2CppObject since the klass header may not match
	bool isForceCast = (forceCastClass && forceCastObj == selectedGameObject);
	if (!isRawClassView && !isForceCast && !Resolver::Protection::IsValidIl2CppObject(selectedGameObject)) {
		ImGui::TextDisabled("Select an object to inspect.");
		selectedGameObject = nullptr;
		return;
	}

	Il2CppClass* klass = nullptr;

	bool readSuccess = Resolver::Protection::safe_call([&]() {
		if (isRawClassView) {
			klass = (Il2CppClass*)selectedGameObject;
		}
		else if (forceCastClass && forceCastObj == selectedGameObject) {
			// Force-cast: use the user-specified class instead of the object's header
			klass = forceCastClass;
		}
		else {
			// Try to get the class directly - works for ALL managed objects,
			// not just UnityEngine.Object subclasses
			klass = il2cpp_object_get_class(selectedGameObject);
		}
		});

	if (!readSuccess || !klass) {
		ImGui::TextColored(ImVec4(1, 0, 0, 1), "Error: Could not read object class (ptr=0x%p)", selectedGameObject);
		if (ImGui::Button("Clear Selection")) selectedGameObject = nullptr;
		return;
	}

	// Get class name/namespace safely for logging and display
	const char* className = nullptr;
	const char* classNamespace = nullptr;
	bool nameSuccess = Resolver::Protection::safe_call([&]() {
		className = il2cpp_class_get_name(klass);
		classNamespace = il2cpp_class_get_namespace(klass);
		});
	if (!nameSuccess || !className) {
		DebugLog("CRASH PREVENTED: il2cpp_class_get_name/namespace crashed for klass=0x%p", klass);
		ImGui::TextColored(ImVec4(1, 0, 0, 1), "Error: Could not read class name (corrupt metadata?)");
		if (ImGui::Button("Clear Selection")) selectedGameObject = nullptr;
		return;
	}

	// Only log on selection change to avoid spamming every frame
	static void* lastLoggedPtr = nullptr;
	if (lastLoggedPtr != selectedGameObject) {
		lastLoggedPtr = selectedGameObject;
		DebugLog("DrawInspector: class=%s::%s ptr=0x%p rawView=%d", classNamespace ? classNamespace : "", className, klass, isRawClassView);
	}

	if (!inspectionHistory.empty()) {
		if (ImGui::Button("< Back")) {
			bool foundValid = false;
			while (!inspectionHistory.empty()) {
				InspectorHistoryItem item = inspectionHistory.top();
				inspectionHistory.pop();

				bool isValid = false;

				Resolver::Protection::safe_call([&]() {
					if (Resolver::Protection::IsValidIl2CppObject(item.ptr)) {
						if (item.isClassView) {
							isValid = true;
						}
						else {
							isValid = Resolver::Protection::IsAlive(item.ptr);
						}
					}
					});

				if (isValid) {
					selectedGameObject = item.ptr;
					isRawClassView = item.isClassView;
					forceCastClass = nullptr;
					forceCastObj = nullptr;
					methodParamBuffers.clear();
					methodLastResults.clear();
					methodLastReturnObj.clear();
					foundValid = true;
					break;
				}
			}

			if (!foundValid && inspectionHistory.empty()) {
				if (!Resolver::Protection::IsValidIl2CppObject(selectedGameObject)) {
					selectedGameObject = nullptr;
				}
			}
		}
		ImGui::SameLine();
	}

	if (ImGui::Button("Refresh")) needsRefresh = true;
	ImGui::SameLine();

	char addrBuf[64];
	sprintf_s(addrBuf, sizeof(addrBuf), "0x%p", selectedGameObject);
	if (ImGui::Button("Copy Addr")) Resolver::Helpers::CopyToClipboard(addrBuf);
	ImGui::SameLine();
	ImGui::TextDisabled("[%s]", addrBuf);

	if (isRawClassView) {
		static Il2CppClass* mbClass = Resolver::FindClass("UnityEngine", "MonoBehaviour");
		bool isMonoBehaviour = false;

		Resolver::Protection::safe_call([&]() {
			if (mbClass && klass) isMonoBehaviour = il2cpp_class_is_assignable_from(mbClass, klass);
			});

		ImGui::SameLine();
		if (!isMonoBehaviour) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
			if (ImGui::Button("Create Instance")) {
				DebugLog("  Creating instance of %s...", className);
				Resolver::Protection::safe_call([&]() {
					Il2CppObject* newObj = il2cpp_object_new(klass);
					if (newObj) {
						il2cpp_runtime_object_init(newObj);
						InspectObject(newObj);
					}
				});
			}
			ImGui::PopStyleColor();
		}
		else {
			ImGui::TextDisabled("(MonoBehaviour)");
		}

		// Cast pointer: enter a hex address, inspect as object or force-cast as this class
		static char castPtrBuf[32] = "";
		ImGui::SetNextItemWidth(160);
		ImGui::InputTextWithHint("##castptr", "0x...", castPtrBuf, sizeof(castPtrBuf));
		ImGui::SameLine();
		// Inspect: reads the object's own class from its header
		if (ImGui::Button("Inspect Ptr")) {
			uintptr_t addr = 0;
			try {
				std::string s(castPtrBuf);
				if (s.size() > 2 && (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
					s = s.substr(2);
				addr = std::stoull(s, nullptr, 16);
			} catch (...) {}
			if (addr) {
				Il2CppObject* castObj = (Il2CppObject*)addr;
				bool valid = Resolver::Protection::safe_call([&]() {
					Resolver::Protection::IsValidIl2CppObject(castObj);
				});
				if (valid) {
					inspectionHistory.push({ selectedGameObject, isRawClassView });
					selectedGameObject = castObj;
					isRawClassView = false;
					forceCastClass = nullptr;
					forceCastObj = nullptr;
					methodParamBuffers.clear();
					methodLastResults.clear();
					methodLastReturnObj.clear();
				}
			}
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read object at this address (uses object's own class)");
		ImGui::SameLine();
		// Force Cast: treats the pointer as this exact class, ignoring the object's klass header
		if (ImGui::Button("Force Cast")) {
			uintptr_t addr = 0;
			try {
				std::string s(castPtrBuf);
				if (s.size() > 2 && (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
					s = s.substr(2);
				addr = std::stoull(s, nullptr, 16);
			} catch (...) {}
			if (addr) {
				Il2CppObject* castObj = (Il2CppObject*)addr;
				// Don't validate klass — user is forcing the cast
				bool readable = Resolver::Protection::safe_call([&]() {
					volatile uint8_t probe = *(uint8_t*)castObj; (void)probe;
				});
				if (readable) {
					inspectionHistory.push({ selectedGameObject, isRawClassView });
					// Store the forced class as a tag so DrawInspector uses it
					forceCastClass = klass;
					forceCastObj = castObj;
					selectedGameObject = castObj;
					isRawClassView = false;
					methodParamBuffers.clear();
					methodLastResults.clear();
					methodLastReturnObj.clear();
				}
			}
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Force-read pointer as %s (ignores object header)", className);
	}

	ImGui::Separator();

	static Il2CppClass* goClass = Resolver::FindClass("UnityEngine", "GameObject");
	static Il2CppClass* compClass = Resolver::FindClass("UnityEngine", "Component");
	bool isGameObject = false;

	Resolver::Protection::safe_call([&]() {
		if (!isRawClassView && klass && goClass) {
			isGameObject = il2cpp_class_is_assignable_from(goClass, klass);
		}
		});

	if (isGameObject) {
		static const MethodInfo* getName = il2cpp_class_get_method_from_name(goClass, "get_name", 0);
		Il2CppString* nStr = (Il2CppString*)Resolver::Protection::SafeRuntimeInvoke(getName, selectedGameObject, nullptr);
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "GameObject: %s", nStr ? il2cppi_to_string(nStr).c_str() : "Unnamed");
		ImGui::Separator();
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##memberSearchGO", "Search fields, properties, methods...", memberSearchFilter, 128);
		ImGui::Separator();

		ImGui::BeginChild("CompScroll");
		static const MethodInfo* getComps = il2cpp_class_get_method_from_name(goClass, "GetComponents", 1);
		Il2CppReflectionType* compTypeObj = (Il2CppReflectionType*)il2cpp_type_get_object(il2cpp_class_get_type(compClass));
		void* pComps[] = { compTypeObj };
		Il2CppArray* components = (Il2CppArray*)Resolver::Protection::SafeRuntimeInvoke(getComps, selectedGameObject, pComps);

		if (components) {
			for (uint32_t i = 0; i < il2cpp_array_length(components); i++) {
				Il2CppObject* comp = GET_ARRAY_ELEMENT(components, i);
				if (!comp) continue;

				Il2CppClass* cKlass = nullptr;
				const char* cName = "???";
				bool compOk = Resolver::Protection::safe_call([&]() {
					cKlass = il2cpp_object_get_class(comp);
					cName = il2cpp_class_get_name(cKlass);
					});
				if (!compOk || !cKlass) continue;

				ImGui::PushID(comp);

				if (ImGui::CollapsingHeader(cName)) {
					ImGui::Indent();

					const MethodInfo* getEn = il2cpp_class_get_method_from_name(cKlass, "get_enabled", 0);
					const MethodInfo* setEn = il2cpp_class_get_method_from_name(cKlass, "set_enabled", 1);

					if (getEn && setEn) {
						Il2CppObject* resEn = Resolver::Protection::SafeRuntimeInvoke(getEn, comp, nullptr);
						bool isEnabled = Resolver::Protection::SafeUnbox<bool>(resEn, true);

						if (ImGui::Checkbox("Enabled", &isEnabled)) {
							void* params[] = { &isEnabled };
							Resolver::Protection::SafeRuntimeInvoke(setEn, comp, params);
						}
						ImGui::Separator();
					}

					Resolver::Protection::safe_call([&]() {
						void* fIter = nullptr;
						while (FieldInfo* f = il2cpp_class_get_fields(cKlass, &fIter)) {
							const char* fname = il2cpp_field_get_name(f);
							if (!MatchesMemberFilter(fname)) continue;
							DrawField(comp, f);
						}
					});
					ImGui::Spacing(); ImGui::TextDisabled("Properties");
					Resolver::Protection::safe_call([&]() {
						void* pIter = nullptr;
						while (const PropertyInfo* p = il2cpp_class_get_properties(cKlass, &pIter)) {
							if (!MatchesMemberFilter(p->name)) continue;
							DrawProperty(comp, p);
						}
					});
					ImGui::Spacing();
					Resolver::Protection::safe_call([&]() {
						DrawMethods(comp, cKlass);
					});
					ImGui::Unindent();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}
	else {
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Class: %s", className);
		if (forceCastClass && forceCastObj == selectedGameObject) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "(Force Cast @ 0x%p)", selectedGameObject);
		}
		ImGui::TextDisabled("Namespace: %s", classNamespace ? classNamespace : "");

		// --- Inheritance Chain ---
		{
			// Build parent chain
			std::vector<Il2CppClass*> parents;
			Resolver::Protection::safe_call([&]() {
				Il2CppClass* p = il2cpp_class_get_parent(klass);
				while (p) {
					parents.push_back(p);
					p = il2cpp_class_get_parent(p);
				}
			});
			if (!parents.empty()) {
				ImGui::TextDisabled("Inherits:");
				ImGui::SameLine();
				for (size_t pi = 0; pi < parents.size(); pi++) {
					const char* pName = nullptr;
					Resolver::Protection::safe_call([&]() { pName = il2cpp_class_get_name(parents[pi]); });
					if (!pName) pName = "?";
					ImGui::PushID((int)(pi + 50000));
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.5f, 0.5f));
					if (ImGui::SmallButton(pName)) InspectClass(parents[pi]);
					ImGui::PopStyleColor(2);
					ImGui::PopID();
					if (pi + 1 < parents.size()) { ImGui::SameLine(); ImGui::TextDisabled(">"); ImGui::SameLine(); }
				}
			}

			// Interfaces
			std::vector<Il2CppClass*> interfaces;
			Resolver::Protection::safe_call([&]() {
				void* iIter = nullptr;
				Il2CppClass* iface;
				while ((iface = il2cpp_class_get_interfaces(klass, &iIter)) != nullptr) {
					interfaces.push_back(iface);
				}
			});
			if (!interfaces.empty()) {
				ImGui::TextDisabled("Implements:");
				ImGui::SameLine();
				for (size_t ii = 0; ii < interfaces.size(); ii++) {
					const char* iName = nullptr;
					Resolver::Protection::safe_call([&]() { iName = il2cpp_class_get_name(interfaces[ii]); });
					if (!iName) iName = "?";
					ImGui::PushID((int)(ii + 60000));
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.5f, 0.5f));
					if (ImGui::SmallButton(iName)) InspectClass(interfaces[ii]);
					ImGui::PopStyleColor(2);
					ImGui::PopID();
					if (ii + 1 < interfaces.size()) { ImGui::SameLine(); ImGui::TextDisabled(","); ImGui::SameLine(); }
				}
			}
		}

		const char* singletonNames[] = { "instance", "Instance", "singleton", "_instance", "SharedInstance" };
		for (const char* sName : singletonNames) {
			Resolver::Protection::safe_call([&]() {
				FieldInfo* field = il2cpp_class_get_field_from_name(klass, sName);
				if (field && (field->type->attrs & 0x0010)) {
					Il2CppObject* val = nullptr;
					il2cpp_field_static_get_value(field, &val);
					if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
						ImGui::Separator();
						ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "[!] Found Singleton: %s", sName);
						ImGui::SameLine();
						if (ImGui::Button("Inspect Singleton")) InspectObject(val);
					}
				}
			});
		}

		ImGui::Separator();
		static std::vector<Il2CppObject*> foundInstances;
		static bool showInstanceList = false;
		static Il2CppClass* lastSearchedClass = nullptr;

		if (lastSearchedClass != klass) {
			foundInstances.clear();
			showInstanceList = false;
			lastSearchedClass = klass;
		}

		if (ImGui::Button("Find Active Objects in Scene")) {
			foundInstances = Resolver::FindObjectsByType(klass);
			showInstanceList = true;
		}

		if (showInstanceList) {
			ImGui::Text("Found %d objects.", (int)foundInstances.size());
			if (!foundInstances.empty()) {
				ImGui::BeginChild("InstList", ImVec2(0, 120), true);
				for (size_t i = 0; i < foundInstances.size(); i++) {
					if (!Resolver::Protection::IsAlive(foundInstances[i])) continue;
					ImGui::PushID((int)i);
					if (ImGui::Button("Inspect")) InspectObject(foundInstances[i]);
					ImGui::SameLine();
					ImGui::Text("0x%p", foundInstances[i]);
					ImGui::PopID();
				}
				ImGui::EndChild();
			}
		}

		// --- Find References ---
		ImGui::SameLine();
		if (ImGui::Button("Find References")) {
			ScanReferences(klass);
		}

		if (showRefResults && lastRefSearchClass == klass) {
			if (refResults.empty()) {
				ImGui::TextDisabled("No references found.");
			}
			else {
				ImGui::Text("Found %d reference(s).", (int)refResults.size());

				// Count statics vs instance
				int staticCount = 0;
				for (auto& r : refResults) if (r.isStatic) staticCount++;
				int instanceCount = (int)refResults.size() - staticCount;

				if (staticCount > 0 && ImGui::TreeNode("Static Fields##refs")) {
					for (size_t ri = 0; ri < refResults.size(); ri++) {
						auto& r = refResults[ri];
						if (!r.isStatic) continue;
						ImGui::PushID((int)ri);

						// Try to read the static field value
						Il2CppObject* val = nullptr;
						Resolver::Protection::safe_call([&]() {
							il2cpp_field_static_get_value(r.field, &val);
						});

						if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
							if (ImGui::SmallButton("Inspect")) InspectObject(val);
							ImGui::SameLine();
						}

						ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", r.fieldTypeName.c_str());
						ImGui::SameLine();
						ImGui::Text("%s.%s", r.ownerName.c_str(), r.fieldName.c_str());
						ImGui::PopID();
					}
					ImGui::TreePop();
				}

				if (instanceCount > 0 && ImGui::TreeNode("Instance Fields##refs")) {
					for (size_t ri = 0; ri < refResults.size(); ri++) {
						auto& r = refResults[ri];
						if (r.isStatic) continue;
						ImGui::PushID((int)(ri + 10000));

						if (ImGui::SmallButton("Browse")) InspectClass(r.ownerClass);
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", r.fieldTypeName.c_str());
						ImGui::SameLine();
						ImGui::Text("%s.%s", r.ownerName.c_str(), r.fieldName.c_str());
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
			}
		}

		// --- Tool buttons row ---
		ImGui::Separator();
		{
			Il2CppObject* inspObj = isRawClassView ? nullptr : selectedGameObject;

			// Snapshot (field + property change tracker)
			if (ImGui::Button("Snapshot")) {
				snapshot.clear();
				snapshotMap.clear();
				propSnapshot.clear();
				propSnapshotMap.clear();
				snapshotObj = inspObj;
				snapshotKlass = klass;
				hasSnapshot = true;
				Resolver::Protection::safe_call([&]() {
					void* fIter = nullptr;
					while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
						FieldSnapshot fs;
						fs.field = f;
						fs.name = il2cpp_field_get_name(f);
						fs.value = ReadFieldValueAsString(inspObj, f);
						snapshot.push_back(fs);
						snapshotMap[f] = fs.value;
					}
				});
				Resolver::Protection::safe_call([&]() {
					void* pIter = nullptr;
					while (const PropertyInfo* p = il2cpp_class_get_properties(klass, &pIter)) {
						if (!p->get) continue;
						PropertySnapshot ps;
						ps.prop = p;
						ps.name = p->name ? p->name : "?";
						ps.value = ReadPropertyValueAsString(inspObj, p);
						propSnapshot.push_back(ps);
						propSnapshotMap[p] = ps.value;
					}
				});
			}
			if (hasSnapshot && snapshotKlass == klass) {
				ImGui::SameLine();
				ImGui::TextDisabled("(%d fields, %d props)", (int)snapshot.size(), (int)propSnapshot.size());
			}

			ImGui::SameLine();

			// Dump to clipboard (fields + properties)
			if (ImGui::Button("Dump")) {
				std::string dump;
				std::string ns = classNamespace ? classNamespace : "";
				dump += "class " + (ns.empty() ? "" : ns + ".") + className + "\n";
				dump += "--- Fields ---\n";
				Resolver::Protection::safe_call([&]() {
					void* fIter = nullptr;
					while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
						const char* fn = il2cpp_field_get_name(f);
						const Il2CppType* ft = il2cpp_field_get_type(f);
						bool isSt = (ft->attrs & 0x0010);
						char* tn = il2cpp_type_get_name(ft);
						std::string val = ReadFieldValueAsString(inspObj, f);
						dump += (isSt ? "static " : "  ");
						dump += (tn ? tn : "?");
						dump += " ";
						dump += fn;
						dump += " = ";
						dump += val;
						dump += "\n";
						if (tn) il2cpp_free(tn);
					}
				});
				dump += "--- Properties ---\n";
				Resolver::Protection::safe_call([&]() {
					void* pIter = nullptr;
					while (const PropertyInfo* p = il2cpp_class_get_properties(klass, &pIter)) {
						if (!p->get) continue;
						const char* pn = p->name ? p->name : "?";
						const Il2CppType* rt = p->get->return_type;
						char* tn = rt ? il2cpp_type_get_name(rt) : nullptr;
						std::string val = ReadPropertyValueAsString(inspObj, p);
						dump += "  ";
						dump += (tn ? tn : "?");
						dump += " ";
						dump += pn;
						dump += " = ";
						dump += val;
						dump += "\n";
						if (tn) il2cpp_free(tn);
					}
				});
				Resolver::Helpers::CopyToClipboard(dump.c_str());
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy all fields + properties to clipboard");
		}

		// Show snapshot diff if we have one
		if (hasSnapshot && snapshotKlass == klass) {
			Il2CppObject* inspObj = isRawClassView ? nullptr : selectedGameObject;
			int changedFields = 0, changedProps = 0;
			Resolver::Protection::safe_call([&]() {
				for (auto& fs : snapshot) {
					std::string cur = ReadFieldValueAsString(inspObj, fs.field);
					if (cur != fs.value) changedFields++;
				}
			});
			Resolver::Protection::safe_call([&]() {
				for (auto& ps : propSnapshot) {
					std::string cur = ReadPropertyValueAsString(inspObj, ps.prop);
					if (cur != ps.value) changedProps++;
				}
			});
			int total = changedFields + changedProps;
			if (total > 0) {
				ImGui::TextColored(ImVec4(1, 1, 0, 1), "%d changed since snapshot (%d fields, %d props)", total, changedFields, changedProps);
			}
		}

		ImGui::Separator();

		// Member search filter
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##memberSearch", "Search fields, properties, methods...", memberSearchFilter, 128);
		ImGui::Separator();

		ImGui::BeginChild("RawScroll");

		bool fieldsOk = Resolver::Protection::safe_call([&]() {
			void* fIter = nullptr;
			while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
				const char* fname = il2cpp_field_get_name(f);
				if (!MatchesMemberFilter(fname)) continue;
				DrawField(isRawClassView ? nullptr : selectedGameObject, f);
			}
		});
		if (!fieldsOk) {
			DebugLog("  CRASH PREVENTED in il2cpp_class_get_fields for %s::%s", classNamespace, className);
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "[!] Field enumeration crashed (class init failure)");
		}

		ImGui::Spacing(); ImGui::TextDisabled("Properties");
		bool propsOk = Resolver::Protection::safe_call([&]() {
			void* pIter = nullptr;
			while (const PropertyInfo* p = il2cpp_class_get_properties(klass, &pIter)) {
				if (!MatchesMemberFilter(p->name)) continue;
				DrawProperty(isRawClassView ? nullptr : selectedGameObject, p);
			}
		});
		if (!propsOk) {
			DebugLog("  CRASH PREVENTED in il2cpp_class_get_properties for %s::%s", classNamespace, className);
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "[!] Property enumeration crashed (class init failure)");
		}

		ImGui::Spacing();
		bool methodsOk = Resolver::Protection::safe_call([&]() {
			DrawMethods(isRawClassView ? nullptr : selectedGameObject, klass);
		});
		if (!methodsOk) {
			DebugLog("  CRASH PREVENTED in DrawMethods for %s::%s", classNamespace, className);
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "[!] Method enumeration crashed (class init failure)");
		}

		// Collection instance search
		ImGui::Spacing();
		DrawCollectionSearch(klass);

		ImGui::EndChild();
	}
}

void UnityExplorerTAB::Helpers::DrawObjectNode(Il2CppObject* gameObject)
{
	if (!Resolver::Protection::IsAlive(gameObject)) return;

	static Il2CppClass* goClass = Resolver::FindClass("UnityEngine", "GameObject");
	static const MethodInfo* getName = il2cpp_class_get_method_from_name(goClass, "get_name", 0);
	static const MethodInfo* getActive = il2cpp_class_get_method_from_name(goClass, "get_activeInHierarchy", 0);
	static const MethodInfo* getTransform = il2cpp_class_get_method_from_name(goClass, "get_transform", 0);

	Il2CppString* nameIl2Cpp = (Il2CppString*)il2cpp_runtime_invoke(getName, gameObject, nullptr, nullptr);
	bool isActive = Resolver::Protection::SafeUnbox<bool>(il2cpp_runtime_invoke(getActive, gameObject, nullptr, nullptr), false);

	if (!isActive) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

	Il2CppObject* transform = il2cpp_runtime_invoke(getTransform, gameObject, nullptr, nullptr);
	int childCount = 0;
	if (transform) {
		static Il2CppClass* trClass = Resolver::FindClass("UnityEngine", "Transform");
		childCount = Resolver::Protection::SafeUnbox<int>(il2cpp_runtime_invoke(il2cpp_class_get_method_from_name(trClass, "get_childCount", 0), transform, nullptr, nullptr), 0);
	}

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (childCount == 0) flags |= ImGuiTreeNodeFlags_Leaf;
	if (selectedGameObject == gameObject) flags |= ImGuiTreeNodeFlags_Selected;

	bool opened = ImGui::TreeNodeEx((void*)gameObject, flags, "%s", nameIl2Cpp ? il2cppi_to_string(nameIl2Cpp).c_str() : "Unnamed");
	if (ImGui::IsItemClicked()) {
		InspectObject(gameObject);
	}

	if (opened) {
		if (childCount > 0) {
			static Il2CppClass* trClass = Resolver::FindClass("UnityEngine", "Transform");
			static const MethodInfo* getChild = il2cpp_class_get_method_from_name(trClass, "GetChild", 1);
			static const MethodInfo* getGO = il2cpp_class_get_method_from_name(trClass, "get_gameObject", 0);
			for (int i = 0; i < childCount; i++) {
				void* p[] = { &i };
				Il2CppObject* cTr = il2cpp_runtime_invoke(getChild, transform, p, nullptr);
				if (cTr) DrawObjectNode((Il2CppObject*)il2cpp_runtime_invoke(getGO, cTr, nullptr, nullptr));
			}
		}
		ImGui::TreePop();
	}
	if (!isActive) ImGui::PopStyleColor();
}

void UnityExplorerTAB::Helpers::InspectObject(Il2CppObject* obj)
{
	if (!obj || !Resolver::Protection::IsValidIl2CppObject(obj)) return;
	if (selectedGameObject == obj) return;

	if (selectedGameObject) {
		inspectionHistory.push({ selectedGameObject, isRawClassView });
	}

	selectedGameObject = obj;
	isRawClassView = false;
	forceCastClass = nullptr;
	forceCastObj = nullptr;

	methodParamBuffers.clear();
	methodLastResults.clear();
	methodLastReturnObj.clear();
}

// Check if a field type references targetClass (directly, as array element, or as generic arg)
static bool TypeReferencesClass(const Il2CppType* fieldType, Il2CppClass* targetClass) {
	if (!fieldType || !targetClass) return false;
	int t = fieldType->type;

	if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_VALUETYPE) {
		Il2CppClass* fClass = il2cpp_class_from_type(fieldType);
		if (fClass == targetClass) return true;
	}
	else if (t == IL2CPP_TYPE_SZARRAY) {
		Il2CppClass* elemClass = il2cpp_type_get_class_or_element_class(fieldType);
		if (elemClass == targetClass) return true;
	}
	else if (t == IL2CPP_TYPE_GENERICINST) {
		Il2CppGenericClass* genClass = fieldType->data.generic_class;
		if (genClass && genClass->context.class_inst) {
			const Il2CppGenericInst* inst = genClass->context.class_inst;
			for (uint32_t g = 0; g < inst->type_argc; g++) {
				const Il2CppType* argType = inst->type_argv[g];
				if (!argType) continue;
				Il2CppClass* argClass = il2cpp_class_from_type(argType);
				if (argClass == targetClass) return true;
			}
		}
	}
	return false;
}

static void ScanReferences(Il2CppClass* targetClass) {
	refResults.clear();
	showRefResults = true;
	lastRefSearchClass = targetClass;
	if (!targetClass) return;

	auto domain = il2cpp_domain_get();
	size_t asmCount = 0;
	const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);
	if (!assemblies) return;

	const size_t MAX_RESULTS = 200;

	for (size_t a = 0; a < asmCount && refResults.size() < MAX_RESULTS; a++) {
		const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[a]);
		if (!image) continue;
		size_t classCount = il2cpp_image_get_class_count(image);

		for (size_t c = 0; c < classCount && refResults.size() < MAX_RESULTS; c++) {
			Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, c);
			if (!klass || klass == targetClass) continue;

			Resolver::Protection::safe_call([&]() {
				const char* cName = il2cpp_class_get_name(klass);
				const char* cNs = il2cpp_class_get_namespace(klass);

				void* fIter = nullptr;
				while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
					if (refResults.size() >= MAX_RESULTS) break;
					const Il2CppType* fType = il2cpp_field_get_type(f);
					if (!fType) continue;

					bool match = false;
					Resolver::Protection::safe_call([&]() {
						match = TypeReferencesClass(fType, targetClass);
					});

					if (match) {
						RefResult r;
						r.ownerClass = klass;
						r.field = f;
						r.isStatic = (fType->attrs & 0x0010) != 0;
						r.fieldName = il2cpp_field_get_name(f);
						std::string ns = cNs ? cNs : "";
						r.ownerName = ns.empty() ? cName : (ns + "::" + cName);
						char* tn = il2cpp_type_get_name(fType);
						r.fieldTypeName = tn ? tn : "?";
						if (tn) il2cpp_free(tn);
						refResults.push_back(r);
					}
				}
			});
		}
	}
}

// --- ReadFieldValueAsString: reads any field value as a display string ---
static std::string ReadFieldValueAsString(Il2CppObject* obj, FieldInfo* field) {
	if (!field) return "?";
	const Il2CppType* type = il2cpp_field_get_type(field);
	if (!type) return "?";
	int t = type->type;
	bool isStatic = (type->attrs & 0x0010);
	bool hasAccess = (obj != nullptr) || isStatic;
	if (!hasAccess) return "(instance only)";

	std::string result = "?";
	Resolver::Protection::safe_call([&]() {
		switch (t) {
		case IL2CPP_TYPE_BOOLEAN: { bool v = false; Resolver::GetFieldValue(obj, field, &v); result = v ? "true" : "false"; break; }
		case IL2CPP_TYPE_CHAR: { uint16_t v = 0; Resolver::GetFieldValue(obj, field, &v); char b[16]; sprintf_s(b, "%u", v); result = b; break; }
		case IL2CPP_TYPE_I1: { int8_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_U1: { uint8_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_I2: { int16_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_U2: { uint16_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_I4: { int32_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_U4: { uint32_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_I8: { int64_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_U8: { uint64_t v = 0; Resolver::GetFieldValue(obj, field, &v); result = std::to_string(v); break; }
		case IL2CPP_TYPE_R4: { float v = 0; Resolver::GetFieldValue(obj, field, &v); char b[64]; sprintf_s(b, "%.4f", v); result = b; break; }
		case IL2CPP_TYPE_R8: { double v = 0; Resolver::GetFieldValue(obj, field, &v); char b[64]; sprintf_s(b, "%.6f", v); result = b; break; }
		case IL2CPP_TYPE_STRING: {
			Il2CppString* s = nullptr; Resolver::GetFieldValue(obj, field, &s);
			result = (s && s->chars) ? ("\"" + il2cppi_to_string(s) + "\"") : "null";
			break;
		}
		case IL2CPP_TYPE_VALUETYPE: {
			Il2CppClass* vc = il2cpp_class_from_type(type);
			if (vc && vc->enumtype) {
				int32_t v = 0; Resolver::GetFieldValue(obj, field, &v);
				result = std::to_string(v);
			}
			else if (vc) {
				std::string sn = vc->name;
				if (sn == "Vector3") { app::Vector3 v; Resolver::GetFieldValue(obj, field, &v); char b[96]; sprintf_s(b, "(%.2f, %.2f, %.2f)", v.x, v.y, v.z); result = b; }
				else if (sn == "Vector2") { float v[2]; Resolver::GetFieldValue(obj, field, &v); char b[64]; sprintf_s(b, "(%.2f, %.2f)", v[0], v[1]); result = b; }
				else result = "{" + sn + "}";
			}
			break;
		}
		case IL2CPP_TYPE_CLASS: case IL2CPP_TYPE_OBJECT: case IL2CPP_TYPE_GENERICINST: {
			Il2CppObject* v = nullptr; Resolver::GetFieldValue(obj, field, &v);
			if (v && Resolver::Protection::IsValidIl2CppObject(v)) {
				char b[32]; sprintf_s(b, "0x%p", v); result = b;
			} else result = "null";
			break;
		}
		case IL2CPP_TYPE_SZARRAY: {
			Il2CppArray* arr = nullptr; Resolver::GetFieldValue(obj, field, &arr);
			if (arr) { char b[32]; sprintf_s(b, "[%d]", il2cpp_array_length(arr)); result = b; }
			else result = "null";
			break;
		}
		default: result = "..."; break;
		}
	});
	return result;
}

// --- ReadPropertyValueAsString: reads any property value as a display string ---
static std::string ReadPropertyValueAsString(Il2CppObject* obj, const PropertyInfo* prop) {
	if (!prop || !prop->get) return "?";
	const Il2CppType* returnType = prop->get->return_type;
	if (!returnType) return "?";
	int t = returnType->type;

	std::string result = "?";
	Resolver::Protection::safe_call([&]() {
		Il2CppObject* boxedVal = Resolver::Protection::SafeRuntimeInvoke(prop->get, obj, nullptr);
		switch (t) {
		case IL2CPP_TYPE_BOOLEAN: { result = Resolver::Protection::SafeUnbox<bool>(boxedVal, false) ? "true" : "false"; break; }
		case IL2CPP_TYPE_CHAR: { uint16_t v = Resolver::Protection::SafeUnbox<uint16_t>(boxedVal, 0); char b[16]; sprintf_s(b, "%u", v); result = b; break; }
		case IL2CPP_TYPE_I1: { result = std::to_string(Resolver::Protection::SafeUnbox<int8_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_U1: { result = std::to_string(Resolver::Protection::SafeUnbox<uint8_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_I2: { result = std::to_string(Resolver::Protection::SafeUnbox<int16_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_U2: { result = std::to_string(Resolver::Protection::SafeUnbox<uint16_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_I4: { result = std::to_string(Resolver::Protection::SafeUnbox<int32_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_U4: { result = std::to_string(Resolver::Protection::SafeUnbox<uint32_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_I8: { result = std::to_string(Resolver::Protection::SafeUnbox<int64_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_U8: { result = std::to_string(Resolver::Protection::SafeUnbox<uint64_t>(boxedVal, 0)); break; }
		case IL2CPP_TYPE_R4: { char b[64]; sprintf_s(b, "%.4f", Resolver::Protection::SafeUnbox<float>(boxedVal, 0.0f)); result = b; break; }
		case IL2CPP_TYPE_R8: { char b[64]; sprintf_s(b, "%.6f", Resolver::Protection::SafeUnbox<double>(boxedVal, 0.0)); result = b; break; }
		case IL2CPP_TYPE_STRING: {
			if (boxedVal) result = "\"" + il2cppi_to_string((Il2CppString*)boxedVal) + "\"";
			else result = "null";
			break;
		}
		case IL2CPP_TYPE_VALUETYPE: {
			Il2CppClass* vc = il2cpp_class_from_type(returnType);
			if (vc && vc->enumtype) {
				int32_t v = Resolver::Protection::SafeUnbox<int32_t>(boxedVal, 0);
				result = std::to_string(v);
			}
			else if (vc) {
				std::string sn = vc->name;
				if (sn == "Vector3") { app::Vector3 v = Resolver::Protection::SafeUnbox<app::Vector3>(boxedVal); char b[96]; sprintf_s(b, "(%.2f, %.2f, %.2f)", v.x, v.y, v.z); result = b; }
				else if (sn == "Vector2") { float v[2] = {}; void* u = boxedVal ? il2cpp_object_unbox(boxedVal) : nullptr; if (u) memcpy(v, u, sizeof(float) * 2); char b[64]; sprintf_s(b, "(%.2f, %.2f)", v[0], v[1]); result = b; }
				else result = "{" + sn + "}";
			}
			break;
		}
		case IL2CPP_TYPE_CLASS: case IL2CPP_TYPE_OBJECT: case IL2CPP_TYPE_GENERICINST: {
			if (boxedVal && Resolver::Protection::IsValidIl2CppObject(boxedVal)) {
				char b[32]; sprintf_s(b, "0x%p", boxedVal); result = b;
			} else result = "null";
			break;
		}
		case IL2CPP_TYPE_SZARRAY: {
			if (boxedVal) {
				Il2CppArray* arr = (Il2CppArray*)boxedVal;
				uint32_t len = il2cpp_array_length(arr);
				Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
				Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
				const char* en = elemClass ? il2cpp_class_get_name(elemClass) : "?";
				char b[64]; sprintf_s(b, "%s[%d]", en, len);
				result = b;

				// For byte[], also include the raw text if it looks like a string
				if (elemClass) {
					const Il2CppType* et = il2cpp_class_get_type(elemClass);
					if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1) && len > 0) {
						uint32_t cap = len > 4096 ? 4096 : len;
						std::string txt;
						char* data = (char*)arr + 0x20;
						for (uint32_t i = 0; i < cap; i++) {
							uint8_t c = (uint8_t)data[i];
							txt += (c >= 32 && c < 127) ? (char)c : '.';
						}
						result += " \"" + txt + "\"";
					}
				}
			} else result = "null";
			break;
		}
		default: result = "..."; break;
		}
	});
	return result;
}

// --- Static Field Scanner ---
static void ScanStaticFields() {
	staticScanResults.clear();
	showStaticScan = true;

	auto domain = il2cpp_domain_get();
	size_t asmCount = 0;
	const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);
	if (!assemblies) return;

	std::string filter(staticScanFilter);
	std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

	const size_t MAX_RESULTS = 500;

	for (size_t a = 0; a < asmCount && staticScanResults.size() < MAX_RESULTS; a++) {
		const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[a]);
		if (!image) continue;
		size_t classCount = il2cpp_image_get_class_count(image);

		for (size_t c = 0; c < classCount && staticScanResults.size() < MAX_RESULTS; c++) {
			Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, c);
			if (!klass) continue;

			Resolver::Protection::safe_call([&]() {
				const char* cName = il2cpp_class_get_name(klass);
				const char* cNs = il2cpp_class_get_namespace(klass);

				void* fIter = nullptr;
				while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
					if (staticScanResults.size() >= MAX_RESULTS) break;
					const Il2CppType* fType = il2cpp_field_get_type(f);
					if (!fType || !(fType->attrs & 0x0010)) continue; // static only

					int t = fType->type;
					// Only care about reference types (classes, objects, generics, arrays)
					if (t != IL2CPP_TYPE_CLASS && t != IL2CPP_TYPE_OBJECT &&
						t != IL2CPP_TYPE_GENERICINST && t != IL2CPP_TYPE_SZARRAY) continue;

					Il2CppObject* val = nullptr;
					il2cpp_field_static_get_value(f, &val);
					if (!val || !Resolver::Protection::IsValidIl2CppObject(val)) continue;

					std::string fullName = (cNs && cNs[0]) ? (std::string(cNs) + "." + cName) : cName;
					const char* fn = il2cpp_field_get_name(f);

					// Apply filter
					if (!filter.empty()) {
						std::string lName = fullName + "." + fn;
						std::transform(lName.begin(), lName.end(), lName.begin(), ::tolower);
						if (lName.find(filter) == std::string::npos) continue;
					}

					char* tn = il2cpp_type_get_name(fType);
					StaticFieldResult r;
					r.klass = klass;
					r.field = f;
					r.className = fullName;
					r.fieldName = fn;
					r.typeName = tn ? tn : "?";
					if (tn) il2cpp_free(tn);
					staticScanResults.push_back(r);
				}
			});
		}
	}
}

// --- Value Scanner engine (optimized) ---
// Scan limits — prevent exponential explosion
static constexpr int VSCAN_MAX_DEPTH = 4;           // max recursion depth (was 6)
static constexpr size_t VSCAN_MAX_VISITED = 200000;  // max unique objects before abort
static constexpr int VSCAN_COLLECTION_CAP = 50;      // max elements per collection (was 200)
static constexpr int VSCAN_ARRAY_CAP = 200;           // max elements per array (was 500)
static constexpr DWORD VSCAN_TIMEOUT_MS = 15000;      // 15 second hard timeout

// Scan context — bundles all state to avoid passing 10+ params through recursion
struct VScanCtx {
	int64_t intA = 0, intB = 0;
	double fltA = 0, fltB = 0;
	bool boolTarget = false;
	std::string strTargetLower;          // pre-lowercased target for fast matching
	int scanType = 0;
	int scanMode = 0;
	double eps = 0.01;
	DWORD startTick = 0;
	std::unordered_set<void*> visited;   // O(1) lookup vs O(log n) for std::set
	int64_t totalScanned = 0;

	bool shouldStop() const {
		return !vscanRunning.load(std::memory_order_relaxed) ||
			valueScanResults.size() >= VSCAN_MAX_RESULTS ||
			visited.size() >= VSCAN_MAX_VISITED ||
			(GetTickCount() - startTick) > VSCAN_TIMEOUT_MS;
	}
	const char* stopReason() const {
		if (!vscanRunning.load(std::memory_order_relaxed)) return "cancelled";
		if (valueScanResults.size() >= VSCAN_MAX_RESULTS) return "max results";
		if (visited.size() >= VSCAN_MAX_VISITED) return "max objects";
		if ((GetTickCount() - startTick) > VSCAN_TIMEOUT_MS) return "timeout 15s";
		return nullptr;
	}
};

// Comparison helpers
static bool VScanMatchInt(int64_t v, int64_t a, int64_t b, int mode) {
	switch (mode) {
	case VMODE_EXACT:     return v == a;
	case VMODE_NOT_EQUAL: return v != a;
	case VMODE_GREATER:   return v > a;
	case VMODE_LESS:      return v < a;
	case VMODE_BETWEEN:   return v >= a && v <= b;
	}
	return false;
}
static bool VScanMatchFloat(double v, double a, double b, double eps, int mode) {
	switch (mode) {
	case VMODE_EXACT:     return fabs(v - a) <= eps;
	case VMODE_NOT_EQUAL: return fabs(v - a) > eps;
	case VMODE_GREATER:   return v > a;
	case VMODE_LESS:      return v < a;
	case VMODE_BETWEEN:   return v >= a && v <= b;
	}
	return false;
}
static bool VScanMatchBool(bool v, bool target, int mode) {
	return (mode == VMODE_NOT_EQUAL) ? (v != target) : (v == target);
}
// Pre-lowercased target version — avoids re-lowering target on every call
static bool VScanMatchString(const std::string& v, const std::string& targetLower, int mode) {
	if (mode == VMODE_NOT_EQUAL) {
		std::string lv(v);
		for (auto& c : lv) c = (char)tolower((unsigned char)c);
		return lv != targetLower;
	}
	std::string lv(v);
	for (auto& c : lv) c = (char)tolower((unsigned char)c);
	if (mode == VMODE_CONTAINS) return lv.find(targetLower) != std::string::npos;
	return lv == targetLower;
}

// Recursive scanner — optimized with context struct, budget limits, cancel support
static void VScanObject(Il2CppObject* obj, Il2CppClass* klass, const std::string& pathPrefix,
	int depth, VScanCtx& ctx)
{
	if (!obj || !klass || depth > VSCAN_MAX_DEPTH) return;
	if (ctx.shouldStop()) return;
	if (!ctx.visited.insert(obj).second) return; // O(1) cycle guard

	void* fIter = nullptr;
	while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
		if (ctx.shouldStop()) return;
		const Il2CppType* fType = il2cpp_field_get_type(f);
		if (!fType) continue;
		const char* fn = il2cpp_field_get_name(f);
		if (!fn) continue;
		if (fType->attrs & 0x0010) continue; // skip static fields in recursion (roots handle them)
		if (fType->attrs & 0x0040) continue; // skip const/literal

		int t = fType->type;
		ctx.totalScanned++;

		// --- Check primitive fields based on scan type ---
		if (ctx.scanType == VSCAN_INT) {
			int64_t v = 0; bool matched = false;
			switch (t) {
			case IL2CPP_TYPE_I1: { int8_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; break; }
			case IL2CPP_TYPE_U1: { uint8_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; break; }
			case IL2CPP_TYPE_I2: { int16_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; break; }
			case IL2CPP_TYPE_U2: { uint16_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; break; }
			case IL2CPP_TYPE_I4: { int32_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; break; }
			case IL2CPP_TYPE_U4: { uint32_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = (int64_t)x; matched = true; break; }
			case IL2CPP_TYPE_I8: { Resolver::GetFieldValue(obj, f, &v); matched = true; break; }
			case IL2CPP_TYPE_U8: { uint64_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = (int64_t)x; matched = true; break; }
			case IL2CPP_TYPE_VALUETYPE: {
				Il2CppClass* vc = il2cpp_class_from_type(fType);
				if (vc && vc->enumtype) { int32_t x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; }
				break;
			}
			}
			if (matched && VScanMatchInt(v, ctx.intA, ctx.intB, ctx.scanMode)) {
				std::string fieldPath = pathPrefix + "." + fn;
				ValueScanResult r; r.path = fieldPath; r.valueStr = std::to_string(v);
				r.obj = obj; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
				valueScanResults.push_back(r);
			}
		}
		else if (ctx.scanType == VSCAN_FLOAT) {
			double v = 0; bool matched = false;
			if (t == IL2CPP_TYPE_R4) { float x = 0; Resolver::GetFieldValue(obj, f, &x); v = x; matched = true; }
			else if (t == IL2CPP_TYPE_R8) { Resolver::GetFieldValue(obj, f, &v); matched = true; }
			if (matched && VScanMatchFloat(v, ctx.fltA, ctx.fltB, ctx.eps, ctx.scanMode)) {
				char buf[64]; sprintf_s(buf, "%.4f", v);
				std::string fieldPath = pathPrefix + "." + fn;
				ValueScanResult r; r.path = fieldPath; r.valueStr = buf;
				r.obj = obj; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
				valueScanResults.push_back(r);
			}
		}
		else if (ctx.scanType == VSCAN_BOOL && t == IL2CPP_TYPE_BOOLEAN) {
			bool v = false; Resolver::GetFieldValue(obj, f, &v);
			if (VScanMatchBool(v, ctx.boolTarget, ctx.scanMode)) {
				std::string fieldPath = pathPrefix + "." + fn;
				ValueScanResult r; r.path = fieldPath; r.valueStr = v ? "true" : "false";
				r.obj = obj; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
				valueScanResults.push_back(r);
			}
		}
		else if (ctx.scanType == VSCAN_STRING && t == IL2CPP_TYPE_STRING) {
			Il2CppString* s = nullptr; Resolver::GetFieldValue(obj, f, &s);
			if (s && s->chars) {
				std::string sv = il2cppi_to_string(s);
				if (VScanMatchString(sv, ctx.strTargetLower, ctx.scanMode)) {
					std::string fieldPath = pathPrefix + "." + fn;
					ValueScanResult r; r.path = fieldPath; r.valueStr = "\"" + sv + "\"";
					r.obj = obj; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
					valueScanResults.push_back(r);
				}
			}
		}

		// --- Recurse into reference fields ---
		if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_GENERICINST) {
			Il2CppObject* child = nullptr;
			Resolver::GetFieldValue(obj, f, &child);
			if (child && Resolver::Protection::IsValidIl2CppObject(child)) {
				Il2CppClass* childClass = il2cpp_object_get_class(child);
				if (childClass) {
					std::string fieldPath = pathPrefix + "." + fn;
					// Collections: only iterate if indexed by int (IList pattern)
					if (t == IL2CPP_TYPE_GENERICINST) {
						const MethodInfo* getCount = il2cpp_class_get_method_from_name(childClass, "get_Count", 0);
						const MethodInfo* getItem = il2cpp_class_get_method_from_name(childClass, "get_Item", 1);
						if (getCount && getItem) {
							// Add the collection object itself to visited to avoid re-iterating
							if (!ctx.visited.insert(child).second) continue;
							Il2CppObject* cntObj = Resolver::Protection::SafeRuntimeInvoke(getCount, child, nullptr);
							if (cntObj) {
								int cnt = *(int*)il2cpp_object_unbox(cntObj);
								int cap = (cnt > VSCAN_COLLECTION_CAP) ? VSCAN_COLLECTION_CAP : cnt;
								if (cap < 0) cap = 0;
								for (int ei = 0; ei < cap && !ctx.shouldStop(); ei++) {
									void* args[] = { &ei };
									Il2CppObject* elem = Resolver::Protection::SafeRuntimeInvoke(getItem, child, args);
									if (elem && Resolver::Protection::IsValidIl2CppObject(elem)) {
										Il2CppClass* ek = il2cpp_object_get_class(elem);
										if (ek) {
											char idx[16]; sprintf_s(idx, "[%d]", ei);
											VScanObject(elem, ek, fieldPath + idx, depth + 1, ctx);
										}
									}
								}
							}
							continue;
						}
					}
					VScanObject(child, childClass, fieldPath, depth + 1, ctx);
				}
			}
		}
		// --- Recurse into arrays ---
		else if (t == IL2CPP_TYPE_SZARRAY) {
			Il2CppArray* arr = nullptr;
			Resolver::GetFieldValue(obj, f, &arr);
			if (!arr || !Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)arr)) continue;
			// Add array itself to visited — prevents re-scanning same array from different paths
			if (!ctx.visited.insert(arr).second) continue;
			uint32_t len = il2cpp_array_length(arr);
			if (len == 0 || len > 0x1000000) continue;
			Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
			Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
			if (!elemClass) continue;
			const Il2CppType* et = il2cpp_class_get_type(elemClass);
			if (!et) continue;
			bool isRef = !il2cpp_class_is_valuetype(elemClass);

			// Skip arrays whose element type can't contain what we're looking for
			if (!isRef) {
				// Value type array: only useful for INT/FLOAT/BOOL scans with matching elem type
				bool useful = false;
				if (ctx.scanType == VSCAN_INT) {
					switch (et->type) { case IL2CPP_TYPE_I1: case IL2CPP_TYPE_U1: case IL2CPP_TYPE_I2:
					case IL2CPP_TYPE_U2: case IL2CPP_TYPE_I4: case IL2CPP_TYPE_U4:
					case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8: useful = true; }
				} else if (ctx.scanType == VSCAN_FLOAT) {
					if (et->type == IL2CPP_TYPE_R4 || et->type == IL2CPP_TYPE_R8) useful = true;
				} else if (ctx.scanType == VSCAN_BOOL) {
					if (et->type == IL2CPP_TYPE_BOOLEAN) useful = true;
				}
				// STRING scan: value type arrays can't hold strings — skip entirely
				if (!useful) continue;
			}

			int elemSize = isRef ? 0 : il2cpp_array_element_size(arrClass);
			uint32_t cap = (len > (uint32_t)VSCAN_ARRAY_CAP) ? (uint32_t)VSCAN_ARRAY_CAP : len;
			std::string fieldPath = pathPrefix + "." + fn;

			for (uint32_t ai = 0; ai < cap && !ctx.shouldStop(); ai++) {
				ctx.totalScanned++;
				if (isRef) {
					Il2CppObject* e = GET_ARRAY_ELEMENT(arr, ai);
					if (!e || !Resolver::Protection::IsValidIl2CppObject(e)) continue;
					if (ctx.scanType == VSCAN_STRING && et->type == IL2CPP_TYPE_STRING) {
						std::string sv = il2cppi_to_string((Il2CppString*)e);
						if (VScanMatchString(sv, ctx.strTargetLower, ctx.scanMode)) {
							char idx[16]; sprintf_s(idx, "[%d]", ai);
							ValueScanResult r; r.path = fieldPath + idx; r.valueStr = "\"" + sv + "\"";
							r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
							valueScanResults.push_back(r);
						}
					} else {
						Il2CppClass* ek = il2cpp_object_get_class(e);
						if (ek) { char idx[16]; sprintf_s(idx, "[%d]", ai); VScanObject(e, ek, fieldPath + idx, depth + 1, ctx); }
					}
				} else {
					char* dataBase = (char*)arr + 0x20;
					void* elemPtr = dataBase + (size_t)ai * elemSize;
					if (ctx.scanType == VSCAN_INT) {
						int64_t v = 0; bool m = false;
						switch (et->type) {
						case IL2CPP_TYPE_I1: v = *(int8_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_U1: v = *(uint8_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_I2: v = *(int16_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_U2: v = *(uint16_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_I4: v = *(int32_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_U4: v = (int64_t)*(uint32_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_I8: v = *(int64_t*)elemPtr; m = true; break;
						case IL2CPP_TYPE_U8: v = (int64_t)*(uint64_t*)elemPtr; m = true; break;
						default: break;
						}
						if (m && VScanMatchInt(v, ctx.intA, ctx.intB, ctx.scanMode)) {
							char idx[16]; sprintf_s(idx, "[%d]", ai);
							ValueScanResult r; r.path = fieldPath + idx; r.valueStr = std::to_string(v);
							r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
							valueScanResults.push_back(r);
						}
					} else if (ctx.scanType == VSCAN_FLOAT) {
						double v = 0; bool m = false;
						if (et->type == IL2CPP_TYPE_R4) { v = *(float*)elemPtr; m = true; }
						else if (et->type == IL2CPP_TYPE_R8) { v = *(double*)elemPtr; m = true; }
						if (m && VScanMatchFloat(v, ctx.fltA, ctx.fltB, ctx.eps, ctx.scanMode)) {
							char buf[64]; sprintf_s(buf, "%.4f", v);
							char idx[16]; sprintf_s(idx, "[%d]", ai);
							ValueScanResult r; r.path = fieldPath + idx; r.valueStr = buf;
							r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
							valueScanResults.push_back(r);
						}
					} else if (ctx.scanType == VSCAN_BOOL && et->type == IL2CPP_TYPE_BOOLEAN) {
						bool v = *(bool*)elemPtr;
						if (VScanMatchBool(v, ctx.boolTarget, ctx.scanMode)) {
							char idx[16]; sprintf_s(idx, "[%d]", ai);
							ValueScanResult r; r.path = fieldPath + idx; r.valueStr = v ? "true" : "false";
							r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
							valueScanResults.push_back(r);
						}
					}
				}
			}
		}
	}
}

// Entry point: iterates all assemblies, finds all static root objects, then recursively scans
static void RunValueScan() {
	VScanCtx ctx;
	ctx.scanType = vscanType;
	ctx.scanMode = vscanMode;
	ctx.eps = vscanFloatEpsilon;
	ctx.startTick = GetTickCount();
	ctx.visited.reserve(100000); // pre-allocate hash buckets

	if (vscanType == VSCAN_INT) {
		try { ctx.intA = std::stoll(vscanValue1); } catch (...) {}
		try { ctx.intB = std::stoll(vscanValue2); } catch (...) {}
	} else if (vscanType == VSCAN_FLOAT) {
		try { ctx.fltA = std::stod(vscanValue1); } catch (...) {}
		try { ctx.fltB = std::stod(vscanValue2); } catch (...) {}
	} else if (vscanType == VSCAN_BOOL) {
		std::string s(vscanValue1);
		for (auto& c : s) c = (char)tolower((unsigned char)c);
		ctx.boolTarget = (s == "true" || s == "1");
	} else if (vscanType == VSCAN_STRING) {
		ctx.strTargetLower = vscanValue1;
		for (auto& c : ctx.strTargetLower) c = (char)tolower((unsigned char)c);
	}

	vscanAbortReason.clear();
	il2cpp_thread_attach(il2cpp_domain_get());

	auto domain = il2cpp_domain_get();
	size_t asmCount = 0;
	const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);
	if (!assemblies) { vscanRunning = false; vscanDone = true; return; }

	vscanProgress = 0;
	vscanProgressMax = (int)asmCount;

	for (size_t a = 0; a < asmCount && !ctx.shouldStop(); a++) {
		vscanProgress = (int)a;
		const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[a]);
		if (!image) continue;
		size_t classCount = il2cpp_image_get_class_count(image);

		for (size_t c = 0; c < classCount && !ctx.shouldStop(); c++) {
			Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, c);
			if (!klass) continue;

			Resolver::Protection::safe_call([&]() {
				const char* cName = il2cpp_class_get_name(klass);
				const char* cNs = il2cpp_class_get_namespace(klass);
				std::string className = (cNs && cNs[0]) ? (std::string(cNs) + "." + cName) : (cName ? cName : "?");

				void* fIter = nullptr;
				while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
					if (ctx.shouldStop()) break;
					const Il2CppType* fType = il2cpp_field_get_type(f);
					if (!fType) continue;
					if (!(fType->attrs & 0x0010)) continue; // static fields only as roots
					if (fType->attrs & 0x0040) continue;

					int t = fType->type;
					const char* fn = il2cpp_field_get_name(f);
					std::string rootPath = className + "." + (fn ? fn : "?");
					ctx.totalScanned++;

					// Check static primitive fields directly
					if (ctx.scanType == VSCAN_INT) {
						int64_t v = 0; bool matched = false;
						switch (t) {
						case IL2CPP_TYPE_I4: { int32_t x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; break; }
						case IL2CPP_TYPE_U4: { uint32_t x = 0; il2cpp_field_static_get_value(f, &x); v = (int64_t)x; matched = true; break; }
						case IL2CPP_TYPE_I8: { il2cpp_field_static_get_value(f, &v); matched = true; break; }
						case IL2CPP_TYPE_U8: { uint64_t x = 0; il2cpp_field_static_get_value(f, &x); v = (int64_t)x; matched = true; break; }
						case IL2CPP_TYPE_I1: { int8_t x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; break; }
						case IL2CPP_TYPE_U1: { uint8_t x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; break; }
						case IL2CPP_TYPE_I2: { int16_t x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; break; }
						case IL2CPP_TYPE_U2: { uint16_t x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; break; }
						case IL2CPP_TYPE_VALUETYPE: {
							Il2CppClass* vc = il2cpp_class_from_type(fType);
							if (vc && vc->enumtype) { int32_t x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; }
							break;
						}
						}
						if (matched && VScanMatchInt(v, ctx.intA, ctx.intB, ctx.scanMode)) {
							ValueScanResult r; r.path = rootPath; r.valueStr = std::to_string(v);
							r.obj = nullptr; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
							valueScanResults.push_back(r);
						}
					}
					else if (ctx.scanType == VSCAN_FLOAT) {
						double v = 0; bool matched = false;
						if (t == IL2CPP_TYPE_R4) { float x = 0; il2cpp_field_static_get_value(f, &x); v = x; matched = true; }
						else if (t == IL2CPP_TYPE_R8) { il2cpp_field_static_get_value(f, &v); matched = true; }
						if (matched && VScanMatchFloat(v, ctx.fltA, ctx.fltB, ctx.eps, ctx.scanMode)) {
							char buf[64]; sprintf_s(buf, "%.4f", v);
							ValueScanResult r; r.path = rootPath; r.valueStr = buf;
							r.obj = nullptr; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
							valueScanResults.push_back(r);
						}
					}
					else if (ctx.scanType == VSCAN_BOOL && t == IL2CPP_TYPE_BOOLEAN) {
						bool v = false; il2cpp_field_static_get_value(f, &v);
						if (VScanMatchBool(v, ctx.boolTarget, ctx.scanMode)) {
							ValueScanResult r; r.path = rootPath; r.valueStr = v ? "true" : "false";
							r.obj = nullptr; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
							valueScanResults.push_back(r);
						}
					}
					else if (ctx.scanType == VSCAN_STRING && t == IL2CPP_TYPE_STRING) {
						Il2CppString* s = nullptr; il2cpp_field_static_get_value(f, &s);
						if (s && s->chars) {
							std::string sv = il2cppi_to_string(s);
							if (VScanMatchString(sv, ctx.strTargetLower, ctx.scanMode)) {
								ValueScanResult r; r.path = rootPath; r.valueStr = "\"" + sv + "\"";
								r.obj = nullptr; r.field = f; r.arrayIndex = -1; r.arrayObj = nullptr;
								valueScanResults.push_back(r);
							}
						}
					}

					// Recurse into static reference/array/collection fields
					if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_GENERICINST) {
						Il2CppObject* val = nullptr;
						il2cpp_field_static_get_value(f, &val);
						if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
							Il2CppClass* ck = il2cpp_object_get_class(val);
							if (ck) {
								if (t == IL2CPP_TYPE_GENERICINST) {
									const MethodInfo* gc = il2cpp_class_get_method_from_name(ck, "get_Count", 0);
									const MethodInfo* gi = il2cpp_class_get_method_from_name(ck, "get_Item", 1);
									if (gc && gi) {
										if (!ctx.visited.insert(val).second) continue;
										Il2CppObject* co = Resolver::Protection::SafeRuntimeInvoke(gc, val, nullptr);
										if (co) {
											int cnt = *(int*)il2cpp_object_unbox(co);
											int cap2 = (cnt > VSCAN_COLLECTION_CAP) ? VSCAN_COLLECTION_CAP : cnt;
											if (cap2 < 0) cap2 = 0;
											for (int ei = 0; ei < cap2 && !ctx.shouldStop(); ei++) {
												void* args[] = { &ei };
												Il2CppObject* elem = Resolver::Protection::SafeRuntimeInvoke(gi, val, args);
												if (elem && Resolver::Protection::IsValidIl2CppObject(elem)) {
													Il2CppClass* ek = il2cpp_object_get_class(elem);
													if (ek) { char idx[16]; sprintf_s(idx, "[%d]", ei);
														VScanObject(elem, ek, rootPath + idx, 1, ctx); }
												}
											}
										}
										continue;
									}
								}
								VScanObject(val, ck, rootPath, 1, ctx);
							}
						}
					}
					else if (t == IL2CPP_TYPE_SZARRAY) {
						Il2CppArray* arr = nullptr;
						il2cpp_field_static_get_value(f, &arr);
						if (!arr || !Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)arr)) continue;
						if (!ctx.visited.insert(arr).second) continue;
						uint32_t len = il2cpp_array_length(arr);
						if (len == 0 || len > 0x1000000) continue;
						Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
						Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
						if (!elemClass) continue;
						const Il2CppType* et = il2cpp_class_get_type(elemClass);
						if (!et) continue;
						bool isRef = !il2cpp_class_is_valuetype(elemClass);
						int elemSize = isRef ? 0 : il2cpp_array_element_size(arrClass);
						uint32_t cap2 = (len > (uint32_t)VSCAN_ARRAY_CAP) ? (uint32_t)VSCAN_ARRAY_CAP : len;

						for (uint32_t ai = 0; ai < cap2 && !ctx.shouldStop(); ai++) {
							ctx.totalScanned++;
							if (isRef) {
								Il2CppObject* e = GET_ARRAY_ELEMENT(arr, ai);
								if (!e || !Resolver::Protection::IsValidIl2CppObject(e)) continue;
								if (ctx.scanType == VSCAN_STRING && et->type == IL2CPP_TYPE_STRING) {
									std::string sv = il2cppi_to_string((Il2CppString*)e);
									if (VScanMatchString(sv, ctx.strTargetLower, ctx.scanMode)) {
										char idx[16]; sprintf_s(idx, "[%d]", ai);
										ValueScanResult r; r.path = rootPath + idx; r.valueStr = "\"" + sv + "\"";
										r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
										valueScanResults.push_back(r);
									}
								} else {
									Il2CppClass* ek = il2cpp_object_get_class(e);
									if (ek) { char idx[16]; sprintf_s(idx, "[%d]", ai);
										VScanObject(e, ek, rootPath + idx, 1, ctx); }
								}
							} else {
								char* dataBase = (char*)arr + 0x20;
								void* elemPtr = dataBase + (size_t)ai * elemSize;
								if (ctx.scanType == VSCAN_INT) {
									int64_t v = 0; bool m = false;
									switch (et->type) {
									case IL2CPP_TYPE_I4: v = *(int32_t*)elemPtr; m = true; break;
									case IL2CPP_TYPE_U4: v = (int64_t)*(uint32_t*)elemPtr; m = true; break;
									case IL2CPP_TYPE_I8: v = *(int64_t*)elemPtr; m = true; break;
									case IL2CPP_TYPE_U8: v = (int64_t)*(uint64_t*)elemPtr; m = true; break;
									case IL2CPP_TYPE_I1: v = *(int8_t*)elemPtr; m = true; break;
									case IL2CPP_TYPE_U1: v = *(uint8_t*)elemPtr; m = true; break;
									default: break;
									}
									if (m && VScanMatchInt(v, ctx.intA, ctx.intB, ctx.scanMode)) {
										char idx[16]; sprintf_s(idx, "[%d]", ai);
										ValueScanResult r; r.path = rootPath + idx; r.valueStr = std::to_string(v);
										r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
										valueScanResults.push_back(r);
									}
								} else if (ctx.scanType == VSCAN_FLOAT) {
									double v = 0; bool m = false;
									if (et->type == IL2CPP_TYPE_R4) { v = *(float*)elemPtr; m = true; }
									else if (et->type == IL2CPP_TYPE_R8) { v = *(double*)elemPtr; m = true; }
									if (m && VScanMatchFloat(v, ctx.fltA, ctx.fltB, ctx.eps, ctx.scanMode)) {
										char buf[64]; sprintf_s(buf, "%.4f", v);
										char idx[16]; sprintf_s(idx, "[%d]", ai);
										ValueScanResult r; r.path = rootPath + idx; r.valueStr = buf;
										r.obj = nullptr; r.field = nullptr; r.arrayIndex = (int)ai; r.arrayObj = (Il2CppObject*)arr;
										valueScanResults.push_back(r);
									}
								}
							}
						}
					}
				}
			});
		}
	}

	// Publish final state
	vscanTotalScanned = ctx.totalScanned;
	const char* reason = ctx.stopReason();
	vscanAbortReason = reason ? reason : "";
	vscanRunning = false;
	vscanDone = true;
	vscanCount = 1;

	for (auto& r : valueScanResults) {
		try {
			if (vscanType == VSCAN_INT) r.numericVal = (double)std::stoll(r.valueStr);
			else if (vscanType == VSCAN_FLOAT) r.numericVal = std::stod(r.valueStr);
			else if (vscanType == VSCAN_BOOL) r.numericVal = (r.valueStr == "true") ? 1.0 : 0.0;
			else r.numericVal = 0;
		} catch (...) { r.numericVal = 0; }
	}
}

// --- Next Scan: re-read current results and filter by comparison to previous values ---
static void RunNextScan() {
	if (valueScanResults.empty()) { vscanRunning = false; return; }
	// vscanRunning already set by caller

	il2cpp_thread_attach(il2cpp_domain_get());

	vscanProgress = 0;
	vscanProgressMax = (int)valueScanResults.size();

	// Parse new target value for NSCAN_EXACT
	int64_t intA = 0; double fltA = 0; bool boolTarget = false; std::string strTarget;
	if (nscanMode == NSCAN_EXACT) {
		if (vscanType == VSCAN_INT) { try { intA = std::stoll(vscanValue1); } catch (...) {} }
		else if (vscanType == VSCAN_FLOAT) { try { fltA = std::stod(vscanValue1); } catch (...) {} }
		else if (vscanType == VSCAN_BOOL) {
			std::string s(vscanValue1);
			std::transform(s.begin(), s.end(), s.begin(), ::tolower);
			boolTarget = (s == "true" || s == "1");
		}
		else if (vscanType == VSCAN_STRING) { strTarget = vscanValue1; }
	}
	// Pre-lowercase string target for VScanMatchString
	std::string strTargetLower = strTarget;
	for (auto& c : strTargetLower) c = (char)tolower((unsigned char)c);

	std::vector<ValueScanResult> filtered;
	double eps = vscanFloatEpsilon;

	for (int ri = 0; ri < (int)valueScanResults.size(); ri++) {
		vscanProgress = ri;
		auto& r = valueScanResults[ri];
		double oldVal = r.numericVal;
		double newVal = 0;
		std::string newStr;
		bool readable = false;

		// Re-read the value
		Resolver::Protection::safe_call([&]() {
			if (r.field) {
				// Field on an object (or static)
				const Il2CppType* fType = il2cpp_field_get_type(r.field);
				if (!fType) return;
				bool isStatic = (fType->attrs & 0x0010) != 0;
				Il2CppObject* target = isStatic ? nullptr : r.obj;
				if (!isStatic && (!target || !Resolver::Protection::IsValidIl2CppObject(target))) return;

				int t = fType->type;
				if (vscanType == VSCAN_INT) {
					int64_t v = 0;
					switch (t) {
					case IL2CPP_TYPE_I1: { int8_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; break; }
					case IL2CPP_TYPE_U1: { uint8_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; break; }
					case IL2CPP_TYPE_I2: { int16_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; break; }
					case IL2CPP_TYPE_U2: { uint16_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; break; }
					case IL2CPP_TYPE_I4: { int32_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; break; }
					case IL2CPP_TYPE_U4: { uint32_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = (int64_t)x; break; }
					case IL2CPP_TYPE_I8: { Resolver::GetFieldValue(target, r.field, &v); break; }
					case IL2CPP_TYPE_U8: { uint64_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = (int64_t)x; break; }
					case IL2CPP_TYPE_VALUETYPE: {
						Il2CppClass* vc = il2cpp_class_from_type(fType);
						if (vc && vc->enumtype) { int32_t x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; }
						break;
					}
					default: return;
					}
					newVal = (double)v;
					newStr = std::to_string(v);
					readable = true;
				}
				else if (vscanType == VSCAN_FLOAT) {
					double v = 0;
					if (t == IL2CPP_TYPE_R4) { float x = 0; Resolver::GetFieldValue(target, r.field, &x); v = x; }
					else if (t == IL2CPP_TYPE_R8) { Resolver::GetFieldValue(target, r.field, &v); }
					else return;
					newVal = v;
					char buf[64]; sprintf_s(buf, "%.4f", v);
					newStr = buf;
					readable = true;
				}
				else if (vscanType == VSCAN_BOOL) {
					if (t != IL2CPP_TYPE_BOOLEAN) return;
					bool v = false; Resolver::GetFieldValue(target, r.field, &v);
					newVal = v ? 1.0 : 0.0;
					newStr = v ? "true" : "false";
					readable = true;
				}
				else if (vscanType == VSCAN_STRING) {
					if (t != IL2CPP_TYPE_STRING) return;
					Il2CppString* s = nullptr; Resolver::GetFieldValue(target, r.field, &s);
					newStr = (s && s->chars) ? ("\"" + il2cppi_to_string(s) + "\"") : "null";
					readable = true;
				}
			}
			else if (r.arrayIndex >= 0 && r.arrayObj) {
				// Array element
				if (!Resolver::Protection::IsValidIl2CppObject(r.arrayObj)) return;
				Il2CppArray* arr = (Il2CppArray*)r.arrayObj;
				uint32_t len = il2cpp_array_length(arr);
				if ((uint32_t)r.arrayIndex >= len) return;
				Il2CppClass* arrClass = il2cpp_object_get_class(r.arrayObj);
				Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
				if (!elemClass) return;
				bool isRef = !il2cpp_class_is_valuetype(elemClass);

				if (isRef) {
					Il2CppObject* e = GET_ARRAY_ELEMENT(arr, r.arrayIndex);
					if (vscanType == VSCAN_STRING) {
						newStr = (e) ? ("\"" + il2cppi_to_string((Il2CppString*)e) + "\"") : "null";
						readable = true;
					}
					// For ref-type array elements that are objects, we can't easily re-read a specific field
					// The path would need further resolution — skip for now
				}
				else {
					const Il2CppType* et = il2cpp_class_get_type(elemClass);
					int elemSize = il2cpp_array_element_size(arrClass);
					char* dataBase = (char*)arr + 0x20;
					void* elemPtr = dataBase + (size_t)r.arrayIndex * elemSize;

					if (vscanType == VSCAN_INT && et) {
						int64_t v = 0;
						switch (et->type) {
						case IL2CPP_TYPE_I1: v = *(int8_t*)elemPtr; break;
						case IL2CPP_TYPE_U1: v = *(uint8_t*)elemPtr; break;
						case IL2CPP_TYPE_I2: v = *(int16_t*)elemPtr; break;
						case IL2CPP_TYPE_U2: v = *(uint16_t*)elemPtr; break;
						case IL2CPP_TYPE_I4: v = *(int32_t*)elemPtr; break;
						case IL2CPP_TYPE_U4: v = (int64_t)*(uint32_t*)elemPtr; break;
						case IL2CPP_TYPE_I8: v = *(int64_t*)elemPtr; break;
						case IL2CPP_TYPE_U8: v = (int64_t)*(uint64_t*)elemPtr; break;
						default: return;
						}
						newVal = (double)v; newStr = std::to_string(v); readable = true;
					}
					else if (vscanType == VSCAN_FLOAT && et) {
						double v = 0;
						if (et->type == IL2CPP_TYPE_R4) v = *(float*)elemPtr;
						else if (et->type == IL2CPP_TYPE_R8) v = *(double*)elemPtr;
						else return;
						newVal = v; char buf[64]; sprintf_s(buf, "%.4f", v); newStr = buf; readable = true;
					}
					else if (vscanType == VSCAN_BOOL && et && et->type == IL2CPP_TYPE_BOOLEAN) {
						bool v = *(bool*)elemPtr;
						newVal = v ? 1.0 : 0.0; newStr = v ? "true" : "false"; readable = true;
					}
				}
			}
		});

		if (!readable) continue;

		// Apply comparison filter
		bool pass = false;
		switch (nscanMode) {
		case NSCAN_EXACT:
			if (vscanType == VSCAN_INT) pass = ((int64_t)newVal == intA);
			else if (vscanType == VSCAN_FLOAT) pass = (fabs(newVal - fltA) <= eps);
			else if (vscanType == VSCAN_BOOL) pass = ((newVal != 0) == boolTarget);
			else if (vscanType == VSCAN_STRING) {
				std::string plain = newStr;
				if (plain.size() >= 2 && plain.front() == '"' && plain.back() == '"')
					plain = plain.substr(1, plain.size() - 2);
				int strMode = (nscanMode == NSCAN_CONTAINS) ? VMODE_CONTAINS : VMODE_EXACT;
				pass = VScanMatchString(plain, strTargetLower, strMode);
			}
			break;
		case NSCAN_CHANGED:
			if (vscanType == VSCAN_STRING) pass = (newStr != r.valueStr);
			else pass = (fabs(newVal - oldVal) > eps);
			break;
		case NSCAN_UNCHANGED:
			if (vscanType == VSCAN_STRING) pass = (newStr == r.valueStr);
			else pass = (fabs(newVal - oldVal) <= eps);
			break;
		case NSCAN_INCREASED:
			pass = (newVal > oldVal + eps);
			break;
		case NSCAN_DECREASED:
			pass = (newVal < oldVal - eps);
			break;
		case NSCAN_CONTAINS:
			if (vscanType == VSCAN_STRING) {
				std::string plain = newStr;
				if (plain.size() >= 2 && plain.front() == '"' && plain.back() == '"')
					plain = plain.substr(1, plain.size() - 2);
				pass = VScanMatchString(plain, strTargetLower, VMODE_CONTAINS);
			}
			break;
		}

		if (pass) {
			ValueScanResult nr = r;
			nr.prevStr = r.valueStr;
			nr.valueStr = newStr;
			nr.numericVal = newVal;
			filtered.push_back(std::move(nr));
		}
	}

	valueScanResults = std::move(filtered);
	vscanRunning = false;
	vscanCount++;
}

static std::string ResolveEnumName(Il2CppClass* enumClass, int32_t value) {
	if (!enumClass || !enumClass->enumtype) return "";

	// Check cache first
	auto classIt = enumNameCache.find(enumClass);
	if (classIt != enumNameCache.end()) {
		auto valIt = classIt->second.find(value);
		return (valIt != classIt->second.end()) ? valIt->second : "";
	}

	// Build cache for this enum class
	auto& cache = enumNameCache[enumClass];
	Resolver::Protection::safe_call([&]() {
		void* fIter = nullptr;
		while (FieldInfo* f = il2cpp_class_get_fields(enumClass, &fIter)) {
			if (!(f->type->attrs & 0x0010)) continue; // must be static
			if (!(f->type->attrs & 0x0040)) continue; // must be literal
			int32_t fVal = 0;
			il2cpp_field_static_get_value(f, &fVal);
			cache[fVal] = il2cpp_field_get_name(f);
		}
	});

	auto valIt = cache.find(value);
	return (valIt != cache.end()) ? valIt->second : "";
}

// Helper: draw array elements in a collapsible list
static void DrawArrayElements(Il2CppArray* arr, Il2CppClass* elemClass) {
	if (!arr || !elemClass) { ImGui::TextDisabled("null"); return; }

	uint32_t len = 0;
	Resolver::Protection::safe_call([&]() { len = il2cpp_array_length(arr); });

	const Il2CppType* elemType = nullptr;
	int elemTypeEnum = 0;
	bool isRefType = true;
	int elemSize = 0;

	Resolver::Protection::safe_call([&]() {
		elemType = il2cpp_class_get_type(elemClass);
		elemTypeEnum = elemType->type;
		isRefType = !il2cpp_class_is_valuetype(elemClass);
		if (!isRefType) {
			Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
			if (arrClass) elemSize = il2cpp_array_element_size(arrClass);
		}
	});

	const uint32_t MAX_DISPLAY = 200;
	uint32_t displayCount = (len < MAX_DISPLAY) ? len : MAX_DISPLAY;

	for (uint32_t i = 0; i < displayCount; i++) {
		ImGui::PushID((int)i);

		if (isRefType) {
			// Reference type elements (objects, strings, arrays, generics)
			Il2CppObject* elem = nullptr;
			Resolver::Protection::safe_call([&]() { elem = GET_ARRAY_ELEMENT(arr, i); });

			if (!elem) {
				ImGui::TextDisabled("[%d] null", i);
			}
			else if (elemTypeEnum == IL2CPP_TYPE_STRING) {
				// Editable string array element
				std::string cur;
				Resolver::Protection::safe_call([&]() {
					cur = il2cppi_to_string((Il2CppString*)elem);
				});
				char buf[256];
				strncpy_s(buf, cur.c_str(), sizeof(buf) - 1);
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10);
				if (ImGui::InputText("##str", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
					Resolver::Protection::safe_call([&]() {
						Il2CppString* newStr = il2cpp_string_new(buf);
						if (newStr) {
							Il2CppObject** refSlot = ((Il2CppObject**)((char*)(arr) + 0x20));
							refSlot[i] = (Il2CppObject*)newStr;
						}
					});
				}
			}
			else {
				const char* cName = "???";
				Resolver::Protection::safe_call([&]() {
					Il2CppClass* objClass = il2cpp_object_get_class(elem);
					if (objClass) cName = il2cpp_class_get_name(objClass);
				});
				if (ImGui::SmallButton("Inspect"))
					UnityExplorerTAB::Helpers::InspectObject(elem);
				ImGui::SameLine();
				ImGui::TextDisabled("[%d] %s @ 0x%p", i, cName, elem);
			}
		}
		else if (elemSize > 0) {
			// Value type elements - inline data (editable)
			char* dataBase = (char*)arr + 0x20;
			void* elemPtr = dataBase + (size_t)i * elemSize;

			switch (elemTypeEnum) {
			case IL2CPP_TYPE_BOOLEAN: {
				bool val = *(bool*)elemPtr;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				if (ImGui::Checkbox("##v", &val)) *(bool*)elemPtr = val;
				break;
			}
			case IL2CPP_TYPE_I1: {
				int display = (int)*(int8_t*)elemPtr;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(100);
				if (ImGui::DragInt("##v", &display, 1.0f, -128, 127)) *(int8_t*)elemPtr = (int8_t)display;
				break;
			}
			case IL2CPP_TYPE_U1: {
				int display = (int)*(uint8_t*)elemPtr;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(100);
				if (ImGui::DragInt("##v", &display, 1.0f, 0, 255)) *(uint8_t*)elemPtr = (uint8_t)display;
				break;
			}
			case IL2CPP_TYPE_CHAR: {
				uint16_t ch = *(uint16_t*)elemPtr;
				int display = (int)ch;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(100);
				if (ImGui::DragInt("##v", &display, 1.0f, 0, 65535)) *(uint16_t*)elemPtr = (uint16_t)display;
				if (ch >= 32 && ch < 127) { ImGui::SameLine(); ImGui::TextDisabled("'%c'", (char)ch); }
				break;
			}
			case IL2CPP_TYPE_I2: {
				int display = (int)*(int16_t*)elemPtr;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(100);
				if (ImGui::DragInt("##v", &display, 1.0f, -32768, 32767)) *(int16_t*)elemPtr = (int16_t)display;
				break;
			}
			case IL2CPP_TYPE_U2: {
				int display = (int)*(uint16_t*)elemPtr;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(100);
				if (ImGui::DragInt("##v", &display, 1.0f, 0, 65535)) *(uint16_t*)elemPtr = (uint16_t)display;
				break;
			}
			case IL2CPP_TYPE_I4: {
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				ImGui::DragInt("##v", (int32_t*)elemPtr);
				break;
			}
			case IL2CPP_TYPE_U4: {
				int display = (int)*(uint32_t*)elemPtr;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::DragInt("##v", &display)) *(uint32_t*)elemPtr = (uint32_t)display;
				break;
			}
			case IL2CPP_TYPE_I8: {
				int64_t val = *(int64_t*)elemPtr;
				char buf[64]; sprintf_s(buf, "%lld", val);
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(160);
				if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
					try { *(int64_t*)elemPtr = std::stoll(buf); } catch (...) {}
				}
				break;
			}
			case IL2CPP_TYPE_U8: {
				uint64_t val = *(uint64_t*)elemPtr;
				char buf[64]; sprintf_s(buf, "%llu", val);
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(160);
				if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
					try { *(uint64_t*)elemPtr = std::stoull(buf); } catch (...) {}
				}
				break;
			}
			case IL2CPP_TYPE_R4: {
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				ImGui::DragFloat("##v", (float*)elemPtr, 0.05f);
				break;
			}
			case IL2CPP_TYPE_R8: {
				double val = *(double*)elemPtr;
				float fval = (float)val;
				ImGui::Text("[%d]", i); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::DragFloat("##v", &fval, 0.05f)) *(double*)elemPtr = (double)fval;
				ImGui::SameLine(); ImGui::TextDisabled("(%.6f)", val);
				break;
			}
			case IL2CPP_TYPE_VALUETYPE: {
				std::string sName = elemClass->name;
				if (sName == "Vector3") {
					float* v = (float*)elemPtr;
					ImGui::Text("[%d]", i); ImGui::SameLine();
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10);
					ImGui::DragFloat3("##v3", v, 0.1f);
				}
				else if (sName == "Vector2") {
					float* v = (float*)elemPtr;
					ImGui::Text("[%d]", i); ImGui::SameLine();
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10);
					ImGui::DragFloat2("##v2", v, 0.1f);
				}
				else if (elemClass->enumtype) {
					int32_t* val = (int32_t*)elemPtr;
					std::string eName = ResolveEnumName(elemClass, *val);
					ImGui::Text("[%d]", i); ImGui::SameLine();
					ImGui::SetNextItemWidth(100);
					ImGui::DragInt("##v", val);
					if (!eName.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", eName.c_str()); }
				}
				else {
					if (ImGui::SmallButton("Browse##ast"))
						UnityExplorerTAB::Helpers::InspectClass(elemClass);
					ImGui::SameLine();
					ImGui::TextDisabled("[%d] {%s}", i, sName.c_str());
				}
				break;
			}
			default:
				ImGui::TextDisabled("[%d] ?", i);
				break;
			}
		}
		else {
			ImGui::TextDisabled("[%d] ?", i);
		}

		ImGui::PopID();
	}

	if (len > displayCount) {
		ImGui::TextDisabled("... and %d more elements", len - displayCount);
	}
}

void UnityExplorerTAB::Helpers::DrawField(Il2CppObject* obj, FieldInfo* field)
{
	if (!field) return;

	const char* fieldName = il2cpp_field_get_name(field);
	const Il2CppType* type = il2cpp_field_get_type(field);
	int typeEnum = type->type;
	bool isStatic = (type->attrs & 0x0010);
	bool hasValueAccess = (obj != nullptr) || isStatic;

	ImGui::PushID(field);

	ImGui::Columns(2, "FieldCols", true);
	static bool initWidth = true;
	if (initWidth) {
		ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.45f);
		initWidth = false;
	}

	char* typeName = il2cpp_type_get_name(type);

	// Make type name clickable for browsable class/struct types
	Il2CppClass* browseClass = nullptr;
	if (typeEnum == IL2CPP_TYPE_CLASS || typeEnum == IL2CPP_TYPE_OBJECT ||
		typeEnum == IL2CPP_TYPE_VALUETYPE || typeEnum == IL2CPP_TYPE_GENERICINST ||
		typeEnum == IL2CPP_TYPE_SZARRAY) {
		Resolver::Protection::safe_call([&]() {
			if (typeEnum == IL2CPP_TYPE_SZARRAY)
				browseClass = il2cpp_type_get_class_or_element_class(type);
			else
				browseClass = il2cpp_class_from_type(type);
		});
	}

	if (browseClass) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.5f, 0.5f));
		char tLabel[256];
		sprintf_s(tLabel, "[%s]##ftype", typeName ? typeName : "???");
		if (ImGui::SmallButton(tLabel)) InspectClass(browseClass);
		ImGui::PopStyleColor(2);
	}
	else {
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[%s]", typeName ? typeName : "???");
	}
	if (typeName) il2cpp_free(typeName);

	ImGui::SameLine();
	if (isStatic) ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "(S) %s", fieldName);
	else ImGui::Text("%s", fieldName);

	// Watch pin button
	ImGui::SameLine();
	if (ImGui::SmallButton("+W##watch")) {
		std::string ns = "";
		std::string cn = "";
		Resolver::Protection::safe_call([&]() {
			Il2CppClass* fk = il2cpp_field_get_parent(field);
			if (fk) {
				const char* n = il2cpp_class_get_name(fk);
				const char* s = il2cpp_class_get_namespace(fk);
				cn = n ? n : "?";
				ns = s ? s : "";
			}
		});
		WatchEntry w;
		w.obj = obj;
		w.field = field;
		w.isStatic = isStatic;
		w.label = (ns.empty() ? cn : ns + "." + cn) + "::" + fieldName;
		watchList.push_back(w);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pin to Watchlist");

	// Snapshot diff indicator
	if (hasSnapshot && snapshotMap.count(field)) {
		std::string cur = ReadFieldValueAsString(obj, field);
		if (cur != snapshotMap[field]) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 1, 0, 1), "*");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Changed: was %s", snapshotMap[field].c_str());
		}
	}

	ImGui::NextColumn();
	ImGui::PushItemWidth(-1);

	if (!hasValueAccess) {
		ImGui::TextDisabled("null (Instance only)");
		ImGui::PopItemWidth();
		ImGui::Columns(1);
		ImGui::PopID();
		return;
	}

	switch (typeEnum) {
	case IL2CPP_TYPE_BOOLEAN: {
		bool val = false;
		Resolver::GetFieldValue(obj, field, &val);
		if (ImGui::Checkbox("##v", &val)) {
			if (isStatic) il2cpp_field_static_set_value(field, &val);
			else il2cpp_field_set_value(obj, field, &val);
		}
		break;
	}
	case IL2CPP_TYPE_CHAR: {
		uint16_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		int display = val;
		if (ImGui::DragInt("##v", &display, 1.0f, 0, 65535)) {
			uint16_t set = (uint16_t)display;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		ImGui::SameLine();
		if (val >= 32 && val < 127) ImGui::TextDisabled("'%c'", (char)val);
		break;
	}
	case IL2CPP_TYPE_I1: {
		int8_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		int display = val;
		if (ImGui::DragInt("##v", &display, 1.0f, -128, 127)) {
			int8_t set = (int8_t)display;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		break;
	}
	case IL2CPP_TYPE_U1: {
		uint8_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		int display = val;
		if (ImGui::DragInt("##v", &display, 1.0f, 0, 255)) {
			uint8_t set = (uint8_t)display;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		break;
	}
	case IL2CPP_TYPE_I2: {
		int16_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		int display = val;
		if (ImGui::DragInt("##v", &display, 1.0f, -32768, 32767)) {
			int16_t set = (int16_t)display;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		break;
	}
	case IL2CPP_TYPE_U2: {
		uint16_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		int display = val;
		if (ImGui::DragInt("##v", &display, 1.0f, 0, 65535)) {
			uint16_t set = (uint16_t)display;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		break;
	}
	case IL2CPP_TYPE_I4: {
		int32_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		if (ImGui::DragInt("##v", &val)) {
			if (isStatic) il2cpp_field_static_set_value(field, &val);
			else il2cpp_field_set_value(obj, field, &val);
		}
		break;
	}
	case IL2CPP_TYPE_U4: {
		uint32_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display)) {
			uint32_t set = (uint32_t)display;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		break;
	}
	case IL2CPP_TYPE_I8: {
		int64_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		char buf[64]; sprintf_s(buf, "%lld", val);
		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
			try {
				int64_t set = std::stoll(buf);
				if (isStatic) il2cpp_field_static_set_value(field, &set);
				else il2cpp_field_set_value(obj, field, &set);
			} catch (...) {}
		}
		break;
	}
	case IL2CPP_TYPE_U8: {
		uint64_t val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		char buf[64]; sprintf_s(buf, "%llu", val);
		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
			try {
				uint64_t set = std::stoull(buf);
				if (isStatic) il2cpp_field_static_set_value(field, &set);
				else il2cpp_field_set_value(obj, field, &set);
			} catch (...) {}
		}
		break;
	}
	case IL2CPP_TYPE_R4: {
		float val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		if (ImGui::DragFloat("##v", &val, 0.05f)) {
			if (isStatic) il2cpp_field_static_set_value(field, &val);
			else il2cpp_field_set_value(obj, field, &val);
		}
		break;
	}
	case IL2CPP_TYPE_R8: {
		double val = 0;
		Resolver::GetFieldValue(obj, field, &val);
		float fval = (float)val;
		if (ImGui::DragFloat("##v", &fval, 0.05f)) {
			double set = (double)fval;
			if (isStatic) il2cpp_field_static_set_value(field, &set);
			else il2cpp_field_set_value(obj, field, &set);
		}
		ImGui::SameLine(); ImGui::TextDisabled("(%.6f)", val);
		break;
	}
	case IL2CPP_TYPE_VALUETYPE: {
		Il2CppClass* structKlass = il2cpp_class_from_type(type);
		if (!structKlass) break;

		// Handle enums
		if (structKlass->enumtype) {
			int32_t val = 0;
			Resolver::GetFieldValue(obj, field, &val);
			std::string eName = ResolveEnumName(structKlass, val);
			if (ImGui::DragInt("##v", &val)) {
				if (isStatic) il2cpp_field_static_set_value(field, &val);
				else il2cpp_field_set_value(obj, field, &val);
			}
			if (!eName.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", eName.c_str());
			}
			break;
		}

		std::string sName = structKlass->name;
		if (sName == "Vector3") {
			app::Vector3 v; Resolver::GetFieldValue(obj, field, &v);
			if (ImGui::DragFloat3("##v3", &v.x, 0.1f)) {
				if (isStatic) il2cpp_field_static_set_value(field, &v);
				else il2cpp_field_set_value(obj, field, &v);
			}
		}
		else if (sName == "Vector2") {
			float v[2]; Resolver::GetFieldValue(obj, field, &v);
			if (ImGui::DragFloat2("##v2", v, 0.1f)) {
				if (isStatic) il2cpp_field_static_set_value(field, &v);
				else il2cpp_field_set_value(obj, field, &v);
			}
		}
		else if (sName == "Vector4" || sName == "Quaternion") {
			float v[4]; Resolver::GetFieldValue(obj, field, &v);
			if (ImGui::DragFloat4("##v4", v, 0.1f)) {
				if (isStatic) il2cpp_field_static_set_value(field, &v);
				else il2cpp_field_set_value(obj, field, &v);
			}
		}
		else if (sName == "Color" || sName == "Color32") {
			float c[4]; Resolver::GetFieldValue(obj, field, &c);
			if (ImGui::ColorEdit4("##clr", c)) {
				if (isStatic) il2cpp_field_static_set_value(field, &c);
				else il2cpp_field_set_value(obj, field, &c);
			}
		}
		else {
			if (ImGui::SmallButton("Browse##st")) InspectClass(structKlass);
			ImGui::SameLine();
			ImGui::TextDisabled("{%s}", sName.c_str());
		}
		break;
	}
	case IL2CPP_TYPE_STRING: {
		Il2CppString* strObj = nullptr;
		Resolver::GetFieldValue(obj, field, &strObj);
		if (strObj && strObj->chars) {
			std::string cur = il2cppi_to_string(strObj);
			char buf[256];
			strncpy_s(buf, cur.c_str(), sizeof(buf) - 1);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
				Resolver::Protection::safe_call([&]() {
					Il2CppString* newStr = il2cpp_string_new(buf);
					if (newStr) {
						if (isStatic) il2cpp_field_static_set_value(field, &newStr);
						else il2cpp_field_set_value(obj, field, &newStr);
					}
				});
			}
		}
		else {
			ImGui::TextDisabled("null");
			ImGui::SameLine();
			if (ImGui::SmallButton("Set##str")) {
				Resolver::Protection::safe_call([&]() {
					Il2CppString* newStr = il2cpp_string_new("");
					if (newStr) {
						if (isStatic) il2cpp_field_static_set_value(field, &newStr);
						else il2cpp_field_set_value(obj, field, &newStr);
					}
				});
			}
		}
		break;
	}
	case IL2CPP_TYPE_SZARRAY: {
		Il2CppArray* arr = nullptr;
		Resolver::GetFieldValue(obj, field, &arr);
		if (!arr) {
			ImGui::TextDisabled("null");
			break;
		}

		uint32_t len = 0;
		Il2CppClass* elemClass = nullptr;
		const char* elemName = "?";
		Resolver::Protection::safe_call([&]() {
			len = il2cpp_array_length(arr);
			Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
			if (arrClass) elemClass = il2cpp_class_get_element_class(arrClass);
			if (elemClass) elemName = il2cpp_class_get_name(elemClass);
		});

		char treeLabel[128];
		sprintf_s(treeLabel, "%s[%d]##arr", elemName, len);
		if (ImGui::TreeNode(treeLabel)) {
			if (elemClass) {
				Resolver::Protection::safe_call([&]() {
					DrawArrayElements(arr, elemClass);
				});
			}
			else {
				ImGui::TextDisabled("Could not resolve element type");
			}
			ImGui::TreePop();
		}
		break;
	}
	case IL2CPP_TYPE_CLASS:
	case IL2CPP_TYPE_OBJECT:
	case IL2CPP_TYPE_GENERICINST: {
		Il2CppObject* val = nullptr;
		Resolver::GetFieldValue(obj, field, &val);
		if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
			if (ImGui::SmallButton("Inspect")) InspectObject(val);
			ImGui::SameLine();
			if (ImGui::SmallButton("Copy")) {
				char buf[64]; sprintf_s(buf, "0x%p", val);
				Resolver::Helpers::CopyToClipboard(buf);
			}
			ImGui::SameLine();

			// Try to detect and display containers (List<T>, Dictionary<K,V>, HashSet<T>, etc.)
			bool drewContainer = false;
			if (typeEnum == IL2CPP_TYPE_GENERICINST) {
				Resolver::Protection::safe_call([&]() {
					Il2CppClass* valClass = il2cpp_object_get_class(val);
					if (!valClass) return;
					std::string cName = il2cpp_class_get_name(valClass);

					// get_Count works for List<T>, Dictionary<K,V>, HashSet<T>, Queue<T>, Stack<T>
					const MethodInfo* getCount = il2cpp_class_get_method_from_name(valClass, "get_Count", 0);
					if (!getCount) return;
					Il2CppObject* countObj = Resolver::Protection::SafeRuntimeInvoke(getCount, val, nullptr);
					if (!countObj) return;
					int count = *(int*)il2cpp_object_unbox(countObj);

					char header[128];
					sprintf_s(header, "%s Count=%d##container", cName.c_str(), count);

					if (ImGui::TreeNode(header)) {
						const int MAX_SHOW = 50;
						int show = count > MAX_SHOW ? MAX_SHOW : count;

						// List<T>: has get_Item(int)
						const MethodInfo* getItem = il2cpp_class_get_method_from_name(valClass, "get_Item", 1);
						if (getItem) {
							for (int ci = 0; ci < show; ci++) {
								ImGui::PushID(ci);
								void* idxParam[] = { il2cpp_value_box(Resolver::FindClass("System", "Int32"), &ci) };
								Il2CppObject* elem = Resolver::Protection::SafeRuntimeInvoke(getItem, val, idxParam);
								if (elem && Resolver::Protection::IsValidIl2CppObject(elem)) {
									// Try to get a readable representation
									Il2CppClass* eClass = il2cpp_object_get_class(elem);
									const Il2CppType* eType = eClass ? il2cpp_class_get_type(eClass) : nullptr;
									int et = eType ? eType->type : 0;

									if (et == IL2CPP_TYPE_STRING) {
										ImGui::Text("[%d] \"%s\"", ci, il2cppi_to_string((Il2CppString*)elem).c_str());
									}
									else if (il2cpp_class_is_valuetype(eClass)) {
										// Unbox and show primitives
										void* raw = il2cpp_object_unbox(elem);
										switch (et) {
										case IL2CPP_TYPE_I4: ImGui::Text("[%d] %d", ci, *(int32_t*)raw); break;
										case IL2CPP_TYPE_R4: ImGui::Text("[%d] %.4f", ci, *(float*)raw); break;
										case IL2CPP_TYPE_BOOLEAN: ImGui::Text("[%d] %s", ci, *(bool*)raw ? "true" : "false"); break;
										default: {
											const char* eName = il2cpp_class_get_name(eClass);
											if (ImGui::SmallButton("Inspect##e")) InspectObject(elem);
											ImGui::SameLine();
											ImGui::Text("[%d] {%s}", ci, eName);
											break;
										}
										}
									}
									else {
										if (ImGui::SmallButton("Inspect##e")) InspectObject(elem);
										ImGui::SameLine();
										const char* eName = il2cpp_class_get_name(eClass);
										ImGui::Text("[%d] %s@0x%p", ci, eName, elem);
									}
								}
								else {
									ImGui::TextDisabled("[%d] null", ci);
								}
								ImGui::PopID();
							}
						}
						else {
							// No get_Item — try GetEnumerator for Dictionary etc.
							// For dictionaries, try to access Keys collection
							const MethodInfo* getKeys = il2cpp_class_get_method_from_name(valClass, "get_Keys", 0);
							const MethodInfo* getValues = il2cpp_class_get_method_from_name(valClass, "get_Values", 0);
							const MethodInfo* tryGetItem = il2cpp_class_get_method_from_name(valClass, "get_Item", 1);

							if (getKeys && tryGetItem) {
								Il2CppObject* keys = Resolver::Protection::SafeRuntimeInvoke(getKeys, val, nullptr);
								if (keys) {
									Il2CppClass* keysClass = il2cpp_object_get_class(keys);
									const MethodInfo* keysGetCount = keysClass ? il2cpp_class_get_method_from_name(keysClass, "get_Count", 0) : nullptr;
									const MethodInfo* keysGetItem = keysClass ? il2cpp_class_get_method_from_name(keysClass, "get_Item", 1) : nullptr;

									// Copy keys to array for indexed access
									const MethodInfo* copyTo = keysClass ? il2cpp_class_get_method_from_name(keysClass, "CopyTo", 2) : nullptr;

									// Simpler: just show count info
									ImGui::TextDisabled("Dictionary with %d entries (use Inspect to browse)", count);
								}
							}
							else {
								ImGui::TextDisabled("Container with %d entries (use Inspect to browse)", count);
							}
						}
						if (count > show) ImGui::TextDisabled("... and %d more", count - show);
						ImGui::TreePop();
					}
					drewContainer = true;
				});
			}
			if (!drewContainer) {
				ImGui::TextDisabled("0x%p", val);
			}
		}
		else {
			ImGui::TextDisabled("null");
			if (browseClass) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Type")) InspectClass(browseClass);
			}
		}
		break;
	}
	default:
		ImGui::TextDisabled("? (type: 0x%02X)", typeEnum);
		break;
	}

	ImGui::PopItemWidth();
	ImGui::Columns(1);
	ImGui::PopID();
}

void UnityExplorerTAB::Helpers::DrawProperty(Il2CppObject* obj, const PropertyInfo* prop)
{
	if (!prop || !prop->get) return;

	const char* propName = prop->name;
	const Il2CppType* returnType = prop->get->return_type;
	int typeEnum = returnType->type;

	ImGui::PushID(prop);

	ImGui::Columns(2, "PropCols", true);
	static bool initPropWidth = true;
	if (initPropWidth) {
		ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.35f);
		initPropWidth = false;
	}

	ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "[P] %s", propName);
	ImGui::SameLine();
	if (ImGui::SmallButton("Cp##p")) {
		std::string val = ReadPropertyValueAsString(obj, prop);
		// Strip surrounding quotes for strings
		if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
			val = val.substr(1, val.size() - 2);
		Resolver::Helpers::CopyToClipboard(val.c_str());
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy value");

	// Property snapshot diff indicator
	if (hasSnapshot && propSnapshotMap.count(prop)) {
		std::string cur = ReadPropertyValueAsString(obj, prop);
		if (cur != propSnapshotMap[prop]) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 1, 0, 1), "*");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Changed: was %s", propSnapshotMap[prop].c_str());
		}
	}

	ImGui::NextColumn();
	ImGui::PushItemWidth(-1);

	Il2CppObject* boxedVal = Resolver::Protection::SafeRuntimeInvoke(prop->get, obj, nullptr);

	switch (typeEnum) {
	case IL2CPP_TYPE_BOOLEAN: {
		bool val = Resolver::Protection::SafeUnbox<bool>(boxedVal, false);
		if (ImGui::Checkbox("##v", &val) && prop->set) {
			void* p[] = { &val };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_CHAR: {
		uint16_t val = Resolver::Protection::SafeUnbox<uint16_t>(boxedVal, 0);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display, 1.0f, 0, 65535) && prop->set) {
			uint16_t set = (uint16_t)display;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		if (val >= 32 && val < 127) { ImGui::SameLine(); ImGui::TextDisabled("'%c'", (char)val); }
		break;
	}
	case IL2CPP_TYPE_I1: {
		int8_t val = Resolver::Protection::SafeUnbox<int8_t>(boxedVal, 0);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display, 1.0f, -128, 127) && prop->set) {
			int8_t set = (int8_t)display;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_U1: {
		uint8_t val = Resolver::Protection::SafeUnbox<uint8_t>(boxedVal, 0);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display, 1.0f, 0, 255) && prop->set) {
			uint8_t set = (uint8_t)display;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_I2: {
		int16_t val = Resolver::Protection::SafeUnbox<int16_t>(boxedVal, 0);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display, 1.0f, -32768, 32767) && prop->set) {
			int16_t set = (int16_t)display;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_U2: {
		uint16_t val = Resolver::Protection::SafeUnbox<uint16_t>(boxedVal, 0);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display, 1.0f, 0, 65535) && prop->set) {
			uint16_t set = (uint16_t)display;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_I4: {
		int32_t val = Resolver::Protection::SafeUnbox<int32_t>(boxedVal, 0);
		if (ImGui::DragInt("##v", &val) && prop->set) {
			void* p[] = { &val };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_U4: {
		uint32_t val = Resolver::Protection::SafeUnbox<uint32_t>(boxedVal, 0);
		int display = (int)val;
		if (ImGui::DragInt("##v", &display) && prop->set) {
			uint32_t set = (uint32_t)display;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_I8: {
		int64_t val = Resolver::Protection::SafeUnbox<int64_t>(boxedVal, 0);
		char buf[64]; sprintf_s(buf, "%lld", val);
		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && prop->set) {
			try {
				int64_t set = std::stoll(buf);
				void* p[] = { &set };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			} catch (...) {}
		}
		break;
	}
	case IL2CPP_TYPE_U8: {
		uint64_t val = Resolver::Protection::SafeUnbox<uint64_t>(boxedVal, 0);
		char buf[64]; sprintf_s(buf, "%llu", val);
		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && prop->set) {
			try {
				uint64_t set = std::stoull(buf);
				void* p[] = { &set };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			} catch (...) {}
		}
		break;
	}
	case IL2CPP_TYPE_R4: {
		float val = Resolver::Protection::SafeUnbox<float>(boxedVal, 0.0f);
		if (ImGui::DragFloat("##v", &val, 0.05f) && prop->set) {
			void* p[] = { &val };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		break;
	}
	case IL2CPP_TYPE_R8: {
		double val = Resolver::Protection::SafeUnbox<double>(boxedVal, 0.0);
		float fval = (float)val;
		if (ImGui::DragFloat("##v", &fval, 0.05f) && prop->set) {
			double set = (double)fval;
			void* p[] = { &set };
			Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
		}
		ImGui::SameLine(); ImGui::TextDisabled("(%.6f)", val);
		break;
	}
	case IL2CPP_TYPE_STRING: {
		if (boxedVal) {
			std::string cur = il2cppi_to_string((Il2CppString*)boxedVal);
			if (prop->set) {
				char buf[256];
				strncpy_s(buf, cur.c_str(), sizeof(buf) - 1);
				ImGui::SetNextItemWidth(-1);
				if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
					Resolver::Protection::safe_call([&]() {
						Il2CppString* newStr = il2cpp_string_new(buf);
						if (newStr) {
							void* p[] = { newStr };
							Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
						}
					});
				}
			}
			else {
				ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "\"%s\"", cur.c_str());
			}
		}
		else ImGui::TextDisabled("null");
		break;
	}
	case IL2CPP_TYPE_VALUETYPE: {
		Il2CppClass* structKlass = il2cpp_class_from_type(returnType);
		if (!structKlass) break;
		if (!boxedVal) { ImGui::TextDisabled("null"); break; }

		// Handle enums
		if (structKlass->enumtype) {
			int val = Resolver::Protection::SafeUnbox<int>(boxedVal, 0);
			std::string eName = ResolveEnumName(structKlass, val);
			if (ImGui::DragInt("##v", &val) && prop->set) {
				void* p[] = { &val };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			}
			if (!eName.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", eName.c_str());
			}
			break;
		}

		std::string sName = structKlass->name;
		if (sName == "Vector3") {
			app::Vector3 v = Resolver::Protection::SafeUnbox<app::Vector3>(boxedVal);
			if (ImGui::DragFloat3("##v3", &v.x, 0.1f) && prop->set) {
				void* p[] = { &v };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			}
			else if (!prop->set) {
				ImGui::Text("X: %.2f Y: %.2f Z: %.2f", v.x, v.y, v.z);
			}
		}
		else if (sName == "Vector2") {
			float v[2] = { 0, 0 };
			void* unboxed = il2cpp_object_unbox(boxedVal);
			if (unboxed) memcpy(v, unboxed, sizeof(float) * 2);
			if (ImGui::DragFloat2("##v2", v, 0.1f) && prop->set) {
				void* p[] = { v };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			}
			else if (!prop->set) {
				ImGui::Text("X: %.2f Y: %.2f", v[0], v[1]);
			}
		}
		else if (sName == "Quaternion" || sName == "Vector4") {
			app::Vector4 q = Resolver::Protection::SafeUnbox<app::Vector4>(boxedVal);
			if (ImGui::DragFloat4("##q", &q.x, 0.1f) && prop->set) {
				void* p[] = { &q };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			}
			else if (!prop->set) {
				ImGui::Text("X: %.2f Y: %.2f Z: %.2f W: %.2f", q.x, q.y, q.z, q.w);
			}
		}
		else if (sName == "Color" || sName == "Color32") {
			float c[4] = { 0, 0, 0, 0 };
			void* unboxed = il2cpp_object_unbox(boxedVal);
			if (unboxed) memcpy(c, unboxed, sizeof(float) * 4);
			if (ImGui::ColorEdit4("##clr", c) && prop->set) {
				void* p[] = { c };
				Resolver::Protection::SafeRuntimeInvoke(prop->set, obj, p);
			}
			else if (!prop->set) {
				ImGui::ColorEdit4("##clr", c, ImGuiColorEditFlags_NoInputs);
			}
		}
		else {
			if (ImGui::SmallButton("Browse##pst")) InspectClass(structKlass);
			ImGui::SameLine();
			void* unboxed = il2cpp_object_unbox(boxedVal);
			ImGui::TextDisabled("{%s} at 0x%p", sName.c_str(), unboxed);
		}
		break;
	}
	case IL2CPP_TYPE_SZARRAY: {
		if (!boxedVal) { ImGui::TextDisabled("null"); break; }
		Il2CppArray* arr = (Il2CppArray*)boxedVal;
		uint32_t len = 0;
		Il2CppClass* elemClass = nullptr;
		const char* elemName = "?";
		bool isByteArray = false;
		Resolver::Protection::safe_call([&]() {
			len = il2cpp_array_length(arr);
			Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
			if (arrClass) elemClass = il2cpp_class_get_element_class(arrClass);
			if (elemClass) {
				elemName = il2cpp_class_get_name(elemClass);
				const Il2CppType* et = il2cpp_class_get_type(elemClass);
				if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1)) isByteArray = true;
			}
		});
		char treeLabel[128];
		sprintf_s(treeLabel, "%s[%d]##parr", elemName, len);

		// For byte[], show hex copy / ASCII copy buttons before the tree
		if (isByteArray && len > 0) {
			if (ImGui::SmallButton("Hex##parrhex")) {
				Resolver::Protection::safe_call([&]() {
					uint32_t cap = len > 4096 ? 4096 : len;
					char* data = (char*)arr + 0x20;
					std::string hex;
					for (uint32_t i = 0; i < cap; i++) {
						char b[4]; sprintf_s(b, "%02X ", (uint8_t)data[i]);
						hex += b;
					}
					Resolver::Helpers::CopyToClipboard(hex.c_str());
				});
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy hex bytes");
			ImGui::SameLine();
			if (ImGui::SmallButton("ASCII##parrascii")) {
				Resolver::Protection::safe_call([&]() {
					uint32_t cap = len > 4096 ? 4096 : len;
					char* data = (char*)arr + 0x20;
					std::string txt;
					for (uint32_t i = 0; i < cap; i++) {
						uint8_t c = (uint8_t)data[i];
						txt += (c >= 32 && c < 127) ? (char)c : '.';
					}
					Resolver::Helpers::CopyToClipboard(txt.c_str());
				});
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy as ASCII text");
			ImGui::SameLine();
			if (ImGui::SmallButton("Raw##parrraw")) {
				Resolver::Protection::safe_call([&]() {
					uint32_t cap = len > 4096 ? 4096 : len;
					char* data = (char*)arr + 0x20;
					std::string raw(data, cap);
					Resolver::Helpers::CopyToClipboard(raw.c_str());
				});
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy raw bytes as-is (for UTF-8 text)");
			ImGui::SameLine();
		}

		if (ImGui::TreeNode(treeLabel)) {
			if (isByteArray && len > 0) {
				// Show hex:ASCII view for byte arrays
				Resolver::Protection::safe_call([&]() {
					uint32_t cap = len > 4096 ? 4096 : len;
					char* data = (char*)arr + 0x20;
					for (uint32_t row = 0; row < cap; row += 16) {
						uint32_t rowEnd = row + 16 > cap ? cap : row + 16;
						char line[128]; int off = sprintf_s(line, "%04X: ", row);
						for (uint32_t i = row; i < row + 16; i++) {
							if (i < rowEnd) off += sprintf_s(line + off, sizeof(line) - off, "%02X ", (uint8_t)data[i]);
							else off += sprintf_s(line + off, sizeof(line) - off, "   ");
						}
						off += sprintf_s(line + off, sizeof(line) - off, " ");
						for (uint32_t i = row; i < rowEnd; i++) {
							uint8_t c = (uint8_t)data[i];
							line[off++] = (c >= 32 && c < 127) ? (char)c : '.';
						}
						line[off] = 0;
						ImGui::TextUnformatted(line);
					}
					if (len > cap) ImGui::TextDisabled("... (%d more bytes)", len - cap);
				});
			}
			else if (elemClass) {
				Resolver::Protection::safe_call([&]() {
					DrawArrayElements(arr, elemClass);
				});
			}
			ImGui::TreePop();
		}
		break;
	}
	case IL2CPP_TYPE_CLASS:
	case IL2CPP_TYPE_OBJECT:
	case IL2CPP_TYPE_GENERICINST: {
		if (boxedVal && Resolver::Protection::IsValidIl2CppObject(boxedVal)) {
			if (ImGui::SmallButton("Inspect")) InspectObject(boxedVal);
			ImGui::SameLine();
			// Show container count for generic types
			if (typeEnum == IL2CPP_TYPE_GENERICINST) {
				bool showedCount = false;
				Resolver::Protection::safe_call([&]() {
					Il2CppClass* vc = il2cpp_object_get_class(boxedVal);
					if (!vc) return;
					const MethodInfo* gc = il2cpp_class_get_method_from_name(vc, "get_Count", 0);
					if (!gc) return;
					Il2CppObject* co = Resolver::Protection::SafeRuntimeInvoke(gc, boxedVal, nullptr);
					if (!co) return;
					int cnt = *(int*)il2cpp_object_unbox(co);
					const char* cn = il2cpp_class_get_name(vc);
					ImGui::TextDisabled("%s Count=%d", cn, cnt);
					showedCount = true;
				});
				if (!showedCount) ImGui::TextDisabled("0x%p", boxedVal);
			}
			else {
				ImGui::TextDisabled("0x%p", boxedVal);
			}
		}
		else {
			ImGui::TextDisabled("null");
			Il2CppClass* pBrowse = nullptr;
			Resolver::Protection::safe_call([&]() {
				pBrowse = il2cpp_class_from_type(returnType);
			});
			if (pBrowse) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Type")) InspectClass(pBrowse);
			}
		}
		break;
	}
	default:
		ImGui::TextDisabled("? (type: 0x%02X)", typeEnum);
		break;
	}

	ImGui::PopItemWidth();
	ImGui::Columns(1);
	ImGui::PopID();
}

// --- One-shot param capture via INT3 + VEH ---

// Decode a captured argument to a human-readable string (called from UI thread, safe to use IL2CPP)
// --- Deep snapshot: recursively capture object/struct/array fields + properties ---
static SnapNode SnapshotObject(Il2CppObject* obj, int depth);
static SnapNode SnapshotPropertyValue(Il2CppObject* obj, const PropertyInfo* prop, int depth);

static SnapNode SnapshotValue(Il2CppObject* obj, FieldInfo* field, int depth) {
	SnapNode node;
	if (!field) return node;
	const Il2CppType* type = il2cpp_field_get_type(field);
	if (!type) return node;
	node.label = il2cpp_field_get_name(field);
	char* tn = il2cpp_type_get_name(type);
	node.typeName = tn ? tn : "?";
	if (tn) il2cpp_free(tn);

	node.value = ReadFieldValueAsString(obj, field);

	// Deep-capture reference fields if depth allows
	if (depth < 4) {
		int t = type->type;
		if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_GENERICINST) {
			Il2CppObject* val = nullptr;
			Resolver::GetFieldValue(obj, field, &val);
			if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
				SnapNode child = SnapshotObject(val, depth + 1);
				node.children = std::move(child.children);
			}
		}
		else if (t == IL2CPP_TYPE_SZARRAY) {
			Il2CppArray* arr = nullptr;
			Resolver::GetFieldValue(obj, field, &arr);
			if (arr) {
				uint32_t len = il2cpp_array_length(arr);
				Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
				Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
				if (elemClass) {
					const Il2CppType* et = il2cpp_class_get_type(elemClass);
					// byte[] → store raw bytes for hex view
					if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1)) {
						uint32_t cap = len > 4096 ? 4096 : len;
						node.rawBytes.resize(cap);
						memcpy(node.rawBytes.data(), (char*)arr + 0x20, cap);
						char b[32]; sprintf_s(b, "byte[%d]", len);
						node.value = b;
					}
					else {
						// Snapshot first N elements
						uint32_t cap = len > 50 ? 50 : len;
						bool isRef = !il2cpp_class_is_valuetype(elemClass);
						int elemSize = isRef ? 0 : il2cpp_array_element_size(arrClass);
						for (uint32_t i = 0; i < cap; i++) {
							SnapNode elem;
							char idx[16]; sprintf_s(idx, "[%d]", i);
							elem.label = idx;
							elem.typeName = il2cpp_class_get_name(elemClass);
							if (isRef) {
								Il2CppObject* e = GET_ARRAY_ELEMENT(arr, i);
								if (e && Resolver::Protection::IsValidIl2CppObject(e) && depth + 1 < 4) {
									SnapNode sub = SnapshotObject(e, depth + 1);
									elem.children = std::move(sub.children);
									char b[32]; sprintf_s(b, "0x%p", e);
									elem.value = b;
								}
								else {
									elem.value = e ? "0x..." : "null";
								}
							}
							else {
								// Inline value type element
								char* dataBase = (char*)arr + 0x20;
								void* elemPtr = dataBase + (size_t)i * elemSize;
								if (et) {
									switch (et->type) {
									case IL2CPP_TYPE_I4: { char b[32]; sprintf_s(b, "%d", *(int32_t*)elemPtr); elem.value = b; break; }
									case IL2CPP_TYPE_R4: { char b[32]; sprintf_s(b, "%.4f", *(float*)elemPtr); elem.value = b; break; }
									case IL2CPP_TYPE_BOOLEAN: elem.value = *(bool*)elemPtr ? "true" : "false"; break;
									default: { char b[32]; sprintf_s(b, "0x%llX", *(uint64_t*)elemPtr); elem.value = b; break; }
									}
								}
							}
							node.children.push_back(std::move(elem));
						}
						if (len > cap) {
							SnapNode more; more.label = "...";
							char b[32]; sprintf_s(b, "%d more", len - cap);
							more.value = b;
							node.children.push_back(std::move(more));
						}
					}
				}
			}
		}
		// VALUETYPE structs: flat value already captured above, no deep recurse needed
	}
	return node;
}

static SnapNode SnapshotPropertyValue(Il2CppObject* obj, const PropertyInfo* prop, int depth) {
	SnapNode node;
	if (!prop || !prop->get) return node;
	const Il2CppType* type = prop->get->return_type;
	if (!type) return node;
	node.label = std::string("[P] ") + (prop->name ? prop->name : "?");
	char* tn = il2cpp_type_get_name(type);
	node.typeName = tn ? tn : "?";
	if (tn) il2cpp_free(tn);

	node.value = ReadPropertyValueAsString(obj, prop);

	if (depth < 4) {
		int t = type->type;
		if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_GENERICINST) {
			Il2CppObject* val = Resolver::Protection::SafeRuntimeInvoke(prop->get, obj, nullptr);
			if (val && Resolver::Protection::IsValidIl2CppObject(val)) {
				SnapNode child = SnapshotObject(val, depth + 1);
				node.children = std::move(child.children);
			}
		}
		else if (t == IL2CPP_TYPE_SZARRAY) {
			Il2CppObject* val = Resolver::Protection::SafeRuntimeInvoke(prop->get, obj, nullptr);
			if (val) {
				Il2CppArray* arr = (Il2CppArray*)val;
				uint32_t len = il2cpp_array_length(arr);
				Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
				Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
				if (elemClass) {
					const Il2CppType* et = il2cpp_class_get_type(elemClass);
					if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1)) {
						uint32_t cap = len > 4096 ? 4096 : len;
						node.rawBytes.resize(cap);
						memcpy(node.rawBytes.data(), (char*)arr + 0x20, cap);
						char b2[32]; sprintf_s(b2, "byte[%d]", len);
						node.value = b2;
					}
					else {
						uint32_t cap = len > 50 ? 50 : len;
						bool isRef = !il2cpp_class_is_valuetype(elemClass);
						int elemSize = isRef ? 0 : il2cpp_array_element_size(arrClass);
						for (uint32_t i = 0; i < cap; i++) {
							SnapNode elem;
							char idx[16]; sprintf_s(idx, "[%d]", i);
							elem.label = idx;
							elem.typeName = il2cpp_class_get_name(elemClass);
							if (isRef) {
								Il2CppObject* e = GET_ARRAY_ELEMENT(arr, i);
								if (e && Resolver::Protection::IsValidIl2CppObject(e) && depth + 1 < 4) {
									SnapNode sub = SnapshotObject(e, depth + 1);
									elem.children = std::move(sub.children);
									char b2[32]; sprintf_s(b2, "0x%p", e);
									elem.value = b2;
								}
								else { elem.value = e ? "0x..." : "null"; }
							}
							else {
								char* dataBase = (char*)arr + 0x20;
								void* elemPtr = dataBase + (size_t)i * elemSize;
								if (et) {
									switch (et->type) {
									case IL2CPP_TYPE_I4: { char b2[32]; sprintf_s(b2, "%d", *(int32_t*)elemPtr); elem.value = b2; break; }
									case IL2CPP_TYPE_R4: { char b2[32]; sprintf_s(b2, "%.4f", *(float*)elemPtr); elem.value = b2; break; }
									case IL2CPP_TYPE_BOOLEAN: elem.value = *(bool*)elemPtr ? "true" : "false"; break;
									default: { char b2[32]; sprintf_s(b2, "0x%llX", *(uint64_t*)elemPtr); elem.value = b2; break; }
									}
								}
							}
							node.children.push_back(std::move(elem));
						}
						if (len > cap) {
							SnapNode more; more.label = "...";
							char b2[32]; sprintf_s(b2, "%d more", len - cap);
							more.value = b2;
							node.children.push_back(std::move(more));
						}
					}
				}
			}
		}
	}
	return node;
}

static SnapNode SnapshotObject(Il2CppObject* obj, int depth) {
	SnapNode node;
	if (!obj || depth >= 4) return node;

	Resolver::Protection::safe_call([&]() {
		Il2CppClass* klass = il2cpp_object_get_class(obj);
		if (!klass) return;
		const char* cn = il2cpp_class_get_name(klass);
		const char* ns = il2cpp_class_get_namespace(klass);
		node.typeName = (ns && ns[0]) ? (std::string(ns) + "." + cn) : cn;
		char b[32]; sprintf_s(b, "0x%p", obj);
		node.value = b;

		// Snapshot fields
		void* fIter = nullptr;
		while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
			Resolver::Protection::safe_call([&]() {
				node.children.push_back(SnapshotValue(obj, f, depth));
			});
		}

		// Snapshot properties
		void* pIter = nullptr;
		while (const PropertyInfo* p = il2cpp_class_get_properties(klass, &pIter)) {
			if (!p->get) continue;
			Resolver::Protection::safe_call([&]() {
				node.children.push_back(SnapshotPropertyValue(obj, p, depth));
			});
		}
	});
	return node;
}

// Fields-only snapshot (safe for VEH context — no property getter invocations)
static SnapNode SnapshotObjectFieldsOnly(Il2CppObject* obj, int depth, int maxDepth = 3) {
	SnapNode node;
	if (!obj || depth >= maxDepth) return node;

	Resolver::Protection::safe_call([&]() {
		Il2CppClass* klass = il2cpp_object_get_class(obj);
		if (!klass) return;
		const char* cn = il2cpp_class_get_name(klass);
		const char* ns = il2cpp_class_get_namespace(klass);
		node.typeName = (ns && ns[0]) ? (std::string(ns) + "." + cn) : cn;
		char b[32]; sprintf_s(b, "0x%p", obj);
		node.value = b;

		void* fIter = nullptr;
		while (FieldInfo* f = il2cpp_class_get_fields(klass, &fIter)) {
			Resolver::Protection::safe_call([&]() {
				node.children.push_back(SnapshotValue(obj, f, depth));
			});
		}
	});
	return node;
}

// Fields-only snapshot from a raw captured pointer
static SnapNode SnapshotCapturedPtrFieldsOnly(uint64_t regVal, const Il2CppType* pType, int depth, int maxDepth = 3) {
	SnapNode node;
	if (!regVal || !pType || depth >= maxDepth) return node;
	int t = pType->type;

	Resolver::Protection::safe_call([&]() {
		if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_GENERICINST) {
			Il2CppObject* obj = (Il2CppObject*)regVal;
			if (Resolver::Protection::IsValidIl2CppObject(obj))
				node = SnapshotObjectFieldsOnly(obj, depth, maxDepth);
		}
		else if (t == IL2CPP_TYPE_SZARRAY) {
			Il2CppArray* arr = (Il2CppArray*)regVal;
			if (!Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)arr)) return;
			uint32_t len = il2cpp_array_length(arr);
			Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
			Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
			if (!elemClass) return;
			const Il2CppType* et = il2cpp_class_get_type(elemClass);
			node.typeName = std::string(il2cpp_class_get_name(elemClass)) + "[]";
			char b2[32]; sprintf_s(b2, "[%d]", len);
			node.value = b2;

			if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1)) {
				uint32_t cap = len > 4096 ? 4096 : len;
				node.rawBytes.resize(cap);
				memcpy(node.rawBytes.data(), (char*)arr + 0x20, cap);
			}
			else {
				uint32_t cap = len > 20 ? 20 : len;
				bool isRef = !il2cpp_class_is_valuetype(elemClass);
				int elemSize = isRef ? 0 : il2cpp_array_element_size(arrClass);
				for (uint32_t i = 0; i < cap; i++) {
					SnapNode elem;
					char idx[16]; sprintf_s(idx, "[%d]", i);
					elem.label = idx;
					elem.typeName = il2cpp_class_get_name(elemClass);
					if (isRef) {
						Il2CppObject* e = GET_ARRAY_ELEMENT(arr, i);
						if (e && Resolver::Protection::IsValidIl2CppObject(e) && depth + 1 < maxDepth)
							elem = SnapshotObjectFieldsOnly(e, depth + 1, maxDepth);
						else
							elem.value = e ? "0x..." : "null";
						elem.label = idx;
					}
					else {
						char* dataBase = (char*)arr + 0x20;
						void* elemPtr = dataBase + (size_t)i * elemSize;
						char vb[64];
						switch (et ? et->type : 0) {
						case IL2CPP_TYPE_I4: sprintf_s(vb, "%d", *(int32_t*)elemPtr); elem.value = vb; break;
						case IL2CPP_TYPE_U4: sprintf_s(vb, "%u", *(uint32_t*)elemPtr); elem.value = vb; break;
						case IL2CPP_TYPE_R4: sprintf_s(vb, "%.4f", *(float*)elemPtr); elem.value = vb; break;
						case IL2CPP_TYPE_R8: sprintf_s(vb, "%.6f", *(double*)elemPtr); elem.value = vb; break;
						case IL2CPP_TYPE_I8: sprintf_s(vb, "%lld", *(int64_t*)elemPtr); elem.value = vb; break;
						default: sprintf_s(vb, "0x%X", *(int32_t*)elemPtr); elem.value = vb; break;
						}
					}
					node.children.push_back(std::move(elem));
				}
				if (len > cap) {
					SnapNode dots; dots.label = "..."; dots.value = std::to_string(len - cap) + " more";
					node.children.push_back(std::move(dots));
				}
			}
		}
		else if (t == IL2CPP_TYPE_VALUETYPE) {
			Il2CppClass* vc = il2cpp_class_from_type(pType);
			if (vc) { node.typeName = vc->name; node.value = "(struct)"; }
		}
	});
	return node;
}

// Snapshot a raw pointer from captured arg
static SnapNode SnapshotFromCapturedPtr(uint64_t regVal, const Il2CppType* pType, int depth) {
	SnapNode node;
	if (!regVal || !pType || depth >= 4) return node;
	int t = pType->type;

	Resolver::Protection::safe_call([&]() {
		if (t == IL2CPP_TYPE_CLASS || t == IL2CPP_TYPE_OBJECT || t == IL2CPP_TYPE_GENERICINST) {
			Il2CppObject* obj = (Il2CppObject*)regVal;
			if (Resolver::Protection::IsValidIl2CppObject(obj)) {
				node = SnapshotObject(obj, depth);
			}
		}
		else if (t == IL2CPP_TYPE_SZARRAY) {
			Il2CppArray* arr = (Il2CppArray*)regVal;
			if (!Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)arr)) return;
			uint32_t len = il2cpp_array_length(arr);
			Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)arr);
			Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
			if (!elemClass) return;
			const Il2CppType* et = il2cpp_class_get_type(elemClass);
			node.typeName = std::string(il2cpp_class_get_name(elemClass)) + "[]";
			char b[32]; sprintf_s(b, "[%d]", len);
			node.value = b;

			// byte[] → hex dump
			if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1)) {
				uint32_t cap = len > 4096 ? 4096 : len;
				node.rawBytes.resize(cap);
				memcpy(node.rawBytes.data(), (char*)arr + 0x20, cap);
			}
			else {
				uint32_t cap = len > 50 ? 50 : len;
				bool isRef = !il2cpp_class_is_valuetype(elemClass);
				int elemSize = isRef ? 0 : il2cpp_array_element_size(arrClass);
				for (uint32_t i = 0; i < cap; i++) {
					SnapNode elem;
					char idx[16]; sprintf_s(idx, "[%d]", i);
					elem.label = idx;
					elem.typeName = il2cpp_class_get_name(elemClass);
					if (isRef) {
						Il2CppObject* e = GET_ARRAY_ELEMENT(arr, i);
						if (e && Resolver::Protection::IsValidIl2CppObject(e) && depth + 1 < 4) {
							elem = SnapshotObject(e, depth + 1);
							elem.label = idx;
						}
						else {
							elem.value = e ? "0x..." : "null";
						}
					}
					else {
						char* dataBase = (char*)arr + 0x20;
						void* elemPtr = dataBase + (size_t)i * elemSize;
						switch (et ? et->type : 0) {
						case IL2CPP_TYPE_I4: { char b2[32]; sprintf_s(b2, "%d", *(int32_t*)elemPtr); elem.value = b2; break; }
						case IL2CPP_TYPE_U4: { char b2[32]; sprintf_s(b2, "%u", *(uint32_t*)elemPtr); elem.value = b2; break; }
						case IL2CPP_TYPE_R4: { char b2[32]; sprintf_s(b2, "%.4f", *(float*)elemPtr); elem.value = b2; break; }
						case IL2CPP_TYPE_BOOLEAN: elem.value = *(bool*)elemPtr ? "true" : "false"; break;
						default: { char b2[32]; sprintf_s(b2, "0x%llX", *(uint64_t*)elemPtr); elem.value = b2; break; }
						}
					}
					node.children.push_back(std::move(elem));
				}
				if (len > cap) {
					SnapNode more; more.label = "...";
					char b2[32]; sprintf_s(b2, "%d more", len - cap);
					more.value = b2;
					node.children.push_back(std::move(more));
				}
			}
		}
		else if (t == IL2CPP_TYPE_VALUETYPE) {
			Il2CppClass* vc = il2cpp_class_from_type(pType);
			if (vc) {
				node.typeName = vc->name;
				node.value = "(struct)";
			}
		}
	});
	return node;
}

// Draw a snapshot tree recursively
static void DrawSnapNode(const SnapNode& node, int id) {
	ImGui::PushID(id);

	// Hex:ASCII dump for byte arrays
	if (!node.rawBytes.empty()) {
		if (ImGui::TreeNode("hexview", "%s %s = %s (hex)", node.typeName.c_str(), node.label.c_str(), node.value.c_str())) {
			// Copy hex button
			if (ImGui::SmallButton("Copy Hex")) {
				std::string hex;
				for (size_t i = 0; i < node.rawBytes.size(); i++) {
					char h[4]; sprintf_s(h, "%02X ", node.rawBytes[i]);
					hex += h;
				}
				Resolver::Helpers::CopyToClipboard(hex.c_str());
			}

			// Hex:ASCII display - 16 bytes per row
			ImGui::BeginChild("HexDump", ImVec2(0, (std::min)((float)(node.rawBytes.size() / 16 + 2) * ImGui::GetTextLineHeightWithSpacing(), 300.0f)), true);
			const size_t bytesPerRow = 16;
			for (size_t row = 0; row < node.rawBytes.size(); row += bytesPerRow) {
				char line[128];
				int pos = sprintf_s(line, "%04X: ", (unsigned)row);
				// Hex part
				for (size_t col = 0; col < bytesPerRow; col++) {
					if (row + col < node.rawBytes.size())
						pos += sprintf_s(line + pos, sizeof(line) - pos, "%02X ", node.rawBytes[row + col]);
					else
						pos += sprintf_s(line + pos, sizeof(line) - pos, "   ");
					if (col == 7) pos += sprintf_s(line + pos, sizeof(line) - pos, " ");
				}
				pos += sprintf_s(line + pos, sizeof(line) - pos, " |");
				// ASCII part
				for (size_t col = 0; col < bytesPerRow && row + col < node.rawBytes.size(); col++) {
					uint8_t b = node.rawBytes[row + col];
					line[pos++] = (b >= 32 && b < 127) ? (char)b : '.';
				}
				line[pos++] = '|';
				line[pos] = '\0';
				ImGui::TextUnformatted(line);
			}
			ImGui::EndChild();
			ImGui::TreePop();
		}
	}
	else if (!node.children.empty()) {
		char label[256];
		if (!node.label.empty())
			sprintf_s(label, "%s %s = %s", node.typeName.c_str(), node.label.c_str(), node.value.c_str());
		else
			sprintf_s(label, "%s %s", node.typeName.c_str(), node.value.c_str());
		if (ImGui::TreeNode("tree", "%s", label)) {
			for (size_t i = 0; i < node.children.size(); i++) {
				DrawSnapNode(node.children[i], (int)i);
			}
			ImGui::TreePop();
		}
	}
	else {
		if (!node.label.empty())
			ImGui::Text("%s %s = %s", node.typeName.c_str(), node.label.c_str(), node.value.c_str());
		else
			ImGui::Text("%s %s", node.typeName.c_str(), node.value.c_str());
	}
	ImGui::PopID();
}

static std::string DecodeCapturedArg(const MethodInfo* method, int paramIdx, uint64_t regVal, uint64_t xmmLow) {
	const Il2CppType* pType = il2cpp_method_get_param(method, paramIdx);
	if (!pType) return "???";
	char buf[256];

	switch (pType->type) {
	case IL2CPP_TYPE_BOOLEAN: return (regVal & 1) ? "true" : "false";
	case IL2CPP_TYPE_I1: sprintf_s(buf, "%d", (int)(int8_t)(regVal & 0xFF)); return buf;
	case IL2CPP_TYPE_U1: sprintf_s(buf, "%u", (unsigned)(uint8_t)(regVal & 0xFF)); return buf;
	case IL2CPP_TYPE_I2: sprintf_s(buf, "%d", (int)(int16_t)(regVal & 0xFFFF)); return buf;
	case IL2CPP_TYPE_U2: sprintf_s(buf, "%u", (unsigned)(uint16_t)(regVal & 0xFFFF)); return buf;
	case IL2CPP_TYPE_CHAR: {
		uint16_t ch = (uint16_t)(regVal & 0xFFFF);
		if (ch >= 32 && ch < 127) sprintf_s(buf, "'%c' (%d)", (char)ch, (int)ch);
		else sprintf_s(buf, "%d", (int)ch);
		return buf;
	}
	case IL2CPP_TYPE_I4: sprintf_s(buf, "%d", (int32_t)(regVal & 0xFFFFFFFF)); return buf;
	case IL2CPP_TYPE_U4: sprintf_s(buf, "%u", (uint32_t)(regVal & 0xFFFFFFFF)); return buf;
	case IL2CPP_TYPE_I8: sprintf_s(buf, "%lld", (int64_t)regVal); return buf;
	case IL2CPP_TYPE_U8: sprintf_s(buf, "%llu", (uint64_t)regVal); return buf;
	case IL2CPP_TYPE_R4: { float f; memcpy(&f, &xmmLow, sizeof(float)); sprintf_s(buf, "%.4f", f); return buf; }
	case IL2CPP_TYPE_R8: { double d; memcpy(&d, &xmmLow, sizeof(double)); sprintf_s(buf, "%.6f", d); return buf; }
	case IL2CPP_TYPE_STRING: {
		Il2CppString* str = (Il2CppString*)regVal;
		if (str && Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)str)) {
			std::string s;
			Resolver::Protection::safe_call([&]() { s = il2cppi_to_string(str); });
			return "\"" + s + "\"";
		}
		return "null";
	}
	case IL2CPP_TYPE_CLASS:
	case IL2CPP_TYPE_OBJECT:
	case IL2CPP_TYPE_GENERICINST: {
		if (!regVal) return "null";
		Il2CppObject* obj = (Il2CppObject*)regVal;
		if (Resolver::Protection::IsValidIl2CppObject(obj)) {
			const char* cName = "???";
			Resolver::Protection::safe_call([&]() {
				Il2CppClass* k = il2cpp_object_get_class(obj);
				if (k) cName = il2cpp_class_get_name(k);
			});
			sprintf_s(buf, "%s@0x%llX", cName, regVal);
			return buf;
		}
		sprintf_s(buf, "0x%llX", regVal);
		return buf;
	}
	case IL2CPP_TYPE_VALUETYPE: {
		Il2CppClass* vClass = nullptr;
		Resolver::Protection::safe_call([&]() { vClass = il2cpp_class_from_type(pType); });
		if (vClass && vClass->enumtype) {
			int32_t val = (int32_t)(regVal & 0xFFFFFFFF);
			std::string eName = ResolveEnumName(vClass, val);
			sprintf_s(buf, "%d", val);
			return eName.empty() ? std::string(buf) : (eName + " (" + buf + ")");
		}
		sprintf_s(buf, "0x%llX", regVal);
		return buf;
	}
	case IL2CPP_TYPE_SZARRAY: {
		if (!regVal) return "null";
		sprintf_s(buf, "Array@0x%llX", regVal);
		return buf;
	}
	default: sprintf_s(buf, "0x%llX", regVal); return buf;
	}
}

// Convert captured arg to the string format the Invoke param buffers expect
static std::string CapturedArgToBuffer(const MethodInfo* method, int paramIdx, uint64_t regVal, uint64_t xmmLow) {
	const Il2CppType* pType = il2cpp_method_get_param(method, paramIdx);
	if (!pType) return "0";
	char buf[256];

	switch (pType->type) {
	case IL2CPP_TYPE_BOOLEAN: return (regVal & 1) ? "true" : "false";
	case IL2CPP_TYPE_I1: sprintf_s(buf, "%d", (int)(int8_t)(regVal & 0xFF)); return buf;
	case IL2CPP_TYPE_U1: sprintf_s(buf, "%u", (unsigned)(uint8_t)(regVal & 0xFF)); return buf;
	case IL2CPP_TYPE_I2: sprintf_s(buf, "%d", (int)(int16_t)(regVal & 0xFFFF)); return buf;
	case IL2CPP_TYPE_U2: sprintf_s(buf, "%u", (unsigned)(uint16_t)(regVal & 0xFFFF)); return buf;
	case IL2CPP_TYPE_CHAR: sprintf_s(buf, "%d", (int)(uint16_t)(regVal & 0xFFFF)); return buf;
	case IL2CPP_TYPE_I4: sprintf_s(buf, "%d", (int32_t)(regVal & 0xFFFFFFFF)); return buf;
	case IL2CPP_TYPE_U4: sprintf_s(buf, "%u", (uint32_t)(regVal & 0xFFFFFFFF)); return buf;
	case IL2CPP_TYPE_I8: sprintf_s(buf, "%lld", (int64_t)regVal); return buf;
	case IL2CPP_TYPE_U8: sprintf_s(buf, "%llu", (uint64_t)regVal); return buf;
	case IL2CPP_TYPE_R4: { float f; memcpy(&f, &xmmLow, sizeof(float)); sprintf_s(buf, "%f", f); return buf; }
	case IL2CPP_TYPE_R8: { double d; memcpy(&d, &xmmLow, sizeof(double)); sprintf_s(buf, "%f", d); return buf; }
	case IL2CPP_TYPE_STRING: {
		Il2CppString* str = (Il2CppString*)regVal;
		if (str && Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)str)) {
			std::string s;
			Resolver::Protection::safe_call([&]() { s = il2cppi_to_string(str); });
			return s;
		}
		return "";
	}
	case IL2CPP_TYPE_CLASS:
	case IL2CPP_TYPE_OBJECT:
	case IL2CPP_TYPE_GENERICINST:
	case IL2CPP_TYPE_SZARRAY:
		sprintf_s(buf, "0x%llX", regVal); return buf;
	case IL2CPP_TYPE_VALUETYPE: {
		Il2CppClass* vClass = nullptr;
		Resolver::Protection::safe_call([&]() { vClass = il2cpp_class_from_type(pType); });
		if (vClass) {
			std::string name = vClass->name;
			if (name == "Vector3") {
				// regVal is a pointer to the struct on stack or passed by value
				// For small structs on x64, passed in register as raw bits
				float* fp = (float*)&regVal;
				sprintf_s(buf, "%.3f,%.3f,%.3f", fp[0], fp[1], 0.0f);
				return buf;
			}
			if (name == "Color") {
				sprintf_s(buf, "%.3f,%.3f,%.3f,%.3f", 0.0f, 0.0f, 0.0f, 1.0f);
				return buf;
			}
			if (vClass->enumtype) {
				sprintf_s(buf, "%d", (int32_t)(regVal & 0xFFFFFFFF));
				return buf;
			}
		}
		sprintf_s(buf, "%d", (int32_t)(regVal & 0xFFFFFFFF));
		return buf;
	}
	default: sprintf_s(buf, "%lld", (int64_t)regVal); return buf;
	}
}

// SEH-safe Custom Pass buffer modification (no C++ objects — safe for __try)
static void ApplyCustomPass(uint64_t arrPtr, int32_t msgId, int32_t len) {
	__try {
		if (msgId == 10109 && len == 56) {
			uint8_t* data = (uint8_t*)arrPtr + 32; // sizeof(Il2CppArray) on x64
			CustomPassLogEntry logEntry;
			logEntry.msgId = msgId;
			logEntry.len = len;
			memcpy(logEntry.original, data, 56);
			data[55] = 0x01;
			memcpy(logEntry.modified, data, 56);
			if (g_customPassLogMutex.try_lock()) {
				g_customPassPendingLogs.push_back(logEntry);
				g_customPassLogMutex.unlock();
			}
		}
		else if (msgId == 10209 && len == 56) {
			uint8_t* data = (uint8_t*)arrPtr + 32; // sizeof(Il2CppArray) on x64
			CustomPassLogEntry logEntry;
			logEntry.msgId = msgId;
			logEntry.len = len;
			memcpy(logEntry.original, data, 56);
			data[55] = 0x01;
			memcpy(logEntry.modified, data, 56);
			if (g_customPassLogMutex.try_lock()) {
				g_customPassPendingLogs.push_back(logEntry);
				g_customPassLogMutex.unlock();
			}
		}
		else if (len == 60) {
			uint8_t* data = (uint8_t*)arrPtr + 32; // sizeof(Il2CppArray) on x64
			CustomPassLogEntry logEntry;
			logEntry.msgId = msgId;
			logEntry.len = len;
			memcpy(logEntry.original, data, 60);
			data[55] = 0x01;
			// data[56] unchanged (wildcard)
			data[57] = 0xFF;
			data[58] = 0x88;
			data[59] = 0x7A;
			memcpy(logEntry.modified, data, 60);
			if (g_customPassLogMutex.try_lock()) {
				g_customPassPendingLogs.push_back(logEntry);
				g_customPassLogMutex.unlock();
			}
		}
		else if (len == 59) {
			uint8_t* data = (uint8_t*)arrPtr + 32; // sizeof(Il2CppArray) on x64
			CustomPassLogEntry logEntry;
			logEntry.msgId = msgId;
			logEntry.len = len;
			memcpy(logEntry.original, data, 59);
			data[55] = 0x01;
			// data[56] unchanged (wildcard)
			data[57] = 0x80;
			data[58] = 0x7D;
			memcpy(logEntry.modified, data, 59);
			if (g_customPassLogMutex.try_lock()) {
				g_customPassPendingLogs.push_back(logEntry);
				g_customPassLogMutex.unlock();
			}
		}
		else if (len == 58) {
			uint8_t* data = (uint8_t*)arrPtr + 32; // sizeof(Il2CppArray) on x64
			CustomPassLogEntry logEntry;
			logEntry.msgId = msgId;
			logEntry.len = len;
			memcpy(logEntry.original, data, 58);
			data[55] = 0x01;
			memcpy(logEntry.modified, data, 58);
			if (g_customPassLogMutex.try_lock()) {
				g_customPassPendingLogs.push_back(logEntry);
				g_customPassLogMutex.unlock();
			}
		}
	} __except(EXCEPTION_EXECUTE_HANDLER) {}
}

LONG WINAPI MultiHookVEH(PEXCEPTION_POINTERS ex) {
	DWORD code = ex->ExceptionRecord->ExceptionCode;
	void* addr = ex->ExceptionRecord->ExceptionAddress;

	// ======== BREAKPOINT (INT3) ========
	if (code == EXCEPTION_BREAKPOINT) {

		// 1. One-shot capture (backward compat)
		if (g_capture.active && addr == g_capture.targetAddr) {
			// Restore original byte first (must do this to avoid infinite BP)
			DWORD oldProt;
			VirtualProtect(g_capture.targetAddr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
			*(uint8_t*)g_capture.targetAddr = g_capture.savedByte;
			VirtualProtect(g_capture.targetAddr, 1, oldProt, &oldProt);
			FlushInstructionCache(GetCurrentProcess(), g_capture.targetAddr, 1);

			// Class filter: for instance methods, check if 'this' matches filterClass
			if (!g_capture.isStatic && g_capture.filterClass) {
				uint64_t thisPtr = (uint64_t)ex->ContextRecord->Rcx;
				bool classMatch = false;
				if (thisPtr) {
					Resolver::Protection::safe_call([&]() {
						Il2CppObject* thisObj = (Il2CppObject*)thisPtr;
						if (Resolver::Protection::IsValidIl2CppObject(thisObj)) {
							Il2CppClass* callerClass = il2cpp_object_get_class(thisObj);
							if (callerClass)
								classMatch = il2cpp_class_is_assignable_from(g_capture.filterClass, callerClass);
						}
					});
				}
				if (!classMatch) {
					// Use single-step to let the original instruction execute,
					// then re-arm INT3 in the SINGLE_STEP handler.
					// (Writing 0xCC back here + setting RIP to same addr = infinite loop)
					g_capture.pendingRearm = true;
					ex->ContextRecord->EFlags |= 0x100; // TF — single step
					ex->ContextRecord->Rip = (DWORD64)g_capture.targetAddr;
					return EXCEPTION_CONTINUE_EXECUTION;
				}
			}

			uint64_t intRegs[] = {
				(uint64_t)ex->ContextRecord->Rcx, (uint64_t)ex->ContextRecord->Rdx,
				(uint64_t)ex->ContextRecord->R8,  (uint64_t)ex->ContextRecord->R9
			};
			uint64_t xmmRegs[] = {
				ex->ContextRecord->Xmm0.Low, ex->ContextRecord->Xmm1.Low,
				ex->ContextRecord->Xmm2.Low, ex->ContextRecord->Xmm3.Low
			};
			for (int i = 0; i < g_capture.totalArgCount && i < 20; i++) {
				if (i < 4) {
					g_capture.args[i].regVal = intRegs[i];
					g_capture.args[i].xmmLow = xmmRegs[i];
				}
				else {
					uint64_t* stackSlot = (uint64_t*)(ex->ContextRecord->Rsp + 0x28 + (uint64_t)(i - 4) * 8);
					g_capture.args[i].regVal = *stackSlot;
					g_capture.args[i].xmmLow = 0;
				}
			}

			// Snapshot array data IMMEDIATELY (before method executes and modifies it)
			// This captures pre-execution state (e.g., plaintext before encryption)
			g_capturedBufCount = 0;
			g_capturedBufDone = true;
			for (int api = 0; api < g_capture.numArrayParams; api++) {
				auto& ap = g_capture.arrayParams[api];
				uint64_t arrPtr = g_capture.args[ap.argIdx].regVal;
				if (!arrPtr) continue;
				if (TrySnapshotArray(arrPtr, ap.elemSize, g_capturedBufs[g_capturedBufCount], ap.paramIdx))
					g_capturedBufCount++;
			}

			g_capture.captured = true;
			g_capture.active = false;
			ex->ContextRecord->Rip = (DWORD64)g_capture.targetAddr;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// 2. Persistent hooks — look up BreakpointSlot for this address
		std::shared_ptr<BreakpointSlot> slot;
		{
			// Use try_lock to prevent deadlock if hook management holds this mutex
			if (!g_hooksMutex.try_lock()) return EXCEPTION_CONTINUE_SEARCH;
			auto it = g_slots.find(addr);
			if (it == g_slots.end()) { g_hooksMutex.unlock(); return EXCEPTION_CONTINUE_SEARCH; }
			slot = it->second;
			g_hooksMutex.unlock();
		}
		if (!slot->armed.load()) return EXCEPTION_CONTINUE_SEARCH;

		// Restore original byte (shared across all hooks at this address)
		DWORD oldProt;
		VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
		*(uint8_t*)addr = slot->savedByte;
		VirtualProtect(addr, 1, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), addr, 1);

		// Read registers once (shared by all hooks at this address)
		uint64_t intRegs[] = {
			(uint64_t)ex->ContextRecord->Rcx, (uint64_t)ex->ContextRecord->Rdx,
			(uint64_t)ex->ContextRecord->R8,  (uint64_t)ex->ContextRecord->R9
		};
		uint64_t xmmRegs[] = {
			ex->ContextRecord->Xmm0.Low, ex->ContextRecord->Xmm1.Low,
			ex->ContextRecord->Xmm2.Low, ex->ContextRecord->Xmm3.Low
		};

		// Determine the class of 'this' (RCX for instance methods) — used for filtering
		Il2CppClass* callerClass = nullptr;
		bool callerClassResolved = false;

		// Walk stack once (shared by all hooks at this address) — SEH-protected
		uintptr_t stackBuf[32];
		int stackCount = CaptureStackWalkSafe(ex->ContextRecord, addr, stackBuf, 32);
		std::vector<uintptr_t> sharedStack(stackBuf, stackBuf + stackCount);

		// Iterate all hooks at this address, filter by class, log to matching ones
		// Take a snapshot of the hook list under lock to avoid holding lock during heavy work
		std::vector<std::shared_ptr<HookEntry>> hooksSnapshot;
		{
			if (g_hooksMutex.try_lock()) {
				auto it = g_slots.find(addr);
				if (it != g_slots.end())
					hooksSnapshot = it->second->hooks;
				g_hooksMutex.unlock();
			}
		}

		for (auto& hook : hooksSnapshot) {
			if (hook->paused.load()) continue;

			// Class filter: for instance methods, check if this->klass matches filterClass
			if (!hook->isStatic && hook->filterClass) {
				// Lazily resolve caller class from RCX on first hook that needs it
				if (!callerClassResolved) {
					callerClassResolved = true;
					uint64_t thisPtr = intRegs[0]; // RCX = this
					if (thisPtr) {
						Resolver::Protection::safe_call([&]() {
							Il2CppObject* thisObj = (Il2CppObject*)thisPtr;
							if (Resolver::Protection::IsValidIl2CppObject(thisObj))
								callerClass = il2cpp_object_get_class(thisObj);
						});
					}
				}
				if (!callerClass) continue;
				bool classMatch = false;
				Resolver::Protection::safe_call([&]() {
					classMatch = il2cpp_class_is_assignable_from(hook->filterClass, callerClass);
				});
				if (!classMatch) continue;
			}

			// Build HookCallRecord for this hook
			HookCallRecord rec;
			rec.totalArgCount = hook->totalArgCount;
			rec.timestamp = GetTickCount64();

			for (int i = 0; i < rec.totalArgCount && i < 20; i++) {
				if (i < 4) {
					rec.args[i].regVal = intRegs[i];
					rec.args[i].xmmLow = xmmRegs[i];
				}
				else {
					uint64_t* stackSlot = (uint64_t*)(ex->ContextRecord->Rsp + 0x28 + (uint64_t)(i - 4) * 8);
					rec.args[i].regVal = *stackSlot;
					rec.args[i].xmmLow = 0;
				}
			}

			rec.stack = sharedStack; // copy shared stack

			if (hook->customPass && hook->totalArgCount >= 4) {
				int32_t msgId = (int32_t)rec.args[1].regVal;
				int32_t len   = (int32_t)rec.args[3].regVal;
				uint64_t arrPtr = rec.args[2].regVal;
				if (msgId == 10109 && len == 56 && arrPtr) {
					ApplyCustomPass(arrPtr, msgId, len);
				}
				else if (msgId == 4709 && len == 60 && arrPtr) {
					ApplyCustomPass(arrPtr, msgId, len);
				}
				else if (msgId == 4709 && len == 59 && arrPtr) {
					ApplyCustomPass(arrPtr, msgId, len);
				}
				else if (msgId == 4709 && len == 58 && arrPtr) {
					ApplyCustomPass(arrPtr, msgId, len);
				}
				else if (msgId == 10209 && len == 56 && arrPtr) {
					ApplyCustomPass(arrPtr, msgId, len);
				}
			}

			// Snapshot array params into HookCapturedBuffers (pre-execution data)
			for (int api = 0; api < hook->numArrayParams; api++) {
				auto& ap = hook->arrayParams[api];
				if (ap.argIdx >= rec.totalArgCount || !rec.args[ap.argIdx].regVal) continue;
				uint32_t arrLen = 0, byteLen = 0;
				if (TrySnapshotArrayRaw(rec.args[ap.argIdx].regVal, ap.elemSize, arrLen, byteLen)) {
					HookCapturedBuffer hcb;
					hcb.valid = true;
					hcb.paramIdx = ap.paramIdx;
					hcb.elemSize = ap.elemSize;
					hcb.arrayLen = arrLen;
					hcb.byteLen = byteLen;
					hcb.data.assign(g_hookSnapScratch, g_hookSnapScratch + byteLen);
					hcb.originalArrayPtr = (void*)rec.args[ap.argIdx].regVal;
					rec.buffers.push_back(std::move(hcb));
				}
			}

			// NOTE: Deep snapshots (SnapshotObjectFieldsOnly) removed from VEH handler.
			// Calling IL2CPP runtime functions inside VEH causes deadlocks when the
			// hooked method is called while IL2CPP holds internal locks (GC, class init, etc).
			// Raw register values + array snapshots are captured here; the UI can inspect
			// live objects from the pointer values when the user views a call record.

			// Custom Pass: modify buffer in-place before the original function runs


			// Push to this hook's call log (try_lock to avoid deadlock in VEH)
			if (hook->logMutex.try_lock()) {
				hook->callLog.push_back(std::move(rec));
				if (hook->callLog.size() > HookEntry::MAX_HOOK_LOG)
					hook->callLog.erase(hook->callLog.begin());
				hook->logMutex.unlock();
			}
			hook->totalCallCount.fetch_add(1);
		}

		// Set trap flag for single-step re-arm (operates on the slot, not individual hooks)
		slot->armed.store(false);
		slot->singleStepping.store(true);
		slot->singleStepThreadId = GetCurrentThreadId();
		if (g_singleStepMutex.try_lock()) {
			g_pendingSingleStep[GetCurrentThreadId()] = slot;
			g_singleStepMutex.unlock();
		}

		ex->ContextRecord->EFlags |= 0x100; // TF
		ex->ContextRecord->Rip = (DWORD64)addr;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	// ======== SINGLE STEP (TF trap) ========
	if (code == EXCEPTION_SINGLE_STEP) {

		// 1. One-shot capture re-arm (class filter skipped this call)
		if (g_capture.pendingRearm) {
			g_capture.pendingRearm = false;
			ex->ContextRecord->EFlags &= ~0x100; // clear TF
			// Re-arm INT3 so the next call to this method triggers capture again
			if (g_capture.active) {
				DWORD oldProt;
				VirtualProtect(g_capture.targetAddr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
				*(uint8_t*)g_capture.targetAddr = 0xCC;
				VirtualProtect(g_capture.targetAddr, 1, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), g_capture.targetAddr, 1);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// 2. Persistent hook re-arm
		DWORD tid = GetCurrentThreadId();
		std::shared_ptr<BreakpointSlot> slot;
		{
			if (!g_singleStepMutex.try_lock()) return EXCEPTION_CONTINUE_SEARCH;
			auto it = g_pendingSingleStep.find(tid);
			if (it == g_pendingSingleStep.end()) { g_singleStepMutex.unlock(); return EXCEPTION_CONTINUE_SEARCH; }
			slot = it->second;
			g_pendingSingleStep.erase(it);
			g_singleStepMutex.unlock();
		}

		ex->ContextRecord->EFlags &= ~0x100; // clear TF
		slot->singleStepping.store(false);

		// Re-insert INT3 if the slot still exists and has any non-paused hooks
		bool shouldRearm = false;
		if (g_hooksMutex.try_lock()) {
			auto it = g_slots.find(slot->addr);
			if (it != g_slots.end()) {
				for (auto& h : it->second->hooks) {
					if (!h->paused.load()) { shouldRearm = true; break; }
				}
			}
			g_hooksMutex.unlock();
		}

		if (shouldRearm) {
			DWORD oldProt;
			VirtualProtect(slot->addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
			*(uint8_t*)slot->addr = 0xCC;
			VirtualProtect(slot->addr, 1, oldProt, &oldProt);
			FlushInstructionCache(GetCurrentProcess(), slot->addr, 1);
			slot->armed.store(true);
		}

		return EXCEPTION_CONTINUE_EXECUTION;
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static void SetOneShotCapture(const MethodInfo* method, Il2CppClass* fromClass) {
	if (g_capture.active) CancelOneShotCapture();
	g_capture.pendingRearm = false;
	if (!method || !method->methodPointer) return;
	// Clear previous buffer snapshots (cloned arrays are GC-managed, no need to free)
	g_capturedBufCount = 0;
	g_capturedBufDone = false;
	for (int i = 0; i < MAX_CAPTURE_BUFS; i++) {
		g_capturedBufs[i].valid = false;
		g_capturedBufs[i].clonedArray = nullptr;
		g_capturedBufs[i].originalArrayPtr = nullptr;
	}

	bool isStatic = (method->flags & 0x0010) != 0;
	int paramCount = il2cpp_method_get_param_count(method);

	g_capture.method = method;
	g_capture.targetAddr = (void*)method->methodPointer;
	g_capture.captured = false;
	g_capture.totalArgCount = paramCount + (isStatic ? 0 : 1); // params + optional this
	g_capture.isStatic = isStatic;
	g_capture.filterClass = fromClass;

	// Pre-compute which params are arrays (for immediate snapshot in VEH)
	g_capture.numArrayParams = 0;
	for (int pi = 0; pi < paramCount && g_capture.numArrayParams < MAX_CAPTURE_BUFS; pi++) {
		const Il2CppType* pType = il2cpp_method_get_param(method, pi);
		if (!pType) continue;
		int pt = pType->type;
		if (pt == IL2CPP_TYPE_SZARRAY || pt == IL2CPP_TYPE_ARRAY) {
			auto& ap = g_capture.arrayParams[g_capture.numArrayParams];
			ap.paramIdx = pi;
			ap.argIdx = pi + (isStatic ? 0 : 1);
			ap.elemSize = 1;
			Resolver::Protection::safe_call([&]() {
				Il2CppClass* paramClass = il2cpp_class_from_type(pType);
				if (paramClass) ap.elemSize = (uint32_t)il2cpp_array_element_size(paramClass);
			});
			if (ap.elemSize == 0) ap.elemSize = 1;
			g_capture.numArrayParams++;
		}
	}

	// Register VEH once
	if (!g_vehHandle) {
		g_vehHandle = AddVectoredExceptionHandler(1, MultiHookVEH);
	}

	// Save original byte and write INT3
	DWORD oldProt;
	VirtualProtect(g_capture.targetAddr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
	g_capture.savedByte = *(uint8_t*)g_capture.targetAddr;
	*(uint8_t*)g_capture.targetAddr = 0xCC;
	VirtualProtect(g_capture.targetAddr, 1, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), g_capture.targetAddr, 1);

	g_capture.active = true;
	DebugLog("OneShot: armed on %s (0x%p), %d args", method->name, g_capture.targetAddr, g_capture.totalArgCount);
}

static void CancelOneShotCapture() {
	g_capture.pendingRearm = false;
	if (!g_capture.active) return;

	DWORD oldProt;
	VirtualProtect(g_capture.targetAddr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
	*(uint8_t*)g_capture.targetAddr = g_capture.savedByte;
	VirtualProtect(g_capture.targetAddr, 1, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), g_capture.targetAddr, 1);

	g_capture.active = false;
	DebugLog("OneShot: cancelled");
}

// --- Method Pointer Resolution ---
// Finds the REAL call target for a method, checking:
// 1. methodPointer (may be stale if hotfix patched it)
// 2. vtable entry (for virtual methods — hotfix systems often patch the vtable)
// 3. invoker disassembly (scan for CALL instructions to find what il2cpp_runtime_invoke actually calls)
struct ResolvedMethodPtrs {
	void* methodPointer = nullptr;   // from MethodInfo.methodPointer
	void* vtablePointer = nullptr;   // from klass->vtable[slot] (if virtual)
	void* invokerTarget = nullptr;   // first CALL target inside invoker_method
	void* bestTarget = nullptr;      // the one we should hook
	std::string notes;
};

static ResolvedMethodPtrs ResolveMethodPointers(const MethodInfo* method, Il2CppClass* klass) {
	ResolvedMethodPtrs r;
	if (!method) return r;
	r.methodPointer = (void*)method->methodPointer;

	// 1. Check vtable for virtual methods
	if (method->slot != 65535 && klass) {
		Resolver::Protection::safe_call([&]() {
			// vtable is at the end of Il2CppClass struct, accessed via il2cpp API
			// But we can also read it directly: klass->vtable[slot].methodPtr
			// The VirtualInvokeData is { Il2CppMethodPointer methodPtr; const MethodInfo* method; }
			// vtable is an array at the end of the Il2CppClass struct
			uint16_t slot = method->slot;
			// il2cpp_class_get_methods populates vtable, but let's use the runtime way
			// We need to find the vtable. In il2cpp, vtable is at a fixed offset.
			// Use the runtime API approach: iterate to find the actual pointer
			void* mIter = nullptr;
			while (const MethodInfo* m = il2cpp_class_get_methods(klass, &mIter)) {
				if (m == method || (strcmp(m->name, method->name) == 0 &&
					m->parameters_count == method->parameters_count)) {
					if (m->methodPointer && m->methodPointer != method->methodPointer) {
						r.vtablePointer = (void*)m->methodPointer;
					}
					break;
				}
			}
		});
	}

	// 2. Scan invoker_method for CALL instructions to find real target
	if (method->invoker_method) {
		Resolver::Protection::safe_call([&]() {
			uint8_t* code = (uint8_t*)method->invoker_method;
			uintptr_t invokerAddr = (uintptr_t)code;

			// Collect all CALL targets in the first 200 bytes of invoker
			struct CallSite { uintptr_t target; int offset; };
			std::vector<CallSite> calls;
			for (int i = 0; i < 200; i++) {
				if (code[i] == 0xE8) {
					int32_t rel = *(int32_t*)(code + i + 1);
					uintptr_t target = (uintptr_t)(code + i + 5) + rel;
					calls.push_back({ target, i });
					i += 4; // skip rel32 bytes
				}
				else if (code[i] == 0xFF && i + 1 < 200) {
					uint8_t modrm = code[i + 1];
					if (modrm == 0x15) {
						// CALL [rip+disp32] (indirect)
						int32_t disp = *(int32_t*)(code + i + 2);
						uintptr_t memAddr = (uintptr_t)(code + i + 6) + disp;
						uintptr_t target = *(uintptr_t*)memAddr;
						calls.push_back({ target, i });
						i += 5;
					}
				}
				else if (code[i] == 0xC3 || code[i] == 0xCC) {
					break; // hit RET or INT3 — stop scanning
				}
			}

			// Pick the best call: prefer one that targets methodPointer (confirms it),
			// or the last call before RET (usually the actual method call in invoker pattern)
			// Runtime helpers like il2cpp_runtime_class_init are called first, method last.
			uintptr_t mPtr = (uintptr_t)method->methodPointer;
			void* bestCall = nullptr;

			// First: check if any call targets methodPointer directly (confirms normal path)
			for (auto& c : calls) {
				if (c.target == mPtr) {
					bestCall = (void*)c.target;
					break;
				}
			}

			// If no direct match, use the LAST call (method call comes after arg setup)
			if (!bestCall && !calls.empty()) {
				bestCall = (void*)calls.back().target;
			}

			r.invokerTarget = bestCall;
		});
	}

	// 2b. If bestTarget starts with a JMP trampoline, follow it to the real body
	auto followTrampoline = [](void* addr) -> void* {
		if (!addr) return addr;
		void* result = addr;
		Resolver::Protection::safe_call([&]() {
			uint8_t* code = (uint8_t*)addr;
			// Follow up to 3 levels of trampolines
			for (int depth = 0; depth < 3; depth++) {
				if (code[0] == 0xE9) {
					int32_t rel = *(int32_t*)(code + 1);
					result = (void*)((uintptr_t)(code + 5) + rel);
					code = (uint8_t*)result;
				}
				else if (code[0] == 0xFF && code[1] == 0x25) {
					int32_t disp = *(int32_t*)(code + 2);
					uintptr_t memAddr = (uintptr_t)(code + 6) + disp;
					result = (void*)(*(uintptr_t*)memAddr);
					code = (uint8_t*)result;
				}
				else if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
					result = (void*)(*(uintptr_t*)(code + 2));
					code = (uint8_t*)result;
				}
				else break; // not a trampoline
			}
		});
		return result;
	};

	// 3. Follow trampolines on methodPointer to find actual body
	void* methodPtrResolved = followTrampoline(r.methodPointer);
	void* invokerResolved = r.invokerTarget ? followTrampoline(r.invokerTarget) : nullptr;

	// 4. Determine best target to hook
	// If methodPointer has a JMP trampoline, the real body is elsewhere — hook at methodPointer
	// (our INT3 replaces the JMP, catches all callers to that address)
	// If invokerTarget differs from methodPointer, there's a redirect — hook invokerTarget
	if (r.invokerTarget && r.invokerTarget != r.methodPointer &&
		invokerResolved != methodPtrResolved) {
		// Invoker calls a completely different address — likely hotfix redirect
		r.bestTarget = r.invokerTarget;
		r.notes = "invoker calls different addr (hotfix?)";
	}
	else if (r.vtablePointer && r.vtablePointer != r.methodPointer) {
		r.bestTarget = r.vtablePointer;
		r.notes = "vtable entry differs (override?)";
	}
	else {
		r.bestTarget = r.methodPointer;
		if (methodPtrResolved != r.methodPointer)
			r.notes = "trampoline (body follows JMP)";
		else
			r.notes = "direct";
	}

	return r;
}

// --- Persistent Hook Management ---
static std::shared_ptr<HookEntry> AddPersistentHook(const MethodInfo* method, Il2CppClass* fromClass, void* overrideAddr) {
	if (!method) return nullptr;
	// Force class init to ensure methodPointer is populated
	if (method->klass)
		Resolver::Protection::safe_call([&]() { il2cpp_runtime_class_init((Il2CppClass*)method->klass); });
	if (!method->methodPointer) return nullptr;

	Il2CppClass* filterKlass = fromClass ? fromClass : method->klass;
	void* addr = overrideAddr ? overrideAddr : (void*)method->methodPointer;

	// Cancel one-shot if conflicting
	if (g_capture.active && g_capture.targetAddr == addr)
		CancelOneShotCapture();

	// Register VEH if needed
	if (!g_vehHandle)
		g_vehHandle = AddVectoredExceptionHandler(1, MultiHookVEH);

	// Build display name from fromClass (the class the user is browsing), not method->klass
	std::string dispName;
	Resolver::Protection::safe_call([&]() {
		const char* ns = filterKlass ? il2cpp_class_get_namespace(filterKlass) : "";
		const char* cn = filterKlass ? il2cpp_class_get_name(filterKlass) : "?";
		dispName = (ns && ns[0]) ? (std::string(ns) + "." + cn + "::" + method->name)
			: (std::string(cn) + "::" + method->name);
	});

	std::lock_guard<std::mutex> lock(g_hooksMutex);

	// Check if this exact class already has a hook at this address
	auto slotIt = g_slots.find(addr);
	if (slotIt != g_slots.end()) {
		for (auto& existing : slotIt->second->hooks) {
			if (existing->filterClass == filterKlass && existing->method == method)
				return existing; // already hooked for this class+method
		}
	}

	// Create the HookEntry
	auto hook = std::make_shared<HookEntry>();
	hook->id = g_nextHookId++;
	hook->method = method;
	hook->targetAddr = addr;
	hook->isStatic = (method->flags & 0x0010) != 0;
	hook->paramCount = il2cpp_method_get_param_count(method);
	hook->totalArgCount = hook->paramCount + (hook->isStatic ? 0 : 1);
	hook->displayName = dispName;
	hook->filterClass = filterKlass;

	// Pre-compute which params are arrays (for VEH-time snapshot)
	hook->numArrayParams = 0;
	for (int pi = 0; pi < hook->paramCount && hook->numArrayParams < 8; pi++) {
		const Il2CppType* pType = il2cpp_method_get_param(method, pi);
		if (!pType) continue;
		int pt = pType->type;
		if (pt == IL2CPP_TYPE_SZARRAY || pt == IL2CPP_TYPE_ARRAY) {
			auto& ap = hook->arrayParams[hook->numArrayParams];
			ap.paramIdx = pi;
			ap.argIdx = pi + (hook->isStatic ? 0 : 1);
			ap.elemSize = 1;
			Resolver::Protection::safe_call([&]() {
				Il2CppClass* paramClass = il2cpp_class_from_type(pType);
				if (paramClass) ap.elemSize = (uint32_t)il2cpp_array_element_size(paramClass);
			});
			if (ap.elemSize == 0) ap.elemSize = 1;
			hook->numArrayParams++;
		}
	}

	// Get or create the BreakpointSlot for this address
	std::shared_ptr<BreakpointSlot> slot;
	if (slotIt != g_slots.end()) {
		// Address already has an INT3 — just add our hook to the slot
		slot = slotIt->second;
	}
	else {
		// First hook at this address — create slot and patch INT3
		slot = std::make_shared<BreakpointSlot>();
		slot->addr = addr;

		DWORD oldProt;
		VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
		slot->savedByte = *(uint8_t*)addr;
		*(uint8_t*)addr = 0xCC;
		VirtualProtect(addr, 1, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), addr, 1);
		slot->armed.store(true);

		g_slots[addr] = slot;
	}

	hook->savedByte = slot->savedByte; // copy for reference
	slot->hooks.push_back(hook);
	g_hookList.push_back(hook);

	// Ensure the INT3 is armed if it was paused (a new hook should activate the breakpoint)
	if (!slot->armed.load() && !slot->singleStepping.load()) {
		DWORD oldProt;
		VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
		*(uint8_t*)addr = 0xCC;
		VirtualProtect(addr, 1, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), addr, 1);
		slot->armed.store(true);
	}

	DebugLog("Hook: armed persistent on %s [filter: %s] addr=0x%p filterClass=0x%p method=0x%p, %d args",
		method->name, dispName.c_str(), addr, (void*)filterKlass, (void*)method, hook->totalArgCount);
	return hook;
}

// Helper: check if any non-paused hook exists in a slot
static bool SlotHasActiveHook(const std::shared_ptr<BreakpointSlot>& slot) {
	for (auto& h : slot->hooks)
		if (!h->paused.load()) return true;
	return false;
}

// Helper: arm or disarm a slot's INT3 based on whether any hooks are active
// Must be called with g_hooksMutex held or with safe access
static void UpdateSlotArm(const std::shared_ptr<BreakpointSlot>& slot) {
	if (!slot || slot->singleStepping.load()) return; // don't touch during single-step
	bool needArm = SlotHasActiveHook(slot);
	if (needArm && !slot->armed.load()) {
		DWORD oldProt;
		VirtualProtect(slot->addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
		*(uint8_t*)slot->addr = 0xCC;
		VirtualProtect(slot->addr, 1, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), slot->addr, 1);
		slot->armed.store(true);
	}
	else if (!needArm && slot->armed.load()) {
		DWORD oldProt;
		VirtualProtect(slot->addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
		*(uint8_t*)slot->addr = slot->savedByte;
		VirtualProtect(slot->addr, 1, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), slot->addr, 1);
		slot->armed.store(false);
	}
}

static void RemovePersistentHook(std::shared_ptr<HookEntry> hook) {
	if (!hook) return;
	hook->paused.store(true); // prevent re-arm on pending single-step

	{
		std::lock_guard<std::mutex> lock(g_hooksMutex);

		// Remove hook from the slot
		auto slotIt = g_slots.find(hook->targetAddr);
		if (slotIt != g_slots.end()) {
			auto& slot = slotIt->second;
			auto& hv = slot->hooks;
			hv.erase(std::remove(hv.begin(), hv.end(), hook), hv.end());

			if (hv.empty()) {
				// Last hook at this address — restore original byte and remove slot
				if (slot->armed.load() || slot->singleStepping.load()) {
					DWORD oldProt;
					VirtualProtect(slot->addr, 1, PAGE_EXECUTE_READWRITE, &oldProt);
					*(uint8_t*)slot->addr = slot->savedByte;
					VirtualProtect(slot->addr, 1, oldProt, &oldProt);
					FlushInstructionCache(GetCurrentProcess(), slot->addr, 1);
					slot->armed.store(false);
				}
				g_slots.erase(slotIt);
			}
			else {
				// Other hooks remain — update INT3 state based on remaining hooks
				UpdateSlotArm(slot);
			}
		}

		g_hookList.erase(
			std::remove(g_hookList.begin(), g_hookList.end(), hook),
			g_hookList.end());
	}

	if (g_selectedHook == hook) {
		g_selectedHook = nullptr;
		g_selectedCallIdx = -1;
		g_hasSelectedCall = false;
	}

	DebugLog("Hook: removed %s", hook->displayName.c_str());
}

static void TogglePauseHook(std::shared_ptr<HookEntry> hook) {
	if (!hook) return;
	bool wasPaused = hook->paused.load();

	if (wasPaused) {
		// Resume this hook
		hook->paused.store(false);
	}
	else {
		// Pause this hook
		hook->paused.store(true);
	}

	// Update the shared breakpoint slot based on whether ANY hook at this address is active
	{
		std::lock_guard<std::mutex> lock(g_hooksMutex);
		auto slotIt = g_slots.find(hook->targetAddr);
		if (slotIt != g_slots.end())
			UpdateSlotArm(slotIt->second);
	}
}

static void RemoveAllHooks() {
	// Take copy of hook list, then remove each
	std::vector<std::shared_ptr<HookEntry>> copy;
	{
		std::lock_guard<std::mutex> lock(g_hooksMutex);
		copy = g_hookList;
	}
	for (auto& h : copy) RemovePersistentHook(h);
}

// --- Stack Trace Resolution ---
// Helper: recursively collect methods from a class and its nested types
static void CollectMethodsFromClass(Il2CppClass* klass) {
	if (!klass) return;
	Resolver::Protection::safe_call([&]() {
		void* mIter = nullptr;
		while (const MethodInfo* m = il2cpp_class_get_methods(klass, &mIter)) {
			if (m->methodPointer)
				g_addrToMethod.push_back({ (uintptr_t)m->methodPointer, m });
		}
		// Also collect from nested types
		void* nIter = nullptr;
		while (Il2CppClass* nested = (Il2CppClass*)il2cpp_class_get_nested_types(klass, &nIter)) {
			CollectMethodsFromClass(nested);
		}
	});
}

static void BuildAddrToMethodMap() {
	if (g_addrMapBuilt.load()) return;
	std::lock_guard<std::mutex> lock(g_addrMapMutex);
	if (g_addrMapBuilt.load()) return;

	auto domain = il2cpp_domain_get();
	size_t asmCount = 0;
	const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);
	if (!assemblies) { g_addrMapBuilt.store(true); return; }

	for (size_t a = 0; a < asmCount; a++) {
		const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[a]);
		if (!image) continue;
		size_t classCount = il2cpp_image_get_class_count(image);
		for (size_t c = 0; c < classCount; c++) {
			Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, c);
			CollectMethodsFromClass(klass);
		}
	}

	std::sort(g_addrToMethod.begin(), g_addrToMethod.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	// Remove duplicates (same address from generic instantiations)
	g_addrToMethod.erase(
		std::unique(g_addrToMethod.begin(), g_addrToMethod.end(),
			[](const auto& a, const auto& b) { return a.first == b.first; }),
		g_addrToMethod.end());

	g_addrMapBuilt.store(true);
	DebugLog("Hook: built addr->method map with %d entries", (int)g_addrToMethod.size());
}

// Resolve a module name + offset for addresses outside IL2CPP (Unity engine, system DLLs)
static std::string ResolveModuleName(uintptr_t addr) {
	HMODULE hMod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)addr, &hMod) && hMod) {
		char modPath[MAX_PATH] = {};
		if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
			// Extract just the filename
			const char* name = modPath;
			for (const char* p = modPath; *p; p++) {
				if (*p == '\\' || *p == '/') name = p + 1;
			}
			uintptr_t base = (uintptr_t)hMod;
			char buf[256];
			sprintf_s(buf, "%s+0x%llX", name, (uint64_t)(addr - base));
			return buf;
		}
	}
	char buf[32];
	sprintf_s(buf, "0x%llX", (uint64_t)addr);
	return buf;
}

static std::string ResolveAddress(uintptr_t addr) {
	if (!g_addrMapBuilt.load()) BuildAddrToMethodMap();

	// Try IL2CPP method map first
	auto it = std::upper_bound(g_addrToMethod.begin(), g_addrToMethod.end(),
		std::make_pair(addr, (const MethodInfo*)nullptr),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	if (it != g_addrToMethod.begin()) {
		--it;
		// Check if address is within reasonable range of method start
		// Use next method's address as upper bound if available, else 256KB
		uintptr_t maxRange = 0x40000; // 256KB default
		auto next = it;
		++next;
		if (next != g_addrToMethod.end())
			maxRange = next->first - it->first;

		if (addr >= it->first && (addr - it->first) < maxRange) {
			const MethodInfo* m = it->second;
			std::string result;
			Resolver::Protection::safe_call([&]() {
				const char* cn = m->klass ? il2cpp_class_get_name(m->klass) : "?";
				const char* ns = m->klass ? il2cpp_class_get_namespace(m->klass) : "";
				if (ns && ns[0])
					result = std::string(ns) + "." + cn + "::" + m->name;
				else
					result = std::string(cn) + "::" + m->name;
			});
			if (!result.empty()) {
				char off[32];
				sprintf_s(off, "+0x%X", (unsigned)(addr - it->first));
				return result + off;
			}
		}
	}

	// Fallback: resolve module name (UnityPlayer.dll, GameAssembly.dll, ntdll.dll, etc.)
	return ResolveModuleName(addr);
}

static void ResolveCallStack(HookCallRecord& rec) {
	if (!rec.stackResolved.empty() || rec.stack.empty()) return;
	BuildAddrToMethodMap();
	rec.stackResolved.reserve(rec.stack.size());
	for (auto a : rec.stack)
		rec.stackResolved.push_back(ResolveAddress(a));
}

// --- Shared Hex Editor Helpers ---

// Parse a hex string like "00 01 0E 1A" or "00010E1A" or "0x00,0x01" into bytes
static std::vector<uint8_t> ParseHexString(const char* input) {
	std::vector<uint8_t> result;
	const char* p = input;
	while (*p) {
		// Skip whitespace, commas, semicolons
		while (*p && (*p == ' ' || *p == '\t' || *p == ',' || *p == ';' || *p == '\n' || *p == '\r')) p++;
		if (!*p) break;
		// Skip "0x" or "0X" prefix
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
		if (!*p) break;
		// Read hex digit(s)
		char hi = 0, lo = 0;
		bool hasHi = false, hasLo = false;
		if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
			hi = *p++; hasHi = true;
		}
		if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
			lo = *p++; hasLo = true;
		}
		if (hasHi) {
			if (hasLo) {
				char tmp[3] = { hi, lo, 0 };
				result.push_back((uint8_t)strtoul(tmp, nullptr, 16));
			}
			else {
				char tmp[2] = { hi, 0 };
				result.push_back((uint8_t)strtoul(tmp, nullptr, 16));
			}
		}
		else {
			p++; // skip unknown char
		}
	}
	return result;
}

// Draw hex editor widget for a data buffer. Returns true if data was modified.
// widgetId must be unique across all hex editors visible at once.
struct HexPasteState { std::string buf; int offset = 0; bool showInput = false; };

// ImGui InputText callback for std::string resize
static int HexInputTextCallback(ImGuiInputTextCallbackData* data) {
	if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
		std::string* str = (std::string*)data->UserData;
		str->resize(data->BufTextLen);
		data->Buf = (char*)str->c_str();
	}
	return 0;
}

static bool DrawHexEditorWidget(uint8_t* data, uint32_t byteLen, int widgetId) {
	if (!data || byteLen == 0) return false;
	bool modified = false;

	ImGui::PushID(widgetId);

	// Copy Hex button
	if (ImGui::SmallButton("Copy Hex")) {
		std::string hexStr;
		hexStr.reserve(byteLen * 3);
		for (uint32_t b = 0; b < byteLen; b++) {
			char hb[4]; sprintf_s(hb, "%02X ", data[b]);
			hexStr += hb;
		}
		Resolver::Helpers::CopyToClipboard(hexStr.c_str());
	}

	// Paste section
	static std::map<int, HexPasteState> pasteStates;
	auto& ps = pasteStates[widgetId];

	ImGui::SameLine();
	if (ImGui::SmallButton("Paste from Clipboard")) {
		const char* clip = ImGui::GetClipboardText();
		if (clip) {
			auto parsed = ParseHexString(clip);
			if (!parsed.empty()) {
				uint32_t available = byteLen - (uint32_t)ps.offset;
				uint32_t toCopy = ((uint32_t)parsed.size() > available) ? available : (uint32_t)parsed.size();
				memcpy(data + ps.offset, parsed.data(), toCopy);
				modified = true;
			}
		}
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Read hex from clipboard and apply at offset %d", ps.offset);

	ImGui::SameLine();
	ImGui::SetNextItemWidth(60);
	ImGui::InputInt("##off", &ps.offset, 0, 0);
	if (ps.offset < 0) ps.offset = 0;
	if (ps.offset >= (int)byteLen) ps.offset = (int)byteLen - 1;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Offset to paste at");

	// Expandable manual hex input
	ImGui::SameLine();
	ImGui::Checkbox("Manual Input", &ps.showInput);
	if (ps.showInput) {
		ImGui::InputTextMultiline("##hexinput", (char*)ps.buf.c_str(), ps.buf.capacity() + 1,
			ImVec2(-1, 60), ImGuiInputTextFlags_CallbackResize, HexInputTextCallback, &ps.buf);
		if (ImGui::SmallButton("Apply Hex")) {
			auto parsed = ParseHexString(ps.buf.c_str());
			if (!parsed.empty()) {
				uint32_t available = byteLen - (uint32_t)ps.offset;
				uint32_t toCopy = ((uint32_t)parsed.size() > available) ? available : (uint32_t)parsed.size();
				memcpy(data + ps.offset, parsed.data(), toCopy);
				modified = true;
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(%d bytes in buffer, offset %d)", (int)ParseHexString(ps.buf.c_str()).size(), ps.offset);
	}

	// Show all checkbox for large buffers
	uint32_t displayBytes = (byteLen > 512) ? 512 : byteLen;
	static std::map<int, bool> showFullMap;
	if (byteLen > 512) {
		ImGui::SameLine();
		ImGui::Checkbox("All", &showFullMap[widgetId]);
		if (showFullMap[widgetId]) displayBytes = byteLen;
	}

	// Edit state — only one byte is edited at a time globally
	static int s_editWidgetId = -1;
	static int s_editByteOff = -1;
	static char s_editHexInput[3] = {};
	static bool s_editJustStarted = false;

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 1));
	for (uint32_t row = 0; row < displayBytes; row += 16) {
		char rowLabel[16];
		sprintf_s(rowLabel, "  %04X:", row);
		ImGui::TextDisabled("%s", rowLabel);

		for (uint32_t col = 0; col < 16 && (row + col) < displayBytes; col++) {
			uint32_t off = row + col;
			ImGui::SameLine();
			ImGui::PushID((int)off);

			bool isEditing = (s_editWidgetId == widgetId && s_editByteOff == (int)off);
			if (isEditing) {
				ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(22);
				if (ImGui::InputText("##e", s_editHexInput, 3,
					ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
					data[off] = (uint8_t)strtoul(s_editHexInput, nullptr, 16);
					modified = true;
					if (off + 1 < byteLen) {
						s_editByteOff = off + 1;
						sprintf_s(s_editHexInput, "%02X", data[off + 1]);
						s_editJustStarted = true;
					}
					else {
						s_editWidgetId = -1; s_editByteOff = -1;
					}
				}
				if (s_editJustStarted) {
					s_editJustStarted = false;
				}
				else if (!ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
					s_editWidgetId = -1; s_editByteOff = -1;
				}
			}
			else {
				char hexByte[4];
				sprintf_s(hexByte, "%02X", data[off]);
				if (ImGui::SmallButton(hexByte)) {
					s_editWidgetId = widgetId;
					s_editByteOff = (int)off;
					sprintf_s(s_editHexInput, "%02X", data[off]);
					s_editJustStarted = true;
				}
			}
			ImGui::PopID();
		}

		// ASCII column
		ImGui::SameLine(0, 8);
		char ascii[17] = {};
		for (uint32_t col = 0; col < 16 && (row + col) < displayBytes; col++) {
			uint8_t c = data[row + col];
			ascii[col] = (c >= 32 && c < 127) ? (char)c : '.';
		}
		ImGui::TextDisabled("%s", ascii);
	}
	ImGui::PopStyleVar();
	if (displayBytes < byteLen)
		ImGui::TextDisabled("  ... %u more bytes", byteLen - displayBytes);

	ImGui::PopID();
	return modified;
}

// --- Hook Detail View (right panel when Hooks tab selected) ---
static void DrawHookDetail(std::shared_ptr<HookEntry> hook) {
	if (!hook) return;

	// Header
	ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", hook->displayName.c_str());
	{
		// Show filter class and whether shared
		std::string filterInfo;
		if (hook->filterClass) {
			const char* fn = hook->filterClass->name ? hook->filterClass->name : "?";
			filterInfo = std::string("Filter: ") + fn;
			// Check if other hooks share this address
			std::lock_guard<std::mutex> lock(g_hooksMutex);
			auto slotIt = g_slots.find(hook->targetAddr);
			if (slotIt != g_slots.end() && slotIt->second->hooks.size() > 1)
				filterInfo += " (shared addr, " + std::to_string(slotIt->second->hooks.size()) + " hooks)";
		}
		ImGui::TextDisabled("Address: 0x%p | Params: %d | Total calls: %llu",
			hook->targetAddr, hook->paramCount, hook->totalCallCount.load());
		if (!filterInfo.empty())
			ImGui::TextDisabled("%s", filterInfo.c_str());
	}

	// Controls
	bool hp = hook->paused.load();
	if (ImGui::Button(hp ? "Resume" : "Pause")) TogglePauseHook(hook);
	ImGui::SameLine();
	if (ImGui::Button("Remove")) { RemovePersistentHook(hook); return; }
	ImGui::SameLine();
	if (ImGui::Button("Clear Log")) {
		std::lock_guard<std::mutex> lock(hook->logMutex);
		hook->callLog.clear();
		g_hasSelectedCall = false;
	}
	ImGui::SameLine();
	if (ImGui::Button("Go to Class")) {
		Il2CppClass* navClass = hook->filterClass ? hook->filterClass : (Il2CppClass*)hook->method->klass;
		if (navClass) {
			UnityExplorerTAB::Helpers::InspectClass(navClass);
		}
	}

	// Show "Custom Pass" checkbox for SendRaw hooks
	if (hook->method && hook->method->name && strcmp(hook->method->name, "SendRaw") == 0) {
		ImGui::SameLine();
		ImGui::Checkbox("Custom Pass", &hook->customPass);
		if (hook->customPass)
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Custom Pass ACTIVE: msgID 4709 len 60 -> patch last 4 bytes");
	}

	// Flush Custom Pass logs to file (deferred from VEH)
	{
		std::vector<CustomPassLogEntry> pending;
		if (g_customPassLogMutex.try_lock()) {
			pending.swap(g_customPassPendingLogs);
			g_customPassLogMutex.unlock();
		}
		if (!pending.empty()) {
			std::ofstream logFile("d:\\dp\\custompass_log.txt", std::ios::app);
			for (auto& e : pending) {
				logFile << "=== Custom Pass (msgId=" << e.msgId << " len=" << e.len << ") ===\n";
				logFile << "Original:\n";
				for (int i = 0; i < e.len; i++) {
					char buf[4]; snprintf(buf, sizeof(buf), "%02X ", e.original[i]);
					logFile << buf;
					if ((i + 1) % 16 == 0) logFile << "\n";
				}
				logFile << "\nModified:\n";
				for (int i = 0; i < e.len; i++) {
					char buf[4]; snprintf(buf, sizeof(buf), "%02X ", e.modified[i]);
					logFile << buf;
					if ((i + 1) % 16 == 0) logFile << "\n";
				}
				logFile << "\n\n";
			}
		}
	}

	ImGui::Separator();

	// Take a FULL copy of the log once per frame (100 entries max, stable for rendering)
	static std::vector<HookCallRecord> logCopy;
	static uint32_t logCopyHookId = 0;
	static size_t logCopySize = 0;
	{
		std::lock_guard<std::mutex> lock(hook->logMutex);
		// Re-copy if hook changed or log size changed
		if (logCopyHookId != hook->id || hook->callLog.size() != logCopySize) {
			logCopy = hook->callLog;
			logCopyHookId = hook->id;
			logCopySize = logCopy.size();
		}
	}

	if (logCopy.empty()) {
		ImGui::TextDisabled("No calls recorded yet.");
		if (hp) ImGui::TextDisabled("Hook is paused - resume to start capturing.");
		return;
	}

	// Auto-select latest if nothing selected
	if (!g_hasSelectedCall && !logCopy.empty()) {
		g_selectedCallRecord = logCopy.back();
		g_selectedCallTimestamp = g_selectedCallRecord.timestamp;
		g_selectedCallIdx = (int)logCopy.size() - 1;
		g_hasSelectedCall = true;
	}

	// Refresh button to re-read log
	ImGui::Text("Call Log (%d entries):", (int)logCopy.size());
	ImGui::SameLine();
	if (ImGui::SmallButton("Refresh")) {
		std::lock_guard<std::mutex> lock(hook->logMutex);
		logCopy = hook->callLog;
		logCopySize = logCopy.size();
		// Auto-select latest after refresh
		if (!logCopy.empty()) {
			g_selectedCallRecord = logCopy.back();
			g_selectedCallTimestamp = g_selectedCallRecord.timestamp;
			g_selectedCallIdx = (int)logCopy.size() - 1;
			g_hasSelectedCall = true;
		}
	}

	// Call log list (newest first, scrollable) — reads from stable logCopy
	ImGui::BeginChild("CallLogList", ImVec2(0, 160), true);
	for (int i = (int)logCopy.size() - 1; i >= 0; i--) {
		ImGui::PushID(i);
		auto& entry = logCopy[i];

		bool isSelected = (g_hasSelectedCall && g_selectedCallIdx == i &&
			g_selectedCallTimestamp == entry.timestamp);

		uint64_t firstVal = entry.totalArgCount > 0 ? entry.args[0].regVal : 0;

		// Stable ID label — time is rendered separately so it doesn't break selection
		char idLabel[128];
		sprintf_s(idLabel, "#%d  0x%llX  (%d args)##call_%d_%llu", i, firstVal, entry.totalArgCount, i, entry.timestamp);

		if (ImGui::Selectable(idLabel, isSelected)) {
			// Copy from the STABLE logCopy, not the live log
			g_selectedCallRecord = logCopy[i];
			g_selectedCallTimestamp = g_selectedCallRecord.timestamp;
			g_selectedCallIdx = i;
			g_hasSelectedCall = true;
		}

		ImGui::PopID();
	}
	ImGui::EndChild();

	// Display selected call detail from the STABLE COPY
	if (!g_hasSelectedCall) {
		ImGui::TextDisabled("Select a call entry above to view details.");
		return;
	}

	ImGui::Separator();
	auto& rec = g_selectedCallRecord;

	// Show call time info
	{
		uint64_t ago = GetTickCount64() - rec.timestamp;
		char timeBuf[128];
		if (ago < 1000)
			sprintf_s(timeBuf, "Call #%d  |  %llums ago  |  %d args", g_selectedCallIdx, ago, rec.totalArgCount);
		else if (ago < 60000)
			sprintf_s(timeBuf, "Call #%d  |  %.1fs ago  |  %d args", g_selectedCallIdx, ago / 1000.0, rec.totalArgCount);
		else
			sprintf_s(timeBuf, "Call #%d  |  %.0fm ago  |  %d args", g_selectedCallIdx, ago / 60000.0, rec.totalArgCount);
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%s", timeBuf);
	}

	// Parameters
	ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.8f, 1.0f), "Parameters:");
	ImGui::Indent();

	bool isStaticM = hook->isStatic;
	int argOff = isStaticM ? 0 : 1;

	if (!isStaticM && rec.totalArgCount > 0) {
		char buf[64]; sprintf_s(buf, "this = 0x%llX", rec.args[0].regVal);
		ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "%s", buf);
		if (rec.args[0].regVal) {
			ImGui::SameLine();
			if (ImGui::SmallButton("Inspect##this")) {
				Il2CppObject* thisObj = (Il2CppObject*)rec.args[0].regVal;
				if (Resolver::Protection::IsValidIl2CppObject(thisObj))
					UnityExplorerTAB::Helpers::InspectObject(thisObj);
			}
		}
	}

	for (int pi = 0; pi < hook->paramCount; pi++) {
		int ai = pi + argOff;
		if (ai >= rec.totalArgCount) break;
		const char* pName = il2cpp_method_get_param_name(hook->method, pi);
		const char* pLabel = pName ? pName : "arg";

		const Il2CppType* pType = il2cpp_method_get_param(hook->method, pi);
		int pt = pType ? pType->type : -1;

		ImGui::PushID(pi + 80000);

		// Editable primitive params for replay
		bool drawnEditable = false;
		if (pType) {
			switch (pt) {
			case IL2CPP_TYPE_BOOLEAN: {
				bool v = (rec.args[ai].regVal != 0);
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				if (ImGui::Checkbox("##v", &v))
					rec.args[ai].regVal = v ? 1 : 0;
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_I1: case IL2CPP_TYPE_U1:
			case IL2CPP_TYPE_I2: case IL2CPP_TYPE_U2:
			case IL2CPP_TYPE_I4: {
				int32_t v = (int32_t)rec.args[ai].regVal;
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::InputInt("##v", &v))
					rec.args[ai].regVal = (uint64_t)(int64_t)v;
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_U4: {
				int v = (int)(uint32_t)rec.args[ai].regVal;
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::InputInt("##v", &v))
					rec.args[ai].regVal = (uint64_t)(uint32_t)v;
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_I8: {
				int64_t v = (int64_t)rec.args[ai].regVal;
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(160);
				if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v))
					rec.args[ai].regVal = (uint64_t)v;
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_U8: {
				uint64_t v = rec.args[ai].regVal;
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(160);
				if (ImGui::InputScalar("##v", ImGuiDataType_U64, &v))
					rec.args[ai].regVal = v;
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_R4: {
				float v;
				memcpy(&v, &rec.args[ai].xmmLow, sizeof(float));
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::InputFloat("##v", &v, 0, 0, "%.4f"))
					memcpy(&rec.args[ai].xmmLow, &v, sizeof(float));
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_R8: {
				double v;
				memcpy(&v, &rec.args[ai].xmmLow, sizeof(double));
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(160);
				if (ImGui::InputDouble("##v", &v, 0, 0, "%.6f"))
					memcpy(&rec.args[ai].xmmLow, &v, sizeof(double));
				drawnEditable = true;
			} break;
			case IL2CPP_TYPE_VALUETYPE: {
				// Enum — edit as int32
				int32_t v = (int32_t)rec.args[ai].regVal;
				ImGui::Text("  %s:", pLabel); ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::InputInt("##v", &v))
					rec.args[ai].regVal = (uint64_t)(int64_t)v;
				ImGui::SameLine(); ImGui::TextDisabled("(enum/valuetype)");
				drawnEditable = true;
			} break;
			}
		}

		if (!drawnEditable) {
			// Non-editable: show decoded text + inspect button for objects
			std::string decoded;
			Resolver::Protection::safe_call([&]() {
				decoded = DecodeCapturedArg(hook->method, pi, rec.args[ai].regVal, rec.args[ai].xmmLow);
			});
			ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.8f, 1.0f), "  %s = %s", pLabel, decoded.c_str());

			if (pType && rec.args[ai].regVal) {
				if (pt == IL2CPP_TYPE_CLASS || pt == IL2CPP_TYPE_OBJECT || pt == IL2CPP_TYPE_GENERICINST) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Inspect")) {
						Il2CppObject* pObj = (Il2CppObject*)rec.args[ai].regVal;
						if (Resolver::Protection::IsValidIl2CppObject(pObj))
							UnityExplorerTAB::Helpers::InspectObject(pObj);
					}
				}
			}
		}

		ImGui::PopID();

		// Hex editor for array buffer snapshots belonging to this param
		for (size_t bi = 0; bi < rec.buffers.size(); bi++) {
			auto& hcb = rec.buffers[bi];
			if (!hcb.valid || hcb.paramIdx != pi) continue;

			char hdr[128];
			sprintf_s(hdr, "  Buffer: %u bytes (%u elems x %u)",
				hcb.byteLen, hcb.arrayLen, hcb.elemSize);
			ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", hdr);
			if (hcb.byteLen < hcb.arrayLen * hcb.elemSize) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1, 1, 0, 1), "(truncated from %u)", hcb.arrayLen * hcb.elemSize);
			}

			// Unique widget ID: combine call index + buffer index
			int wid = 200000 + g_selectedCallIdx * 100 + (int)bi;
			DrawHexEditorWidget(hcb.data.data(), hcb.byteLen, wid);
		}
	}
	ImGui::Unindent();

	// Replay button — re-invoke the method with captured (and possibly edited) params
	{
		static std::string hookReplayResult;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.3f, 1.0f));
		if (ImGui::Button("Replay Call")) {
			hookReplayResult.clear();

			// Lazily create GC clones for any array buffers that need them
			for (auto& hcb : rec.buffers) {
				if (!hcb.valid) continue;
				Resolver::Protection::safe_call([&]() {
					if (!hcb.originalArrayPtr) return;
					if (!Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)hcb.originalArrayPtr)) return;
					Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)hcb.originalArrayPtr);
					Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
					if (!elemClass) return;
					Il2CppArray* clone = il2cpp_array_new(elemClass, hcb.arrayLen);
					if (!clone) return;
					void* dstData = (void*)((uintptr_t)clone + sizeof(Il2CppArray));
					memcpy(dstData, hcb.data.data(), hcb.byteLen);
					hcb.clonedArray = clone;
				});
			}

			// Build parameter list
			int paramCount = hook->paramCount;
			std::vector<void*> rawParams;
			std::vector<uint8_t> replayStorage(paramCount * 64, 0);
			for (int i = 0; i < paramCount; i++) {
				int ai = i + argOff;
				const Il2CppType* pType = il2cpp_method_get_param(hook->method, i);
				if (!pType) { rawParams.push_back(nullptr); continue; }
				int pt = pType->type;
				uint8_t* rslot = &replayStorage[i * 64];
				if (pt == IL2CPP_TYPE_STRING || pt == IL2CPP_TYPE_CLASS || pt == IL2CPP_TYPE_OBJECT ||
					pt == IL2CPP_TYPE_GENERICINST) {
					rawParams.push_back((void*)rec.args[ai].regVal);
				}
				else if (pt == IL2CPP_TYPE_SZARRAY || pt == IL2CPP_TYPE_ARRAY) {
					void* arrayToUse = (void*)rec.args[ai].regVal;
					for (auto& hcb : rec.buffers) {
						if (hcb.valid && hcb.paramIdx == i && hcb.clonedArray) {
							arrayToUse = (void*)hcb.clonedArray;
							break;
						}
					}
					rawParams.push_back(arrayToUse);
				}
				else if (pt == IL2CPP_TYPE_R4) {
					memcpy(rslot, &rec.args[ai].xmmLow, sizeof(float));
					rawParams.push_back(rslot);
				}
				else if (pt == IL2CPP_TYPE_R8) {
					memcpy(rslot, &rec.args[ai].xmmLow, sizeof(double));
					rawParams.push_back(rslot);
				}
				else {
					uint64_t val = rec.args[ai].regVal;
					memcpy(rslot, &val, sizeof(val));
					rawParams.push_back(rslot);
				}
			}

			Il2CppObject* thisObj = isStaticM ? nullptr : (Il2CppObject*)rec.args[0].regVal;
			if (thisObj && !Resolver::Protection::IsValidIl2CppObject(thisObj))
				thisObj = nullptr;
			Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(
				hook->method, thisObj, rawParams.data());
			if (!res) hookReplayResult = "=> Null / Void";
			else {
				char hx[64]; sprintf_s(hx, "=> 0x%p", res);
				hookReplayResult = hx;
			}
		}
		ImGui::PopStyleColor();
		if (!hookReplayResult.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", hookReplayResult.c_str());
		}
	}

	// Deep Snapshot (captured at hook time — survives object lifetime)
	if (!rec.snapshots.empty()) {
		ImGui::Separator();
		ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.6f, 1.0f), "Snapshot (captured at call time):");
		ImGui::BeginChild("HookSnapshot", ImVec2(0, 250), true);
		for (size_t si = 0; si < rec.snapshots.size(); si++) {
			DrawSnapNode(rec.snapshots[si].root, (int)(si + 95000));
		}
		ImGui::EndChild();
	}

	// Stack trace
	ImGui::Separator();
	ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Stack Trace:");

	if (rec.stackResolved.empty() && !rec.stack.empty())
		ResolveCallStack(g_selectedCallRecord);

	if (rec.stack.empty()) {
		ImGui::TextDisabled("  (no stack captured)");
	}
	else {
		ImGui::BeginChild("StackTraceView", ImVec2(0, 180), true);
		for (size_t si = 0; si < rec.stackResolved.size(); si++) {
			ImGui::PushID((int)si);
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "  [%d] %s",
				(int)si, rec.stackResolved[si].c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Cp"))
				Resolver::Helpers::CopyToClipboard(rec.stackResolved[si].c_str());
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	// Copy buttons
	ImGui::Spacing();
	if (ImGui::Button("Copy Call Info")) {
		std::string dump = "=== Hook Call ===\n";
		dump += "Method: " + hook->displayName + "\n";
		dump += "Parameters:\n";
		if (!isStaticM && rec.totalArgCount > 0) {
			char b[64]; sprintf_s(b, "  this = 0x%llX\n", rec.args[0].regVal);
			dump += b;
		}
		for (int pi = 0; pi < hook->paramCount; pi++) {
			int ai = pi + argOff;
			if (ai >= rec.totalArgCount) break;
			const char* pn = il2cpp_method_get_param_name(hook->method, pi);
			std::string dec;
			Resolver::Protection::safe_call([&]() {
				dec = DecodeCapturedArg(hook->method, pi, rec.args[ai].regVal, rec.args[ai].xmmLow);
			});
			dump += "  " + std::string(pn ? pn : "arg") + " = " + dec + "\n";
		}
		dump += "Stack Trace:\n";
		for (size_t si = 0; si < rec.stackResolved.size(); si++)
			dump += "  [" + std::to_string(si) + "] " + rec.stackResolved[si] + "\n";
		Resolver::Helpers::CopyToClipboard(dump.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Copy All Calls")) {
		std::string dump = "=== Hook: " + hook->displayName + " ===\n";
		dump += "Total calls: " + std::to_string(hook->totalCallCount.load()) + "\n\n";
		for (int ci = (int)logCopy.size() - 1; ci >= 0; ci--) {
			auto& r = logCopy[ci];
			dump += "--- Call #" + std::to_string(ci) + " ---\n";
			if (!isStaticM && r.totalArgCount > 0) {
				char b[64]; sprintf_s(b, "  this = 0x%llX\n", r.args[0].regVal);
				dump += b;
			}
			for (int pi = 0; pi < hook->paramCount; pi++) {
				int ai = pi + argOff;
				if (ai >= r.totalArgCount) break;
				const char* pn = il2cpp_method_get_param_name(hook->method, pi);
				std::string dec;
				Resolver::Protection::safe_call([&]() {
					dec = DecodeCapturedArg(hook->method, pi, r.args[ai].regVal, r.args[ai].xmmLow);
				});
				dump += "  " + std::string(pn ? pn : "arg") + " = " + dec + "\n";
			}
			dump += "\n";
		}
		Resolver::Helpers::CopyToClipboard(dump.c_str());
	}
}

void UnityExplorerTAB::Helpers::DrawMethods(Il2CppObject* obj, Il2CppClass* klass)
{
	if (ImGui::CollapsingHeader("Methods (Advanced)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		// Force class initialization so all method pointers are populated
		Resolver::Protection::safe_call([&]() { il2cpp_runtime_class_init(klass); });

		static bool showAccessors = false;
		ImGui::Checkbox("get/set", &showAccessors);
		ImGui::SameLine();
		ImGui::TextDisabled("(.ctor / .cctor always shown)");

		void* mIter = nullptr;

		while (const MethodInfo* method = il2cpp_class_get_methods(klass, &mIter)) {
			const char* mName = method->name;

			// Filter: hide property accessors by default (get_/set_) but show constructors
			if (!showAccessors && (strstr(mName, "get_") || strstr(mName, "set_"))) continue;
			if (!MatchesMemberFilter(mName)) {
				// Also check param names
				bool paramMatch = false;
				int mpc = il2cpp_method_get_param_count(method);
				for (int mpi = 0; mpi < mpc && !paramMatch; mpi++) {
					const char* mpn = il2cpp_method_get_param_name(method, mpi);
					if (mpn && MatchesMemberFilter(mpn)) paramMatch = true;
				}
				if (!paramMatch) continue;
			}

			ImGui::PushID(method);

			// Cache resolved pointers per MethodInfo (used by display + hook buttons)
			static std::unordered_map<const MethodInfo*, ResolvedMethodPtrs> s_resolveCache;
			if (method->methodPointer && s_resolveCache.find(method) == s_resolveCache.end())
				s_resolveCache.emplace(method, ResolveMethodPointers(method, klass));

			bool isStatic = (method->flags & 0x0010);
			if (isStatic) { ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "[Static]"); ImGui::SameLine(); }

			ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", mName);
			// Show method pointer + resolved invoker target + module info
			{
				ImGui::SameLine();
				if (!method->methodPointer) {
					ImGui::TextDisabled("[NULL ptr - not initialized]");
				}
				else {
					const auto& resolved = s_resolveCache[method];

					uintptr_t mPtr = (uintptr_t)resolved.methodPointer;
					const char* declClass = method->klass ? il2cpp_class_get_name(method->klass) : "?";

					// Resolve module for methodPointer
					auto resolveModule = [](uintptr_t addr) -> std::string {
						std::string mod;
						if (!addr) return mod;
						Resolver::Protection::safe_call([&]() {
							HMODULE hMod = nullptr;
							if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								(LPCSTR)addr, &hMod) && hMod) {
								char modPath[MAX_PATH] = {};
								if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
									const char* name = modPath;
									for (const char* p = modPath; *p; p++)
										if (*p == '\\' || *p == '/') name = p + 1;
									mod = name;
								}
							}
						});
						return mod;
					};
					std::string modInfo = resolveModule(mPtr);

					// Check if any hook exists at methodPointer OR invokerTarget
					int hooksAtAddr = 0;
					{
						std::lock_guard<std::mutex> lock(g_hooksMutex);
						auto sit = g_slots.find((void*)mPtr);
						if (sit != g_slots.end())
							hooksAtAddr = (int)sit->second->hooks.size();
						if (hooksAtAddr == 0 && resolved.invokerTarget && resolved.invokerTarget != resolved.methodPointer) {
							sit = g_slots.find(resolved.invokerTarget);
							if (sit != g_slots.end())
								hooksAtAddr = (int)sit->second->hooks.size();
						}
					}

					// Does the invoker call a DIFFERENT address than methodPointer?
					bool hasOriginal = resolved.invokerTarget && resolved.invokerTarget != resolved.methodPointer;

					char info[512];
					if (hasOriginal) {
						// Hotfixed: methodPointer is dispatcher, invokerTarget is the real body
						std::string origMod = resolveModule((uintptr_t)resolved.invokerTarget);
						sprintf_s(info, "[0x%llX] (%s) [%s] | original: 0x%llX [%s]",
							(uint64_t)mPtr, declClass,
							modInfo.empty() ? "?" : modInfo.c_str(),
							(uint64_t)resolved.invokerTarget,
							origMod.empty() ? "?" : origMod.c_str());
						if (hooksAtAddr > 0) {
							char buf[32]; sprintf_s(buf, " [%d hooks]", hooksAtAddr);
							strcat_s(info, buf);
						}
						ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", info);
					}
					else if (hooksAtAddr > 0) {
						sprintf_s(info, "[0x%llX] (%s) [%s] [%d hooks]",
							(uint64_t)mPtr, declClass, modInfo.empty() ? "?" : modInfo.c_str(), hooksAtAddr);
						ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", info);
					}
					else {
						sprintf_s(info, "[0x%llX] (%s) [%s] %s",
							(uint64_t)mPtr, declClass, modInfo.empty() ? "?" : modInfo.c_str(),
							resolved.notes.c_str());
						ImGui::TextDisabled("%s", info);
					}

					// Tooltip: hex dump of first 32 bytes at methodPointer + full resolve details
					if (ImGui::IsItemHovered()) {
						ImGui::BeginTooltip();
						ImGui::Text("MethodInfo*:    0x%llX", (uint64_t)method);
						ImGui::Text("methodPointer:  0x%llX  [%s]", (uint64_t)mPtr, modInfo.c_str());
						if (resolved.invokerTarget) {
							ImGui::Text("invokerTarget:  0x%llX%s",
								(uint64_t)resolved.invokerTarget,
								hasOriginal ? "  << ORIGINAL BODY" : "");
						}
						if (resolved.vtablePointer && resolved.vtablePointer != resolved.methodPointer)
							ImGui::Text("vtablePointer:  0x%llX", (uint64_t)resolved.vtablePointer);
						if (!resolved.notes.empty())
							ImGui::Text("Note: %s", resolved.notes.c_str());
						// Hex dump of first 32 bytes at methodPointer
						ImGui::Separator();
						ImGui::Text("Bytes at methodPointer:");
						Resolver::Protection::safe_call([&]() {
							uint8_t* code = (uint8_t*)mPtr;
							char hex1[128] = {}, hex2[128] = {};
							for (int b = 0; b < 16; b++) sprintf_s(hex1 + b * 3, 4, "%02X ", code[b]);
							for (int b = 0; b < 16; b++) sprintf_s(hex2 + b * 3, 4, "%02X ", code[16 + b]);
							ImGui::TextUnformatted(hex1);
							ImGui::TextUnformatted(hex2);
						});
						if (hasOriginal) {
							ImGui::Separator();
							ImGui::Text("Bytes at original body:");
							Resolver::Protection::safe_call([&]() {
								uint8_t* code = (uint8_t*)resolved.invokerTarget;
								char hex1[128] = {}, hex2[128] = {};
								for (int b = 0; b < 16; b++) sprintf_s(hex1 + b * 3, 4, "%02X ", code[b]);
								for (int b = 0; b < 16; b++) sprintf_s(hex2 + b * 3, 4, "%02X ", code[16 + b]);
								ImGui::TextUnformatted(hex1);
								ImGui::TextUnformatted(hex2);
							});
						}
						ImGui::EndTooltip();
					}
				}
			}

			int paramCount = il2cpp_method_get_param_count(method);
			auto& buffers = methodParamBuffers[method];
			if (buffers.size() != (size_t)paramCount) buffers.resize(paramCount, "0");

			if (paramCount > 0) {
				ImGui::Indent();
				for (int i = 0; i < paramCount; i++) {
					const Il2CppType* pType = il2cpp_method_get_param(method, i);
					Il2CppClass* pKlass = il2cpp_class_from_type(pType);
					std::string pKlassName = pKlass ? pKlass->name : "Unknown";
					const char* pName = il2cpp_method_get_param_name(method, i);

					ImGui::PushID(i);

					// --- Fix: Parsing directly from buffer instead of using static variables ---
					if (pType->type == IL2CPP_TYPE_VALUETYPE && pKlassName == "Vector3") {
						float v[3] = { 0, 0, 0 };
						sscanf_s(buffers[i].c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
						if (ImGui::DragFloat3("##v3", v, 0.1f)) {
							char b[128]; sprintf_s(b, "%.3f,%.3f,%.3f", v[0], v[1], v[2]);
							buffers[i] = b;
						}
					}
					else if (pType->type == IL2CPP_TYPE_VALUETYPE && pKlassName == "Color") {
						float c[4] = { 1, 1, 1, 1 };
						sscanf_s(buffers[i].c_str(), "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]);
						if (ImGui::ColorEdit4("##clr", c, ImGuiColorEditFlags_NoInputs)) {
							char b[128]; sprintf_s(b, "%.3f,%.3f,%.3f,%.3f", c[0], c[1], c[2], c[3]);
							buffers[i] = b;
						}
					}
					else {
						bool isRef = (pType->type == IL2CPP_TYPE_CLASS || pType->type == IL2CPP_TYPE_OBJECT ||
							pType->type == IL2CPP_TYPE_GENERICINST || pType->type == IL2CPP_TYPE_SZARRAY ||
							pType->type == IL2CPP_TYPE_ARRAY || pType->type == IL2CPP_TYPE_PTR ||
							pType->type == IL2CPP_TYPE_I || pType->type == IL2CPP_TYPE_U);
						ImGui::SetNextItemWidth(isRef ? 140.0f : 200.0f);

						char buf[512];
						strncpy_s(buf, buffers[i].c_str(), sizeof(buf) - 1);
						if (ImGui::InputText("##v", buf, sizeof(buf))) buffers[i] = buf;

						if (isRef) {
							ImGui::SameLine();
							if (ImGui::Button("Paste")) {
								const char* clip = ImGui::GetClipboardText();
								if (clip) buffers[i] = clip;
							}
						}
					}

					ImGui::SameLine();
					ImGui::TextDisabled("%s (%s)", pName ? pName : "arg", pKlassName.c_str());
					ImGui::PopID();
				}
				ImGui::Unindent();
			}

			// Check if this method has a persistent hook FOR THIS CLASS
		// Search g_hookList so we find hooks regardless of which address they target
		bool isHooked = false;
		std::shared_ptr<HookEntry> existingHook;
		bool anyHookAtAddr = false;
		{
			std::lock_guard<std::mutex> lock(g_hooksMutex);
			for (auto& h : g_hookList) {
				if (h->method == method) {
					anyHookAtAddr = true;
					if (h->filterClass == klass) {
						isHooked = true;
						existingHook = h;
					}
				}
			}
		}

		// One-shot capture button (disabled when hooked)
		bool isCaptureTarget = (g_capture.method == method);
		if (g_capture.active && isCaptureTarget) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.0f, 1.0f));
			if (ImGui::Button("Cancel")) CancelOneShotCapture();
			ImGui::PopStyleColor();
			ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 1, 0, 1), "Waiting...");
		}
		else {
			if (g_capture.active || anyHookAtAddr) ImGui::BeginDisabled();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.4f, 0.6f, 1.0f));
			if (ImGui::Button("Capture")) SetOneShotCapture(method, klass);
			ImGui::PopStyleColor();
			if (g_capture.active || anyHookAtAddr) ImGui::EndDisabled();
		}
		ImGui::SameLine();

		// Persistent hook button
		if (existingHook) {
			bool hp = existingHook->paused.load();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
			if (ImGui::Button("Unhook")) RemovePersistentHook(existingHook);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
			if (ImGui::Button("View")) {
				g_selectedHook = existingHook;
				std::lock_guard<std::mutex> lock(existingHook->logMutex);
				if (!existingHook->callLog.empty()) {
					g_selectedCallIdx = (int)existingHook->callLog.size() - 1;
					g_selectedCallRecord = existingHook->callLog.back();
					g_selectedCallTimestamp = g_selectedCallRecord.timestamp;
					g_hasSelectedCall = true;
				} else {
					g_selectedCallIdx = -1;
					g_hasSelectedCall = false;
				}
				explorerMode = 5;
			}
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), hp ? "(Paused, %llu)" : "(Active, %llu)",
				existingHook->totalCallCount.load());
		}
		else {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.3f, 1.0f));
			if (ImGui::Button("Hook")) AddPersistentHook(method, klass);
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hook at methodPointer (0x%llX).\nLogs every call with params + stack trace.", (uint64_t)method->methodPointer);
			// "Hook@Orig" button: hook the invoker's original call target instead of methodPointer
			{
				auto cacheIt = s_resolveCache.find(method);
				if (cacheIt != s_resolveCache.end() && cacheIt->second.invokerTarget &&
					cacheIt->second.invokerTarget != cacheIt->second.methodPointer) {
					ImGui::SameLine();
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.4f, 0.3f, 1.0f));
					if (ImGui::Button("Hook@Orig"))
						AddPersistentHook(method, klass, cacheIt->second.invokerTarget);
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Hook at ORIGINAL body (0x%llX) instead of dispatcher.\nUse when methodPointer is a shared hotfix dispatcher.",
							(uint64_t)cacheIt->second.invokerTarget);
				}
			}
		}
		ImGui::SameLine();

		// Replay button: re-invoke with exact captured raw pointers (no parsing)
		if (g_capture.captured && g_capture.method == method) {
			if (ImGui::Button("Replay")) {
				// Sync edited data into cloned arrays before replay
				for (int bi = 0; bi < g_capturedBufCount; bi++) {
					auto& cb = g_capturedBufs[bi];
					if (!cb.valid || !cb.clonedArray) continue;
					Resolver::Protection::safe_call([&]() {
						void* cloneData = (void*)((uintptr_t)cb.clonedArray + sizeof(Il2CppArray));
						memcpy(cloneData, cb.data, cb.byteLen);
					});
				}

				bool isStaticM = (method->flags & 0x0010) != 0;
				int argOff = isStaticM ? 0 : 1;
				std::vector<void*> rawParams;
				std::vector<uint8_t> replayStorage(paramCount * 64, 0);
				for (int i = 0; i < paramCount; i++) {
					int ai = i + argOff;
					const Il2CppType* pType = il2cpp_method_get_param(method, i);
					if (!pType) { rawParams.push_back(nullptr); continue; }
					int pt = pType->type;
					uint8_t* rslot = &replayStorage[i * 64];
					// For reference types, pass the raw pointer directly
					if (pt == IL2CPP_TYPE_STRING || pt == IL2CPP_TYPE_CLASS || pt == IL2CPP_TYPE_OBJECT ||
						pt == IL2CPP_TYPE_GENERICINST) {
						rawParams.push_back((void*)g_capture.args[ai].regVal);
					}
					else if (pt == IL2CPP_TYPE_SZARRAY || pt == IL2CPP_TYPE_ARRAY) {
						// Use cloned array if we have one for this param, otherwise original
						void* arrayToUse = (void*)g_capture.args[ai].regVal;
						for (int bi = 0; bi < g_capturedBufCount; bi++) {
							if (g_capturedBufs[bi].valid && g_capturedBufs[bi].paramIdx == i && g_capturedBufs[bi].clonedArray) {
								arrayToUse = (void*)g_capturedBufs[bi].clonedArray;
								break;
							}
						}
						rawParams.push_back(arrayToUse);
					}
					else if (pt == IL2CPP_TYPE_R4) {
						memcpy(rslot, &g_capture.args[ai].xmmLow, sizeof(float));
						rawParams.push_back(rslot);
					}
					else if (pt == IL2CPP_TYPE_R8) {
						memcpy(rslot, &g_capture.args[ai].xmmLow, sizeof(double));
						rawParams.push_back(rslot);
					}
					else {
						// All integer/value types: pass pointer to raw value
						uint64_t val = g_capture.args[ai].regVal;
						memcpy(rslot, &val, sizeof(val));
						rawParams.push_back(rslot);
					}
				}
				Il2CppObject* thisObj = isStaticM ? nullptr : (obj ? obj : (Il2CppObject*)g_capture.args[0].regVal);
				Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(method, thisObj, rawParams.data());
				if (!res) { methodLastResults[method] = "Null / Void (Replay)"; methodLastReturnObj.erase(method); }
				else {
					char hx[64]; sprintf_s(hx, "0x%p (Replay)", res);
					methodLastResults[method] = hx;
					methodLastReturnObj[method] = res;
				}
			}
			ImGui::SameLine();
		}

		if (ImGui::Button("Invoke")) {
				// il2cpp_runtime_invoke expects void** where:
				//   - value types: pointer to the RAW unboxed value (NOT a boxed Il2CppObject)
				//   - reference types: the object pointer itself
				// We use valStorage to keep raw values alive until after the invoke call.
				std::vector<void*> params;
				std::vector<uint8_t> valStorage(paramCount * 64, 0); // 64 bytes per param slot
				for (int i = 0; i < paramCount; i++) {
					const Il2CppType* pType = il2cpp_method_get_param(method, i);
					Il2CppClass* pKlass = il2cpp_class_from_type(pType);
					std::string pKlassName = pKlass ? pKlass->name : "";
					uint8_t* slot = &valStorage[i * 64];

					// Helper: parse hex pointer from buffer (strips prefixes like "Array@")
					auto parseHexPtr = [](const std::string& buf) -> uintptr_t {
						uintptr_t addr = 0;
						try {
							std::string s = buf;
							size_t pos = s.find("0x");
							if (pos != std::string::npos) s = s.substr(pos);
							addr = std::stoull(s, nullptr, 16);
						} catch (...) {}
						return addr;
					};

					int pt = pType->type;

					// --- Reference types: pass object pointer directly ---
					if (pt == IL2CPP_TYPE_STRING) {
						Il2CppString* str = nullptr;
						Resolver::Protection::safe_call([&]() {
							str = il2cpp_string_new(buffers[i].c_str());
						});
						params.push_back((void*)str);
					}
					else if (pt == IL2CPP_TYPE_CLASS || pt == IL2CPP_TYPE_OBJECT ||
							 pt == IL2CPP_TYPE_GENERICINST || pt == IL2CPP_TYPE_SZARRAY ||
							 pt == IL2CPP_TYPE_ARRAY) {
						params.push_back((void*)parseHexPtr(buffers[i]));
					}
					// --- Pointer / IntPtr / UIntPtr: native-sized, parse as hex ---
					else if (pt == IL2CPP_TYPE_PTR || pt == IL2CPP_TYPE_I || pt == IL2CPP_TYPE_U ||
							 pt == IL2CPP_TYPE_FNPTR) {
						*(uintptr_t*)slot = parseHexPtr(buffers[i]);
						params.push_back(slot);
					}
					// --- ByRef (ref/out): pass pointer to inner value ---
					else if (pt == IL2CPP_TYPE_BYREF) {
						// For ref/out params, runtime_invoke expects a pointer to the value.
						// Parse based on what the user typed (hex for objects, decimal for ints)
						*(uintptr_t*)slot = parseHexPtr(buffers[i]);
						if (*(uintptr_t*)slot == 0) // fallback: try as decimal
							*(int64_t*)slot = (int64_t)std::atoll(buffers[i].c_str());
						params.push_back(slot);
					}
					// --- Value types: write raw value into slot, pass slot pointer ---
					else if (pt == IL2CPP_TYPE_VALUETYPE || pt == IL2CPP_TYPE_ENUM) {
						if (pKlassName == "Vector2") {
							float* v = (float*)slot;
							sscanf_s(buffers[i].c_str(), "%f,%f", &v[0], &v[1]);
						}
						else if (pKlassName == "Vector3") {
							app::Vector3* v = (app::Vector3*)slot;
							sscanf_s(buffers[i].c_str(), "%f,%f,%f", &v->x, &v->y, &v->z);
						}
						else if (pKlassName == "Vector4" || pKlassName == "Quaternion") {
							float* v = (float*)slot;
							sscanf_s(buffers[i].c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
						}
						else if (pKlassName == "Color") {
							float* c = (float*)slot;
							sscanf_s(buffers[i].c_str(), "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]);
						}
						else if (pKlassName == "Rect") {
							float* r = (float*)slot;
							sscanf_s(buffers[i].c_str(), "%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]);
						}
						else if (pKlass && pKlass->enumtype) {
							*(int32_t*)slot = std::atoi(buffers[i].c_str());
						}
						else {
							// Generic valuetype: use actual instance size for zeroing, write as int
							uint32_t instSize = pKlass ? il2cpp_class_instance_size(pKlass) : 4;
							// instance_size includes Il2CppObject header (16 bytes), raw value size is smaller
							uint32_t rawSize = (instSize > 16) ? (instSize - 16) : instSize;
							if (rawSize > 64) rawSize = 64;
							memset(slot, 0, rawSize);
							*(int32_t*)slot = std::atoi(buffers[i].c_str());
						}
						params.push_back(slot);
					}
					// --- Primitive value types ---
					else if (pt == IL2CPP_TYPE_BOOLEAN) {
						*(int32_t*)slot = (buffers[i] == "1" || buffers[i] == "true") ? 1 : 0;
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_CHAR) {
						*(uint16_t*)slot = buffers[i].empty() ? 0 : (uint16_t)buffers[i][0];
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_I1) {
						*(int8_t*)slot = (int8_t)std::atoi(buffers[i].c_str());
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_U1) {
						*(uint8_t*)slot = (uint8_t)std::atoi(buffers[i].c_str());
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_I2) {
						*(int16_t*)slot = (int16_t)std::atoi(buffers[i].c_str());
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_U2) {
						*(uint16_t*)slot = (uint16_t)std::atoi(buffers[i].c_str());
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_I4) {
						*(int32_t*)slot = std::atoi(buffers[i].c_str());
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_U4) {
						*(uint32_t*)slot = (uint32_t)std::strtoul(buffers[i].c_str(), nullptr, 10);
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_I8) {
						try { *(int64_t*)slot = std::stoll(buffers[i]); } catch (...) {}
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_U8) {
						try { *(uint64_t*)slot = std::stoull(buffers[i]); } catch (...) {}
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_R4) {
						*(float*)slot = (float)std::atof(buffers[i].c_str());
						params.push_back(slot);
					}
					else if (pt == IL2CPP_TYPE_R8) {
						*(double*)slot = std::atof(buffers[i].c_str());
						params.push_back(slot);
					}
					else {
						// Unknown type: try as hex pointer (safest fallback)
						*(uintptr_t*)slot = parseHexPtr(buffers[i]);
						if (*(uintptr_t*)slot == 0)
							*(int64_t*)slot = (int64_t)std::atoll(buffers[i].c_str());
						params.push_back(slot);
					}
				}

				Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(method, isStatic ? nullptr : obj, params.data());

				// --- Return Value Analysis ---
				if (!res) methodLastResults[method] = "Null / Void";
				else {
					const Il2CppType* retType = method->return_type;
					int rt = retType ? retType->type : 0;
					char hx[128];
					Resolver::Protection::safe_call([&]() {
						switch (rt) {
						case IL2CPP_TYPE_VOID:
							methodLastResults[method] = "Void (success)";
							break;
						case IL2CPP_TYPE_BOOLEAN:
							methodLastResults[method] = *(bool*)il2cpp_object_unbox(res) ? "true" : "false";
							break;
						case IL2CPP_TYPE_CHAR: {
							uint16_t v = *(uint16_t*)il2cpp_object_unbox(res);
							sprintf_s(hx, "'%c' (%d)", (v >= 32 && v < 127) ? (char)v : '?', v);
							methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_I1: case IL2CPP_TYPE_I2: case IL2CPP_TYPE_I4: {
							int32_t v = *(int32_t*)il2cpp_object_unbox(res);
							sprintf_s(hx, "%d", v); methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_U1: case IL2CPP_TYPE_U2: case IL2CPP_TYPE_U4: {
							uint32_t v = *(uint32_t*)il2cpp_object_unbox(res);
							sprintf_s(hx, "%u", v); methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_I8: {
							int64_t v = *(int64_t*)il2cpp_object_unbox(res);
							sprintf_s(hx, "%lld", v); methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_U8: {
							uint64_t v = *(uint64_t*)il2cpp_object_unbox(res);
							sprintf_s(hx, "%llu", v); methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_R4: {
							float v = *(float*)il2cpp_object_unbox(res);
							sprintf_s(hx, "%.4f", v); methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_R8: {
							double v = *(double*)il2cpp_object_unbox(res);
							sprintf_s(hx, "%.6f", v); methodLastResults[method] = hx;
							break;
						}
						case IL2CPP_TYPE_STRING:
							methodLastResults[method] = "\"" + il2cppi_to_string((Il2CppString*)res) + "\"";
							break;
						case IL2CPP_TYPE_VALUETYPE: {
							Il2CppClass* retKlass = il2cpp_class_from_type(retType);
							if (!retKlass) { methodLastResults[method] = "ValueType"; break; }
							std::string rn = retKlass->name;
							if (rn == "Vector3") {
								app::Vector3 v = *(app::Vector3*)il2cpp_object_unbox(res);
								sprintf_s(hx, "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
								methodLastResults[method] = hx;
							}
							else if (rn == "Vector2") {
								float* v = (float*)il2cpp_object_unbox(res);
								sprintf_s(hx, "(%.2f, %.2f)", v[0], v[1]);
								methodLastResults[method] = hx;
							}
							else if (retKlass->enumtype) {
								int32_t v = *(int32_t*)il2cpp_object_unbox(res);
								std::string en = ResolveEnumName(retKlass, v);
								sprintf_s(hx, "%d", v);
								methodLastResults[method] = en.empty() ? std::string(hx) : en + " (" + hx + ")";
							}
							else {
								methodLastResults[method] = "{" + rn + "}";
							}
							break;
						}
						case IL2CPP_TYPE_SZARRAY: {
							uint32_t len = il2cpp_array_length((Il2CppArray*)res);
							sprintf_s(hx, "Array[%d] @ 0x%p", len, res);
							methodLastResults[method] = hx;
							methodLastReturnObj[method] = res;
							break;
						}
						default:
							sprintf_s(hx, "0x%p", res);
							methodLastResults[method] = hx;
							methodLastReturnObj[method] = res;
							break;
						}
					});
				}
			}

			if (methodLastResults.count(method)) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0, 1, 0, 1), "-> %s", methodLastResults[method].c_str());

				// Copy result button
				ImGui::SameLine();
				if (ImGui::SmallButton("Cp##ret")) {
					std::string val = methodLastResults[method];
					if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
						val = val.substr(1, val.size() - 2);
					Resolver::Helpers::CopyToClipboard(val.c_str());
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy result");

				if (methodLastResults[method].find("0x") == 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Inspect Result")) {
						try {
							uintptr_t addr = std::stoull(methodLastResults[method], nullptr, 16);
							InspectObject((Il2CppObject*)addr);
						}
						catch (...) {}
					}
				}

				// Byte array return: show hex/ASCII copy + hex dump view
				if (methodLastReturnObj.count(method) && methodLastResults[method].find("Array[") == 0) {
					Il2CppObject* retObj = methodLastReturnObj[method];
					if (retObj && Resolver::Protection::IsValidIl2CppObject(retObj)) {
						bool isByteRet = false;
						uint32_t arrLen = 0;
						Resolver::Protection::safe_call([&]() {
							Il2CppArray* arr = (Il2CppArray*)retObj;
							arrLen = il2cpp_array_length(arr);
							Il2CppClass* ac = il2cpp_object_get_class(retObj);
							Il2CppClass* ec = ac ? il2cpp_class_get_element_class(ac) : nullptr;
							if (ec) {
								const Il2CppType* et = il2cpp_class_get_type(ec);
								if (et && (et->type == IL2CPP_TYPE_U1 || et->type == IL2CPP_TYPE_I1)) isByteRet = true;
							}
						});
						if (isByteRet && arrLen > 0) {
							if (ImGui::SmallButton("Hex##rethex")) {
								Resolver::Protection::safe_call([&]() {
									uint32_t cap = arrLen > 4096 ? 4096 : arrLen;
									char* data = (char*)retObj + 0x20;
									std::string hex;
									for (uint32_t i = 0; i < cap; i++) {
										char b[4]; sprintf_s(b, "%02X ", (uint8_t)data[i]);
										hex += b;
									}
									Resolver::Helpers::CopyToClipboard(hex.c_str());
								});
							}
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy hex bytes");
							ImGui::SameLine();
							if (ImGui::SmallButton("ASCII##retascii")) {
								Resolver::Protection::safe_call([&]() {
									uint32_t cap = arrLen > 4096 ? 4096 : arrLen;
									char* data = (char*)retObj + 0x20;
									std::string txt;
									for (uint32_t i = 0; i < cap; i++) {
										uint8_t c = (uint8_t)data[i];
										txt += (c >= 32 && c < 127) ? (char)c : '.';
									}
									Resolver::Helpers::CopyToClipboard(txt.c_str());
								});
							}
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy as ASCII text");
							ImGui::SameLine();
							if (ImGui::SmallButton("Raw##retraw")) {
								Resolver::Protection::safe_call([&]() {
									uint32_t cap = arrLen > 4096 ? 4096 : arrLen;
									char* data = (char*)retObj + 0x20;
									std::string raw(data, cap);
									Resolver::Helpers::CopyToClipboard(raw.c_str());
								});
							}
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy raw bytes as-is (for UTF-8 text)");

							// Expandable hex:ASCII dump
							char retTreeLabel[64]; sprintf_s(retTreeLabel, "Hex View [%d bytes]##rethexview", arrLen);
							if (ImGui::TreeNode(retTreeLabel)) {
								Resolver::Protection::safe_call([&]() {
									uint32_t cap = arrLen > 4096 ? 4096 : arrLen;
									char* data = (char*)retObj + 0x20;
									for (uint32_t row = 0; row < cap; row += 16) {
										uint32_t rowEnd = row + 16 > cap ? cap : row + 16;
										char line[128]; int off = sprintf_s(line, "%04X: ", row);
										for (uint32_t i = row; i < row + 16; i++) {
											if (i < rowEnd) off += sprintf_s(line + off, sizeof(line) - off, "%02X ", (uint8_t)data[i]);
											else off += sprintf_s(line + off, sizeof(line) - off, "   ");
										}
										off += sprintf_s(line + off, sizeof(line) - off, " ");
										for (uint32_t i = row; i < rowEnd; i++) {
											uint8_t c = (uint8_t)data[i];
											line[off++] = (c >= 32 && c < 127) ? (char)c : '.';
										}
										line[off] = 0;
										ImGui::TextUnformatted(line);
									}
									if (arrLen > cap) ImGui::TextDisabled("... (%d more bytes)", arrLen - cap);
								});
								ImGui::TreePop();
							}
						}
					}
				}
			}

			// Show captured params for this method
			if (g_capture.captured && g_capture.method == method) {
				bool isStaticM = (method->flags & 0x0010) != 0;
				int pCount = il2cpp_method_get_param_count(method);
				int argOff = isStaticM ? 0 : 1;

				// Lazily create GC clones for captured array buffers (data was already
				// snapshotted in VEH handler before the method executed)
				for (int bi = 0; bi < g_capturedBufCount; bi++) {
					auto& cb = g_capturedBufs[bi];
					if (!cb.valid || cb.clonedArray) continue; // already cloned or invalid
					Resolver::Protection::safe_call([&]() {
						if (!cb.originalArrayPtr) return;
						if (!Resolver::Protection::IsValidIl2CppObject((Il2CppObject*)cb.originalArrayPtr)) return;
						Il2CppClass* arrClass = il2cpp_object_get_class((Il2CppObject*)cb.originalArrayPtr);
						Il2CppClass* elemClass = arrClass ? il2cpp_class_get_element_class(arrClass) : nullptr;
						if (!elemClass) return;
						Il2CppArray* clone = il2cpp_array_new(elemClass, cb.arrayLen);
						if (!clone) return;
						// Copy our VEH-captured snapshot data (pre-execution) into clone
						void* dstData = (void*)((uintptr_t)clone + sizeof(Il2CppArray));
						memcpy(dstData, cb.data, cb.byteLen);
						cb.clonedArray = clone;
					});
				}

				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.8f, 1.0f));
				ImGui::Text("Captured:");
				ImGui::PopStyleColor();
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.3f, 1.0f));
				if (ImGui::SmallButton("Fill Params")) {
					for (int pi = 0; pi < pCount; pi++) {
						int ai = pi + argOff;
						Resolver::Protection::safe_call([&]() {
							buffers[pi] = CapturedArgToBuffer(method, pi, g_capture.args[ai].regVal, g_capture.args[ai].xmmLow);
						});
					}
				}
				ImGui::PopStyleColor();

				// Deep snapshot button
				ImGui::SameLine();
				if (ImGui::SmallButton("Deep Snapshot")) {
					capturedSnapshots.clear();
					for (int pi = 0; pi < pCount; pi++) {
						int ai = pi + argOff;
						const Il2CppType* pType = il2cpp_method_get_param(method, pi);
						const char* pn = il2cpp_method_get_param_name(method, pi);
						CapturedParamSnapshot ps;
						ps.paramName = pn ? pn : "arg";
						if (pType) {
							int pt = pType->type;
							if (pt == IL2CPP_TYPE_CLASS || pt == IL2CPP_TYPE_OBJECT ||
								pt == IL2CPP_TYPE_GENERICINST || pt == IL2CPP_TYPE_SZARRAY) {
								Resolver::Protection::safe_call([&]() {
									ps.root = SnapshotFromCapturedPtr(g_capture.args[ai].regVal, pType, 0);
								});
							}
						}
						if (!ps.root.children.empty() || !ps.root.rawBytes.empty() || !ps.root.value.empty()) {
							ps.root.label = ps.paramName;
							capturedSnapshots.push_back(std::move(ps));
						}
					}
					// Also snapshot 'this' if instance method
					if (!isStaticM && g_capture.args[0].regVal) {
						CapturedParamSnapshot ps;
						ps.paramName = "this";
						Resolver::Protection::safe_call([&]() {
							Il2CppObject* thisObj = (Il2CppObject*)g_capture.args[0].regVal;
							if (Resolver::Protection::IsValidIl2CppObject(thisObj)) {
								ps.root = SnapshotObject(thisObj, 0);
								ps.root.label = "this";
							}
						});
						if (!ps.root.children.empty())
							capturedSnapshots.insert(capturedSnapshots.begin(), std::move(ps));
					}
					showCapturedSnapshotWindow = !capturedSnapshots.empty();
				}

				ImGui::Indent();
				if (!isStaticM) {
					char buf[64]; sprintf_s(buf, "this = 0x%llX", g_capture.args[0].regVal);
					ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "%s", buf);
					// Inspect this button
					if (g_capture.args[0].regVal) {
						ImGui::SameLine();
						if (ImGui::SmallButton("Inspect##this")) {
							Il2CppObject* thisObj = (Il2CppObject*)g_capture.args[0].regVal;
							if (Resolver::Protection::IsValidIl2CppObject(thisObj))
								InspectObject(thisObj);
						}
					}
				}
				for (int pi = 0; pi < pCount; pi++) {
					const char* pName = il2cpp_method_get_param_name(method, pi);
					int ai = pi + argOff;
					std::string decoded;
					Resolver::Protection::safe_call([&]() {
						decoded = DecodeCapturedArg(method, pi, g_capture.args[ai].regVal, g_capture.args[ai].xmmLow);
					});
					ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.8f, 1.0f), "  %s = %s",
						pName ? pName : "arg", decoded.c_str());

					// Inspect button for pointer params
					const Il2CppType* pType = il2cpp_method_get_param(method, pi);
					if (pType && g_capture.args[ai].regVal) {
						int pt = pType->type;
						if (pt == IL2CPP_TYPE_CLASS || pt == IL2CPP_TYPE_OBJECT || pt == IL2CPP_TYPE_GENERICINST) {
							ImGui::SameLine();
							ImGui::PushID(pi + 70000);
							if (ImGui::SmallButton("Inspect")) {
								Il2CppObject* pObj = (Il2CppObject*)g_capture.args[ai].regVal;
								if (Resolver::Protection::IsValidIl2CppObject(pObj))
									InspectObject(pObj);
							}
							ImGui::PopID();
						}
					}

					// Hex editor for captured array/byte[] buffers
					for (int bi = 0; bi < g_capturedBufCount; bi++) {
						auto& cb = g_capturedBufs[bi];
						if (!cb.valid || cb.paramIdx != pi) continue;

						char hdr[128];
						sprintf_s(hdr, "  Buffer: %u bytes (%u elems x %u) [clone 0x%p]",
							cb.byteLen, cb.arrayLen, cb.elemSize, cb.clonedArray);
						ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", hdr);
						if (cb.byteLen < cb.arrayLen * cb.elemSize) {
							ImGui::SameLine();
							ImGui::TextColored(ImVec4(1, 1, 0, 1), "(truncated from %u)", cb.arrayLen * cb.elemSize);
						}

						// Unique widget ID: 100000 + buffer index
						DrawHexEditorWidget(cb.data, cb.byteLen, 100000 + bi);
					}
				}
				ImGui::Unindent();
			}

			ImGui::Separator();
			ImGui::PopID();
		}
		ImGui::Unindent();
	}
}

void UnityExplorerTAB::Helpers::DrawClassSearch()
{
	ImGui::TextColored(ImVec4(1, 1, 0, 1), "Assembly Class Browser");
	ImGui::Separator();

	ImGui::SetNextItemWidth(-100);
	ImGui::InputTextWithHint("##ClassSearch", "Search Class Name...", classSearchFilter, 128);
	ImGui::SameLine();

	if (ImGui::Button("Search", ImVec2(80, 0))) {
		searchResults.clear();
		std::string filter = classSearchFilter;
		std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

		auto domain = il2cpp_domain_get();
		size_t assembly_count;
		const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &assembly_count);

		for (size_t i = 0; i < assembly_count; ++i) {
			const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
			size_t class_count = il2cpp_image_get_class_count(image);
			for (size_t j = 0; j < class_count; ++j) {
				Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(image, j);
				std::string name = il2cpp_class_get_name(klass);
				std::string lower_name = name;
				std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

				if (lower_name.find(filter) != std::string::npos) {
					searchResults.push_back(klass);
				}
			}
		}
	}

	ImGui::BeginChild("SearchList", ImVec2(0, 0), true);
	for (auto klass : searchResults) {
		std::string name = il2cpp_class_get_name(klass);
		std::string ns = il2cpp_class_get_namespace(klass);
		if (ImGui::Selectable((ns + "::" + name).c_str())) {
			InspectClass(klass); 
		}
	}
	ImGui::EndChild();
}

void UnityExplorerTAB::Helpers::InspectClass(Il2CppClass* klass)
{
	if (!klass) return;

	const char* name = nullptr;
	const char* ns = nullptr;
	Resolver::Protection::safe_call([&]() {
		name = il2cpp_class_get_name(klass);
		ns = il2cpp_class_get_namespace(klass);
	});
	DebugLog("InspectClass: %s::%s ptr=0x%p", ns ? ns : "?", name ? name : "?", klass);

	if (selectedGameObject) {
		inspectionHistory.push({ selectedGameObject, isRawClassView });
	}

	selectedGameObject = (Il2CppObject*)klass;
	isRawClassView = true;
	forceCastClass = nullptr;
	forceCastObj = nullptr;
}

void UnityExplorerTAB::Helpers::DrawVector3(const char* label, Il2CppObject* transform, const char* propName)
{
	app::Vector3 vec = Resolver::GetProperty<app::Vector3>(transform, propName);

	ImGui::Text("%s", label); ImGui::SameLine(80);
	ImGui::PushItemWidth(60.0f);

	bool changed = false;
	char idX[64], idY[64], idZ[64];
	sprintf_s(idX, "X##%s", label);
	sprintf_s(idY, "Y##%s", label);
	sprintf_s(idZ, "Z##%s", label);

	if (ImGui::DragFloat(idX, &vec.x, 0.1f)) changed = true; ImGui::SameLine();
	if (ImGui::DragFloat(idY, &vec.y, 0.1f)) changed = true; ImGui::SameLine();
	if (ImGui::DragFloat(idZ, &vec.z, 0.1f)) changed = true;

	if (changed) {
		Resolver::SetProperty<app::Vector3>(transform, propName, vec);
	}

	ImGui::PopItemWidth();
}
