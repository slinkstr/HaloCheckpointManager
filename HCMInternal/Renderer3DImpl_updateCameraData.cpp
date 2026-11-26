#include "pch.h"
#include "Renderer3DImpl.h"
#include "DepthCapture.h"
#include "directxtk\SimpleMath.h"


// This function is only used as a fallback if we don't have the games actual vertical FOV pointer
template<GameState::Value mGame>
float horizontalFOVToVerticalFOV(float horizontalFOVMeasured, float screenWidth, float screenHeight)
{
	// https://docs.google.com/document/d/1QXaMTPOpLmJP_YWYV_U3g1eMG50cd595fd0_-B3_Ebk/edit
	if constexpr (mGame == GameState::Value::Halo1)
	{
		// very close to perfect. this is essentially H16ML9 from above documentation
		float verticalFOVIn169 = 2 * std::atan(std::tan(horizontalFOVMeasured / 2) * (1080 / 1920.f));
		float actualHorizontal = 2 * std::atan(std::tan(verticalFOVIn169 / 2) * (screenWidth / screenHeight));

		return 2 * std::atan(std::tan(actualHorizontal / 2) * (screenHeight / screenWidth));
	}
	else if constexpr (mGame == GameState::Value::Halo2)
	{
		// fairly close to perfect
		return  2 * std::atan(std::tan(horizontalFOVMeasured / 2) * (screenHeight / screenWidth));
	}
	else
	{
		return  2 * std::atan(std::tan(horizontalFOVMeasured / 2) * (screenHeight / screenWidth));
	}
}



template<GameState::Value mGame>
bool Renderer3DImpl<mGame>::updateCameraData(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, SimpleMath::Vector2 screenSizeIn, ID3D11RenderTargetView* pMainRenderTargetView)
{
	// TODO: currently having some safety issues - specifically if the user quits to main menu in the middle of this function being called, 
	// various bad things will happen (mostly bad ptr reads but that means a crash). Not sure what the best way to guard against this is. 
	// Problem is that the order of MCCStateChanged func calls are random as far as we're concerned, 
	// so RenderImpl might recieve it either BEFORE or AFTER a consumer (ie Waypoint3D) recieves it. 

	try
	{
		lockOrThrow(getGameCameraDataWeak, getGameCameraData);

		auto cameraData = getGameCameraData->getGameCameraData();
		// importantly we quickly copy off in case of game quit-out
		this->cameraPosition = *cameraData.position;
		this->cameraDirection = *cameraData.lookDirForward;
		this->cameraUp = *cameraData.lookDirUp;

		// The anniversary renderers present each frame drawn with the camera from the previous game update,
		// so the overlay has to use that camera too or it runs ahead of the world during camera motion.
		const CameraPose currentCameraPose { this->cameraPosition, this->cameraDirection, this->cameraUp };
		if (this->previousCameraPose.has_value() && anniversaryGraphicsActive())
		{
			this->cameraPosition  = this->previousCameraPose->position;
			this->cameraDirection = this->previousCameraPose->direction;
			this->cameraUp        = this->previousCameraPose->up;
		}
		this->previousCameraPose = currentCameraPose;


		float aspectRatio = screenSizeIn.x / screenSizeIn.y; // aspect ratio is width div height!

		float verticalFov;
		if (this->pVerticalFOVCached != nullptr)
		{
			if (IsBadReadPtr(this->pVerticalFOVCached, 4))
				throw HCMRuntimeException(std::format("Bad pVerticalFOVCached read at: {}", (uintptr_t)this->pVerticalFOVCached));
			verticalFov = *this->pVerticalFOVCached;
		}
		else
		{
			verticalFov = horizontalFOVToVerticalFOV<mGame>(*cameraData.FOV, screenSizeIn.x, screenSizeIn.y);
		}
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "pVerticalFOVCached: " << cached, cached = (uintptr_t)this->pVerticalFOVCached);

		if (XMScalarNearEqual(verticalFov, 0.0f, 0.00001f * 2.0f))
			throw HCMRuntimeException(std::format("Bad vertical FOV value: {}", verticalFov));



		this->projectionMatrix = DirectX::SimpleMath::Matrix::CreatePerspectiveFieldOfView(verticalFov, aspectRatio, 0.001f, renderDistance);
		auto lookAt = this->cameraPosition + this->cameraDirection;
		this->viewMatrix = DirectX::SimpleMath::Matrix::CreateLookAt(this->cameraPosition, lookAt, this->cameraUp);
		this->viewProjectionMatrix = (viewMatrix * projectionMatrix);

		this->occlusionSource = selectOcclusionSource();
		if (this->occlusionSource)
		{
			const float occlusionYScale = 1.f / std::tan(verticalFov * 0.5f);
			const float occlusionXScale = occlusionYScale / aspectRatio;
			const DirectX::SimpleMath::Matrix occlusionProjectionMatrix(
				occlusionXScale, 0.f,             0.f,                               0.f,
				0.f,             occlusionYScale, 0.f,                               0.f,
				0.f,             0.f,             -this->occlusionSource->slope,     -1.f,
				0.f,             0.f,             this->occlusionSource->intercept,  0.f);
			this->occlusionViewProjectionMatrix = this->viewMatrix * occlusionProjectionMatrix;
		}
		this->pDevice = pDevice;
		this->pDeviceContext = pDeviceContext;
		this->pMainRenderTargetView = pMainRenderTargetView;
		this->screenSize = screenSizeIn;
		this->screenCenter = { screenSize.x / 2.f, screenSize.y / 2.f };


		DirectX::BoundingFrustum::CreateFromMatrix(this->frustumViewWorld, this->projectionMatrix, true); // frustrum in view space
		this->frustumViewWorld.Transform(this->frustumViewWorld, this->viewMatrix.Invert()); // transform to world space


		if (this->init == false)
		{
			initialise();
			this->init = true;
		}

		if (lastScreenSize != screenSizeIn)
		{
			createDepthStencilView(screenSizeIn);
		}






		this->pDeviceContext->OMSetBlendState(commonStates->AlphaBlend(), Colors::White, 0xFFFFFFFF);


		// so now we have "working" depth.. but only for non-transparent textures
		// I think we'd need to create a custom shader to fix this? maybe not
		this->pDeviceContext->OMSetRenderTargets(1, &pMainRenderTargetView, this->m_depthStencilView.Get());
		this->pDeviceContext->ClearDepthStencilView(this->m_depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
		this->pDeviceContext->OMSetDepthStencilState(this->m_depthStencilState.Get(), 0);

		//this->pDeviceContext->OMSetBlendState(this->commonStates->NonPremultiplied(), nullptr, 0xFFFFFFFF);
		//this->pDeviceContext->RSSetState(this->commonStates->CullNone());



		ID3D11SamplerState* samplerState = this->commonStates->LinearWrap();
		this->pDeviceContext->PSSetSamplers(0, 1, &samplerState);






		if (this->occlusionSource && !refreshOcclusionDepth())
		{
			this->occlusionSource.reset();
		}
		this->primitiveBatchEffect->SetProjection(this->occlusionSource ? this->occlusionViewProjectionMatrix : this->viewProjectionMatrix);
		this->primitiveBatchEffect->Apply(this->pDeviceContext);


		// TODO: make this created once (per resizeBuffers) in d3dhook and passed along as render args
		// (should really use dxtk resources type)
		// since making this every time is.. actually not that expensive nvm
		D3D11_VIEWPORT g_stViewport;

		g_stViewport.TopLeftX = 0;
		g_stViewport.TopLeftY = 0;
		g_stViewport.Width = screenSize.x;
		g_stViewport.Height = screenSize.y;
		g_stViewport.MinDepth = 0.0f;
		g_stViewport.MaxDepth = 1.0f;


		pDeviceContext->RSSetViewports(1, &g_stViewport);


		lastScreenSize = screenSizeIn;
		return true;
	}
	catch (HCMRuntimeException ex)
	{
		if (this->haveShownError == false)
		{
			this->haveShownError = true;
			ex.append("\nSuppressing further errors");
			runtimeExceptions->handleMessage(ex);
		}
		else
		{
			runtimeExceptions->handleSilent(ex);
		}

		return false;
	}
}


namespace
{
	struct DepthPlanes
	{
		float nearPlane;
		float farPlane;
		float engineUnitsPerGameUnit;

		OcclusionSource sourceFrom(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, Microsoft::WRL::ComPtr<ID3D11Texture2D> nearestOf = nullptr) const
		{
			return OcclusionSource { std::move(texture), std::move(nearestOf), -nearPlane / farPlane, nearPlane / engineUnitsPerGameUnit };
		}
	};

	// Reversed-Z: stored depth = (near / viewDepth) - (near / far), viewDepth in game units.
	// The anniversary renderers work in meters, 3.048 m per game unit.
	// Sources are the game DLLs as shipped (MCC 1.3528): initialized globals (near, far adjacent) or the immediates of the
	// instruction pair that writes near and far into the camera.
	// Halo 2 anniversary also has a 10000 m far plane in its camera at halo2.dll+0xe427e8, but the depth buffer fits 750000 m.
	constexpr float kMetersPerGameUnit = 3.048f;
	constexpr DepthPlanes kHalo1ClassicPlanes     { 0.0078125f, 1024.f,   1.f                }; // halo1.dll .rdata +0x188b120
	constexpr DepthPlanes kHalo1AnniversaryPlanes { 0.025f,     10000.f,  kMetersPerGameUnit }; // halo1.dll .text +0x45312c (mov [rbx], 0.025f; mov [rbx+4], 10000.f)
	constexpr DepthPlanes kHalo2ClassicPlanes     { 0.06f,      1024.f,   1.f                }; // halo2.dll .data +0xe19200
	constexpr DepthPlanes kHalo2AnniversaryPlanes { 0.1f,       750000.f, kMetersPerGameUnit }; // halo2.dll .text +0x2ddd01 (mov [rcx+8], 0.1f; mov [rcx+0xc], 750000.f)
	constexpr DepthPlanes kHalo3Planes            { 0.0078125f, 10240.f,  1.f                }; // halo3.dll .data +0x8ac414 (far, near), halo3odst.dll .data +0x8f02b0 (far, near), haloreach.dll .data +0xb43a88, halo4.dll .data +0xe84600
}

// Halo 2 classic clears its depth buffer between the world pass and the viewmodel pass, so the world is only
// available as a pre-clear snapshot; the viewmodel is merged back in from the end-of-frame contents.
template<GameState::Value mGame>
std::optional<OcclusionSource> Renderer3DImpl<mGame>::selectOcclusionSource()
{
	const bool anniversary = anniversaryGraphicsActive();
	DepthCapture::watchPreClear(mGame == GameState::Value::Halo2 && !anniversary ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
	if constexpr (mGame == GameState::Value::Halo1)
	{
		auto depth = DepthCapture::lastFrameDepth(DXGI_FORMAT_R32G8X24_TYPELESS);
		if (depth.fullResolution == nullptr)
		{
			return std::nullopt;
		}
		const DepthPlanes& planes = anniversary ? kHalo1AnniversaryPlanes : kHalo1ClassicPlanes;
		return planes.sourceFrom(std::move(depth.fullResolution));
	}
	else if constexpr (mGame == GameState::Value::Halo2)
	{
		if (anniversary)
		{
			auto depth = DepthCapture::lastFrameDepth(DXGI_FORMAT_R32G8X24_TYPELESS);
			if (depth.fullResolution == nullptr)
			{
				return std::nullopt;
			}
			return kHalo2AnniversaryPlanes.sourceFrom(std::move(depth.fullResolution));
		}
		auto classic = DepthCapture::lastFrameDepth(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
		if (classic.lastPreClear == nullptr || classic.endOfFrame == nullptr)
		{
			return std::nullopt;
		}
		return kHalo2ClassicPlanes.sourceFrom(std::move(classic.lastPreClear), std::move(classic.endOfFrame));
	}
	else if constexpr (mGame == GameState::Value::HaloReach || mGame == GameState::Value::Halo3 || mGame == GameState::Value::Halo3ODST || mGame == GameState::Value::Halo4)
	{
		auto depth = DepthCapture::lastFrameDepth(DXGI_FORMAT_R32G8X24_TYPELESS);
		if (depth.fullResolution == nullptr)
		{
			return std::nullopt;
		}
		return kHalo3Planes.sourceFrom(std::move(depth.fullResolution));
	}
	else
	{
		return std::nullopt;
	}
}

template<GameState::Value mGame>
bool Renderer3DImpl<mGame>::anniversaryGraphicsActive()
{
	if (this->pAnniversaryGraphicsCached == nullptr)
	{
		return false;
	}
	if (IsBadReadPtr(this->pAnniversaryGraphicsCached, sizeof(int32_t)))
	{
		throw HCMRuntimeException(std::format("Bad pAnniversaryGraphicsCached read at: {}", (uintptr_t)this->pAnniversaryGraphicsCached));
	}
	return *this->pAnniversaryGraphicsCached != 0;
}

// explicit template instantiation
template class Renderer3DImpl<GameState::Value::Halo1>;
template class Renderer3DImpl<GameState::Value::Halo2>;
template class Renderer3DImpl<GameState::Value::Halo3>;
template class Renderer3DImpl<GameState::Value::Halo3ODST>;
template class Renderer3DImpl<GameState::Value::HaloReach>;
template class Renderer3DImpl<GameState::Value::Halo4>;