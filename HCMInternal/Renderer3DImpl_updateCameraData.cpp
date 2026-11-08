#include "pch.h"
#include "Renderer3DImpl.h"
#include "D3D11Hook.h"
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
		if (this->cameraDirection.LengthSquared() > 1e-8f)
		{
			this->cameraForwardNormalized = this->cameraDirection / this->cameraDirection.Length();
		}


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

		/**/
		// occlusion probe: drain one queued depth sample every other frame
		if ((D3D11Hook::occlusionProbeFrameCount() & 1) == 0 && !this->occlusionPendingSamples.empty())
		{
			const auto pending = this->occlusionPendingSamples.front();
			this->occlusionPendingSamples.pop_front();

			this->occlusionSampleRejectReason = 0;
			if (this->pointBehindCamera(pending.worldPosition))
			{
				this->occlusionSampleRejectReason = 1;
			}
			else
			{
				const auto screenPos = this->worldPointToScreenPosition(pending.worldPosition, false);
				const float viewDepth = (pending.worldPosition - this->cameraPosition).Dot(this->cameraForwardNormalized);
				float storedDepth = 0.f;
				auto* sourceTexture = D3D11Hook::occlusionProbeSelectDepthTex();
				if (screenPos.x < 0.f || screenPos.x > screenSizeIn.x
					|| screenPos.y < 0.f || screenPos.y > screenSizeIn.y)
				{
					this->occlusionSampleRejectReason = 2;
				}
				else if (viewDepth <= 0.05f || viewDepth >= 5000.f)
				{
					this->occlusionSampleRejectReason = 3;
				}
				else if (!D3D11Hook::occlusionProbeSampleDepth(screenPos.x / screenSizeIn.x, screenPos.y / screenSizeIn.y, storedDepth))
				{
					this->occlusionSampleRejectReason = 4;
				}
				else if (storedDepth <= 0.f || storedDepth >= 1.f)
				{
					this->occlusionSampleRejectReason = 5;
				}
				else
				{
					this->occlusionDepthSamples.push_back({ viewDepth, storedDepth, screenPos.x / screenSizeIn.x, screenPos.y / screenSizeIn.y, sourceTexture });
					if (this->occlusionDepthSamples.size() > 300)
					{
						this->occlusionDepthSamples.pop_front();
					}
				}
			}
		}
		if ((D3D11Hook::occlusionProbeFrameCount() % 120) == 0)
		{
			this->occlusionUpdateCalibration();
		}

		// occlusion probe: projection calibrated so ndc depth ~= stored game depth (stored = A/d + B)
		const float occlusionYScale = 1.f / std::tan(verticalFov * 0.5f);
		const float occlusionXScale = occlusionYScale / aspectRatio;
		this->occlusionProjectionMatrix = DirectX::SimpleMath::Matrix(
			occlusionXScale, 0.f,                     0.f,            0.f,
			0.f,            occlusionYScale,         0.f,            0.f,
			0.f,            0.f,                    -this->occlusionCalibSlope, -1.f,
			0.f,            0.f,                     this->occlusionCalibIntercept, 0.f);
		this->occlusionViewProjectionMatrix = this->viewMatrix * this->occlusionProjectionMatrix;
		/**/
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






		this->primitiveBatchEffect->SetProjection(this->occlusionViewProjectionMatrix);
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

/**/
template<GameState::Value mGame>
void Renderer3DImpl<mGame>::occlusionFeedDepthSample(const TriggerModel& model)
{
	const auto front = model.getFrontSurfacePoint(this->cameraForwardNormalized);
	if (this->occlusionPendingSamples.size() < 12)
	{
		this->occlusionPendingSamples.push_back({ front });
		return;
	}
	// reservoir-style replacement so the queue samples all on-screen triggers, not just the first ones in iteration order
	this->occlusionFeedRng = this->occlusionFeedRng * 1664525u + 1013904223u;
	this->occlusionPendingSamples[this->occlusionFeedRng % this->occlusionPendingSamples.size()] = { front };
}

template<GameState::Value mGame>
void Renderer3DImpl<mGame>::occlusionUpdateCalibration()
{
	/**/if ((D3D11Hook::occlusionProbeFrameCount() % 600) == 0)
	{
		for (const auto& sample : this->occlusionDepthSamples)
		{
			PLOG_INFO << "occlusion probe: raw u=" << sample.screenU << " v=" << sample.screenV << " z=" << sample.viewDepth << " stored=" << sample.storedDepth;
		}
	}
	constexpr auto occlusionParams = occlusionDepthParams();
	this->occlusionCalibIntercept = occlusionParams.intercept;
	this->occlusionCalibSlope = occlusionParams.slope;
	this->occlusionCalibValid = true;
	PLOG_INFO << "occlusion probe: calibration A=" << this->occlusionCalibIntercept << " B=" << this->occlusionCalibSlope << " func=" << (int)occlusionParams.depthFunc;
	return;/**/

	const auto* currentSource = D3D11Hook::occlusionProbeSelectDepthTex();
	std::vector<float> ys;
	ys.reserve(this->occlusionDepthSamples.size());
	for (const auto& sample : this->occlusionDepthSamples)
	{
		if (sample.sourceTexture == currentSource && sample.viewDepth > 1e-3f)
		{
			ys.push_back(sample.storedDepth * sample.viewDepth);
		}
	}
	PLOG_INFO << "occlusion probe: calib diag pending=" << this->occlusionPendingSamples.size()
		<< " depthSamples=" << this->occlusionDepthSamples.size()
		<< " matching=" << ys.size()
		<< " lastReject=" << this->occlusionSampleRejectReason;

	if (ys.size() < 32)
	{
		return;
	}

	constexpr float yMax = 16.f;
	constexpr int yBins = 64;
	std::vector<int> histogram(yBins, 0);
	for (const float y : ys)
	{
		if (y >= 0.f && y < yMax)
		{
			++histogram[(int)(y / yMax * yBins)];
		}
	}
	int peakBin = 0;
	for (int i = 1; i < yBins; ++i)
	{
		if (histogram[i] > histogram[peakBin])
		{
			peakBin = i;
		}
	}
	if (histogram[peakBin] < 8)
	{
		return;
	}

	float estimate = 0.f;
	int inPeak = 0;
	for (const float y : ys)
	{
		const int b = (int)(y / yMax * yBins);
		if (b >= peakBin - 1 && b <= peakBin + 1)
		{
			estimate += y;
			++inPeak;
		}
	}
	estimate /= (float)inPeak;

	// smooth toward the new estimate instead of snapping, so transient bad samples can't flip occlusion
	float intercept = estimate;
	if (this->occlusionCalibValid)
	{
		intercept = this->occlusionCalibIntercept + (intercept - this->occlusionCalibIntercept) * 0.4f;
	}

	PLOG_INFO << "occlusion probe: calibration A=" << intercept << " estimate=" << estimate
		<< " samples=" << ys.size() << " peak=" << histogram[peakBin];

	/**/if ((D3D11Hook::occlusionProbeFrameCount() % 600) == 0)
	{
		for (const auto& sample : this->occlusionDepthSamples)
		{
			if (sample.sourceTexture == currentSource)
			{
				PLOG_INFO << "occlusion probe: raw u=" << sample.screenU << " v=" << sample.screenV << " z=" << sample.viewDepth << " stored=" << sample.storedDepth;
			}
		}
	}/**/

	this->occlusionCalibIntercept = intercept;
	this->occlusionCalibSlope = 0.f;
	this->occlusionCalibValid = true;
}
/**/


// explicit template instantiation
template class Renderer3DImpl<GameState::Value::Halo1>;
template class Renderer3DImpl<GameState::Value::Halo2>;
template class Renderer3DImpl<GameState::Value::Halo3>;
template class Renderer3DImpl<GameState::Value::Halo3ODST>;
template class Renderer3DImpl<GameState::Value::HaloReach>;
template class Renderer3DImpl<GameState::Value::Halo4>;