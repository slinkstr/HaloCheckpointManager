#pragma once
#include "ModuleHook.h"
#include "ModuleHookManager.h"
#include "PointerDataStore.h"

// imgui
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_stdlib.h"
// directx 11
#include <d3d11.h>
#include <unordered_set>

// TODO: move shit to impl so we can hind all the headers

// Define the DX functions we're going to hook
typedef HRESULT __stdcall DX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT __stdcall DX11ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

/**/
typedef void __stdcall DX11OMSetRenderTargets(ID3D11DeviceContext* pContext, UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView);
/**/typedef void __stdcall DX11OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* pContext, UINT NumRTVs, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUAVs, const UINT* pUAVInitialCounts);
/**/typedef HRESULT __stdcall DX11CreateDeferredContext(ID3D11Device* pDevice, UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext);
/**/typedef HRESULT __stdcall DX11CreateDepthStencilView(ID3D11Device* pDevice, ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView);
/**/typedef HRESULT __stdcall DX11CreateTexture2D(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D);
/**/typedef HRESULT (WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);
/**/typedef HRESULT (WINAPI* D3D11CreateDeviceAndSwapChainFn)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

//SCALAR TO ImVec4 OPERATIONS
inline ImVec4 operator + (const ImVec4& v1, float s) { return ImVec4(v1.x + s, v1.y + s, v1.z + s, v1.w + s); }
inline ImVec4 operator - (const ImVec4& v1, float s) { return ImVec4(v1.x - s, v1.y - s, v1.z - s, v1.w - s); }
inline ImVec4 operator * (const ImVec4& v1, float s) { return ImVec4(v1.x * s, v1.y * s, v1.z * s, v1.w * s); }
inline ImVec4 operator / (const ImVec4& v1, float s) { return ImVec4(v1.x / s, v1.y / s, v1.z / s, v1.w / s); }


//ImVec2 TO ImVec4 OPERATIONS
inline ImVec4 operator + (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x + v2.x, v1.y + v2.y, v1.z + v2.z, v1.w + v2.w); }
inline ImVec4 operator - (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x - v2.x, v1.y - v2.y, v1.z - v2.z, v1.w - v2.w); }
inline ImVec4 operator * (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x * v2.x, v1.y * v2.y, v1.z * v2.z, v1.w * v2.w); }
inline ImVec4 operator / (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x / v2.x, v1.y / v2.y, v1.z / v2.z, v1.w / v2.w); }


// Singleton: on construction, attaches hooks (VMT hook so our stuff shows up in OBS etc) to the games Present and ResizeBuffers function.
// The hooks will invoke presentHookEvent for other classes to listen to.
// On destruction, hooks will be unattached.


class D3D11Hook : public std::enable_shared_from_this<D3D11Hook>
{
private:
	static D3D11Hook* instance; 	// Private Singleton instance so static hooks/callbacks can access
	//static inline std::mutex mDestructionGuard{}; // Protects against Singleton destruction while hooks are executing
	static inline std::atomic_bool presentHookRunning = false;
	// Our hook functions
	static DX11Present newDX11Present;
	static DX11ResizeBuffers newDX11ResizeBuffers;

	static DX11Present newDX11PresentOBSBypass;
	static DX11Present mandatoryManualRender;

	/**/static void occlusionProbeRecordDSV(ID3D11DepthStencilView* dsv);
	/**/static void occlusionProbeHookContext(void* contextObject);

	std::weak_ptr<PointerDataStore> pointerDataStoreWeak; // not required but used to attempt to resolve vmt entries without needing a dummy swapchain

	safetyhook::VmtHook DXGIHook;
	safetyhook::VmHook presentHook; //  present hook
	safetyhook::VmHook resizebuffersHook; // resizeBuffers hook
	std::shared_ptr<ModuleInlineHook> OBSPresentHook; // obs hook for obs bypass


	// D3D data
	ID3D11Device* m_pDevice = nullptr;
	ID3D11DeviceContext* m_pDeviceContext = nullptr;
	ID3D11RenderTargetView* m_pMainRenderTargetView = nullptr;

	void CreateDummySwapchain(IDXGISwapChain*& pDummySwapchain, ID3D11Device*& pDummyDevice);
	void initializeD3Ddevice(IDXGISwapChain*);
	bool isD3DdeviceInitialized = false;

	static SimpleMath::Vector2 mScreenSize;
	static SimpleMath::Vector2 mScreenCenter;

public:

	explicit D3D11Hook(std::weak_ptr<PointerDataStore> pointerDataStore);
	~D3D11Hook();


	// Callback for present rendering
	std::shared_ptr<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>> presentHookEvent = std::make_shared<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>>();



	// Callback for window resize
	std::shared_ptr<eventpp::CallbackList<void(SimpleMath::Vector2 newScreenSize)>> resizeBuffersHookEvent = std::make_shared<eventpp::CallbackList<void(SimpleMath::Vector2 newScreenSize)>>();

	// Banned operations for singleton
	D3D11Hook(const D3D11Hook& arg) = delete; // Copy constructor
	D3D11Hook(const D3D11Hook&& arg) = delete;  // Move constructor
	D3D11Hook& operator=(const D3D11Hook& arg) = delete; // Assignment operator
	D3D11Hook& operator=(const D3D11Hook&& arg) = delete; // Move operator

	SimpleMath::Vector2 getScreenSize() { return mScreenSize; }
	SimpleMath::Vector2 getScreenCenter() { return mScreenCenter; }

	void beginHook();

	void setOBSBypass(bool enabled);

	/**/
	static void occlusionProbeSnapshot();
	/**/static void occlusionProbeIgnoreTexture(ID3D11Texture2D* texture);
	/**/static ID3D11Texture2D* occlusionProbeSelectDepthTex();
	/**/static uint64_t occlusionProbeFrameCount();
	/**/static DXGI_FORMAT occlusionProbeDepthFormat();
	/**/static bool occlusionProbeSampleDepth(float u, float v, float& outDepth);
	/**/static void occlusionProbeHookDevice(ID3D11Device* device);
	/**/static void installOcclusionExportHooks();

	/**/
	static DX11OMSetRenderTargets newOMSetRenderTargets;
	/**/static DX11OMSetRenderTargetsAndUAVs newOMSetRenderTargetsAndUAVs;
	/**/static DX11CreateDeferredContext newCreateDeferredContext;
	/**/static DX11CreateDepthStencilView newCreateDepthStencilView;
	/**/static DX11CreateTexture2D newCreateTexture2D;
	/**/static safetyhook::InlineHook occlusionCreateDeviceExportHook;
	/**/static safetyhook::InlineHook occlusionCreateDeviceSwapChainExportHook;
};
