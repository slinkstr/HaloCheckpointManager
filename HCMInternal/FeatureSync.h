#pragma once
#include "pch.h"
#include "PointerDataStore.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"

// PoC: syncs HCM commands between co-op peers over MCC text chat (PlayFab Party).
// Send: direct call to the post-verify wire send fn (bypasses censor/UI/rate-mute).
// Recv: inline hook on the inbound dispatcher; HCMF| messages are eaten before display.
// Scheduling: commands carry a target game tick; all peers apply at the same tick.
// Internal service (not a GUI optional cheat) - fixed hotkeys, MessagesGUI status.
class FeatureSync
{
private:
	class FeatureSyncImpl;
	std::unique_ptr<FeatureSyncImpl> pimpl;

public:
	FeatureSync(std::shared_ptr<PointerDataStore> ptr, std::shared_ptr<IMessagesGUI> mes, std::shared_ptr<RuntimeExceptionHandler> exp);
	~FeatureSync();
};
