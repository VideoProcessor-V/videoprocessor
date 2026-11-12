// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the GPU form of the direct photometric refinement (see PhotometricRefineGpu.h). The kernels
// (PhotometricRefineGpuShaders.inl) follow PhotometricRefine.cpp step by step; the CPU side here prepares the
// per-call tables (picture margin and inlier-block lookups per column / row, the inlier cells), dispatches every
// pass up front and, after the one readback, takes the final decisions with the CPU reference's own functions.
#include "PhotometricRefineGpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "GlobalModelGeometry.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")   // IID_ID3D11ShaderReflection

namespace CameraModel
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

#include "PhotometricRefineGpuShaders.inl"

		enum Kernel
		{
			kClear, kCand, kThr, kRowCount, kRowPrefix, kCompact, kResid, kMedian, kNormal, kSolve, kVerify, kFinal, kKernels
		};
		const char* const kEntry[kKernels] = {
			"CSClear", "CSCand", "CSThr", "CSRowCount", "CSRowPrefix", "CSCompact", "CSResid", "CSMedian", "CSNormal", "CSSolve", "CSVerify", "CSFinal"
		};

		// State slots (uint2 each), as the HLSL.
		const int kStop = 0, kFlags = 1, kCounts = 2, kPass = 4, kP = 5, kGain = 14, kOffset = 15, kSigma = 16, kLast = 17, kBefore = 18,
			kAfter = 19, kBoth = 20, kStateSlots = 32;
		const int kMaxWidth = 8191, kMaxHeight = 2304;   // 13-bit packed column; the row-prefix kernel's groupshared rows

		struct CB
		{
			uint32_t w, h, useMask, gridW;
			uint32_t bicubic, nGeo, nPar, fade;
			uint32_t target, minPixels, maxIter, unit;
			uint32_t numGroups, pad0, pad1, pad2;
			double s, invS;
			double inv2S, k;
			double cx, cy;
			double L, invL;
			double diag, invDiagPic;
			double minGrad, huberK;
			double minScale, convThr;
			double hw, hh;
		};
		static_assert(sizeof(CB) % 16 == 0, "constant buffer size");

		// The compiled kernels, once per process (D3DCompile of the double kernels takes seconds).
		struct Blobs
		{
			std::mutex lock;
			bool done = false;
			bool ok = false;
			std::string why;
			ComPtr<ID3DBlob> blob[kKernels];
		};
		Blobs& SharedBlobs()
		{
			static Blobs b;
			return b;
		}

		bool CompileAll(std::string* why)
		{
			Blobs& b = SharedBlobs();
			std::lock_guard<std::mutex> guard(b.lock);
			if (!b.done)
			{
				b.done = true;
				b.ok = true;
				std::string src;
				for (size_t i = 0; i < sizeof(kRefineHlsl) / sizeof(kRefineHlsl[0]); ++i)
					src += kRefineHlsl[i];
				for (int e = 0; e < kKernels && b.ok; ++e)
				{
					ComPtr<ID3DBlob> err;
					const HRESULT hr = D3DCompile(src.data(), src.size(), "PhotometricRefineGpu", nullptr, nullptr, kEntry[e], "cs_5_0",
						D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b.blob[e], &err);
					if (FAILED(hr))
					{
						b.ok = false;
						b.why = std::string("kernel ") + kEntry[e] + " did not compile";
						if (err)
							b.why += std::string(": ") + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
						break;
					}
					ComPtr<ID3D11ShaderReflection> refl;
					if (SUCCEEDED(D3DReflect(b.blob[e]->GetBufferPointer(), b.blob[e]->GetBufferSize(), IID_ID3D11ShaderReflection,
						reinterpret_cast<void**>(refl.GetAddressOf()))))
					{
						if (refl->GetRequiresFlags() & D3D_SHADER_REQUIRES_11_1_DOUBLE_EXTENSIONS)
						{
							b.ok = false;
							b.why = std::string("kernel ") + kEntry[e] + " needs the 11.1 double extensions";
						}
					}
				}
			}
			if (!b.ok && why)
				*why = b.why;
			return b.ok;
		}

		double MsSince(std::chrono::steady_clock::time_point t0)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}

		bool ParamsFiniteGpu(const Params& p)
		{
			for (int i = 0; i < 9; ++i)
				if (!IsFiniteBits(p.h[i]))
					return false;
			return true;
		}

		double LoadD(const uint32_t* slot)
		{
			uint64_t bits = static_cast<uint64_t>(slot[0]) | (static_cast<uint64_t>(slot[1]) << 32);
			double d;
			std::memcpy(&d, &bits, sizeof(d));
			return d;
		}
		void StoreD(uint32_t* slot, double d)
		{
			uint64_t bits;
			std::memcpy(&bits, &d, sizeof(bits));
			slot[0] = static_cast<uint32_t>(bits);
			slot[1] = static_cast<uint32_t>(bits >> 32);
		}
	}

	// ---- input: CPU upload --------------------------------------------------------------------------------------

	struct RefineUploadInput::Tex
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11Texture2D> tex[2];
		ComPtr<ID3D11ShaderResourceView> srv[2];
		int w = 0, h = 0;
		bool r16 = false;
	};

	RefineUploadInput::RefineUploadInput() : m_tex(new Tex()) {}
	RefineUploadInput::~RefineUploadInput() { delete m_tex; }

	bool RefineUploadInput::Init(ID3D11Device* device, std::string* why)
	{
		if (!device)
		{
			if (why) *why = "no device";
			return false;
		}
		m_tex->device = device;
		return true;
	}

	bool RefineUploadInput::Prepare(ID3D11DeviceContext* context, RefineGpuLuma* out, std::string* why)
	{
		m_uploadMs = 0;
		const LumaPlane& a = m_view.from;
		const LumaPlane& b = m_view.to;
		if (!m_tex->device || !context || !out || !a.Valid() || !b.Valid() || (a.data8 != nullptr) != (b.data8 != nullptr)
			|| a.width != b.width || a.height != b.height)
		{
			if (why) *why = "no luma pair";
			return false;
		}
		const bool r16 = a.data16 != nullptr;
		if (!m_tex->tex[0] || m_tex->w != a.width || m_tex->h != a.height || m_tex->r16 != r16)
		{
			for (int i = 0; i < 2; ++i)
			{
				m_tex->srv[i].Reset();
				m_tex->tex[i].Reset();
			}
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = static_cast<UINT>(a.width);
			td.Height = static_cast<UINT>(a.height);
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = r16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			for (int i = 0; i < 2; ++i)
			{
				if (FAILED(m_tex->device->CreateTexture2D(&td, nullptr, &m_tex->tex[i]))
					|| FAILED(m_tex->device->CreateShaderResourceView(m_tex->tex[i].Get(), nullptr, &m_tex->srv[i])))
				{
					m_tex->tex[0].Reset();
					if (why) *why = "luma texture creation failed";
					return false;
				}
			}
			m_tex->w = a.width;
			m_tex->h = a.height;
			m_tex->r16 = r16;
		}
		const auto t0 = std::chrono::steady_clock::now();
		const UINT elem = r16 ? 2u : 1u;
		context->UpdateSubresource(m_tex->tex[0].Get(), 0, nullptr, r16 ? static_cast<const void*>(a.data16) : a.data8,
			static_cast<UINT>(a.stride) * elem, 0);
		context->UpdateSubresource(m_tex->tex[1].Get(), 0, nullptr, r16 ? static_cast<const void*>(b.data16) : b.data8,
			static_cast<UINT>(b.stride) * elem, 0);
		m_uploadMs = MsSince(t0);
		out->from = m_tex->srv[0].Get();
		out->to = m_tex->srv[1].Get();
		out->width = a.width;
		out->height = a.height;
		out->r16 = r16;
		out->sourcePxPerPixel = m_view.sourcePxPerPixel;
		out->codeScale = m_view.codeScale;
		return true;
	}

	// ---- the refiner --------------------------------------------------------------------------------------------

	struct PhotometricRefinerGpu::Impl
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11ComputeShader> cs[kKernels];
		ComPtr<ID3D11Buffer> cb;

		struct Buf
		{
			ComPtr<ID3D11Buffer> buf;
			ComPtr<ID3D11UnorderedAccessView> uav;
			ComPtr<ID3D11ShaderResourceView> srv;
			UINT count = 0, stride = 0;
		};
		Buf bin, hist, row, pts, res, part, state;   // u0..u6
		Buf colBx, rowBy, cell;                      // t2..t4
		ComPtr<ID3D11Buffer> staging;
		ComPtr<ID3D11Query> done;
		ComPtr<ID3D11Query> disjoint, tsBegin, tsEnd;

		// The pending call.
		bool pending = false;       // a submission is in flight
		bool isFinal = false;       // the result is already final (nothing queued)
		bool timed = false;
		RefineResult result;
		Params model;
		Rect picture;
		double s = 0, diag = 0;
		bool fade = false;
		std::vector<int32_t> colTab, rowTab;
		std::vector<uint32_t> cellTab;
		std::vector<uint32_t> stateInit;

		bool Ensure(Buf& b, UINT count, UINT stride, bool uav, std::string* why)
		{
			if (count == 0)
				count = 1;
			if (b.buf && b.count >= count && b.stride == stride)
				return true;
			b.buf.Reset();
			b.uav.Reset();
			b.srv.Reset();
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = count * stride;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = uav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = stride;
			if (FAILED(device->CreateBuffer(&bd, nullptr, &b.buf)))
			{
				if (why) *why = "buffer creation failed";
				return false;
			}
			HRESULT hr;
			if (uav)
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
				ud.Format = DXGI_FORMAT_UNKNOWN;
				ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				ud.Buffer.NumElements = count;
				hr = device->CreateUnorderedAccessView(b.buf.Get(), &ud, &b.uav);
			}
			else
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
				sd.Format = DXGI_FORMAT_UNKNOWN;
				sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				sd.Buffer.NumElements = count;
				hr = device->CreateShaderResourceView(b.buf.Get(), &sd, &b.srv);
			}
			if (FAILED(hr))
			{
				b.buf.Reset();
				if (why) *why = "buffer view creation failed";
				return false;
			}
			b.count = count;
			b.stride = stride;
			return true;
		}
	};

	PhotometricRefinerGpu::PhotometricRefinerGpu(const RefineOptions& options) : m_impl(new Impl()), m_options(options) {}
	PhotometricRefinerGpu::~PhotometricRefinerGpu() { delete m_impl; }

	bool PhotometricRefinerGpu::Init(ID3D11Device* device, std::string* why)
	{
		m_ready = false;
		m_why.clear();
		Impl& im = *m_impl;
		if (!device)
			m_why = "no device";
		else if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
			m_why = "feature level below 11_0";
		else
		{
			D3D11_FEATURE_DATA_DOUBLES dbl = {};
			if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dbl, sizeof(dbl))) || !dbl.DoublePrecisionFloatShaderOps)
				m_why = "no double-precision shader ops";
		}
		if (m_why.empty())
			CompileAll(&m_why);
		if (m_why.empty())
		{
			im.device = device;
			Blobs& b = SharedBlobs();
			for (int e = 0; e < kKernels && m_why.empty(); ++e)
				if (FAILED(device->CreateComputeShader(b.blob[e]->GetBufferPointer(), b.blob[e]->GetBufferSize(), nullptr, &im.cs[e])))
					m_why = std::string("kernel ") + kEntry[e] + " creation failed";
		}
		if (m_why.empty())
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(CB);
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			D3D11_BUFFER_DESC sd = {};
			sd.ByteWidth = kStateSlots * 8;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			D3D11_QUERY_DESC qd = {};
			qd.Query = D3D11_QUERY_EVENT;
			D3D11_QUERY_DESC qj = {};
			qj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
			D3D11_QUERY_DESC qt = {};
			qt.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateBuffer(&bd, nullptr, &im.cb)) || FAILED(device->CreateBuffer(&sd, nullptr, &im.staging))
				|| FAILED(device->CreateQuery(&qd, &im.done)) || FAILED(device->CreateQuery(&qj, &im.disjoint))
				|| FAILED(device->CreateQuery(&qt, &im.tsBegin)) || FAILED(device->CreateQuery(&qt, &im.tsEnd)))
				m_why = "buffer / query creation failed";
		}
		if (m_why.empty())
		{
			std::string e;
			if (!im.Ensure(im.hist, 4096, 4, true, &e) || !im.Ensure(im.state, kStateSlots, 8, true, &e))
				m_why = e;
		}
		m_ready = m_why.empty();
		if (!m_ready)
			im.device.Reset();
		if (why)
			*why = m_why;
		return m_ready;
	}

	bool PhotometricRefinerGpu::Submit(ID3D11DeviceContext* context, const Params& model, const RefineGpuLuma& luma, const Rect& picture,
		const InlierMask& mask)
	{
		Impl& im = *m_impl;
		const RefineOptions& o = m_options;
		if (im.pending)
			++m_failures.droppedPending;
		m_lastFailure = RefineGpuFailure::None;
		im.pending = false;
		im.isFinal = true;
		im.timed = false;
		m_lastError.clear();
		m_lastTiming = RefineGpuTiming();
		RefineResult& res = im.result;
		res = RefineResult();
		res.model = model;
		im.model = model;
		im.picture = picture;

		// The CPU reference's precheck (PhotometricRefiner::Refine), then the GPU's own limits (reported as no luma).
		const double s = luma.sourcePxPerPixel;
		const bool lumaOk = luma.from != nullptr && luma.to != nullptr && luma.width >= 8 && luma.height >= 8 && s > 0 && IsFiniteBits(s)
			&& luma.codeScale > 0 && IsFiniteBits(luma.codeScale) && !picture.Empty() && picture.left >= 0 && picture.top >= 0
			&& picture.right <= luma.width * s + s && picture.bottom <= luma.height * s + s;
		if (!m_ready || !context)
		{
			res.status = RefineStatus::SkippedNoLuma;
			Fail(RefineGpuFailure::Unavailable);
			m_lastError = m_ready ? "no device context" : (m_why.empty() ? std::string("not initialised") : m_why);
			return false;
		}
		if (!lumaOk)
		{
			res.status = RefineStatus::SkippedNoLuma;
			return false;
		}
		if (model.type == ModelType::None || !ParamsFiniteGpu(model) || IsDegenerate(model, picture))
		{
			res.status = RefineStatus::SkippedModel;
			return false;
		}
		if (luma.width > kMaxWidth || luma.height > kMaxHeight)
		{
			res.status = RefineStatus::SkippedNoLuma;
			Fail(RefineGpuFailure::Limit);
			m_lastError = "luma larger than the GPU limits (8191 x 2304)";
			return false;
		}

		const int w = luma.width, h = luma.height;
		const double cx = picture.CentreX(), cy = picture.CentreY();
		const double margin = (std::max)(0.0, o.borderLumaPx) * s;
		const bool useMask = mask.label != nullptr && mask.gridW > 0 && mask.gridH > 0 && mask.blockSize > 0;
		res.maskUsed = useMask;

		// Column / row tables: -1 = outside the picture margin (or the block grid), else the block column / row.
		im.colTab.assign(static_cast<size_t>(w), -1);
		im.rowTab.assign(static_cast<size_t>(h), -1);
		for (int i = 1; i < w - 1; ++i)
		{
			const double fx = s * (i + 0.5);
			if (fx < picture.left + margin || fx > picture.right - margin)
				continue;
			int bx = 0;
			if (useMask)
			{
				bx = static_cast<int>(fx / mask.blockSize);
				if (bx >= mask.gridW)
					continue;
			}
			im.colTab[static_cast<size_t>(i)] = bx;
		}
		for (int j = 1; j < h - 1; ++j)
		{
			const double fy = s * (j + 0.5);
			if (fy < picture.top + margin || fy > picture.bottom - margin)
				continue;
			int by = 0;
			if (useMask)
			{
				by = static_cast<int>(fy / mask.blockSize);
				if (by >= mask.gridH)
					continue;
			}
			im.rowTab[static_cast<size_t>(j)] = by;
		}
		const size_t cells = useMask ? static_cast<size_t>(mask.gridW) * static_cast<size_t>(mask.gridH) : 1u;
		im.cellTab.assign(cells, 1u);
		if (useMask)
			for (size_t c = 0; c < cells; ++c)
				im.cellTab[c] = (mask.label[c] == static_cast<uint8_t>(Label::Camera) && !(mask.excluded && mask.excluded[c])) ? 1u : 0u;

		const uint32_t target = static_cast<uint32_t>((std::max)(1, (std::max)(o.minPixels, o.targetPixels)));
		const uint32_t minPixels = static_cast<uint32_t>((std::max)(1, o.minPixels));
		const uint32_t maxIter = static_cast<uint32_t>((std::max)(1, o.maxIterations));
		const uint32_t numGroups = (target + 1023u) / 1024u;
		const double L = 0.5 * std::sqrt(static_cast<double>(picture.Width()) * picture.Width() + static_cast<double>(picture.Height()) * picture.Height());
		const int nGeo = (o.translationOnly || model.type == ModelType::Translation) ? 2 : 4;
		const int nPar = nGeo + (o.fadeTerms ? 2 : 0);
		im.s = s;
		im.diag = 2.0 * L;
		im.fade = o.fadeTerms;

		std::string e;
		if (!im.Ensure(im.bin, static_cast<UINT>(w) * static_cast<UINT>(h), 4, true, &e) || !im.Ensure(im.row, static_cast<UINT>(h), 16, true, &e)
			|| !im.Ensure(im.pts, target, 4, true, &e) || !im.Ensure(im.res, target, 16, true, &e) || !im.Ensure(im.part, numGroups * 32u, 8, true, &e)
			|| !im.Ensure(im.colBx, static_cast<UINT>(w), 4, false, &e) || !im.Ensure(im.rowBy, static_cast<UINT>(h), 4, false, &e)
			|| !im.Ensure(im.cell, static_cast<UINT>(cells), 4, false, &e))
		{
			res.status = RefineStatus::SkippedNoLuma;
			Fail(RefineGpuFailure::Ensure);
			m_lastError = e;
			return false;
		}

		CB cb = {};
		cb.w = static_cast<uint32_t>(w);
		cb.h = static_cast<uint32_t>(h);
		cb.useMask = useMask ? 1u : 0u;
		cb.gridW = useMask ? static_cast<uint32_t>(mask.gridW) : 1u;
		cb.bicubic = o.interp == RefineInterp::Bicubic ? 1u : 0u;
		cb.nGeo = static_cast<uint32_t>(nGeo);
		cb.nPar = static_cast<uint32_t>(nPar);
		cb.fade = o.fadeTerms ? 1u : 0u;
		cb.target = target;
		cb.minPixels = minPixels;
		cb.maxIter = maxIter;
		cb.unit = luma.codeScale == 1.0 ? 1u : 0u;
		cb.numGroups = numGroups;
		cb.s = s;
		cb.invS = 1.0 / s;
		cb.inv2S = 1.0 / (2.0 * s);
		cb.k = luma.codeScale;
		cb.cx = cx;
		cb.cy = cy;
		cb.L = L;
		cb.invL = 1.0 / L;
		cb.diag = im.diag;
		cb.invDiagPic = 1.0 / std::sqrt(static_cast<double>(picture.Width()) * picture.Width() + static_cast<double>(picture.Height()) * picture.Height());
		cb.minGrad = o.minGradientCodes;
		cb.huberK = o.huberK;
		cb.minScale = o.minScaleCodes;
		cb.convThr = o.convergeLumaPx * s;
		cb.hw = 0.5 * picture.Width();
		cb.hh = 0.5 * picture.Height();

		im.stateInit.assign(kStateSlots * 2, 0u);
		for (int q = 0; q < 9; ++q)
			StoreD(&im.stateInit[(kP + q) * 2], model.h[q]);
		StoreD(&im.stateInit[kGain * 2], 1.0);
		StoreD(&im.stateInit[kOffset * 2], 0.0);
		StoreD(&im.stateInit[kSigma * 2], o.minScaleCodes);

		ID3D11DeviceContext* ctx = context;
		ctx->UpdateSubresource(im.cb.Get(), 0, nullptr, &cb, 0, 0);
		ctx->UpdateSubresource(im.state.buf.Get(), 0, nullptr, im.stateInit.data(), 0, 0);
		D3D11_BOX box = {};
		box.bottom = 1;
		box.back = 1;
		box.right = static_cast<UINT>(w) * 4u;
		ctx->UpdateSubresource(im.colBx.buf.Get(), 0, &box, im.colTab.data(), 0, 0);
		box.right = static_cast<UINT>(h) * 4u;
		ctx->UpdateSubresource(im.rowBy.buf.Get(), 0, &box, im.rowTab.data(), 0, 0);
		box.right = static_cast<UINT>(cells) * 4u;
		ctx->UpdateSubresource(im.cell.buf.Get(), 0, &box, im.cellTab.data(), 0, 0);

		if (m_timing)
		{
			ctx->Begin(im.disjoint.Get());
			ctx->End(im.tsBegin.Get());
			im.timed = true;
		}
		ID3D11Buffer* cbs[1] = { im.cb.Get() };
		ID3D11ShaderResourceView* srvs[5] = { luma.from, luma.to, im.colBx.srv.Get(), im.rowBy.srv.Get(), im.cell.srv.Get() };
		ID3D11UnorderedAccessView* uavs[7] = { im.bin.uav.Get(), im.hist.uav.Get(), im.row.uav.Get(), im.pts.uav.Get(), im.res.uav.Get(),
			im.part.uav.Get(), im.state.uav.Get() };
		ctx->CSSetConstantBuffers(0, 1, cbs);
		ctx->CSSetShaderResources(0, 5, srvs);
		ctx->CSSetUnorderedAccessViews(0, 7, uavs, nullptr);
		auto run = [&](Kernel k, UINT x, UINT y) {
			ctx->CSSetShader(im.cs[k].Get(), nullptr, 0);
			ctx->Dispatch(x, y, 1);
		};
		run(kClear, 64, 1);
		run(kCand, static_cast<UINT>((w + 63) / 64), static_cast<UINT>((h + 63) / 64));
		run(kThr, 1, 1);
		run(kRowCount, static_cast<UINT>(h), 1);
		run(kRowPrefix, 1, 1);
		run(kCompact, static_cast<UINT>(h), 1);
		for (uint32_t pass = 0; pass <= maxIter; ++pass)
		{
			run(kResid, (target + 63u) / 64u, 1);
			run(kMedian, 1, 1);
			if (pass < maxIter)
			{
				run(kNormal, numGroups, 1);
				run(kSolve, 1, 1);
			}
		}
		run(kVerify, numGroups, 1);
		run(kFinal, 1, 1);
		ID3D11UnorderedAccessView* nullUav[7] = {};
		ID3D11ShaderResourceView* nullSrv[5] = {};
		ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
		ctx->CSSetShaderResources(0, 5, nullSrv);
		ctx->CSSetShader(nullptr, nullptr, 0);
		if (im.timed)
		{
			ctx->End(im.tsEnd.Get());
			ctx->End(im.disjoint.Get());
		}
		ctx->CopyResource(im.staging.Get(), im.state.buf.Get());
		ctx->End(im.done.Get());
		ctx->Flush();
		im.pending = true;
		im.isFinal = false;
		return true;
	}

	bool PhotometricRefinerGpu::Collect(ID3D11DeviceContext* context, bool wait, RefineResult* out)
	{
		Impl& im = *m_impl;
		if (im.isFinal)
		{
			// Nothing was queued: the precheck's status, the input model unchanged (as the CPU reference).
			RefineResult& res = im.result;
			res.model = im.model;
			res.gain = 1;
			res.offset = 0;
			m_counters.Add(res.status);
			im.isFinal = false;
		}
		if (!im.pending)
		{
			if (out)
				*out = im.result;
			return true;
		}
		if (!context)
			return false;
		for (;;)
		{
			const HRESULT hr = context->GetData(im.done.Get(), nullptr, 0, 0);
			if (hr == S_OK)
				break;
			if (FAILED(hr))
			{
				m_lastError = "event query failed";
				break;
			}
			if (!wait)
				return false;
			std::this_thread::yield();
		}
		im.pending = false;
		RefineResult& res = im.result;
		const RefineOptions& o = m_options;
		uint32_t st[kStateSlots * 2] = {};
		D3D11_MAPPED_SUBRESOURCE map = {};
		bool read = SUCCEEDED(context->Map(im.staging.Get(), 0, D3D11_MAP_READ, 0, &map));
		if (read)
		{
			std::memcpy(st, map.pData, sizeof(st));
			context->Unmap(im.staging.Get(), 0);
		}
		if (im.timed)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
			UINT64 t0 = 0, t1 = 0;
			while (context->GetData(im.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE)
				std::this_thread::yield();
			if (context->GetData(im.tsBegin.Get(), &t0, sizeof(t0), 0) == S_OK && context->GetData(im.tsEnd.Get(), &t1, sizeof(t1), 0) == S_OK
				&& !dj.Disjoint && dj.Frequency > 0)
			{
				m_lastTiming.valid = true;
				m_lastTiming.gpuMs = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
			}
		}

		const Params& model = im.model;
		const Rect& pic = im.picture;
		if (!read)
		{
			res.status = RefineStatus::SkippedNoLuma;
			Fail(RefineGpuFailure::Readback);
			m_lastError = "readback failed";
		}
		else
		{
			res.candidates = st[kCounts * 2];
			res.pixels = st[kCounts * 2 + 1];
			const bool fewPixels = st[kFlags * 2] != 0;
			const bool degenerate = st[kFlags * 2 + 1] != 0;
			if (fewPixels)
			{
				res.pixels = 0;
				res.status = RefineStatus::SkippedFewPixels;
			}
			else
			{
				Params p = model;
				for (int q = 0; q < 9; ++q)
					p.h[q] = LoadD(&st[(kP + q) * 2]);
				const double gain = LoadD(&st[kGain * 2]);
				const double offset = LoadD(&st[kOffset * 2]);
				res.iterations = static_cast<int>(st[kPass * 2 + 1]);
				res.lastStepPx = LoadD(&st[kLast * 2]);
				res.robustSigma = LoadD(&st[kSigma * 2]);
				if (degenerate || IsDegenerate(p, pic) || !IsFiniteBits(gain) || !IsFiniteBits(offset))
					res.status = RefineStatus::FellBackDegenerate;
				else
				{
					res.correctionPx = MaxCornerDifference(p, model, pic) * im.diag;
					const double before = LoadD(&st[kBefore * 2]);
					const double after = LoadD(&st[kAfter * 2]);
					const double both = LoadD(&st[kBoth * 2]);
					if (!(both >= static_cast<double>((std::max)(1, o.minPixels))))
						res.status = RefineStatus::FellBackDegenerate;
					else
					{
						res.residualBefore = before / both;
						res.residualAfter = after / both;
						if (o.fadeTerms && !(gain >= o.minGain && gain <= o.maxGain))
							res.status = RefineStatus::FellBackPhotometric;
						else if (res.correctionPx > o.maxCorrectionLumaPx * im.s)
							res.status = RefineStatus::FellBackTooFar;
						else if (!(res.residualAfter < res.residualBefore * (1.0 - o.minImprovement)))
							res.status = RefineStatus::FellBackNoImprovement;
						else
						{
							res.status = RefineStatus::Refined;
							res.model = p;
							res.gain = gain;
							res.offset = offset;
						}
					}
				}
			}
		}
		if (res.status != RefineStatus::Refined)
		{
			res.model = model;
			res.gain = 1;
			res.offset = 0;
		}
		res.model.type = model.type;
		m_counters.Add(res.status);
		if (out)
			*out = res;
		return true;
	}

	RefineResult PhotometricRefinerGpu::Refine(ID3D11DeviceContext* context, const Params& model, const RefineGpuLuma& luma, const Rect& picture,
		const InlierMask& mask)
	{
		Submit(context, model, luma, picture, mask);
		RefineResult r;
		Collect(context, true, &r);
		return r;
	}
}
