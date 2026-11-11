// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 5a: the GPU twin of the producer's engine luma (EngineLumaGpu.h).
#include <VP-V/modules/input/source_analysis/lane/shared/EngineLumaGpu.h>

#include <d3dcompiler.h>

#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace VpUnit
{
namespace EngineLumaGpu
{
	namespace
	{
		const char kSource[] =
#include "EngineLumaGpu.hlsl"
			;

		struct Params
		{
			uint32_t srcW, srcH, packing, quarter, halfW, halfH, outW, outH;
		};
		static_assert(sizeof(Params) == 32, "the cbuffer layout (two 16-byte registers)");
	}

	const char* Source() { return kSource; }

	bool Kernel::Open(ID3D11Device* deviceIn)
	{
		Close();
		error.clear();
		if (!deviceIn)
		{
			error = "no device";
			return false;
		}
		CComPtr<ID3DBlob> code, messages;
		const HRESULT compiled = D3DCompile(kSource, std::strlen(kSource), "EngineLumaGpu.hlsl", nullptr, nullptr, "main",
			"cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
		if (FAILED(compiled) || !code)
		{
			error = "engine luma kernel does not compile";
			if (messages)
				error += ": " + std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
			return false;
		}
		if (FAILED(deviceIn->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)))
		{
			error = "cannot create the engine luma compute shader";
			return false;
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(Params);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(deviceIn->CreateBuffer(&desc, nullptr, &params)))
		{
			shader.Release();
			error = "cannot create the engine luma constants";
			return false;
		}
		device = deviceIn;
		return true;
	}

	void Kernel::Close()
	{
		params.Release();
		shader.Release();
		device.Release();
	}

	bool Kernel::Run(ID3D11DeviceContext* context, ID3D11Texture2D* raw, Packing packing, uint32_t width, uint32_t height,
		bool quarter, ID3D11Texture2D* out)
	{
		if (!shader || !context || !raw || !out || width == 0 || height == 0)
		{
			error = "engine luma: not open or no input";
			return false;
		}
		if (packing != Packing::Planar16 && packing != Packing::V210 && packing != Packing::Uyvy8)
		{
			error = "engine luma: not a twin format";
			return false;
		}
		D3D11_TEXTURE2D_DESC rawDesc{}, outDesc{};
		raw->GetDesc(&rawDesc);
		out->GetDesc(&outDesc);
		const uint32_t outW = OutDim(width, quarter), outH = OutDim(height, quarter);
		if (rawDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || (rawDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			size_t(rawDesc.Width) * 4u < MinRowBytes(packing, width) || rawDesc.Height < height)
		{
			error = "engine luma: the raw texture does not hold the frame";
			return false;
		}
		if (outDesc.Format != DXGI_FORMAT_R8_UNORM || (outDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0 ||
			outDesc.Width < outW || outDesc.Height < outH)
		{
			error = "engine luma: the output texture is not an R8 UAV of the engine size";
			return false;
		}
		CComPtr<ID3D11ShaderResourceView> srv;
		CComPtr<ID3D11UnorderedAccessView> uav;
		if (FAILED(device->CreateShaderResourceView(raw, nullptr, &srv)) ||
			FAILED(device->CreateUnorderedAccessView(out, nullptr, &uav)))
		{
			error = "engine luma: cannot make the views";
			return false;
		}
		Params p{};
		p.srcW = width;
		p.srcH = height;
		p.packing = static_cast<uint32_t>(packing);
		p.quarter = quarter ? 1u : 0u;
		p.halfW = HalfDim(width);
		p.halfH = HalfDim(height);
		p.outW = outW;
		p.outH = outH;
		context->UpdateSubresource(params, 0, nullptr, &p, 0, 0);
		ID3D11ShaderResourceView* srvs[1] = { srv };
		ID3D11UnorderedAccessView* uavs[1] = { uav };
		ID3D11Buffer* buffers[1] = { params };
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, buffers);
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->Dispatch((outW + 7u) / 8u, (outH + 7u) / 8u, 1u);
		ID3D11ShaderResourceView* noSrv[1] = { nullptr };
		ID3D11UnorderedAccessView* noUav[1] = { nullptr };
		ID3D11Buffer* noBuffer[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
		context->CSSetShaderResources(0, 1, noSrv);
		context->CSSetConstantBuffers(0, 1, noBuffer);
		context->CSSetShader(nullptr, nullptr, 0);
		error.clear();
		return true;
	}
}
}
