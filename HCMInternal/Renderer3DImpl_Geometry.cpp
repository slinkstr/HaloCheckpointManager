#include "pch.h"
#include "Renderer3DImpl.h"
#include <d3dcompiler.h>
#include "directxtk\SimpleMath.h"
#include "RenderTextHelper.h"
#include "directxtk\GeometricPrimitive.h"
#include "directxtk\CommonStates.h"
#include "directxtk\Effects.h"

#pragma comment(lib, "d3dcompiler.lib")

using namespace SettingsEnums;

namespace
{
	std::optional<DXGI_FORMAT> occlusionDepthViewFormat(DXGI_FORMAT textureFormat)
	{
		switch (textureFormat)
		{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
			return DXGI_FORMAT_D32_FLOAT;
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM:
			return DXGI_FORMAT_D16_UNORM;
		default:
			return std::nullopt;
		}
	}
}

template<GameState::Value mGame>
void Renderer3DImpl<mGame>::createOcclusionMergeResources()
{
	constexpr const char* vertexSource = R"(
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})";
	constexpr const char* pixelSource = R"(
Texture2D<float> first  : register(t0);
Texture2D<float> second : register(t1);
float main(float4 position : SV_Position) : SV_Depth
{
	int3 texel = int3(position.xy, 0);
	return max(first.Load(texel), second.Load(texel));
})";

	auto compile = [](const char* source, const char* target) -> ComPtr<ID3DBlob>
	{
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr))
		{
			throw HCMInitException(std::format("Failed to compile occlusion merge shader {}: {}", target, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message"));
		}
		return code;
	};
	const auto vertexCode = compile(vertexSource, "vs_5_0");
	const auto pixelCode  = compile(pixelSource,  "ps_5_0");
	if (FAILED(this->pDevice->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &this->occlusionMergeVertexShader))
		|| FAILED(this->pDevice->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &this->occlusionMergePixelShader)))
	{
		throw HCMInitException("Failed to create occlusion merge shaders");
	}

	D3D11_DEPTH_STENCIL_DESC mergeDepthDesc {};
	mergeDepthDesc.DepthEnable    = TRUE;
	mergeDepthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	mergeDepthDesc.DepthFunc      = D3D11_COMPARISON_ALWAYS;
	if (FAILED(this->pDevice->CreateDepthStencilState(&mergeDepthDesc, &this->occlusionMergeDepthState)))
	{
		throw HCMInitException("Failed to create occlusion merge depth state");
	}
}

template<GameState::Value mGame>
bool Renderer3DImpl<mGame>::refreshOcclusionDepth()
{
	ID3D11Texture2D* sourceTexture = this->occlusionSource->texture.Get();
	D3D11_TEXTURE2D_DESC sourceDesc {};
	sourceTexture->GetDesc(&sourceDesc);
	const auto viewFormat = occlusionDepthViewFormat(sourceDesc.Format);
	if (!viewFormat || sourceDesc.SampleDesc.Count != 1)
	{
		return false;
	}

	if (this->occlusionDepthCopy == nullptr
		|| this->occlusionDepthCopyDesc.Width  != sourceDesc.Width
		|| this->occlusionDepthCopyDesc.Height != sourceDesc.Height
		|| this->occlusionDepthCopyDesc.Format != sourceDesc.Format)
	{
		this->occlusionDepthCopyView.Reset();
		this->occlusionDepthCopy.Reset();

		D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
		copyDesc.Usage          = D3D11_USAGE_DEFAULT;
		copyDesc.BindFlags      = D3D11_BIND_DEPTH_STENCIL;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.MiscFlags      = 0;
		if (FAILED(this->pDevice->CreateTexture2D(&copyDesc, nullptr, &this->occlusionDepthCopy)))
		{
			return false;
		}

		D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc {};
		viewDesc.Format        = *viewFormat;
		viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (FAILED(this->pDevice->CreateDepthStencilView(this->occlusionDepthCopy.Get(), &viewDesc, &this->occlusionDepthCopyView)))
		{
			this->occlusionDepthCopy.Reset();
			return false;
		}
		this->occlusionDepthCopyDesc = sourceDesc;
	}

	if (this->occlusionSource->nearestOf != nullptr)
	{
		return mergeOcclusionDepth(sourceTexture, this->occlusionSource->nearestOf.Get());
	}
	this->pDeviceContext->CopyResource(this->occlusionDepthCopy.Get(), sourceTexture);
	return true;
}

template<GameState::Value mGame>
bool Renderer3DImpl<mGame>::mergeOcclusionDepth(ID3D11Texture2D* first, ID3D11Texture2D* second)
{
	const std::array<ID3D11Texture2D*, 2> inputs { first, second };
	for (size_t i = 0; i < inputs.size(); ++i)
	{
		if (this->occlusionMergeViewTextures[i] == inputs[i])
		{
			continue;
		}
		this->occlusionMergeViews[i].Reset();
		this->occlusionMergeViewTextures[i] = nullptr;
		D3D11_TEXTURE2D_DESC desc {};
		inputs[i]->GetDesc(&desc);
		if (desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS || (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
		{
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
		viewDesc.Format              = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		viewDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
		viewDesc.Texture2D.MipLevels = 1;
		if (FAILED(this->pDevice->CreateShaderResourceView(inputs[i], &viewDesc, &this->occlusionMergeViews[i])))
		{
			return false;
		}
		this->occlusionMergeViewTextures[i] = inputs[i];
	}

	UINT savedViewportCount = 1;
	D3D11_VIEWPORT savedViewport {};
	this->pDeviceContext->RSGetViewports(&savedViewportCount, &savedViewport);

	D3D11_VIEWPORT mergeViewport {};
	mergeViewport.Width    = (float)this->occlusionDepthCopyDesc.Width;
	mergeViewport.Height   = (float)this->occlusionDepthCopyDesc.Height;
	mergeViewport.MaxDepth = 1.f;

	ID3D11ShaderResourceView* views[2] { this->occlusionMergeViews[0].Get(), this->occlusionMergeViews[1].Get() };
	this->pDeviceContext->OMSetRenderTargets(0, nullptr, this->occlusionDepthCopyView.Get());
	this->pDeviceContext->OMSetDepthStencilState(this->occlusionMergeDepthState.Get(), 0);
	this->pDeviceContext->RSSetState(this->commonStates->CullNone());
	this->pDeviceContext->RSSetViewports(1, &mergeViewport);
	this->pDeviceContext->IASetInputLayout(nullptr);
	this->pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	this->pDeviceContext->VSSetShader(this->occlusionMergeVertexShader.Get(), nullptr, 0);
	this->pDeviceContext->PSSetShader(this->occlusionMergePixelShader.Get(), nullptr, 0);
	this->pDeviceContext->PSSetShaderResources(0, 2, views);
	this->pDeviceContext->Draw(3, 0);

	ID3D11ShaderResourceView* clearedViews[2] {};
	this->pDeviceContext->PSSetShaderResources(0, 2, clearedViews);
	if (savedViewportCount == 1)
	{
		this->pDeviceContext->RSSetViewports(1, &savedViewport);
	}
	return true;
}

template<GameState::Value mGame>
bool Renderer3DImpl<mGame>::prepareOcclusionDepth()
{
	if (!this->occlusionSource)
	{
		return false;
	}
	ID3D11RenderTargetView* renderTargetView = this->pMainRenderTargetView;
	this->pDeviceContext->OMSetRenderTargets(1, &renderTargetView, this->occlusionDepthCopyView.Get());
	this->pDeviceContext->OMSetDepthStencilState(this->occlusionDepthState.Get(), 0);
	return true;
}

template<GameState::Value mGame>
void Renderer3DImpl<mGame>::restoreOverlayDepth()
{
	ID3D11RenderTargetView* renderTargetView = this->pMainRenderTargetView;
	this->pDeviceContext->OMSetRenderTargets(1, &renderTargetView, this->m_depthStencilView.Get());
	this->pDeviceContext->OMSetDepthStencilState(this->commonStates->DepthRead(), 0);
}

template<GameState::Value mGame>
void Renderer3DImpl<mGame>::setTexture(std::optional<TextureEnum> texture)
{
	if (texture)
	{
		primitiveBatchEffect->SetTextureEnabled(true);
		primitiveBatchEffect->SetTexture(textureResources.at(texture.value())->getTextureView());
		this->pDeviceContext->IASetInputLayout(inputLayoutTextured.Get());
	}
	else
	{
		primitiveBatchEffect->SetTextureEnabled(false);
		this->pDeviceContext->IASetInputLayout(inputLayoutUntextured.Get());
	}


}

template<GameState::Value mGame>
void Renderer3DImpl<mGame>::setCullMode(CullingOption cullingOption)
{

	switch (cullingOption)
	{
	case CullingOption::CullNone:
		this->pDeviceContext->RSSetState(this->commonStates->CullNone());
			break;

	case CullingOption::CullBack:
		this->pDeviceContext->RSSetState(this->commonStates->CullClockwise());
		break;

	case CullingOption::CullFront:
		this->pDeviceContext->RSSetState(this->commonStates->CullCounterClockwise());
			break;

	default:
		throw HCMRuntimeException(std::format("Bad enum passed to setCullMode! int: {}", (int)cullingOption));

	}

}


template<GameState::Value mGame>
void Renderer3DImpl<mGame>::renderSphere(const SimpleMath::Vector3& position, const SimpleMath::Vector4& color, const float& scale, const bool& isWireframe)
{
	setTexture(std::nullopt);
	setCullMode(CullingOption::CullNone);

	// create position and scale transforms
	auto resizeTransform = SimpleMath::Matrix::CreateScale(scale);
	auto translateTransform = SimpleMath::Matrix::CreateTranslation(position);

	unitSphere->Draw(resizeTransform * translateTransform, this->viewMatrix, this->projectionMatrix, color, nullptr, isWireframe);
}


template<GameState::Value mGame>
void Renderer3DImpl<mGame>::drawTriangle(const std::array<SimpleMath::Vector3, 3>& vertexPositions, const SimpleMath::Vector4& color, CullingOption cullingOption, std::optional<TextureEnum> texture)
{

	setTexture(texture);


	this->pDeviceContext->OMSetBlendState(this->commonStates->AlphaBlend(), nullptr, 0xFFFFFFFF); 
	this->pDeviceContext->OMSetDepthStencilState(this->commonStates->DepthRead(), 0); // or depth default?
	setCullMode(cullingOption);

	this->primitiveBatchEffect->SetColorAndAlpha(color);
	this->primitiveBatchEffect->Apply(pDeviceContext);

	primitiveDrawer->Begin();
	primitiveDrawer->DrawTriangle(vertexPositions[0], vertexPositions[1], vertexPositions[2]);
	primitiveDrawer->End();
}


template<GameState::Value mGame>
void Renderer3DImpl<mGame>::drawEdge(const SimpleMath::Vector3& edgeStart, const SimpleMath::Vector3& edgeEnd, const SimpleMath::Vector4& color)
{
	setTexture(std::nullopt);


	this->pDeviceContext->OMSetBlendState(this->commonStates->AlphaBlend(), nullptr, 0xFFFFFFFF);
	this->pDeviceContext->OMSetDepthStencilState(this->commonStates->DepthRead(), 0); // or depth default?
	setCullMode(CullingOption::CullNone);

	this->primitiveBatchEffect->SetColorAndAlpha(color);
	this->primitiveBatchEffect->Apply(this->pDeviceContext);

	primitiveDrawer->Begin();
	primitiveDrawer->DrawLine(edgeStart, edgeEnd);
	primitiveDrawer->End();
}

template<GameState::Value mGame>
void  Renderer3DImpl<mGame>::drawTriangleCollection(const IModelTriangles* model, const SimpleMath::Vector4& color, CullingOption cullingOption, std::optional<TextureEnum> texture)
{
	setTexture(texture);


	this->pDeviceContext->OMSetBlendState(this->commonStates->AlphaBlend(), nullptr, 0xFFFFFFFF);
	this->pDeviceContext->OMSetDepthStencilState(this->commonStates->DepthRead(), 0); // or depth default?
	setCullMode(cullingOption);


	this->primitiveBatchEffect->SetColorAndAlpha(color);
	this->primitiveBatchEffect->Apply(pDeviceContext);

	const bool occlusionDepthBound = prepareOcclusionDepth();

	primitiveDrawer->Begin();
	primitiveDrawer->DrawIndexed(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 
		model->getTriangleIndices().data(), 
		model->getTriangleIndices().size(), 
		model->getTriangleVertices().data(), 
		model->getTriangleVertices().size());
	primitiveDrawer->End();

	if (occlusionDepthBound)
	{
		restoreOverlayDepth();
	}
}

template<GameState::Value mGame>
void  Renderer3DImpl<mGame>::drawEdgeCollection(const IModelEdges* model, const SimpleMath::Vector4& color)
{
	setTexture(std::nullopt);



	this->pDeviceContext->OMSetBlendState(this->commonStates->AlphaBlend(), nullptr, 0xFFFFFFFF);
	this->pDeviceContext->OMSetDepthStencilState(this->commonStates->DepthRead(), 0); // or depth default?
	setCullMode(CullingOption::CullNone);

	this->primitiveBatchEffect->SetColorAndAlpha(color);
	this->primitiveBatchEffect->Apply(this->pDeviceContext);

	const bool occlusionDepthBound = prepareOcclusionDepth();

	primitiveDrawer->Begin();
	primitiveDrawer->DrawIndexed(D3D10_PRIMITIVE_TOPOLOGY_LINELIST, 
		model->getEdgeIndices().data(),
		model->getEdgeIndices().size(),
		model->getEdgeVertices().data(),
		model->getEdgeVertices().size());
	primitiveDrawer->End();

	if (occlusionDepthBound)
	{
		restoreOverlayDepth();
	}
}



/*
template<GameState::Value mGame>
void Renderer3DImpl<mGame>::drawFilledTris(const VertexCollection& vertices, const IndexCollection& indices, const SimpleMath::Vector4& colour, TextureEnum texture)
{
	
	// https://github.com/microsoft/DirectXTK/blob/main/Src/GeometricPrimitive.cpp#L119
	ID3D11InputLayout* inputLayout;
	if (texture != TextureEnum::NoTexture)
	{
		effect->SetTextureEnabled(true);
		effect->SetTexture(texture);

		inputLayout = mResources->inputLayoutTextured.Get();
	}
	else
	{
		effect->SetTextureEnabled(false);
		inputLayout = mResources->inputLayoutUntextured.Get();
	}


	// if we use a vector of inputLayouts we can select inputLayout using inputLayouts.at((int)textureEnum)

	this->primitiveBatchEffect->SetColorAndAlpha(colour);

	const float alpha = XMVectorGetW(colour);

	if (alpha < 1.f)
	{
		// Alpha blended rendering.
		blendState = stateObjects->AlphaBlend();
		depthStencilState = stateObjects->DepthRead();
	}
	else
	{
		// Opaque rendering.
		blendState = stateObjects->Opaque(); // this enables proper depth occlusion.. 
		depthStencilState = stateObjects->DepthDefault(); // not sure difference between depthDefault and depthRead. former writes?
	}

	this->pDeviceContext->OMSetBlendState(blendState, nullptr, 0xFFFFFFFF);
	this->pDeviceContext->OMSetDepthStencilState(depthStencilState, 0);

	this->pDeviceContext->RSSetState(this->commonStates->CullNone()); // or cull counter clockwise?

	// wtf is this? shader stuff? can probably just set once in updateCameraData..
	ID3D11SamplerState* samplerState = this->commonStates->LinearWrap();
	this->pDeviceContext->PSSetSamplers(0, 1, &samplerState);


	// this is why textures were bleeding into barrier earlier.. 
	this->pDeviceContext->IASetInputLayout(inputLayout);
	this->primitiveBatchEffect->Apply(pDeviceContext);



	primitiveDrawer->DrawIndexed(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, indices.data(), indices.size(), vertices.data(), vertices.size());

	
}*/

// also need a draw lines with the same sig (minus the texture)

/*

D3D11_PRIMITIVE_TOPOLOGY_LINELIST for wireframe
trianglelist for solid ? 

TriggerData 
.. vertices and indices?
DrawIndexed ? 
a bounding box has 8 vertexes
6 faces = 12 tris
so 12 * 3 indices for the solid volumes

for wireframe:
8 faces = 12 lines
so 12 * 2 indices for the wireframe



barrier overlay: just do each tri one at a time


texturing: steal code from geometric primitive 

  ID3D11InputLayout *inputLayout;
	if (texture)
	{
		effect->SetTextureEnabled(true);
		effect->SetTexture(texture);

		inputLayout = mResources->inputLayoutTextured.Get();
	}
	else
	{
		effect->SetTextureEnabled(false);

		inputLayout = mResources->inputLayoutUntextured.Get();
	}

	input layout gets reset with every draw!
	effect gets applied with every draw!


	 using IndexCollection = std::vector<uint16_t>;
	 using VertexCollection = std::vector<DirectX::VertexPosition>;


*/


// explicit template instantiation
template class Renderer3DImpl<GameState::Value::Halo1>;
template class Renderer3DImpl<GameState::Value::Halo2>;
template class Renderer3DImpl<GameState::Value::Halo3>;
template class Renderer3DImpl<GameState::Value::Halo3ODST>;
template class Renderer3DImpl<GameState::Value::HaloReach>;
template class Renderer3DImpl<GameState::Value::Halo4>;