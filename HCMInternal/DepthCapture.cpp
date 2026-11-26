#include "pch.h"
#include "DepthCapture.h"
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <atomic>
#include <cfloat>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr size_t kSlotDrawIndexed               = 12;
	constexpr size_t kSlotDraw                      = 13;
	constexpr size_t kSlotDrawIndexedInstanced      = 20;
	constexpr size_t kSlotDrawInstanced             = 21;
	constexpr size_t kSlotOMSetRenderTargets        = 33;
	constexpr size_t kSlotOMSetRenderTargetsAndUAVs = 34;
	constexpr size_t kSlotClearDepthStencilView     = 53;

	constexpr size_t   kMaxContextClasses   = 4;
	constexpr uint64_t kStagingBudgetBytes  = 2ull * 1024 * 1024 * 1024;
	constexpr uint32_t kReadbackDelayFrames = 3;
	constexpr int      kDumpHotkey          = VK_F10;

	enum class CaptureEvent { PreClear, Unbind, EndOfFrame };
	enum class CapturePhase { Idle, Armed, Capturing, AwaitingReadback };
	enum class TexelEncoding { Float32, Unorm24, Unorm16, Float16, Unsupported };

	struct FormatInfo
	{
		const char*   name;
		DXGI_FORMAT   stagingFormat;
		TexelEncoding encoding;
		uint32_t      bytesPerTexel;
	};

	FormatInfo formatInfo(DXGI_FORMAT format);

	DXGI_FORMAT typelessFamily(DXGI_FORMAT format)
	{
		return formatInfo(format).stagingFormat;
	}

	FormatInfo formatInfo(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R32G8X24_TYPELESS:        return { "R32G8X24_TYPELESS",        DXGI_FORMAT_R32G8X24_TYPELESS, TexelEncoding::Float32, 8 };
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:     return { "D32_FLOAT_S8X24_UINT",     DXGI_FORMAT_R32G8X24_TYPELESS, TexelEncoding::Float32, 8 };
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return { "R32_FLOAT_X8X24_TYPELESS", DXGI_FORMAT_R32G8X24_TYPELESS, TexelEncoding::Float32, 8 };
		case DXGI_FORMAT_R32G32_TYPELESS:          return { "R32G32_TYPELESS",          DXGI_FORMAT_R32G32_TYPELESS,   TexelEncoding::Float32, 8 };
		case DXGI_FORMAT_R32G32_FLOAT:             return { "R32G32_FLOAT",             DXGI_FORMAT_R32G32_TYPELESS,   TexelEncoding::Float32, 8 };
		case DXGI_FORMAT_R32_TYPELESS:             return { "R32_TYPELESS",             DXGI_FORMAT_R32_TYPELESS,      TexelEncoding::Float32, 4 };
		case DXGI_FORMAT_D32_FLOAT:                return { "D32_FLOAT",                DXGI_FORMAT_R32_TYPELESS,      TexelEncoding::Float32, 4 };
		case DXGI_FORMAT_R32_FLOAT:                return { "R32_FLOAT",                DXGI_FORMAT_R32_TYPELESS,      TexelEncoding::Float32, 4 };
		case DXGI_FORMAT_R24G8_TYPELESS:           return { "R24G8_TYPELESS",           DXGI_FORMAT_R24G8_TYPELESS,    TexelEncoding::Unorm24, 4 };
		case DXGI_FORMAT_D24_UNORM_S8_UINT:        return { "D24_UNORM_S8_UINT",        DXGI_FORMAT_R24G8_TYPELESS,    TexelEncoding::Unorm24, 4 };
		case DXGI_FORMAT_R16_TYPELESS:             return { "R16_TYPELESS",             DXGI_FORMAT_R16_TYPELESS,      TexelEncoding::Unorm16, 2 };
		case DXGI_FORMAT_D16_UNORM:                return { "D16_UNORM",                DXGI_FORMAT_R16_TYPELESS,      TexelEncoding::Unorm16, 2 };
		case DXGI_FORMAT_R16_UNORM:                return { "R16_UNORM",                DXGI_FORMAT_R16_TYPELESS,      TexelEncoding::Unorm16, 2 };
		case DXGI_FORMAT_R16_FLOAT:                return { "R16_FLOAT",                DXGI_FORMAT_R16_TYPELESS,      TexelEncoding::Float16, 2 };
		default:                                   return { "unsupported",              DXGI_FORMAT_UNKNOWN,           TexelEncoding::Unsupported, 0 };
		}
	}

	const char* eventName(CaptureEvent event)
	{
		switch (event)
		{
		case CaptureEvent::PreClear: return "preclear";
		case CaptureEvent::Unbind:   return "unbind";
		default:                     return "endframe";
		}
	}

	struct TrackedTexture
	{
		ComPtr<ID3D11Texture2D> texture;
		D3D11_TEXTURE2D_DESC    desc {};
		uint32_t                ordinal     = 0;
		bool                    dirty       = false;
		bool                    snapshotted = false;
	};

	struct ContextBindings
	{
		ID3D11Texture2D*                                                     depth = nullptr;
		std::array<ID3D11Texture2D*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> colors {};
	};

	struct Snapshot
	{
		uint32_t                sequence;
		CaptureEvent            event;
		bool                    deferredContext;
		uint32_t                textureOrdinal;
		D3D11_TEXTURE2D_DESC    desc;
		ComPtr<ID3D11Texture2D> staging;
	};

	struct DecodedSnapshot
	{
		uint32_t             sequence;
		CaptureEvent         event;
		bool                 deferredContext;
		uint32_t             textureOrdinal;
		D3D11_TEXTURE2D_DESC desc;
		bool                 readOk;
		std::vector<float>   texels;
	};

	struct DumpJob
	{
		std::filesystem::path        folder;
		std::string                  game;
		UINT                         backbufferWidth;
		UINT                         backbufferHeight;
		uint32_t                     skippedForBudget;
		uint32_t                     skippedMultisampled;
		std::vector<DecodedSnapshot> snapshots;
	};

	std::mutex                                                captureMutex;
	std::atomic<CapturePhase>                                 phase { CapturePhase::Idle };
	ID3D11Device*                                             gameDevice           = nullptr;
	ID3D11DeviceContext*                                      gameImmediateContext = nullptr;
	UINT                                                      backbufferWidth      = 0;
	UINT                                                      backbufferHeight     = 0;
	std::unordered_map<ID3D11Texture2D*, TrackedTexture>      trackedTextures;
	std::unordered_map<ID3D11DeviceContext*, ContextBindings> contextBindings;
	std::vector<Snapshot>                                     snapshots;
	uint64_t                                                  stagingBytes        = 0;
	uint32_t                                                  skippedForBudget    = 0;
	uint32_t                                                  skippedMultisampled = 0;
	uint32_t                                                  readbackCountdown   = 0;
	bool                                                      hotkeyWasDown       = false;
	bool                                                      hooksInstalled      = false;

	std::mutex                                                    frameDepthMutex;
	std::unordered_map<DXGI_FORMAT, DepthCapture::FrameDepth>     currentFrameDepth;
	std::unordered_map<DXGI_FORMAT, DepthCapture::FrameDepth>     lastFrameDepths;
	std::atomic<bool>                                             insidePresent { false };

	std::atomic<DXGI_FORMAT>      watchedFormat { DXGI_FORMAT_UNKNOWN };
	std::atomic<ID3D11Texture2D*> watchedTexture { nullptr };
	std::atomic<ID3D11Texture2D*> immediateBoundDepth { nullptr };
	std::atomic<bool>             watchedDrawnSinceClear { false };
	ComPtr<ID3D11Texture2D>       preClearSnapshot;
	ComPtr<ID3D11Texture2D>       endOfFrameSnapshot;
	bool                          preClearTakenThisFrame = false;

	ComPtr<ID3D11Texture2D> textureOf(ID3D11View* view)
	{
		if (view == nullptr)
		{
			return nullptr;
		}
		ComPtr<ID3D11Resource> resource;
		view->GetResource(&resource);
		ComPtr<ID3D11Texture2D> texture;
		if (resource == nullptr || FAILED(resource.As(&texture)))
		{
			return nullptr;
		}
		return texture;
	}

	DXGI_FORMAT typelessFamily(DXGI_FORMAT format);

	bool ensureShaderReadableCopy(ComPtr<ID3D11Texture2D>& copy, ID3D11Texture2D* source)
	{
		D3D11_TEXTURE2D_DESC sourceDesc {};
		source->GetDesc(&sourceDesc);
		const DXGI_FORMAT copyFormat = typelessFamily(sourceDesc.Format);
		if (copy != nullptr)
		{
			D3D11_TEXTURE2D_DESC copyDesc {};
			copy->GetDesc(&copyDesc);
			if (copyDesc.Width == sourceDesc.Width && copyDesc.Height == sourceDesc.Height && copyDesc.Format == copyFormat)
			{
				return true;
			}
			copy.Reset();
		}
		D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
		copyDesc.Format         = copyFormat;
		copyDesc.Usage          = D3D11_USAGE_DEFAULT;
		copyDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.MiscFlags      = 0;
		const HRESULT hr = gameDevice->CreateTexture2D(&copyDesc, nullptr, &copy);
		if (FAILED(hr))
		{
			PLOG_ERROR << "depth capture: snapshot create failed hr=0x" << std::hex << (uint32_t)hr << std::dec;
			return false;
		}
		return true;
	}

	void onWatchedDraw(ID3D11DeviceContext* context)
	{
		ID3D11Texture2D* watched = watchedTexture.load(std::memory_order_relaxed);
		if (watched != nullptr && context == gameImmediateContext && immediateBoundDepth.load(std::memory_order_relaxed) == watched)
		{
			watchedDrawnSinceClear.store(true, std::memory_order_relaxed);
		}
	}

	void onWatchedClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* depthStencil, UINT clearFlags)
	{
		ID3D11Texture2D* watched = watchedTexture.load(std::memory_order_relaxed);
		if (watched == nullptr || context != gameImmediateContext || (clearFlags & D3D11_CLEAR_DEPTH) == 0 || insidePresent.load(std::memory_order_relaxed))
		{
			return;
		}
		const auto texture = textureOf(depthStencil);
		if (texture.Get() != watched)
		{
			return;
		}
		if (watchedDrawnSinceClear.exchange(false) && ensureShaderReadableCopy(preClearSnapshot, watched))
		{
			context->CopyResource(preClearSnapshot.Get(), watched);
			preClearTakenThisFrame = true;
		}
	}

	void recordSceneDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* depthStencil)
	{
		if (insidePresent.load(std::memory_order_relaxed))
		{
			return;
		}
		const auto texture = textureOf(depthStencil);
		if (context == gameImmediateContext)
		{
			immediateBoundDepth.store(texture.Get(), std::memory_order_relaxed);
		}
		if (texture == nullptr)
		{
			return;
		}
		D3D11_TEXTURE2D_DESC desc {};
		texture->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1)
		{
			return;
		}
		if (desc.Width != backbufferWidth || desc.Height != backbufferHeight)
		{
			return;
		}
		if (desc.Format == watchedFormat.load(std::memory_order_relaxed))
		{
			watchedTexture.store(texture.Get(), std::memory_order_relaxed);
		}
		std::lock_guard lock(frameDepthMutex);
		currentFrameDepth[desc.Format].fullResolution = texture;
	}

	bool isCandidateSize(const D3D11_TEXTURE2D_DESC& desc)
	{
		return desc.Width * 2 >= backbufferWidth && desc.Height * 2 >= backbufferHeight;
	}

	ID3D11Texture2D* trackCandidate(ID3D11View* view)
	{
		if (view == nullptr)
		{
			return nullptr;
		}
		ComPtr<ID3D11Resource> resource;
		view->GetResource(&resource);
		ComPtr<ID3D11Texture2D> texture;
		if (resource == nullptr || FAILED(resource.As(&texture)))
		{
			return nullptr;
		}
		const auto existing = trackedTextures.find(texture.Get());
		if (existing != trackedTextures.end())
		{
			return existing->first;
		}
		D3D11_TEXTURE2D_DESC desc {};
		texture->GetDesc(&desc);
		if (formatInfo(desc.Format).encoding == TexelEncoding::Unsupported || !isCandidateSize(desc))
		{
			return nullptr;
		}
		if (desc.SampleDesc.Count != 1)
		{
			++skippedMultisampled;
			return nullptr;
		}
		auto* key = texture.Get();
		trackedTextures.emplace(key, TrackedTexture { texture, desc, (uint32_t)trackedTextures.size() });
		return key;
	}

	void snapshotTexture(ID3D11DeviceContext* context, ID3D11Texture2D* texture, CaptureEvent event)
	{
		auto& tracked = trackedTextures.at(texture);
		if (!tracked.dirty && tracked.snapshotted)
		{
			return;
		}
		const FormatInfo info = formatInfo(tracked.desc.Format);
		const uint64_t bytes = (uint64_t)tracked.desc.Width * tracked.desc.Height * info.bytesPerTexel * tracked.desc.ArraySize * (tracked.desc.MipLevels == 1 ? 3 : 4) / 3;
		if (stagingBytes + bytes > kStagingBudgetBytes)
		{
			++skippedForBudget;
			return;
		}
		D3D11_TEXTURE2D_DESC stagingDesc = tracked.desc;
		stagingDesc.Format         = info.stagingFormat;
		stagingDesc.Usage          = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags      = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags      = 0;
		ComPtr<ID3D11Texture2D> staging;
		const HRESULT hr = gameDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);
		if (FAILED(hr))
		{
			PLOG_ERROR << "depth capture: staging create failed hr=0x" << std::hex << (uint32_t)hr << std::dec << " format=" << info.name;
			return;
		}
		context->CopyResource(staging.Get(), texture);
		stagingBytes += bytes;
		snapshots.push_back(Snapshot {
			(uint32_t)snapshots.size(),
			event,
			context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED,
			tracked.ordinal,
			tracked.desc,
			staging });
		tracked.dirty       = false;
		tracked.snapshotted = true;
	}

	void onDraw(ID3D11DeviceContext* context)
	{
		if (phase.load(std::memory_order_relaxed) != CapturePhase::Capturing)
		{
			return;
		}
		std::lock_guard lock(captureMutex);
		const auto bindings = contextBindings.find(context);
		if (phase != CapturePhase::Capturing || bindings == contextBindings.end())
		{
			return;
		}
		if (bindings->second.depth != nullptr)
		{
			trackedTextures.at(bindings->second.depth).dirty = true;
		}
		for (auto* color : bindings->second.colors)
		{
			if (color != nullptr)
			{
				trackedTextures.at(color).dirty = true;
			}
		}
	}

	void onBindTargets(ID3D11DeviceContext* context, UINT numViews, ID3D11RenderTargetView* const* renderTargets, ID3D11DepthStencilView* depthStencil)
	{
		if (phase.load(std::memory_order_relaxed) != CapturePhase::Capturing)
		{
			return;
		}
		std::lock_guard lock(captureMutex);
		if (phase != CapturePhase::Capturing)
		{
			return;
		}
		ContextBindings next;
		next.depth = trackCandidate(depthStencil);
		for (UINT i = 0; i < numViews && i < next.colors.size() && renderTargets != nullptr; ++i)
		{
			next.colors[i] = trackCandidate(renderTargets[i]);
		}
		auto& previous = contextBindings[context];
		if (previous.depth != nullptr && previous.depth != next.depth)
		{
			snapshotTexture(context, previous.depth, CaptureEvent::Unbind);
		}
		for (auto* color : previous.colors)
		{
			if (color != nullptr && std::find(next.colors.begin(), next.colors.end(), color) == next.colors.end())
			{
				snapshotTexture(context, color, CaptureEvent::Unbind);
			}
		}
		previous = next;
	}

	void onClearDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* depthStencil)
	{
		if (phase.load(std::memory_order_relaxed) != CapturePhase::Capturing)
		{
			return;
		}
		std::lock_guard lock(captureMutex);
		if (phase != CapturePhase::Capturing)
		{
			return;
		}
		if (auto* texture = trackCandidate(depthStencil))
		{
			snapshotTexture(context, texture, CaptureEvent::PreClear);
		}
	}

	struct ContextHookSet
	{
		safetyhook::InlineHook drawIndexed;
		safetyhook::InlineHook draw;
		safetyhook::InlineHook drawIndexedInstanced;
		safetyhook::InlineHook drawInstanced;
		safetyhook::InlineHook omSetRenderTargets;
		safetyhook::InlineHook omSetRenderTargetsAndUAVs;
		safetyhook::InlineHook clearDepthStencilView;
	};

	std::array<ContextHookSet, kMaxContextClasses> contextHooks;
	std::set<void*>                                hookedTargets;
	size_t                                         usedHookSets = 0;

	template <size_t I>
	struct Detours
	{
		static void __stdcall drawIndexed(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex)
		{
			onWatchedDraw(context);
			onDraw(context);
			contextHooks[I].drawIndexed.call<void>(context, indexCount, startIndex, baseVertex);
		}

		static void __stdcall draw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex)
		{
			onWatchedDraw(context);
			onDraw(context);
			contextHooks[I].draw.call<void>(context, vertexCount, startVertex);
		}

		static void __stdcall drawIndexedInstanced(ID3D11DeviceContext* context, UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance)
		{
			onWatchedDraw(context);
			onDraw(context);
			contextHooks[I].drawIndexedInstanced.call<void>(context, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
		}

		static void __stdcall drawInstanced(ID3D11DeviceContext* context, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance)
		{
			onWatchedDraw(context);
			onDraw(context);
			contextHooks[I].drawInstanced.call<void>(context, vertexCountPerInstance, instanceCount, startVertex, startInstance);
		}

		static void __stdcall omSetRenderTargets(ID3D11DeviceContext* context, UINT numViews, ID3D11RenderTargetView* const* renderTargets, ID3D11DepthStencilView* depthStencil)
		{
			recordSceneDepth(context, depthStencil);
			onBindTargets(context, numViews, renderTargets, depthStencil);
			contextHooks[I].omSetRenderTargets.call<void>(context, numViews, renderTargets, depthStencil);
		}

		static void __stdcall omSetRenderTargetsAndUAVs(ID3D11DeviceContext* context, UINT numRenderTargets, ID3D11RenderTargetView* const* renderTargets, ID3D11DepthStencilView* depthStencil, UINT uavStartSlot, UINT numUavs, ID3D11UnorderedAccessView* const* uavs, const UINT* uavInitialCounts)
		{
			if (numRenderTargets != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
			{
				recordSceneDepth(context, depthStencil);
				onBindTargets(context, numRenderTargets, renderTargets, depthStencil);
			}
			contextHooks[I].omSetRenderTargetsAndUAVs.call<void>(context, numRenderTargets, renderTargets, depthStencil, uavStartSlot, numUavs, uavs, uavInitialCounts);
		}

		static void __stdcall clearDepthStencilView(ID3D11DeviceContext* context, ID3D11DepthStencilView* depthStencil, UINT clearFlags, FLOAT depth, UINT8 stencil)
		{
			onWatchedClear(context, depthStencil, clearFlags);
			onClearDepth(context, depthStencil);
			contextHooks[I].clearDepthStencilView.call<void>(context, depthStencil, clearFlags, depth, stencil);
		}
	};

	bool createDisabledHook(safetyhook::InlineHook& hook, void* target, void* detour, std::vector<safetyhook::InlineHook*>& created)
	{
		if (hookedTargets.contains(target))
		{
			return true;
		}
		auto result = safetyhook::InlineHook::create(target, detour, safetyhook::InlineHook::StartDisabled);
		if (!result)
		{
			PLOG_ERROR << "depth capture: inline hook failed target=0x" << std::hex << (uintptr_t)target << std::dec;
			return false;
		}
		hook = std::move(*result);
		hookedTargets.insert(target);
		created.push_back(&hook);
		return true;
	}

	template <size_t I>
	bool installHookSet(void** vtable)
	{
		auto& set = contextHooks[I];
		std::vector<safetyhook::InlineHook*> created;
		const bool ok =
			   createDisabledHook(set.drawIndexed,               vtable[kSlotDrawIndexed],               reinterpret_cast<void*>(&Detours<I>::drawIndexed),               created)
			&& createDisabledHook(set.draw,                      vtable[kSlotDraw],                      reinterpret_cast<void*>(&Detours<I>::draw),                      created)
			&& createDisabledHook(set.drawIndexedInstanced,      vtable[kSlotDrawIndexedInstanced],      reinterpret_cast<void*>(&Detours<I>::drawIndexedInstanced),      created)
			&& createDisabledHook(set.drawInstanced,             vtable[kSlotDrawInstanced],             reinterpret_cast<void*>(&Detours<I>::drawInstanced),             created)
			&& createDisabledHook(set.omSetRenderTargets,        vtable[kSlotOMSetRenderTargets],        reinterpret_cast<void*>(&Detours<I>::omSetRenderTargets),        created)
			&& createDisabledHook(set.omSetRenderTargetsAndUAVs, vtable[kSlotOMSetRenderTargetsAndUAVs], reinterpret_cast<void*>(&Detours<I>::omSetRenderTargetsAndUAVs), created)
			&& createDisabledHook(set.clearDepthStencilView,     vtable[kSlotClearDepthStencilView],     reinterpret_cast<void*>(&Detours<I>::clearDepthStencilView),     created);
		for (auto* hook : created)
		{
			if (!hook->enable())
			{
				PLOG_ERROR << "depth capture: inline hook enable failed";
			}
		}
		return ok;
	}

	void installHooksForContext(ID3D11DeviceContext* context)
	{
		void** vtable = *reinterpret_cast<void***>(context);
		if (usedHookSets >= kMaxContextClasses)
		{
			PLOG_ERROR << "depth capture: out of context hook sets";
			return;
		}
		bool ok = false;
		switch (usedHookSets)
		{
		case 0: ok = installHookSet<0>(vtable); break;
		case 1: ok = installHookSet<1>(vtable); break;
		case 2: ok = installHookSet<2>(vtable); break;
		case 3: ok = installHookSet<3>(vtable); break;
		}
		++usedHookSets;
		PLOG_INFO << "depth capture: hooked context type=" << (int)context->GetType() << " vtable=0x" << std::hex << (uintptr_t)vtable << std::dec << " ok=" << ok;
	}

	void installHooks()
	{
		installHooksForContext(gameImmediateContext);
		ComPtr<ID3D11DeviceContext> deferred;
		if (SUCCEEDED(gameDevice->CreateDeferredContext(0, &deferred)))
		{
			installHooksForContext(deferred.Get());
		}
		else
		{
			PLOG_INFO << "depth capture: device has no deferred context support";
		}
		hooksInstalled = true;
	}

	std::string loadedGameName()
	{
		static constexpr std::array<std::pair<const wchar_t*, const char*>, 7> gameModules {{
			{ L"halo1.dll",     "halo1"     },
			{ L"halo2.dll",     "halo2"     },
			{ L"groundhog.dll", "halo2mp"   },
			{ L"halo3.dll",     "halo3"     },
			{ L"halo3odst.dll", "halo3odst" },
			{ L"haloreach.dll", "haloreach" },
			{ L"halo4.dll",     "halo4"     },
		}};
		std::string name;
		for (const auto& [module, label] : gameModules)
		{
			if (GetModuleHandleW(module) != nullptr)
			{
				name += name.empty() ? label : std::string("+") + label;
			}
		}
		return name.empty() ? "nogame" : name;
	}

	std::filesystem::path makeDumpFolder(const std::string& game)
	{
		wchar_t tempPath[MAX_PATH] {};
		GetTempPathW(MAX_PATH, tempPath);
		SYSTEMTIME now {};
		GetLocalTime(&now);
		const std::string stamp = std::format("{:04}{:02}{:02}-{:02}{:02}{:02}_{}", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, game);
		return std::filesystem::path(tempPath) / "HCMDepthDumps" / stamp;
	}

	float decodeTexel(const uint8_t* texel, TexelEncoding encoding)
	{
		switch (encoding)
		{
		case TexelEncoding::Float32:
		{
			float value;
			std::memcpy(&value, texel, sizeof(value));
			return value;
		}
		case TexelEncoding::Unorm24:
		{
			uint32_t raw;
			std::memcpy(&raw, texel, sizeof(raw));
			return (float)(raw & 0xFFFFFF) / 16777215.f;
		}
		case TexelEncoding::Unorm16:
		{
			uint16_t raw;
			std::memcpy(&raw, texel, sizeof(raw));
			return (float)raw / 65535.f;
		}
		case TexelEncoding::Float16:
		{
			uint16_t raw;
			std::memcpy(&raw, texel, sizeof(raw));
			return DirectX::PackedVector::XMConvertHalfToFloat(raw);
		}
		default:
			return 0.f;
		}
	}

	DecodedSnapshot readBack(Snapshot& snapshot)
	{
		DecodedSnapshot decoded { snapshot.sequence, snapshot.event, snapshot.deferredContext, snapshot.textureOrdinal, snapshot.desc, false, {} };
		D3D11_MAPPED_SUBRESOURCE mapped {};
		if (FAILED(gameImmediateContext->Map(snapshot.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
		{
			return decoded;
		}
		const FormatInfo info = formatInfo(snapshot.desc.Format);
		decoded.texels.resize((size_t)snapshot.desc.Width * snapshot.desc.Height);
		for (UINT y = 0; y < snapshot.desc.Height; ++y)
		{
			const auto* row = static_cast<const uint8_t*>(mapped.pData) + (size_t)y * mapped.RowPitch;
			for (UINT x = 0; x < snapshot.desc.Width; ++x)
			{
				decoded.texels[(size_t)y * snapshot.desc.Width + x] = decodeTexel(row + (size_t)x * info.bytesPerTexel, info.encoding);
			}
		}
		gameImmediateContext->Unmap(snapshot.staging.Get(), 0);
		decoded.readOk = true;
		return decoded;
	}

	struct TexelStats
	{
		float  minimum;
		float  maximum;
		float  lowPercentile;
		float  highPercentile;
		double coverage;
	};

	TexelStats computeStats(const std::vector<float>& texels)
	{
		TexelStats stats { FLT_MAX, -FLT_MAX, 0.f, 0.f, 0.0 };
		std::vector<float> interior;
		const size_t stride = std::max<size_t>(1, texels.size() / 1000000);
		for (size_t i = 0; i < texels.size(); ++i)
		{
			const float value = texels[i];
			if (!std::isfinite(value))
			{
				continue;
			}
			stats.minimum = std::min(stats.minimum, value);
			stats.maximum = std::max(stats.maximum, value);
			if (value != 0.f && value != 1.f && i % stride == 0)
			{
				interior.push_back(value);
			}
		}
		const size_t sampled = (texels.size() + stride - 1) / stride;
		stats.coverage = sampled == 0 ? 0.0 : (double)interior.size() / (double)sampled;
		if (interior.empty())
		{
			stats.lowPercentile  = stats.minimum;
			stats.highPercentile = stats.maximum;
			return stats;
		}
		const auto percentile = [&interior](double fraction)
		{
			auto nth = interior.begin() + (ptrdiff_t)(fraction * (double)(interior.size() - 1));
			std::nth_element(interior.begin(), nth, interior.end());
			return *nth;
		};
		stats.lowPercentile  = percentile(0.01);
		stats.highPercentile = percentile(0.99);
		return stats;
	}

	uint64_t hashTexels(const std::vector<float>& texels)
	{
		uint64_t hash = 1469598103934665603ull;
		const auto* bytes = reinterpret_cast<const uint8_t*>(texels.data());
		for (size_t i = 0; i < texels.size() * sizeof(float); ++i)
		{
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		}
		return hash;
	}

	void writeGrayscaleBmp(const std::filesystem::path& path, UINT width, UINT height, const std::vector<float>& texels, float low, float high)
	{
		const uint32_t rowStride  = (width + 3) & ~3u;
		const uint32_t paletteSz  = 256 * 4;
		const uint32_t headerSz   = 14 + 40 + paletteSz;
		const uint32_t imageSz    = rowStride * height;
		std::vector<uint8_t> file(headerSz + imageSz, 0);
		auto put16 = [&file](size_t offset, uint16_t value) { std::memcpy(file.data() + offset, &value, sizeof(value)); };
		auto put32 = [&file](size_t offset, uint32_t value) { std::memcpy(file.data() + offset, &value, sizeof(value)); };
		file[0] = 'B';
		file[1] = 'M';
		put32(2,  headerSz + imageSz);
		put32(10, headerSz);
		put32(14, 40);
		put32(18, width);
		put32(22, height);
		put16(26, 1);
		put16(28, 8);
		put32(34, imageSz);
		put32(46, 256);
		for (uint32_t i = 0; i < 256; ++i)
		{
			file[54 + i * 4 + 0] = (uint8_t)i;
			file[54 + i * 4 + 1] = (uint8_t)i;
			file[54 + i * 4 + 2] = (uint8_t)i;
		}
		const bool  logScale = low > 0.f && high > low;
		const float floor    = logScale ? std::max(low, high * 1e-5f) : low;
		const float base     = logScale ? std::log(floor) : floor;
		const float range    = logScale ? std::log(high) - base : (high > low ? high - low : 1.f);
		for (UINT y = 0; y < height; ++y)
		{
			uint8_t* row = file.data() + headerSz + (size_t)(height - 1 - y) * rowStride;
			for (UINT x = 0; x < width; ++x)
			{
				const float value = texels[(size_t)y * width + x];
				float normalized = 0.f;
				if (std::isfinite(value) && (!logScale || value > 0.f))
				{
					const float mapped = logScale ? std::log(std::max(value, floor)) : value;
					normalized = std::clamp((mapped - base) / range, 0.f, 1.f);
				}
				row[x] = (uint8_t)(normalized * 255.f + 0.5f);
			}
		}
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());
	}

	void writeDump(DumpJob job)
	{
		std::error_code error;
		std::filesystem::create_directories(job.folder, error);
		if (error)
		{
			PLOG_ERROR << "depth capture: cannot create " << job.folder.string() << ": " << error.message();
			return;
		}
		std::ofstream index(job.folder / "index.txt");
		index << "game=" << job.game << " backbuffer=" << job.backbufferWidth << "x" << job.backbufferHeight
			<< " snapshots=" << job.snapshots.size() << " skippedForBudget=" << job.skippedForBudget
			<< " skippedMultisampled=" << job.skippedMultisampled << "\n";
		index << "seq event ctx tex format size bind mips array min max p1 p99 coverage file\n";

		std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> lastHashByTexture;
		for (const auto& snapshot : job.snapshots)
		{
			const FormatInfo info = formatInfo(snapshot.desc.Format);
			index << std::format("{:03} {:8} {} tex{:02} {} {}x{} bind=0x{:x} mips={} array={} ",
				snapshot.sequence, eventName(snapshot.event), snapshot.deferredContext ? "def" : "imm", snapshot.textureOrdinal,
				info.name, snapshot.desc.Width, snapshot.desc.Height, snapshot.desc.BindFlags, snapshot.desc.MipLevels, snapshot.desc.ArraySize);
			if (!snapshot.readOk)
			{
				index << "MAP-FAILED\n";
				continue;
			}
			const uint64_t hash = hashTexels(snapshot.texels);
			const auto previous = lastHashByTexture.find(snapshot.textureOrdinal);
			if (previous != lastHashByTexture.end() && previous->second.first == hash)
			{
				index << std::format("same-as={:03}\n", previous->second.second);
				continue;
			}
			lastHashByTexture[snapshot.textureOrdinal] = { hash, snapshot.sequence };
			const TexelStats stats = computeStats(snapshot.texels);
			const std::string fileName = std::format("{:03}_{}_{}_tex{:02}_{}_{}x{}.bmp",
				snapshot.sequence, eventName(snapshot.event), snapshot.deferredContext ? "def" : "imm", snapshot.textureOrdinal,
				info.name, snapshot.desc.Width, snapshot.desc.Height);
			index << std::format("{:.6g} {:.6g} {:.6g} {:.6g} {:.1f}% {}\n",
				stats.minimum, stats.maximum, stats.lowPercentile, stats.highPercentile, stats.coverage * 100.0, fileName);
			writeGrayscaleBmp(job.folder / fileName, snapshot.desc.Width, snapshot.desc.Height, snapshot.texels, stats.lowPercentile, stats.highPercentile);
			std::ofstream raw(job.folder / std::filesystem::path(fileName).replace_extension(".f32"), std::ios::binary);
			raw.write(reinterpret_cast<const char*>(snapshot.texels.data()), (std::streamsize)(snapshot.texels.size() * sizeof(float)));
		}
		PLOG_INFO << "depth capture: dump written to " << job.folder.string();
	}

	void finishReadback()
	{
		DumpJob job {
			makeDumpFolder(loadedGameName()),
			loadedGameName(),
			backbufferWidth,
			backbufferHeight,
			skippedForBudget,
			skippedMultisampled,
			{} };
		{
			std::lock_guard lock(captureMutex);
			for (auto& snapshot : snapshots)
			{
				job.snapshots.push_back(readBack(snapshot));
			}
			snapshots.clear();
			trackedTextures.clear();
			contextBindings.clear();
			stagingBytes = 0;
		}
		PLOG_INFO << "depth capture: read back " << job.snapshots.size() << " snapshots, writing to " << job.folder.string();
		std::thread(writeDump, std::move(job)).detach();
		phase = CapturePhase::Idle;
	}

	bool pollHotkey()
	{
		DWORD foregroundProcess = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
		const bool down = foregroundProcess == GetCurrentProcessId() && (GetAsyncKeyState(kDumpHotkey) & 0x8000) != 0;
		const bool pressed = down && !hotkeyWasDown;
		hotkeyWasDown = down;
		return pressed;
	}

	void seedImmediateBindings()
	{
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets {};
		ID3D11DepthStencilView* depthStencil = nullptr;
		gameImmediateContext->OMGetRenderTargets((UINT)renderTargets.size(), renderTargets.data(), &depthStencil);
		ContextBindings bindings;
		bindings.depth = trackCandidate(depthStencil);
		for (size_t i = 0; i < renderTargets.size(); ++i)
		{
			bindings.colors[i] = trackCandidate(renderTargets[i]);
			if (renderTargets[i] != nullptr)
			{
				renderTargets[i]->Release();
			}
		}
		if (depthStencil != nullptr)
		{
			depthStencil->Release();
		}
		contextBindings[gameImmediateContext] = bindings;
	}
}

namespace DepthCapture
{
	void onPresentBegin(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* immediateContext)
	{
		insidePresent = true;
		gameDevice           = device;
		gameImmediateContext = immediateContext;
		DXGI_SWAP_CHAIN_DESC swapDesc {};
		swapChain->GetDesc(&swapDesc);
		backbufferWidth  = swapDesc.BufferDesc.Width;
		backbufferHeight = swapDesc.BufferDesc.Height;
		if (!hooksInstalled)
		{
			installHooks();
		}
		{
			std::lock_guard lock(frameDepthMutex);
			lastFrameDepths = std::move(currentFrameDepth);
			currentFrameDepth.clear();
			const DXGI_FORMAT watched = watchedFormat;
			const auto watchedFrame = lastFrameDepths.find(watched);
			if (watched != DXGI_FORMAT_UNKNOWN && watchedFrame != lastFrameDepths.end() && watchedFrame->second.fullResolution != nullptr
				&& ensureShaderReadableCopy(endOfFrameSnapshot, watchedFrame->second.fullResolution.Get()))
			{
				immediateContext->CopyResource(endOfFrameSnapshot.Get(), watchedFrame->second.fullResolution.Get());
				watchedFrame->second.endOfFrame = endOfFrameSnapshot;
				if (preClearTakenThisFrame)
				{
					watchedFrame->second.lastPreClear = preClearSnapshot;
				}
			}
			preClearTakenThisFrame = false;
		}

		if (pollHotkey() && phase == CapturePhase::Idle)
		{
			PLOG_INFO << "depth capture: armed, backbuffer=" << backbufferWidth << "x" << backbufferHeight;
			phase = CapturePhase::Armed;
			return;
		}

		if (phase == CapturePhase::Capturing)
		{
			std::lock_guard lock(captureMutex);
			phase = CapturePhase::AwaitingReadback;
			for (auto& [texture, tracked] : trackedTextures)
			{
				snapshotTexture(gameImmediateContext, texture, CaptureEvent::EndOfFrame);
			}
			readbackCountdown = kReadbackDelayFrames;
			PLOG_INFO << "depth capture: frame captured, textures=" << trackedTextures.size() << " snapshots=" << snapshots.size()
				<< " stagingMB=" << stagingBytes / (1024 * 1024) << " skippedForBudget=" << skippedForBudget;
			return;
		}

		if (phase == CapturePhase::AwaitingReadback && --readbackCountdown == 0)
		{
			finishReadback();
		}
	}

	void onPresentEnd()
	{
		insidePresent = false;
		if (phase != CapturePhase::Armed)
		{
			return;
		}
		std::lock_guard lock(captureMutex);
		snapshots.clear();
		trackedTextures.clear();
		contextBindings.clear();
		stagingBytes        = 0;
		skippedForBudget    = 0;
		skippedMultisampled = 0;
		seedImmediateBindings();
		phase = CapturePhase::Capturing;
	}

	FrameDepth lastFrameDepth(DXGI_FORMAT format)
	{
		std::lock_guard lock(frameDepthMutex);
		const auto frame = lastFrameDepths.find(format);
		return frame == lastFrameDepths.end() ? FrameDepth {} : frame->second;
	}

	void watchPreClear(DXGI_FORMAT format)
	{
		if (watchedFormat.exchange(format) != format)
		{
			watchedTexture         = nullptr;
			watchedDrawnSinceClear = false;
		}
	}
}
