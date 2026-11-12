// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 5a: the GPU twin of the producer's engine luma (EngineLumaGpu.h).
#ifndef NOMINMAX
#define NOMINMAX   // AnalysisLumaSource.h -> AnalysisLumaDomain.h uses std::min / std::max (no pch here)
#endif
#include <AnalysisLumaSource.h>
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

	const char* CauseName(Cause c)
	{
		switch (c)
		{
		case Cause::None: return "gpu";
		case Cause::FormatRgb: return "rgb";
		case Cause::FormatDolbyVision: return "dolby vision";
		case Cause::FormatSdrTunnel: return "sdr tunnel";
		case Cause::FullRange: return "full range";
		case Cause::FormatOther: return "format";
		case Cause::KernelOpenFailed: return "kernel failed";
		case Cause::ReaderAttachFailed: return "reader attach failed";
		case Cause::NoSlot: return "no early upload";
		case Cause::SharedOpenFailed: return "shared open failed";
		case Cause::RunFailed: return "run failed";
		case Cause::DeviceRemoved: return "device removed";
		case Cause::DetachTimeout: return "detach timeout";
		default: return "unknown";
		}
	}

	Cause FormatCause(VideoFrameEncoding encoding, bool fullRange, Packing* packing)
	{
		Packing p = Packing::Planar16;
		switch (encoding)
		{
		case VideoFrameEncoding::P010:
		case VideoFrameEncoding::P210: p = Packing::Planar16; break;
		case VideoFrameEncoding::V210: p = Packing::V210; break;
		case VideoFrameEncoding::UYVY:
		case VideoFrameEncoding::HDYC: p = Packing::Uyvy8; break;
		case VideoFrameEncoding::ARGB_8BIT:
		case VideoFrameEncoding::BGRA_8BIT:
		case VideoFrameEncoding::R210:
		case VideoFrameEncoding::R10b:
		case VideoFrameEncoding::R10l:
		case VideoFrameEncoding::R12B:
		case VideoFrameEncoding::R12L: return Cause::FormatRgb;
		default: return Cause::FormatOther;
		}
		if (fullRange)
			return Cause::FullRange;
		if (packing)
			*packing = p;
		return Cause::None;
	}

	Cause FormatCause(AnalysisLumaFormat format, VideoFrameEncoding encoding, AnalysisLumaRange range, Packing* packing)
	{
		const bool fullRange = range == AnalysisLumaRange::Full;
		switch (format)
		{
		case AnalysisLumaFormat::P010: return FormatCause(VideoFrameEncoding::P010, fullRange, packing);
		case AnalysisLumaFormat::P210: return FormatCause(VideoFrameEncoding::P210, fullRange, packing);
		case AnalysisLumaFormat::NativeYuv422:
			if (encoding != VideoFrameEncoding::V210 && encoding != VideoFrameEncoding::UYVY && encoding != VideoFrameEncoding::HDYC)
				return Cause::FormatOther;
			return FormatCause(encoding, fullRange, packing);
		case AnalysisLumaFormat::NativeRgb: return Cause::FormatRgb;
		case AnalysisLumaFormat::DecodedDolbyVision422:
		case AnalysisLumaFormat::PackedDolbyVision422: return Cause::FormatDolbyVision;
		case AnalysisLumaFormat::SdrTunnelRgb: return Cause::FormatSdrTunnel;
		default: return Cause::FormatOther;
		}
	}

	bool Kernel::Fail(Cause c, const char* message)
	{
		cause = c;
		error = message;
		return false;
	}

	bool Kernel::Open(ID3D11Device* deviceIn)
	{
		Close();
		error.clear();
		cause = Cause::None;
		if (!deviceIn)
			return Fail(Cause::KernelOpenFailed, "no device");
		CComPtr<ID3DBlob> code, messages;
		const HRESULT compiled = D3DCompile(kSource, std::strlen(kSource), "EngineLumaGpu.hlsl", nullptr, nullptr, "main",
			"cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
		if (FAILED(compiled) || !code)
		{
			Fail(Cause::KernelOpenFailed, "engine luma kernel does not compile");
			if (messages)
				error += ": " + std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
			return false;
		}
		if (FAILED(deviceIn->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)))
			return Fail(Cause::KernelOpenFailed, "cannot create the engine luma compute shader");
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(Params);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(deviceIn->CreateBuffer(&desc, nullptr, &params)))
		{
			shader.Release();
			return Fail(Cause::KernelOpenFailed, "cannot create the engine luma constants");
		}
		device = deviceIn;
		return true;
	}

	void Kernel::ReleaseViews()
	{
		for (auto& e : srvs) { e.view.Release(); e.tex.Release(); }
		for (auto& e : uavs) { e.view.Release(); e.tex.Release(); }
		nextSrv = nextUav = 0;
	}

	// A cache entry holds a reference to its texture, so a texture pointer is never reused while its entry lives.
	ID3D11ShaderResourceView* Kernel::SrvFor(ID3D11Texture2D* tex)
	{
		for (auto& e : srvs)
			if (e.tex == tex && e.view)
				return e.view;
		SrvEntry& e = srvs[nextSrv];
		nextSrv = (nextSrv + 1) % kViewCache;
		e.view.Release();
		e.tex.Release();
		if (FAILED(device->CreateShaderResourceView(tex, nullptr, &e.view)))
			return nullptr;
		e.tex = tex;
		return e.view;
	}

	ID3D11UnorderedAccessView* Kernel::UavFor(ID3D11Texture2D* tex)
	{
		for (auto& e : uavs)
			if (e.tex == tex && e.view)
				return e.view;
		UavEntry& e = uavs[nextUav];
		nextUav = (nextUav + 1) % kViewCache;
		e.view.Release();
		e.tex.Release();
		if (FAILED(device->CreateUnorderedAccessView(tex, nullptr, &e.view)))
			return nullptr;
		e.tex = tex;
		return e.view;
	}

	void Kernel::Close()
	{
		ReleaseViews();
		params.Release();
		shader.Release();
		device.Release();
	}

	bool Kernel::Run(ID3D11DeviceContext* context, ID3D11Texture2D* raw, Packing packing, uint32_t width, uint32_t height,
		bool quarter, ID3D11Texture2D* out)
	{
		if (!shader || !context || !raw || !out || width == 0 || height == 0)
			return Fail(Cause::RunFailed, "engine luma: not open or no input");
		if (device->GetDeviceRemovedReason() != S_OK)
			return Fail(Cause::DeviceRemoved, "engine luma: the device was removed");
		if (packing != Packing::Planar16 && packing != Packing::V210 && packing != Packing::Uyvy8)
			return Fail(Cause::RunFailed, "engine luma: not a twin format");
		D3D11_TEXTURE2D_DESC rawDesc{}, outDesc{};
		raw->GetDesc(&rawDesc);
		out->GetDesc(&outDesc);
		const uint32_t outW = OutDim(width, quarter), outH = OutDim(height, quarter);
		if (rawDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || (rawDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			size_t(rawDesc.Width) * 4u < MinRowBytes(packing, width) || rawDesc.Height < height)
			return Fail(Cause::RunFailed, "engine luma: the raw texture does not hold the frame");
		if (outDesc.Format != DXGI_FORMAT_R8_UNORM || (outDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0 ||
			outDesc.Width < outW || outDesc.Height < outH)
			return Fail(Cause::RunFailed, "engine luma: the output texture is not an R8 UAV of the engine size");
		ID3D11ShaderResourceView* srv = SrvFor(raw);
		ID3D11UnorderedAccessView* uav = srv ? UavFor(out) : nullptr;
		if (!srv || !uav)
			return Fail(device->GetDeviceRemovedReason() != S_OK ? Cause::DeviceRemoved : Cause::RunFailed,
				"engine luma: cannot make the views");
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
		cause = Cause::None;
		return true;
	}
}
}
