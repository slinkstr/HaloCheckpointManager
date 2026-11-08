#include "pch.h"
#include "Renderer3DImpl.h"
#include "D3D11Hook.h"
#include "directxtk\SimpleMath.h"
#include "RenderTextHelper.h"
#include "directxtk\GeometricPrimitive.h"
#include "directxtk\CommonStates.h"
#include "directxtk\Effects.h"

using namespace SettingsEnums;

/**/
template<GameState::Value mGame>
bool Renderer3DImpl<mGame>::prepareOcclusionDepth()
{
	ID3D11Texture2D* sourceTexture = D3D11Hook::occlusionProbeSelectDepthTex();
	if (sourceTexture == nullptr)
	{
		return false;
	}

	DXGI_FORMAT dsvFormat;
	switch (D3D11Hook::occlusionProbeDepthFormat())
	{
	case DXGI_FORMAT_R32G8X24_TYPELESS:
		dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		break;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_D32_FLOAT:
		dsvFormat = D3D11Hook::occlusionProbeDepthFormat();
		break;
	case DXGI_FORMAT_R32_TYPELESS:
		dsvFormat = DXGI_FORMAT_D32_FLOAT;
		break;
	case DXGI_FORMAT_R24G8_TYPELESS:
		dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		break;
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		break;
	default:
		return false;
	}

	D3D11_TEXTURE2D_DESC sourceDesc{};
	sourceTexture->GetDesc(&sourceDesc);
	if (sourceDesc.SampleDesc.Count != 1)
	{
		return false;
	}

	if (this->occlusionDepthCopy == nullptr
		|| this->occlusionDepthCopyWidth != sourceDesc.Width
		|| this->occlusionDepthCopyHeight != sourceDesc.Height
		|| this->occlusionDepthCopyFormat != sourceDesc.Format)
	{
		this->occlusionDepthCopyView.Reset();
		this->occlusionDepthCopy.Reset();

		D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		copyDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.MiscFlags = 0;
		if (FAILED(this->pDevice->CreateTexture2D(&copyDesc, nullptr, &this->occlusionDepthCopy)))
		{
			return false;
		}

		D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
		viewDesc.Format = dsvFormat;
		viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		viewDesc.Texture2D.MipSlice = 0;
		if (FAILED(this->pDevice->CreateDepthStencilView(this->occlusionDepthCopy.Get(), &viewDesc, &this->occlusionDepthCopyView)))
		{
			return false;
		}

		this->occlusionDepthCopyWidth = sourceDesc.Width;
		this->occlusionDepthCopyHeight = sourceDesc.Height;
		this->occlusionDepthCopyFormat = sourceDesc.Format;
		D3D11Hook::occlusionProbeIgnoreTexture(this->occlusionDepthCopy.Get());
	}

	const uint64_t frameCount = D3D11Hook::occlusionProbeFrameCount();
	if (this->occlusionDepthCopyFrame != frameCount)
	{
		this->pDeviceContext->CopyResource(this->occlusionDepthCopy.Get(), sourceTexture);
		this->occlusionDepthCopyFrame = frameCount;
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
/**/

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

	/**/const bool occlusionDepthBound = prepareOcclusionDepth();/**/

	primitiveDrawer->Begin();
	primitiveDrawer->DrawIndexed(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 
		model->getTriangleIndices().data(), 
		model->getTriangleIndices().size(), 
		model->getTriangleVertices().data(), 
		model->getTriangleVertices().size());
	primitiveDrawer->End();

	/**/if (occlusionDepthBound) { restoreOverlayDepth(); }/**/
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

	/**/const bool occlusionDepthBound = prepareOcclusionDepth();/**/

	primitiveDrawer->Begin();
	primitiveDrawer->DrawIndexed(D3D10_PRIMITIVE_TOPOLOGY_LINELIST, 
		model->getEdgeIndices().data(),
		model->getEdgeIndices().size(),
		model->getEdgeVertices().data(),
		model->getEdgeVertices().size());
	primitiveDrawer->End();

	/**/if (occlusionDepthBound) { restoreOverlayDepth(); }/**/
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