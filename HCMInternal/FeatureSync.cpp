#include "pch.h"
#include "FeatureSync.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"

// PoC protocol: HCMF|v1|s<tag8>|q<seq>|t<tick>|<CMD>|<args..>
//   HI            presence beacon (every 45 ticks while armed)
//   PING          RTT probe (t = send tick); peer answers PONG|origT
//   PONG|origT    RTT answer; sender computes now - origT
//   EXEC|msg|text display proof: MessagesGUI line at target tick
// Hotkeys (fixed, PoC only): F9 status+hello burst, F10 queue EXEC msg at now+30, F11 ping

class FeatureSync::FeatureSyncImpl
{
private:
	static inline FeatureSyncImpl* instance = nullptr;
	static inline safetyhook::InlineHook* recvHookRaw = nullptr;

	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<RuntimeExceptionHandler> expWeak;

	std::shared_ptr<MultilevelPointer> chatManagerGlobal;
	std::shared_ptr<MultilevelPointer> chatRecvFunction;
	std::shared_ptr<MultilevelPointer> chatPumpFunction;
	std::shared_ptr<MultilevelPointer> chatExtractReturn;
	std::shared_ptr<MultilevelPointer> chatExtractReturn2;
	std::shared_ptr<MultilevelPointer> chatExtractReturn3;
	std::shared_ptr<MultilevelPointer> tickCounter;

	std::unique_ptr<ModuleInlineHook> recvHook;
	std::unique_ptr<ModuleMidHook> pumpHook;
	std::unique_ptr<ModuleMidHook> extractHook;
	std::unique_ptr<ModuleMidHook> extractHook2;
	std::unique_ptr<ModuleMidHook> extractHook3;

	std::mutex sendMutex;
	std::deque<std::string> sendQueue;
	std::shared_ptr<std::atomic<bool>> workerStop = std::make_shared<std::atomic<bool>>(false);
	uint64_t ownNetId = 0;
	uint32_t seq = 0;
	uint32_t lastTick = 0;
	uint32_t lastHelloTick = 0;
	uint64_t lastPumpTimeMs = 0;
	std::map<uint32_t, uint32_t> peers; // senderTag -> lastSeenTick
	struct PendingExec { uint32_t targetTick; std::string arg; };
	std::vector<PendingExec> pendingExec;
	std::vector<PendingExec> doneExec; // kept for revert re-fire
	bool keyDown[4] = { false, false, false, false };

	using ChatSubmitFn = void(*)(void* mgr, uint64_t senderKey, uint32_t channel, const char* text, uint8_t flag);
	ChatSubmitFn submitFn = nullptr;

	// IsBadReadPtr is unsafe here: its internal probe faults and MCC's vectored
	// handler returns continue-execution, wedging the caller on the same
	// instruction forever. These checks never dereference the probe target.
	static bool plausiblePtr(const void* p)
	{
		return (uintptr_t)p > 0x10000 && (uintptr_t)p < 0x0000800000000000;
	}

	static bool pageReadable(const void* p, size_t len)
	{
		MEMORY_BASIC_INFORMATION mbi;
		return plausiblePtr(p) &&
			VirtualQuery(p, &mbi, sizeof(mbi)) != 0 &&
			mbi.State == MEM_COMMIT &&
			(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
			(uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)p >= len;
	}

	uint64_t resolveOwnNetId(void* mgr)
	{
		// mgr+0x28: intrusive unordered_map netId->senderRecord. MSVC layout:
		// mapObj+0x08 = sentinel node ptr; node+0x00 next, +0x10 key (netId).
		void* sentinel = *(void**)((uintptr_t)mgr + 0x30);
		void* first = *(void**)sentinel;
		if (first == sentinel || !plausiblePtr(first)) return 0;
		return *(uint64_t*)((uintptr_t)first + 0x10);
	}

	uint32_t readTick()
	{
		uint32_t t = 0;
		tickCounter->readData(&t);
		return t;
	}

	void msg(const std::string& s)
	{
		try { messagesGUIWeak.lock()->addMessage(s); }
		catch (...) {}
		PLOG_INFO << "[FeatureSync] " << s;
	}

	void queueSend(const std::string& wire)
	{
		if (wire.size() > 250) { msg("FS: wire too long, dropped"); return; }
		std::lock_guard<std::mutex> lock(sendMutex);
		sendQueue.push_back(wire);
	}

	// FUN_14025B684 expects a signed verification doc, so raw text is silently
	// dropped; direct PartyChatControlSendText bypasses the verify pipeline and
	// the receiver's record handler derefs the missing context (+0x1f0 null ->
	// std::out_of_range -> terminate). FUN_14025AF70 is MCC's own chat-submit
	// entry: it queues the verification task that produces the doc, so the
	// message arrives as a fully-formed verified text record.
	int partySend(void* mgr, const std::string& text)
	{
		if (!submitFn || !ownNetId) return -5;
		try { submitFn(mgr, ownNetId, 0, text.c_str(), 0); } /**/
		catch (std::exception& ex) { msg(std::format("FS submit threw: {}", ex.what())); return -6; } /**/
		catch (...) { msg("FS submit threw (non-std)"); return -7; } /**/
		return 0;
	}

	void drainSends()
	{
		void* mgr = *(void**)resolveGlobal(chatManagerGlobal);
		if (!mgr || !submitFn) return;
		if (!ownNetId) { ownNetId = resolveOwnNetId(mgr); if (!ownNetId) return; }
		std::lock_guard<std::mutex> lock(sendMutex);
		while (!sendQueue.empty())
		{
			std::string wireText = sendQueue.front(); sendQueue.pop_front();
			int err = partySend(mgr, wireText);
			if (err != 0) { msg(std::format("FS: partySend err {:X} (queued {})", (uint32_t)err, sendQueue.size())); }
		}
	}

	static uintptr_t resolveGlobal(const std::shared_ptr<MultilevelPointer>& p)
	{
		uintptr_t a = 0;
		p->resolve(&a);
		return a;
	}

	uint64_t nowMs() { return GetTickCount64(); }

	bool queueIsEmpty()
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		return sendQueue.empty();
	}

	void onHcmMessage(const char* text)
	{
		uint32_t tag = 0, q = 0, t = 0;
		char cmd[32] = {};
		char arg[200] = {};
		int n = sscanf_s(text, "HCMF|v1|s%x|q%u|t%u|%31[^|]|%199[^\n]", &tag, &q, &t, cmd, (unsigned)sizeof(cmd), arg, (unsigned)sizeof(arg));
		if (n < 4) { PLOG_WARNING << "[FeatureSync] malformed: " << text; return; }
		PLOG_INFO << std::format("[FeatureSync] RX tag={:08x} q={} t={} cmd={}", tag, q, t, cmd); /**/
		if (tag == (uint32_t)(ownNetId & 0xFFFFFFFF)) { msg("FS RX echo ok"); return; } // own echo / self-guard /**/
		uint32_t tick = readTick();
		peers[tag] = tick;
		std::string scmd(cmd);
		if (scmd == "HI") { return; }
		if (scmd == "PING") { queueSend(wire(std::format("PONG|{}", t))); return; }
		if (scmd == "PONG")
		{
			uint32_t origT = 0;
			sscanf_s(arg, "%u", &origT);
			msg(std::format("FS PONG from {:08x}: RTT {} ticks", tag, tick - origT));
			return;
		}
		if (scmd == "EXEC")
		{
			const char* p = strchr(arg, '|');
			if (!p || strncmp(arg, "msg|", 4) != 0) { PLOG_WARNING << "[FeatureSync] bad EXEC: " << arg; return; }
			pendingExec.push_back({ t, std::string(p + 1) });
			msg(std::format("FS EXEC from {:08x} scheduled @{}", tag, t));
			return;
		}
		PLOG_WARNING << "[FeatureSync] unknown cmd: " << cmd;
	}

	std::string wire(const std::string& cmdAndArgs, uint32_t t = 0)
	{
		return std::format("HCMF|v1|s{:08x}|q{}|t{}|{}", (uint32_t)(ownNetId & 0xFFFFFFFF), seq++, t, cmdAndArgs);
	}

	void onPumpTick()
	{
		lastPumpTimeMs = nowMs();
		if (!ownNetId)
		{
			void* mgr = *(void**)resolveGlobal(chatManagerGlobal);
			if (mgr) ownNetId = resolveOwnNetId(mgr);
			if (ownNetId) { PLOG_INFO << std::format("[FeatureSync] mgr={:p} ownNetId={:X}", mgr, ownNetId); } /**/
		}
		uint32_t tick = readTick();

		if (tick < lastTick) // revert: re-arm execs whose target is ahead again
		{
			for (auto it = doneExec.begin(); it != doneExec.end(); )
			{
				if (it->targetTick > tick) { pendingExec.push_back(*it); it = doneExec.erase(it); }
				else ++it;
			}
			msg(std::format("FS: tick regressed {} -> {}", lastTick, tick));
		}
		lastTick = tick;

		if (tick - lastHelloTick >= 45)
		{
			lastHelloTick = tick;
			queueSend(wire("HI"));
		}

		for (auto it = peers.begin(); it != peers.end(); )
		{
			if (tick - it->second > 600) it = peers.erase(it); else ++it;
		}

		for (auto it = pendingExec.begin(); it != pendingExec.end(); )
		{
			if (tick >= it->targetTick)
			{
				msg(std::format("FS EXEC @{}: {}", it->targetTick, it->arg));
				doneExec.push_back(*it);
				it = pendingExec.erase(it);
			}
			else ++it;
		}

		pollKey(0, VK_F9, [this, tick]() {
			msg(std::format("FS status: tick {} peers {} queued {} sendq {}", tick, peers.size(), pendingExec.size(), sendQueue.size())); /**/
			for (auto& [tag, last] : peers) msg(std::format("  peer {:08x} seen {} ticks ago", tag, tick - last));
			queueSend(wire("HI"));
			});
		pollKey(1, VK_F10, [this, tick]() {
			uint32_t target = tick + 120;
			queueSend(wire(std::format("EXEC|msg|hello from {:08x}", (uint32_t)(ownNetId & 0xFFFFFFFF)), target));
			pendingExec.push_back({ target, std::format("hello from self @{}", target) });
			msg(std::format("FS EXEC queued for tick {}", target));
			});
		pollKey(2, VK_F11, [this, tick]() {
			queueSend(wire("PING", tick));
			msg(std::format("FS PING sent @{}", tick));
			});
		pollKey(3, VK_F12, [this, tick]() {
			queueSend(wire("HI"));
			msg(std::format("FS TX queued @{}", tick)); /**/
			});
	}

	template<typename F>
	void pollKey(int idx, int vk, F&& fn)
	{
		bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down && !keyDown[idx]) fn();
		keyDown[idx] = down;
	}

	static void pumpHookFn(SafetyHookContext& ctx)
	{
		if (!instance) return;
		try { instance->onPumpTick(); }
		catch (std::exception& ex) { PLOG_ERROR << "[FeatureSync] pump: " << ex.what(); }
	}

	// Fires on FUN_14025B170 returns inside the chat pipeline; each call site
	// keeps its out std::string at a different [rbp+off]. Blanking produces
	// the normal empty-message (filtered) case. On the b684 (outbound) site
	// the wire text comes from a different string, so blanking only affects
	// the local history echo.
	static void consumeExtracted(uintptr_t rbp, intptr_t off)
	{
		FeatureSyncImpl* i = instance;
		if (!i || !rbp) return;
		uintptr_t ss = rbp + off;
		char* ptr = *(char**)ss;
		size_t size = *(size_t*)(ss + 0x10);
		size_t cap = *(size_t*)(ss + 0x18);
		const char* text = cap >= 0x10 ? ptr : (const char*)ss;
		if (size >= 5 && pageReadable(text, 5) && memcmp(text, "HCMF|", 5) == 0)
		{
			i->onHcmMessage(text);
			*(size_t*)(ss + 0x10) = 0;
			*(char*)(cap >= 0x10 ? (uintptr_t)ptr : ss) = 0;
		}
	}

	// exe+0x260D88: record-loop extraction, out string at [rbp+0x70]
	static void postExtractFn(SafetyHookContext& ctx)
	{
		try { consumeExtracted(ctx.rbp, 0x70); }
		catch (...) {}
	}

	// exe+0x2713DD: history-entry extraction, out string at [rbp+0x07]
	static void postExtractFn2(SafetyHookContext& ctx)
	{
		try { consumeExtracted(ctx.rbp, 0x07); }
		catch (...) {}
	}

	// exe+0x25B71F: outbound (b684) extraction for the local echo, out
	// string at [rbp-0x38]
	static void postExtractFn3(SafetyHookContext& ctx)
	{
		try { consumeExtracted(ctx.rbp, -0x38); }
		catch (...) {}
	}

	static void recvHookFn(uintptr_t mgr, uintptr_t change)
	{
		FeatureSyncImpl* i = instance;
		if (i && change)
		{
			try
			{
				if (*(int*)change == 0x26)
				{
					const char* p = *(const char**)(change + 0x38); /**/
					char hex[96] = {}; int hn = 0; /**/
					if (p && pageReadable(p, 16)) { for (int k = 0; k < 16; k++) hn += sprintf_s(hex + hn, sizeof(hex) - hn, "%02X ", (unsigned char)p[k]); } /**/
					PLOG_INFO << std::format("[FeatureSync] RX26 +4={:X} +40={:X} +38={:X} :: {}", *(int*)(change + 4), *(int*)(change + 0x40), (uintptr_t)p, hex); /**/
					if (*(int*)(change + 4) == 0 && *(int*)(change + 0x40) == 1)
					{
						const char* text = *(const char**)(change + 0x38);
						if (text && pageReadable(text, 8) && strncmp(text, "HCMF|", 5) == 0)
						{
							i->onHcmMessage(text);
							static const char emptyText[] = ""; /**/
							*(const char**)(change + 0x38) = emptyText; // skipping the original handler leaves record state the pipeline dereferences later; blank text still processes as a filtered message /**/
						}
					}
				}
			}
			catch (...) { PLOG_ERROR << "[FeatureSync] recv parse error"; }
		}
		recvHookRaw->call<void, uintptr_t, uintptr_t>(mgr, change);
	}

	void workerDrain(std::shared_ptr<std::atomic<bool>> stop) // Party API can't run inside the pump callback; all sends go through here
	{
		while (!stop->load())
		{
			Sleep(50);
			if (queueIsEmpty()) continue;
			try { drainSends(); } catch (...) {}
		}
	}

public:
	FeatureSyncImpl(std::shared_ptr<PointerDataStore> ptr, std::shared_ptr<IMessagesGUI> mes, std::shared_ptr<RuntimeExceptionHandler> exp)
		: messagesGUIWeak(mes), expWeak(exp)
	{
		chatManagerGlobal = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chatManagerGlobal));

		chatRecvFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chatRecvFunction));
		chatPumpFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chatPumpFunction));
		chatExtractReturn = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chatExtractReturn));
		chatExtractReturn2 = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chatExtractReturn2));
		chatExtractReturn3 = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chatExtractReturn3));
		tickCounter = ptr->getData<std::shared_ptr<MultilevelPointer>>("tickCounter", GameState(GameState::Value::HaloReach));

		HMODULE exe = GetModuleHandleW(L"MCC-Win64-Shipping.exe");
		if (exe) submitFn = (ChatSubmitFn)((uintptr_t)exe + 0x25AF70);
		if (!submitFn) throw HCMInitException("FeatureSync: chat submit fn unresolved");

		recvHook = ModuleInlineHook::make(L"main", chatRecvFunction, (void*)&recvHookFn, false);
		recvHookRaw = &recvHook->getInlineHook();
		recvHook->setWantsToBeAttached(true);
		pumpHook = ModuleMidHook::make(L"main", chatPumpFunction, (safetyhook::MidHookFn)&pumpHookFn, true);
		extractHook = ModuleMidHook::make(L"main", chatExtractReturn, (safetyhook::MidHookFn)&postExtractFn, true);
		extractHook2 = ModuleMidHook::make(L"main", chatExtractReturn2, (safetyhook::MidHookFn)&postExtractFn2, true);
		extractHook3 = ModuleMidHook::make(L"main", chatExtractReturn3, (safetyhook::MidHookFn)&postExtractFn3, true);

		instance = this;
		std::thread([this, stop = workerStop]() { workerDrain(stop); }).detach();
		msg("FeatureSync PoC armed. F9 status | F10 exec-test | F11 ping");
	}

	~FeatureSyncImpl()
	{
		instance = nullptr;
		workerStop->store(true);
		Sleep(150); // let the drain thread observe the stop flag before we unmap
		if (recvHook) recvHook->setWantsToBeAttached(false);
		if (pumpHook) pumpHook->setWantsToBeAttached(false);
		if (extractHook) extractHook->setWantsToBeAttached(false);
		if (extractHook2) extractHook2->setWantsToBeAttached(false);
		if (extractHook3) extractHook3->setWantsToBeAttached(false);
	}
};

FeatureSync::FeatureSync(std::shared_ptr<PointerDataStore> ptr, std::shared_ptr<IMessagesGUI> mes, std::shared_ptr<RuntimeExceptionHandler> exp)
	: pimpl(std::make_unique<FeatureSyncImpl>(ptr, mes, exp))
{
}

FeatureSync::~FeatureSync() = default;
