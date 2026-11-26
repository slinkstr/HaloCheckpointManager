#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace DepthCapture
{
	void onPresentBegin(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* immediateContext);
	void onPresentEnd();

	struct FrameDepth
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> fullResolution;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> lastPreClear;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> endOfFrame;
	};

	FrameDepth lastFrameDepth(DXGI_FORMAT format);
	void watchPreClear(DXGI_FORMAT format);
}
