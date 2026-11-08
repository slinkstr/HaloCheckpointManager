#include "pch.h"
#include "D3D11Hook.h"

#include <unordered_set>
#include <wrl/client.h>

#include "GlobalKill.h"
#include <dxgi.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#pragma comment(lib, "dxgi")

#include "Renderer2D.h"
#include "Lapua.h"
#include "safetyhook.hpp"


D3D11Hook* D3D11Hook::instance = nullptr;
SimpleMath::Vector2 D3D11Hook::mScreenSize{1920, 1080};
SimpleMath::Vector2 D3D11Hook::mScreenCenter{960, 540};

/**/
safetyhook::InlineHook D3D11Hook::occlusionCreateDeviceExportHook;
/**/safetyhook::InlineHook D3D11Hook::occlusionCreateDeviceSwapChainExportHook;

struct rgba {
	float r, g, b, a;
	rgba(float ir, float ig, float ib, float ia) : r(ir), g(ig), b(ib), a(ia) {}
};


enum class IDXGISwapChainVMT {
	QueryInterface,
	AddRef,
	Release,
	SetPrivateData,
	SetPrivateDataInterface,
	GetPrivateData,
	GetParent,
	GetDevice,
	Present,
	GetBuffer,
	SetFullscreenState,
	GetFullscreenState,
	GetDesc,
	ResizeBuffers,
	ResizeTarget,
	GetContainingOutput,
	GetFrameStatistics,
	GetLastPresentCount,
};


const std::map<D3D_DRIVER_TYPE, std::string> driverToString
{
	{ D3D_DRIVER_TYPE_HARDWARE, "D3D_DRIVER_TYPE_HARDWARE"},
	{ D3D_DRIVER_TYPE_WARP, "D3D_DRIVER_TYPE_WARP" },
	{ D3D_DRIVER_TYPE_REFERENCE, "D3D_DRIVER_TYPE_REFERENCE" },
	{ D3D_DRIVER_TYPE_SOFTWARE, "D3D_DRIVER_TYPE_SOFTWARE" },
	{ D3D_DRIVER_TYPE_UNKNOWN, "D3D_DRIVER_TYPE_UNKNOWN" },

};


/**/
namespace
{
	struct OcclusionProbeCandidate
	{
		ID3D11Texture2D* texture = nullptr;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> keepAlive;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT bytesPerPixel = 0;
		uint64_t lastSeenFrame = 0;
		uint64_t lastChecksum = 0;
		uint64_t lastContentChangeFrame = 0;
		bool hasChecksum = false;
	};

	std::mutex occlusionProbeMutex;
	std::vector<OcclusionProbeCandidate> occlusionProbeCandidates;
	std::vector<OcclusionProbeCandidate> occlusionProbeCreatedDepths;
	std::unordered_set<ID3D11Texture2D*> occlusionProbeCreatedSet;
	std::unordered_set<ID3D11Texture2D*> occlusionProbeIgnored;
	std::unordered_set<ID3D11Texture2D*> occlusionProbeSkippedMsaa;
	std::unordered_set<ID3D11Resource*> occlusionProbeSkippedNon2D;
	std::atomic<uint64_t> occlusionProbeFrame{ 0 };
	ID3D11Device* occlusionProbeDevice = nullptr;
	ID3D11DeviceContext* occlusionProbeContext = nullptr;
	ID3D11Texture2D* occlusionProbeSelected = nullptr;
	DXGI_FORMAT occlusionProbeSelectedFormat = DXGI_FORMAT_UNKNOWN;
	struct OcclusionProbeStaging { DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; UINT width = 0; UINT height = 0; UINT samples = 0; Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; };
	OcclusionProbeStaging occlusionProbeStagings[16];
	Microsoft::WRL::ComPtr<ID3D11Texture2D> occlusionProbeFullStaging;
	DXGI_FORMAT occlusionProbeFullStagingFormat = DXGI_FORMAT_UNKNOWN;
	uint64_t occlusionProbeFullStagingWidth = 0;
	uint64_t occlusionProbeFullStagingHeight = 0;
	uint64_t occlusionProbeLastReadbackFrame = 0;

	UINT occlusionProbeBytesPerPixel(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return 8;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return 4;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R8G8_TYPELESS:
			return 2;
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
			return 1;
		default:
			return 0;
		}
	}

	bool occlusionProbeIsSceneDepthFormat(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R32G8X24_TYPELESS || format == DXGI_FORMAT_R32_TYPELESS
			|| format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || format == DXGI_FORMAT_D32_FLOAT
			|| format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_D24_UNORM_S8_UINT;
	}

	ID3D11Texture2D* occlusionProbeEnsureStaging(const D3D11_TEXTURE2D_DESC& desc)
	{
		for (auto& staging : occlusionProbeStagings)
		{
			if (staging.format == desc.Format && staging.width == desc.Width && staging.height == desc.Height && staging.samples == desc.SampleDesc.Count && staging.texture != nullptr)
			{
				return staging.texture.Get();
			}
		}
		for (auto& staging : occlusionProbeStagings)
		{
			if (staging.texture == nullptr)
			{
				D3D11_TEXTURE2D_DESC stagingDesc = desc;
				stagingDesc.Usage = D3D11_USAGE_STAGING;
				stagingDesc.BindFlags = 0;
				stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				stagingDesc.MiscFlags = 0;
				if (SUCCEEDED(occlusionProbeDevice->CreateTexture2D(&stagingDesc, nullptr, &staging.texture)))
				{
					staging.format = desc.Format;
					staging.width = desc.Width;
					staging.height = desc.Height;
					staging.samples = desc.SampleDesc.Count;
					return staging.texture.Get();
				}
				return nullptr;
			}
		}
		return nullptr;
	}

	void occlusionProbeMaybeReadback()
	{
		if (occlusionProbeContext == nullptr || occlusionProbeDevice == nullptr)
		{
			return;
		}
		if (occlusionProbeFrame - occlusionProbeLastReadbackFrame < 120)
		{
			return;
		}
		occlusionProbeLastReadbackFrame = occlusionProbeFrame;

		std::vector<ID3D11Texture2D*> snapshot;
		{
			std::lock_guard<std::mutex> lock(occlusionProbeMutex);
			for (const auto& candidate : occlusionProbeCandidates)
			{
				if (occlusionProbeIsSceneDepthFormat(candidate.format) && candidate.bytesPerPixel > 0)
				{
					snapshot.push_back(candidate.texture);
				}
			}
			for (const auto& created : occlusionProbeCreatedDepths)
			{
				if (created.bytesPerPixel > 0
					&& std::find(snapshot.begin(), snapshot.end(), created.texture) == snapshot.end())
				{
					snapshot.push_back(created.texture);
				}
			}
		}

		for (ID3D11Texture2D* texture : snapshot)
		{
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			if (desc.Width < 64 || desc.Height < 64)
			{
				continue;
			}
			ID3D11Texture2D* staging = occlusionProbeEnsureStaging(desc);
			if (staging == nullptr)
			{
				continue;
			}

			Microsoft::WRL::ComPtr<ID3D11Device> texDevice;
			texture->GetDevice(&texDevice);

			D3D11_MAPPED_SUBRESOURCE sentinel{};
			if (SUCCEEDED(occlusionProbeContext->Map(staging, 0, D3D11_MAP_WRITE, 0, &sentinel)))
			{
				std::memset(sentinel.pData, 0xAB, (size_t)sentinel.RowPitch * desc.Height);
				occlusionProbeContext->Unmap(staging, 0);
			}
			occlusionProbeContext->CopyResource(staging, texture);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(occlusionProbeContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
			{
				continue;
			}

			const UINT bpp = occlusionProbeBytesPerPixel(desc.Format);
			float lo = FLT_MAX;
			float hi = -FLT_MAX;
			float sum = 0.f;
			UINT count = 0;
			UINT sentinelHits = 0;
			uint64_t checksum = 0;
			const bool dumpGrid = texture == occlusionProbeSelected || (desc.Width >= 1920 && desc.Height >= 1080 && desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			float grid[9][16]{};

			const auto* base = static_cast<const uint8_t*>(mapped.pData);
			for (int gy = 0; gy < 9; ++gy)
			{
				const UINT y = (UINT)((gy + 0.5) * desc.Height / 9);
				for (int gx = 0; gx < 16; ++gx)
				{
					const UINT x = (UINT)((gx + 0.5) * desc.Width / 16);
					const auto* px = base + (size_t)y * mapped.RowPitch + (size_t)x * bpp;
					float depth;
					std::memcpy(&depth, px, sizeof(float));
					uint32_t raw;
					std::memcpy(&raw, px, sizeof(uint32_t));
					checksum = checksum * 1099511628211ull + raw;
					grid[gy][gx] = depth;
					if (raw == 0xABABABABu)
					{
						++sentinelHits;
					}
					if (std::isfinite(depth))
					{
						lo = (std::min)(lo, depth);
						hi = (std::max)(hi, depth);
						sum += depth;
						++count;
					}
				}
			}
			occlusionProbeContext->Unmap(staging, 0);

			if (dumpGrid && count > 0)
			{
				std::string art;
				for (int gy = 0; gy < 9; ++gy)
				{
					for (int gx = 0; gx < 16; ++gx)
					{
						const float d = grid[gy][gx];
						char ch = '?';
						if (std::isfinite(d))
						{
							ch = " .:-=+*#%@"[(std::min)(9, (std::max)(0, (int)(d / 0.35f * 9.f)))];
						}
						art += ch;
					}
					art += '\n';
				}
				PLOG_INFO << "occlusion probe grid frame=" << occlusionProbeFrame << " tex=0x" << std::hex << (uintptr_t)texture << std::dec << "\n" << art;
			}

			{
				std::lock_guard<std::mutex> lock(occlusionProbeMutex);
				for (auto& candidate : occlusionProbeCandidates)
				{
					if (candidate.texture == texture)
					{
						if (candidate.hasChecksum && candidate.lastChecksum != checksum)
						{
							candidate.lastContentChangeFrame = occlusionProbeFrame;
						}
						candidate.lastChecksum = checksum;
						candidate.hasChecksum = true;
						break;
					}
				}
			}

			if (count > 0)
			{
				PLOG_INFO << "occlusion probe readback frame=" << occlusionProbeFrame
					<< " tex=0x" << std::hex << (uintptr_t)texture << std::dec
					<< " dev=0x" << std::hex << (uintptr_t)texDevice.Get() << " ours=0x" << (uintptr_t)occlusionProbeDevice << std::dec
					<< " " << desc.Width << "x" << desc.Height
					<< " fmt=" << (int)desc.Format
					<< (sentinelHits == 144 ? " COPYFAIL" : "")
					<< " f[min=" << lo << " max=" << hi << " avg=" << (sum / count) << "]"
					<< " sum=" << std::hex << checksum << std::dec;
			}
		}
	}
}
/**/

void D3D11Hook::occlusionProbeRecordDSV(ID3D11DepthStencilView* dsv)
{
	if (dsv == nullptr)
	{
		return;
	}

	ID3D11Resource* resource = nullptr;
	dsv->GetResource(&resource);
	if (resource == nullptr)
	{
		return;
	}
	ID3D11Texture2D* texture = nullptr;
	const bool isTexture2D = SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture != nullptr;
	if (!isTexture2D)
	{
		D3D11_RESOURCE_DIMENSION dim{};
		resource->GetType(&dim);
		if (occlusionProbeSkippedNon2D.insert(resource).second)
		{
			PLOG_INFO << "occlusion probe: skipped non-2d depth dsv=0x" << std::hex << (uintptr_t)dsv
				<< " res=0x" << (uintptr_t)resource << std::dec
				<< " dim=" << (int)dim;
		}
		resource->Release();
		return;
	}
	resource->Release();

	std::lock_guard<std::mutex> lock(occlusionProbeMutex);
	if (occlusionProbeIgnored.count(texture) != 0)
	{
		texture->Release();
		return;
	}
	for (auto& candidate : occlusionProbeCandidates)
	{
		if (candidate.texture == texture)
		{
			candidate.lastSeenFrame = occlusionProbeFrame;
			texture->Release();
			return;
		}
	}

	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	if (desc.SampleDesc.Count != 1 || desc.Width < 64 || desc.Height < 64 || occlusionProbeCandidates.size() >= 32)
	{
		if (desc.Width >= 64 && desc.Height >= 64 && occlusionProbeSkippedMsaa.count(texture) == 0)
		{
			occlusionProbeSkippedMsaa.insert(texture);
			PLOG_INFO << "occlusion probe: skipped depth dsv=0x" << std::hex << (uintptr_t)dsv
				<< " tex=0x" << (uintptr_t)texture << std::dec
				<< " " << desc.Width << "x" << desc.Height
				<< " fmt=" << (int)desc.Format
				<< " samples=" << desc.SampleDesc.Count;
		}
		texture->Release();
		return;
	}

	OcclusionProbeCandidate candidate{};
	candidate.texture = texture;
	candidate.keepAlive.Attach(texture);
	candidate.width = desc.Width;
	candidate.height = desc.Height;
	candidate.format = desc.Format;
	candidate.bytesPerPixel = occlusionProbeBytesPerPixel(desc.Format);
	candidate.lastSeenFrame = occlusionProbeFrame;
	occlusionProbeCandidates.push_back(candidate);

	PLOG_INFO << "occlusion probe: new depth candidate dsv=0x" << std::hex << (uintptr_t)dsv
		<< " tex=0x" << (uintptr_t)texture << std::dec
		<< " " << desc.Width << "x" << desc.Height
		<< " fmt=" << (int)desc.Format;
}

struct OcclusionVtableSlots
{
	void* orts = nullptr;
	void* ortsUav = nullptr;
	void* createDeferred = nullptr;
	void* createDsv = nullptr;
	void* createTex = nullptr;
};

std::mutex occlusionVtableMutex;
std::unordered_map<void*, OcclusionVtableSlots> occlusionVtableSlots;
std::unordered_set<void*> occlusionLoggedVtables;
std::unordered_set<void*> occlusionLoggedDevices;
std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>> occlusionOrtsImplHooks;
std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>> occlusionOrtsUavImplHooks;
std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>> occlusionCreateDeferredImplHooks;
std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>> occlusionCreateDsvImplHooks;
std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>> occlusionCreateTexImplHooks;

void occlusionInlineHookImpl(void* impl, void* hookFn, std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>>& hooks)
{
	if (impl == nullptr || hooks.find(impl) != hooks.end())
	{
		return;
	}
	PLOG_INFO << "occlusion probe: inline hooking impl=0x" << std::hex << (uintptr_t)impl << std::dec;
	auto hook = safetyhook::create_inline(impl, hookFn);
	PLOG_INFO << "occlusion probe: inline hook result valid=" << (bool)hook;
	if (hook)
	{
		hooks[impl] = std::make_unique<safetyhook::InlineHook>(std::move(hook));
	}
}

void occlusionProbeScanForContextImpls()
{
	static std::once_flag scanned;
	std::call_once(scanned, []
	{
		const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
		if (d3d11 == nullptr)
		{
			return;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(d3d11);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(d3d11) + dos->e_lfanew);
		const auto* section = IMAGE_FIRST_SECTION(nt);
		const uint8_t* textStart = nullptr;
		const uint8_t* rdataStart = nullptr;
		size_t textSize = 0;
		size_t rdataSize = 0;
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			if (std::memcmp(section[i].Name, ".text", 5) == 0)
			{
				textStart = reinterpret_cast<const uint8_t*>(d3d11) + section[i].VirtualAddress;
				textSize = section[i].Misc.VirtualSize;
			}
			else if (std::memcmp(section[i].Name, ".rdata", 6) == 0)
			{
				rdataStart = reinterpret_cast<const uint8_t*>(d3d11) + section[i].VirtualAddress;
				rdataSize = section[i].Misc.VirtualSize;
			}
		}
		if (textStart == nullptr || rdataStart == nullptr)
		{
			return;
		}
		const auto inText = [&](const void* p)
		{
			const auto* b = static_cast<const uint8_t*>(p);
			return b >= textStart && b < textStart + textSize;
		};
		const uint8_t* const moduleBase = reinterpret_cast<const uint8_t*>(d3d11);
		std::lock_guard<std::mutex> lock(occlusionVtableMutex);
		int vtables = 0;
		for (const uint8_t* p = rdataStart + 8; p + 35 * sizeof(void*) < rdataStart + rdataSize; p += sizeof(void*))
		{
			const void* col = *reinterpret_cast<void* const*>(p - sizeof(void*));
			const auto* colBytes = static_cast<const uint8_t*>(col);
			if (colBytes < rdataStart || colBytes >= rdataStart + rdataSize)
			{
				continue;
			}
			if (!inText(*reinterpret_cast<void* const*>(p)) || !inText(*reinterpret_cast<void* const*>(p + 8)) || !inText(*reinterpret_cast<void* const*>(p + 16)))
			{
				continue;
			}
			uint32_t signature = 0;
			uint32_t typeDescriptorRva = 0;
			std::memcpy(&signature, colBytes, sizeof(uint32_t));
			std::memcpy(&typeDescriptorRva, colBytes + 12, sizeof(uint32_t));
			if (signature != 1 || typeDescriptorRva == 0 || typeDescriptorRva >= nt->OptionalHeader.SizeOfImage)
			{
				continue;
			}
			const char* className = reinterpret_cast<const char*>(moduleBase + typeDescriptorRva + 0x10);
			if (std::strstr(className, "DeviceContext") == nullptr)
			{
				continue;
			}
			void* implOrts = *reinterpret_cast<void* const*>(p + 33 * sizeof(void*));
			void* implOrtsUav = *reinterpret_cast<void* const*>(p + 34 * sizeof(void*));
			if (!inText(implOrts) || !inText(implOrtsUav))
			{
				continue;
			}
			occlusionInlineHookImpl(implOrts, reinterpret_cast<void*>(&D3D11Hook::newOMSetRenderTargets), occlusionOrtsImplHooks);
			occlusionInlineHookImpl(implOrtsUav, reinterpret_cast<void*>(&D3D11Hook::newOMSetRenderTargetsAndUAVs), occlusionOrtsUavImplHooks);
			++vtables;
			PLOG_INFO << "occlusion probe: context vtable " << className << " vtable=0x" << std::hex << (uintptr_t)p << std::dec;
		}
		PLOG_INFO << "occlusion probe: vtable scan complete, context vtables=" << vtables;
	});
}

void* occlusionRawVtableSlot(void* object, size_t index)
{
	void** vtable = *reinterpret_cast<void***>(object);
	return vtable[index];
}

void* occlusionVtablePatchSlot(void* object, size_t index, void* hookFn)
{
	void** vtable = *reinterpret_cast<void***>(object);
	DWORD oldProtect = 0;
	void* original = nullptr;
	if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		if (vtable[index] != hookFn)
		{
			original = vtable[index];
			vtable[index] = hookFn;
		}
		VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
	}
	return original;
}

std::string occlusionModuleNameForAddress(const void* address)
{
	HMODULE module = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && module != nullptr)
	{
		char path[MAX_PATH] = {};
		if (GetModuleFileNameA(module, path, MAX_PATH) != 0)
		{
			const char* name = std::strrchr(path, '\\');
			return name != nullptr ? name + 1 : path;
		}
	}
	MEMORY_BASIC_INFORMATION info = {};
	if (VirtualQuery(address, &info, sizeof(info)) != 0)
	{
		char buffer[96] = {};
		std::snprintf(buffer, sizeof(buffer), "type=%u allocbase=0x%llx state=%u protect=0x%lx",
			(unsigned)info.Type, (unsigned long long)(uintptr_t)info.AllocationBase, (unsigned)info.State, info.Protect);
		return buffer;
	}
	return "?";
}

void occlusionEnsureContextVtable(void* contextObject)
{
	void* vtable = *reinterpret_cast<void***>(contextObject);
	std::lock_guard<std::mutex> lock(occlusionVtableMutex);
	auto& slots = occlusionVtableSlots[vtable];
	if (slots.orts != nullptr)
	{
		return;
	}
	PLOG_INFO << "occlusion probe: patching context vtable=0x" << std::hex << (uintptr_t)vtable << std::dec;
	if (void* original = occlusionVtablePatchSlot(contextObject, 33, reinterpret_cast<void*>(&D3D11Hook::newOMSetRenderTargets)))
	{
		slots.orts = original;
		occlusionInlineHookImpl(original, reinterpret_cast<void*>(&D3D11Hook::newOMSetRenderTargets), occlusionOrtsImplHooks);
	}
	if (void* original = occlusionVtablePatchSlot(contextObject, 34, reinterpret_cast<void*>(&D3D11Hook::newOMSetRenderTargetsAndUAVs)))
	{
		slots.ortsUav = original;
		occlusionInlineHookImpl(original, reinterpret_cast<void*>(&D3D11Hook::newOMSetRenderTargetsAndUAVs), occlusionOrtsUavImplHooks);
	}
	if (occlusionLoggedVtables.insert(vtable).second)
	{
		PLOG_INFO << "occlusion probe: patched context vtable=0x" << std::hex << (uintptr_t)vtable << std::dec
			<< " module=" << occlusionModuleNameForAddress(vtable);
	}
}

void occlusionEnsureDeviceVtable(void* device)
{
	void* vtable = *reinterpret_cast<void***>(device);
	std::lock_guard<std::mutex> lock(occlusionVtableMutex);
	auto& slots = occlusionVtableSlots[vtable];
	if (slots.createDeferred == nullptr)
	{
		PLOG_INFO << "occlusion probe: patching device vtable=0x" << std::hex << (uintptr_t)vtable << std::dec;
		if (void* original = occlusionVtablePatchSlot(device, 27, reinterpret_cast<void*>(&D3D11Hook::newCreateDeferredContext)))
		{
			slots.createDeferred = original;
			occlusionInlineHookImpl(original, reinterpret_cast<void*>(&D3D11Hook::newCreateDeferredContext), occlusionCreateDeferredImplHooks);
		}
		if (void* original = occlusionVtablePatchSlot(device, 10, reinterpret_cast<void*>(&D3D11Hook::newCreateDepthStencilView)))
		{
			slots.createDsv = original;
			occlusionInlineHookImpl(original, reinterpret_cast<void*>(&D3D11Hook::newCreateDepthStencilView), occlusionCreateDsvImplHooks);
		}
		if (void* original = occlusionVtablePatchSlot(device, 5, reinterpret_cast<void*>(&D3D11Hook::newCreateTexture2D)))
		{
			slots.createTex = original;
			occlusionInlineHookImpl(original, reinterpret_cast<void*>(&D3D11Hook::newCreateTexture2D), occlusionCreateTexImplHooks);
		}
	}

	if (occlusionLoggedVtables.insert(vtable).second)
	{
		PLOG_INFO << "occlusion probe: patched device vtable=0x" << std::hex << (uintptr_t)vtable << std::dec
			<< " module=" << occlusionModuleNameForAddress(vtable);
	}
}

OcclusionVtableSlots occlusionVtableOriginals(void* object)
{
	void* vtable = *reinterpret_cast<void***>(object);
	std::lock_guard<std::mutex> lock(occlusionVtableMutex);
	const auto it = occlusionVtableSlots.find(vtable);
	if (it == occlusionVtableSlots.end())
	{
		return {};
	}
	return it->second;
}

safetyhook::InlineHook* occlusionFindImplHook(void* impl, std::unordered_map<void*, std::unique_ptr<safetyhook::InlineHook>>& hooks)
{
	std::lock_guard<std::mutex> lock(occlusionVtableMutex);
	const auto it = hooks.find(impl);
	return it == hooks.end() ? nullptr : it->second.get();
}

void D3D11Hook::occlusionProbeHookContext(void* contextObject)
{
	if (contextObject == nullptr)
	{
		return;
	}
	occlusionEnsureContextVtable(contextObject);

	auto tryVersion = [contextObject](REFIID iid)
	{
		IUnknown* versioned = nullptr;
		if (SUCCEEDED(reinterpret_cast<IUnknown*>(contextObject)->QueryInterface(iid, reinterpret_cast<void**>(&versioned))) && versioned != nullptr)
		{
			occlusionEnsureContextVtable(versioned);
			versioned->Release();
		}
	};
	tryVersion(__uuidof(ID3D11DeviceContext1));
	tryVersion(__uuidof(ID3D11DeviceContext2));
	tryVersion(__uuidof(ID3D11DeviceContext3));
	tryVersion(__uuidof(ID3D11DeviceContext4));
}

void D3D11Hook::occlusionProbeHookDevice(ID3D11Device* device)
{
	if (device == nullptr)
	{
		return;
	}
	occlusionProbeScanForContextImpls();
	occlusionEnsureDeviceVtable(device);

	auto tryDeviceVersion = [device](REFIID iid)
	{
		IUnknown* versioned = nullptr;
		if (SUCCEEDED(device->QueryInterface(iid, reinterpret_cast<void**>(&versioned))) && versioned != nullptr)
		{
			occlusionEnsureDeviceVtable(versioned);
			versioned->Release();
		}
	};
	tryDeviceVersion(__uuidof(ID3D11Device1));
	tryDeviceVersion(__uuidof(ID3D11Device2));
	tryDeviceVersion(__uuidof(ID3D11Device3));
	tryDeviceVersion(__uuidof(ID3D11Device4));

	ID3D11DeviceContext* immediateContext = nullptr;
	device->GetImmediateContext(&immediateContext);
	if (immediateContext != nullptr)
	{
		occlusionProbeHookContext(immediateContext);
		immediateContext->Release();
	}

	if (occlusionLoggedDevices.insert(device).second)
	{
		PLOG_INFO << "occlusion probe: hooked device=0x" << std::hex << (uintptr_t)device << std::dec;
	}
}

HRESULT WINAPI occlusionProbeHookedCreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
	const HRESULT hr = D3D11Hook::occlusionCreateDeviceExportHook.call<HRESULT>(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
	if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr)
	{
		D3D11Hook::occlusionProbeHookDevice(*ppDevice);
	}
	return hr;
}

HRESULT WINAPI occlusionProbeHookedCreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
	const HRESULT hr = D3D11Hook::occlusionCreateDeviceSwapChainExportHook.call<HRESULT>(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
	if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr)
	{
		D3D11Hook::occlusionProbeHookDevice(*ppDevice);
	}
	return hr;
}

void D3D11Hook::installOcclusionExportHooks()
{
	const HMODULE d3d11Module = GetModuleHandleW(L"d3d11.dll");
	if (d3d11Module == nullptr)
	{
		return;
	}
	auto createDevice = GetProcAddress(d3d11Module, "D3D11CreateDevice");
	if (createDevice != nullptr)
	{
		occlusionCreateDeviceExportHook = safetyhook::create_inline(reinterpret_cast<void*>(createDevice), reinterpret_cast<void*>(&occlusionProbeHookedCreateDevice));
	}
	auto createDeviceSwapChain = GetProcAddress(d3d11Module, "D3D11CreateDeviceAndSwapChain");
	if (createDeviceSwapChain != nullptr)
	{
		occlusionCreateDeviceSwapChainExportHook = safetyhook::create_inline(reinterpret_cast<void*>(createDeviceSwapChain), reinterpret_cast<void*>(&occlusionProbeHookedCreateDeviceAndSwapChain));
	}
	PLOG_INFO << "occlusion probe: d3d11 export hooks installed";
}
/**/

void D3D11Hook::occlusionProbeSnapshot()
{
	++occlusionProbeFrame;

	if (occlusionProbeContext != nullptr)
	{
		ID3D11RenderTargetView* rtv = nullptr;
		ID3D11DepthStencilView* boundDsv = nullptr;
		occlusionProbeContext->OMGetRenderTargets(1, &rtv, &boundDsv);
		occlusionProbeRecordDSV(boundDsv);
		if (rtv != nullptr) { rtv->Release(); }
		if (boundDsv != nullptr) { boundDsv->Release(); }
	}

	ID3D11Texture2D* previousSelected;
	{
		std::lock_guard<std::mutex> lock(occlusionProbeMutex);
		previousSelected = occlusionProbeSelected;

		ID3D11Texture2D* selected = nullptr;
		DXGI_FORMAT selectedFormat = DXGI_FORMAT_UNKNOWN;
		UINT selectedArea = 0;
		uint64_t selectedLastSeen = 0;
		int selectedRank = -1;
		for (auto& candidate : occlusionProbeCandidates)
		{
			if (!occlusionProbeIsSceneDepthFormat(candidate.format))
			{
				continue;
			}
			// prefer fresh candidates whose contents actually change; among equally ranked, largest area
			const bool candidateFresh = occlusionProbeFrame - candidate.lastSeenFrame < 300;
			const bool candidateLive = candidate.hasChecksum && occlusionProbeFrame - candidate.lastContentChangeFrame < 600;
			const int rank = (candidateLive ? 2 : 0) + (candidateFresh ? 1 : 0);
			const UINT area = candidate.width * candidate.height;
			if (selected == nullptr
				|| rank > selectedRank
				|| (rank == selectedRank && area > selectedArea)
				|| (rank == selectedRank && area == selectedArea && candidate.lastSeenFrame > selectedLastSeen))
			{
				selected = candidate.texture;
				selectedFormat = candidate.format;
				selectedArea = area;
				selectedLastSeen = candidate.lastSeenFrame;
				selectedRank = rank;
			}
		}

		if (selected != previousSelected)
		{
			PLOG_INFO << "occlusion probe: selected depth tex=0x" << std::hex << (uintptr_t)selected << std::dec
				<< " fmt=" << (int)selectedFormat;
			occlusionProbeSelected = selected;
			occlusionProbeSelectedFormat = selectedFormat;
			occlusionProbeLastReadbackFrame = 0;
		}
	}

	occlusionProbeMaybeReadback();
}

void D3D11Hook::occlusionProbeIgnoreTexture(ID3D11Texture2D* texture)
{
	if (texture == nullptr)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(occlusionProbeMutex);
	occlusionProbeIgnored.insert(texture);
}

ID3D11Texture2D* D3D11Hook::occlusionProbeSelectDepthTex()
{
	std::lock_guard<std::mutex> lock(occlusionProbeMutex);
	return occlusionProbeSelected;
}

uint64_t D3D11Hook::occlusionProbeFrameCount()
{
	return occlusionProbeFrame;
}

DXGI_FORMAT D3D11Hook::occlusionProbeDepthFormat()
{
	std::lock_guard<std::mutex> lock(occlusionProbeMutex);
	return occlusionProbeSelectedFormat;
}

bool D3D11Hook::occlusionProbeSampleDepth(float u, float v, float& outDepth)
{
	if (occlusionProbeContext == nullptr || occlusionProbeDevice == nullptr || occlusionProbeSelected == nullptr)
	{
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	occlusionProbeSelected->GetDesc(&desc);
	if (desc.SampleDesc.Count != 1 || !occlusionProbeEnsureStaging(desc))
	{
		return false;
	}

	const UINT bpp = occlusionProbeBytesPerPixel(desc.Format);
	if (bpp < 4)
	{
		return false;
	}

	if (occlusionProbeFullStaging == nullptr
		|| occlusionProbeFullStagingFormat != desc.Format
		|| occlusionProbeFullStagingWidth != desc.Width
		|| occlusionProbeFullStagingHeight != desc.Height)
	{
		occlusionProbeFullStaging = nullptr;
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = 0;
		if (FAILED(occlusionProbeDevice->CreateTexture2D(&stagingDesc, nullptr, &occlusionProbeFullStaging)))
		{
			return false;
		}
		occlusionProbeFullStagingFormat = desc.Format;
		occlusionProbeFullStagingWidth = desc.Width;
		occlusionProbeFullStagingHeight = desc.Height;
	}

	occlusionProbeContext->CopyResource(occlusionProbeFullStaging.Get(), occlusionProbeSelected);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(occlusionProbeContext->Map(occlusionProbeFullStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
	{
		return false;
	}

	const UINT x = (UINT)((std::min)(1.f, (std::max)(0.f, u)) * (desc.Width - 1));
	const UINT y = (UINT)((std::min)(1.f, (std::max)(0.f, v)) * (desc.Height - 1));
	const auto* px = static_cast<const uint8_t*>(mapped.pData) + (size_t)y * mapped.RowPitch + (size_t)x * bpp;
	float depth;
	if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS || desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
	{
		uint32_t packed;
		std::memcpy(&packed, px, sizeof(uint32_t));
		depth = (float)((packed >> 8) & 0xFFFFFFu) / 16777215.f;
	}
	else
	{
		std::memcpy(&depth, px, sizeof(float));
	}
	occlusionProbeContext->Unmap(occlusionProbeFullStaging.Get(), 0);

	if (!std::isfinite(depth))
	{
		return false;
	}
	outDepth = depth;
	return true;
}

void __stdcall D3D11Hook::newOMSetRenderTargets(ID3D11DeviceContext* pContext, UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView)
{
	occlusionProbeRecordDSV(pDepthStencilView);
	const OcclusionVtableSlots slots = occlusionVtableOriginals(pContext);
	void* impl = slots.orts != nullptr ? slots.orts : occlusionRawVtableSlot(pContext, 33);
	if (safetyhook::InlineHook* hook = occlusionFindImplHook(impl, occlusionOrtsImplHooks))
	{
		hook->call<void>(pContext, NumViews, ppRenderTargetViews, pDepthStencilView);
	}
	else if (slots.orts != nullptr)
	{
		reinterpret_cast<DX11OMSetRenderTargets*>(slots.orts)(pContext, NumViews, ppRenderTargetViews, pDepthStencilView);
	}
}

void __stdcall D3D11Hook::newOMSetRenderTargetsAndUAVs(ID3D11DeviceContext* pContext, UINT NumRTVs, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUAVs, const UINT* pUAVInitialCounts)
{
	occlusionProbeRecordDSV(pDSV);
	const OcclusionVtableSlots slots = occlusionVtableOriginals(pContext);
	void* impl = slots.ortsUav != nullptr ? slots.ortsUav : occlusionRawVtableSlot(pContext, 34);
	if (safetyhook::InlineHook* hook = occlusionFindImplHook(impl, occlusionOrtsUavImplHooks))
	{
		hook->call<void>(pContext, NumRTVs, ppRTVs, pDSV, UAVStartSlot, NumUAVs, ppUAVs, pUAVInitialCounts);
	}
	else if (slots.ortsUav != nullptr)
	{
		reinterpret_cast<DX11OMSetRenderTargetsAndUAVs*>(slots.ortsUav)(pContext, NumRTVs, ppRTVs, pDSV, UAVStartSlot, NumUAVs, ppUAVs, pUAVInitialCounts);
	}
}

HRESULT __stdcall D3D11Hook::newCreateDepthStencilView(ID3D11Device* pDevice, ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView)
{
	const OcclusionVtableSlots slots = occlusionVtableOriginals(pDevice);
	void* impl = slots.createDsv != nullptr ? slots.createDsv : occlusionRawVtableSlot(pDevice, 10);
	HRESULT hr = E_FAIL;
	if (safetyhook::InlineHook* hook = occlusionFindImplHook(impl, occlusionCreateDsvImplHooks))
	{
		hr = hook->call<HRESULT>(pDevice, pResource, pDesc, ppDepthStencilView);
	}
	else if (slots.createDsv != nullptr)
	{
		hr = reinterpret_cast<DX11CreateDepthStencilView*>(slots.createDsv)(pDevice, pResource, pDesc, ppDepthStencilView);
	}
	if (SUCCEEDED(hr) && pResource != nullptr)
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		if (SUCCEEDED(pResource->QueryInterface(IID_PPV_ARGS(&texture))) && texture != nullptr)
		{
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			if (desc.Width >= 64 && desc.Height >= 64 && occlusionProbeBytesPerPixel(desc.Format) >= 4)
			{
				std::lock_guard<std::mutex> lock(occlusionProbeMutex);
				if (occlusionProbeCreatedSet.insert(texture.Get()).second)
				{
					OcclusionProbeCandidate created;
					created.texture = texture.Get();
					created.keepAlive = texture;
					created.width = desc.Width;
					created.height = desc.Height;
					created.format = desc.Format;
					created.bytesPerPixel = occlusionProbeBytesPerPixel(desc.Format);
					created.lastSeenFrame = occlusionProbeFrame;
					occlusionProbeCreatedDepths.push_back(created);
					PLOG_INFO << "occlusion probe: created depth tex=0x" << std::hex << (uintptr_t)texture.Get() << std::dec
						<< " " << desc.Width << "x" << desc.Height << " fmt=" << (int)desc.Format
						<< " samples=" << desc.SampleDesc.Count;
				}
			}
		}
	}
	return hr;
}

HRESULT __stdcall D3D11Hook::newCreateTexture2D(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
	const OcclusionVtableSlots slots = occlusionVtableOriginals(pDevice);
	void* impl = slots.createTex != nullptr ? slots.createTex : occlusionRawVtableSlot(pDevice, 5);
	HRESULT hr = E_FAIL;
	if (safetyhook::InlineHook* hook = occlusionFindImplHook(impl, occlusionCreateTexImplHooks))
	{
		hr = hook->call<HRESULT>(pDevice, pDesc, pInitialData, ppTexture2D);
	}
	else if (slots.createTex != nullptr)
	{
		hr = reinterpret_cast<DX11CreateTexture2D*>(slots.createTex)(pDevice, pDesc, pInitialData, ppTexture2D);
	}
	if (SUCCEEDED(hr) && pDesc != nullptr && ppTexture2D != nullptr && *ppTexture2D != nullptr
		&& (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0
		&& pDesc->Width >= 64 && pDesc->Height >= 64
		&& occlusionProbeBytesPerPixel(pDesc->Format) >= 4)
	{
		std::lock_guard<std::mutex> lock(occlusionProbeMutex);
		if (occlusionProbeCreatedSet.insert(*ppTexture2D).second)
		{
			OcclusionProbeCandidate created;
			created.texture = *ppTexture2D;
			created.keepAlive = *ppTexture2D;
			created.width = pDesc->Width;
			created.height = pDesc->Height;
			created.format = pDesc->Format;
			created.bytesPerPixel = occlusionProbeBytesPerPixel(pDesc->Format);
			created.lastSeenFrame = occlusionProbeFrame;
			occlusionProbeCreatedDepths.push_back(created);
			PLOG_INFO << "occlusion probe: created depth-bind tex=0x" << std::hex << (uintptr_t)*ppTexture2D << std::dec
				<< " " << pDesc->Width << "x" << pDesc->Height << " fmt=" << (int)pDesc->Format
				<< " samples=" << pDesc->SampleDesc.Count;
		}
	}
	return hr;
}

HRESULT __stdcall D3D11Hook::newCreateDeferredContext(ID3D11Device* pDevice, UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext)
{
	const OcclusionVtableSlots slots = occlusionVtableOriginals(pDevice);
	void* impl = slots.createDeferred != nullptr ? slots.createDeferred : occlusionRawVtableSlot(pDevice, 27);
	HRESULT hr = E_FAIL;
	if (safetyhook::InlineHook* hook = occlusionFindImplHook(impl, occlusionCreateDeferredImplHooks))
	{
		hr = hook->call<HRESULT>(pDevice, ContextFlags, ppDeferredContext);
	}
	else if (slots.createDeferred != nullptr)
	{
		hr = reinterpret_cast<DX11CreateDeferredContext*>(slots.createDeferred)(pDevice, ContextFlags, ppDeferredContext);
	}
	if (SUCCEEDED(hr) && ppDeferredContext != nullptr && *ppDeferredContext != nullptr)
	{
		occlusionProbeHookContext(*ppDeferredContext);
	}
	return hr;
}

/**/


void D3D11Hook::CreateDummySwapchain(IDXGISwapChain*& pDummySwapchain, ID3D11Device*& pDummyDevice)
{

	std::vector<std::string> errorCodes;

#define logSwapChainFailure(x) PLOG_ERROR << x; errorCodes.push_back(x)

	D3D_FEATURE_LEVEL featLevel;
	DXGI_SWAP_CHAIN_DESC sd{ 0 };
	sd.BufferCount = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.Height = 800;
	sd.BufferDesc.Width = 600;
	sd.BufferDesc.RefreshRate = { 60, 1 };
	sd.OutputWindow = GetForegroundWindow();
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;



	// the following attempts at D3D11CreateDeviceAndSwapChain differ in the driver_type param
	// https://learn.microsoft.com/en-us/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_driver_type



	// use D3D_DRIVER_TYPE_HARDWARE
	// works for me
	{
		auto driverToTry = D3D_DRIVER_TYPE_HARDWARE;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}

	// use D3D_DRIVER_TYPE_WARP
	// Seems to be the most consistent
	{
		auto driverToTry = D3D_DRIVER_TYPE_WARP;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}

	// use D3D_DRIVER_TYPE_REFERENCE. The original code I used.
	// works for me but not gronchy
	{
		auto driverToTry = D3D_DRIVER_TYPE_REFERENCE;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}



	// use D3D_DRIVER_TYPE_SOFTWARE
	// does not work for me (E_INVALIDARG)
	{
		auto driverToTry = D3D_DRIVER_TYPE_SOFTWARE;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}


	// the next call seems to need us to set the adapter parameter ourselves instead of using null ptr

#define UseDXGI1 TRUE

#if UseDXGI1 == TRUE
#define IDXGIFactoryA IDXGIFactory1
#define CreateDXGIFactoryA CreateDXGIFactory1
#define IDXGIAdapterA IDXGIAdapter1
#define EnumAdaptersA EnumAdapters1
#else
#define IDXGIFactoryA IDXGIFactory
#define CreateDXGIFactoryA CreateDXGIFactory
#define IDXGIAdapterA IDXGIAdapter
#define EnumAdaptersA EnumAdapters
#endif

	IDXGIFactoryA* pFactory;
	HRESULT fhr = CreateDXGIFactoryA(__uuidof(IDXGIFactoryA), (void**)(&pFactory));

	std::vector <IDXGIAdapterA*> vAdapters;
	if (fhr != S_OK)
	{
		PLOG_ERROR << "Failed to create DXGIFactory: " << std::hex << (ULONG)fhr;
	}
	else
	{
		UINT i = 0;
		IDXGIAdapterA* pAdapter;
		while (pFactory->EnumAdaptersA(i, &pAdapter) != DXGI_ERROR_NOT_FOUND)
		{
			vAdapters.push_back(pAdapter);
			++i;
		}
		PLOG_DEBUG << "default adapter: " << std::hex << vAdapters.front();
		PLOG_DEBUG << "adapter count: " << vAdapters.size();

	}

	// use D3D_DRIVER_TYPE_UNKNOWN
	// works if I pass it the adapter manually
	// tries each adapter
	PLOG_DEBUG << "Attempting to create dummy swapchain with driver type " << driverToString.at(D3D_DRIVER_TYPE_UNKNOWN);
	for (auto adapter : vAdapters)
	{
		PLOG_INFO << "Attemtping D3D11CreateDeviceAndSwapChain with adapter: " << std::hex << adapter;

		HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with " << driverToString.at(D3D_DRIVER_TYPE_UNKNOWN);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("4: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(D3D_DRIVER_TYPE_UNKNOWN), (ULONG)hr));
		}

	}



	// If execution got here, none of the createdevice calls succeeded. Throw an exception and log the error codes

	std::string resultsString;
	for (auto error : errorCodes)
	{
		resultsString += error + "\n";
	}
	throw HCMInitException(resultsString);


}


D3D11Hook::D3D11Hook(std::weak_ptr<PointerDataStore> pointerDataStore)
	: pointerDataStoreWeak(pointerDataStore)
{

	if (instance != nullptr)
	{
		throw HCMInitException("Cannot have more than one D3D11Hook");
	}
	instance = this;



#ifdef HCM_DEBUG
	// get pointers to present/resizeBuffers using a dummy swapchain.
	// then log em to the console so burnt can use ida/cheatengine to find the idxgi vmt 

	IDXGISwapChain* pDummySwapchain; 
	ID3D11Device* pDummyDevice;
	CreateDummySwapchain(pDummySwapchain, pDummyDevice);

	if (pDummySwapchain && pDummyDevice)
	{
		uintptr_t* dxgi_vmt = (uintptr_t*)::calloc(119, sizeof(uintptr_t));
		::memcpy(dxgi_vmt, *(uintptr_t**)pDummyDevice, magic_enum::enum_count< IDXGISwapChainVMT>() * sizeof(uintptr_t));

		PLOG_DEBUG << "DX11::Present @ 0x" << std::hex << dxgi_vmt[(int)IDXGISwapChainVMT::Present];
		PLOG_DEBUG << "DX11::ResizeBuffers @ 0x" << std::hex << dxgi_vmt[(int)IDXGISwapChainVMT::ResizeBuffers];
	}

	safe_release(pDummyDevice);
	safe_release(pDummySwapchain);

#endif


}





void D3D11Hook::initializeD3Ddevice(IDXGISwapChain* pSwapChain)
{

	PLOG_DEBUG << "Initializing D3Ddevice" << std::endl;

	if (!SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&m_pDevice)))
	{
		throw HCMInitException(std::format("Failed to get D3D11Device, pSwapChain: {:x}", (uint64_t)pSwapChain).c_str());

	}

	// Get Device Context
	m_pDevice->GetImmediateContext(&m_pDeviceContext);
	if (!m_pDeviceContext)
	{
		throw HCMInitException("Failed to get DeviceContext");
	}



	// Use backBuffer to get MainRenderTargetView
	ID3D11Texture2D* pBackBuffer;
	pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
	if (!pBackBuffer) throw HCMInitException("Failed to get BackBuffer");

	// Store the windows size
	D3D11_TEXTURE2D_DESC desc;
	pBackBuffer->GetDesc(&desc);
	PLOG_INFO << "Initializing screen size: " << desc.Width << ", " << desc.Height;
	mScreenSize = { ((float)desc.Width), ((float)desc.Height) };
	mScreenCenter = mScreenSize / 2;

#ifdef HCM_DEBUG
	DXGI_SWAP_CHAIN_DESC sd;
	pSwapChain->GetDesc(&sd);

	PLOG_DEBUG << "swapchain image format: " << sd.BufferDesc.Format;
	// result: DXGI_FORMAT_R8G8B8A8_UNORM
#endif

	PLOG_INFO << "Creating render target view";
	auto CreateRenderTargetViewResult = m_pDevice->CreateRenderTargetView(pBackBuffer, NULL, &m_pMainRenderTargetView);
	PLOG_DEBUG << "CreateRenderTargetViewResult: " << CreateRenderTargetViewResult;
	pBackBuffer->Release();
	if (CreateRenderTargetViewResult || !m_pMainRenderTargetView) throw HCMInitException(std::format("Failed to get MainRenderTargetView, error code: {}", CreateRenderTargetViewResult));

	/**/
	occlusionProbeDevice = m_pDevice;
	occlusionProbeContext = m_pDeviceContext;
	occlusionProbeHookDevice(m_pDevice);
	PLOG_INFO << "occlusion probe: context and device hooks installed";
	/**/




}

// static 
HRESULT D3D11Hook::newDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{


	ScopedAtomicBool lock(presentHookRunning);
	//std::unique_lock<std::mutex> lock(mDestructionGuard); // Protects against D3D11Hook singleton destruction while hooks are executing
	LOG_ONCE(PLOG_DEBUG << "D3D11Hook::newDX11Present");
	auto d3d = instance;
	if (!d3d)
	{
		PLOG_FATAL << "d3d instance was null at newDX11Present";
		return 0;
	}

	//auto guard = d3d->shared_from_this();




	if (!d3d->isD3DdeviceInitialized)
	{
		PLOG_DEBUG << "Initializing d3d device @ present";
		try
		{
			d3d->initializeD3Ddevice(pSwapChain);
			d3d->isD3DdeviceInitialized = true;
			PLOG_DEBUG << "D3D device initialized t. newDX11Present";
			// fire a resizeborders event
			d3d->resizeBuffersHookEvent->operator()(mScreenSize);
			PLOG_DEBUG << "resizeBuffersHookEvent fired";
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize d3d device, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();

			// Call original present
			return d3d->presentHook.stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);


		}

	}
	/**/occlusionProbeSnapshot();/**/
	LOG_ONCE(PLOG_VERBOSE << "invoking mandatoryPresentHookEvent callback");
	// Invoke the callback


	if (!d3d->OBSPresentHook || d3d->OBSPresentHook->isHookInstalled() == false || Renderer2D::good == false)
	{


		LOG_ONCE(PLOG_VERBOSE << "invoking mainPresentHookEvent callback via vmt");
		d3d->presentHookEvent->operator()(d3d->m_pDevice, d3d->m_pDeviceContext, pSwapChain, d3d->m_pMainRenderTargetView);
		Lapua::shouldRunForBypass = false;

		if (Lapua::shouldRunForWatermark)
		{
			if (Renderer2D::good == false)
			{
				Lapua::lapuaGood = false;
			}
			else
			{
				Renderer2D::render(d3d->m_pDevice, d3d->m_pDeviceContext, mScreenSize, d3d->m_pMainRenderTargetView);
			}
		}

	}
	else
	{
		Lapua::shouldRunForBypass = true;
		Renderer2D::render(d3d->m_pDevice, d3d->m_pDeviceContext, mScreenSize, d3d->m_pMainRenderTargetView);
	}



	LOG_ONCE(PLOG_VERBOSE << "calling original present function");
	// Call original present

	auto hr = d3d->presentHook.stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);

	return hr;

}

HRESULT D3D11Hook::newDX11PresentOBSBypass(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	LOG_ONCE(PLOGV << "newDX11PresentOBSBypass");

	// DON'T LOCK! already locked by present!
	//ScopedAtomicBool lock(presentHookRunning);
	auto d3d = instance;
	if (!d3d)
	{
		PLOG_FATAL << "d3d instance was null at newDX11PresentOBSBypass";
		return 0;
	}

	/**/occlusionProbeSnapshot();/**/
	LOG_ONCE(PLOG_VERBOSE << "invoking mainPresentHookEvent callback via inline obs bypass");
	d3d->presentHookEvent->operator()(d3d->m_pDevice, d3d->m_pDeviceContext, pSwapChain, d3d->m_pMainRenderTargetView);
	auto hr = d3d->OBSPresentHook->getInlineHook().stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);

	return hr;
}

// static
HRESULT D3D11Hook::newDX11ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	ScopedAtomicBool lock(presentHookRunning);
	//std::unique_lock<std::mutex> lock(mDestructionGuard); // Protects against D3D11Hook singleton destruction while hooks are executing
	auto d3d = instance;
	if (!d3d)
	{
		PLOG_FATAL << "d3d instance was null at newDX11ResizeBuffers";
	}

	//auto guard = d3d->shared_from_this();

	if (!d3d->isD3DdeviceInitialized)
	{
		PLOG_DEBUG << "Initializing d3d device @ resizeBuffers";
		try
		{
			d3d->initializeD3Ddevice(pSwapChain);
			d3d->isD3DdeviceInitialized = true;
			PLOG_DEBUG << "D3D device initialized t. newDX11ResizeBuffers";
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize d3d device, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();

			// Call original resize buffers
			return d3d->resizebuffersHook.stdcall<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);


		}

	}



	// Need to release mainRenderTargetView before calling ResizeBuffers
	if (d3d->m_pMainRenderTargetView != nullptr)
	{
		d3d->m_pDeviceContext->OMSetRenderTargets(0, 0, 0);
		d3d->m_pMainRenderTargetView->Release();
	}

	// Call original ResizeBuffers
	HRESULT hr = d3d->resizebuffersHook.stdcall<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);


	// Resetup the mainRenderTargetView
	ID3D11Texture2D* pBackBuffer;
	pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
	if (!pBackBuffer) throw HCMInitException("Failed to get BackBuffer");

	d3d->m_pDevice->CreateRenderTargetView(pBackBuffer, NULL, &d3d->m_pMainRenderTargetView);
	pBackBuffer->Release();
	if (!d3d->m_pMainRenderTargetView) throw HCMInitException("Failed to get MainRenderTargetView");

	// Grab the new windows size
	mScreenSize = { (float)Width, (float)Height };
	mScreenCenter = mScreenSize / 2;

	// fire screen resize event
	d3d->resizeBuffersHookEvent->operator()(mScreenSize);

	return hr;
}

// Safely destroys hooks
// Releases D3D resources, if we acquired them
D3D11Hook::~D3D11Hook()
{

	if (presentHookRunning)
	{
		PLOG_INFO << "Waiting for presentHook to finish execution";
		presentHookRunning.wait(true);
	}

	DXGIHook = {};
	OBSPresentHook = {};

	instance = nullptr;

	// D3D resource releasing:
	// need to call release on the device https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3dswapchain9-getdevice

	safe_release(m_pDevice);
	safe_release(m_pDeviceContext);
	safe_release(m_pMainRenderTargetView);



}


void D3D11Hook::beginHook()
{
	PLOG_DEBUG << "beginning vmt hook";


	// set up d3d11 hook. This requires us to get the vmt of Present and ResizeBuffers functions. 

	try
	{

		lockOrThrow(pointerDataStoreWeak, ptr);

		uintptr_t pIDXGISwapChain;

		auto mlp_IDXGISwapChain = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(pIDXGISwapChain));
		if (!mlp_IDXGISwapChain->resolve(&pIDXGISwapChain)) throw HCMInitException(std::format("Could not resolve pIDXGISwapChain: {}-\nThis probably means this version of MCC is not supported by HCM", MultilevelPointer::GetLastError()));

		PLOG_INFO << "Successfully resolved multilevelpointer to IDXGISwapChain virtual method table!";
		PLOG_DEBUG << "pIDXGISwapChain: " << pIDXGISwapChain;

		DXGIHook = safetyhook::create_vmt((void*)pIDXGISwapChain);
		presentHook = safetyhook::create_vm(DXGIHook, (size_t)IDXGISwapChainVMT::Present, &newDX11Present);
		resizebuffersHook = safetyhook::create_vm(DXGIHook, (size_t)IDXGISwapChainVMT::ResizeBuffers, &newDX11ResizeBuffers);

		PLOG_DEBUG << "hooks set";

	}
	catch (HCMRuntimeException ex)
	{
		throw HCMInitException(ex.what());
	}




}



void D3D11Hook::setOBSBypass(bool enabled)
{
	PLOGV << "D3D11Hook::setOBSBypass called with value: " << enabled;
	try
	{
		if (enabled)
		{
			if (presentHookRunning)
			{
				PLOG_INFO << "Waiting for presentHook to finish execution A";
				presentHookRunning.wait(true);
			}


			lockOrThrow(pointerDataStoreWeak, ptr);
			auto dxgiInternalPresentFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(dxgiInternalPresentFunction));

			// starts attached
			OBSPresentHook = ModuleInlineHook::make(L"graphics-hook64.dll", dxgiInternalPresentFunction, newDX11PresentOBSBypass, true);
		}
		else
		{
			if (!OBSPresentHook) return; // don't need to reset if it's already reset

			if (presentHookRunning)
			{
				PLOG_INFO << "Waiting for presentHook to finish execution B";
				presentHookRunning.wait(true);
			}


			OBSPresentHook.reset();
		}

	}
	catch (HCMInitException ex)
	{
		throw HCMRuntimeException(ex);
	}
}