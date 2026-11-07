// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "NvOpticalFlowHinted.h"

#include "../../../../../../3rdparty/nvidia-optical-flow/nvOpticalFlowD3D11.h"

#include <memory>

namespace NvOpticalFlow
{
	namespace
	{
		using CreateInstance = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
		NV_OF_D3D11_API_FUNCTION_LIST* List(void* p) { return static_cast<NV_OF_D3D11_API_FUNCTION_LIST*>(p); }
	}

	HintedEngine::~HintedEngine()
	{
		Close();
		delete List(functions);
	}

	void HintedEngine::Close()
	{
		NV_OF_D3D11_API_FUNCTION_LIST* f = List(functions);
		if (f)
			for (auto& entry : registered)
				f->nvOFUnregisterResourceD3D11(static_cast<NvOFGPUBufferHandle>(entry.second));
		registered.clear();
		if (f && handle)
			f->nvOFDestroy(static_cast<NvOFHandle>(handle));
		handle = nullptr;
	}

	bool HintedEngine::Open(ID3D11Device* device, ID3D11DeviceContext* context, int w, int h, Preset preset)
	{
		Close();
		error.clear();
		if (!device || !context || w <= 0 || h <= 0)
		{
			error = "no device or size";
			return false;
		}
		// The driver library stays loaded for the process (as in Engine); LoadLibraryEx returns the loaded module.
		HMODULE module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module)
		{
			error = "nvofapi64.dll not found (no NVIDIA driver with the optical flow engine)";
			return false;
		}
		if (!functions)
		{
			const auto create = reinterpret_cast<CreateInstance>(GetProcAddress(module, "NvOFAPICreateInstanceD3D11"));
			auto list = std::make_unique<NV_OF_D3D11_API_FUNCTION_LIST>();
			*list = {};
			if (!create || create(NV_OF_API_VERSION, list.get()) != NV_OF_SUCCESS)
			{
				error = "the driver's optical flow interface is older than API 5.0";
				return false;
			}
			functions = list.release();
		}
		NV_OF_D3D11_API_FUNCTION_LIST* f = List(functions);
		NvOFHandle h0 = nullptr;
		if (f->nvCreateOpticalFlowD3D11(device, context, &h0) != NV_OF_SUCCESS || !h0)
		{
			error = "this GPU has no optical flow engine (or it is busy)";
			return false;
		}
		handle = h0;
		// External hints must be supported for the optical flow mode, else the visible fallback takes the pair.
		uint32_t hintCap = 0, capSize = 1;
		if (!f->nvOFGetCaps || f->nvOFGetCaps(h0, NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE, &hintCap, &capSize) != NV_OF_SUCCESS || hintCap == 0)
		{
			error = "this driver / GPU does not support external hints for optical flow";
			Close();
			return false;
		}
		width = w;
		height = h;
		NV_OF_INIT_PARAMS init{};
		init.width = static_cast<uint32_t>(w);
		init.height = static_cast<uint32_t>(h);
		init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
		init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_4;   // hintGridSize must be >= outGridSize: same grid
		init.mode = NV_OF_MODE_OPTICALFLOW;
		init.perfLevel = preset == Preset::Slow ? NV_OF_PERF_LEVEL_SLOW : preset == Preset::Fast ? NV_OF_PERF_LEVEL_FAST :
			NV_OF_PERF_LEVEL_MEDIUM;
		init.enableExternalHints = NV_OF_TRUE;
		init.enableOutputCost = NV_OF_TRUE;
		init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
		init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
		const NV_OF_STATUS status = f->nvOFInit(h0, &init);
		if (status != NV_OF_SUCCESS)
		{
			char text[256] = {};
			uint32_t size = sizeof(text);
			f->nvOFGetLastError(h0, text, &size);
			error = std::string("optical flow (hinted) init failed: ") + text;
			Close();
			return false;
		}
		return true;
	}

	void* HintedEngine::Registered(ID3D11Texture2D* texture)
	{
		auto it = registered.find(texture);
		if (it != registered.end())
			return it->second;
		NvOFGPUBufferHandle buffer = nullptr;
		if (List(functions)->nvOFRegisterResourceD3D11(static_cast<NvOFHandle>(handle), texture, &buffer) != NV_OF_SUCCESS)
			return nullptr;
		registered[texture] = buffer;
		return buffer;
	}

	bool HintedEngine::Run(ID3D11Texture2D* from, ID3D11Texture2D* to, ID3D11Texture2D* hint, ID3D11Texture2D* forward,
		ID3D11Texture2D* backward, ID3D11Texture2D* forwardCost, ID3D11Texture2D* backwardCost)
	{
		if (!handle)
		{
			error = "not open";
			return false;
		}
		void* in = Registered(from);
		void* ref = Registered(to);
		void* fwd = Registered(forward);
		void* bwd = Registered(backward);
		void* cost = Registered(forwardCost);
		void* bwdCost = Registered(backwardCost);
		void* hints = hint ? Registered(hint) : nullptr;
		if (!in || !ref || !fwd || !bwd || !cost || !bwdCost || (hint && !hints))
		{
			error = "a texture was not accepted by the optical flow engine";
			return false;
		}
		NV_OF_EXECUTE_INPUT_PARAMS input{};
		input.inputFrame = static_cast<NvOFGPUBufferHandle>(in);
		input.referenceFrame = static_cast<NvOFGPUBufferHandle>(ref);
		input.externalHints = static_cast<NvOFGPUBufferHandle>(hints);
		input.disableTemporalHints = NV_OF_TRUE;   // independent pair
		NV_OF_EXECUTE_OUTPUT_PARAMS output{};
		output.outputBuffer = static_cast<NvOFGPUBufferHandle>(fwd);
		output.bwdOutputBuffer = static_cast<NvOFGPUBufferHandle>(bwd);
		output.outputCostBuffer = static_cast<NvOFGPUBufferHandle>(cost);
		output.bwdOutputCostBuffer = static_cast<NvOFGPUBufferHandle>(bwdCost);
		const NV_OF_STATUS status = List(functions)->nvOFExecute(static_cast<NvOFHandle>(handle), &input, &output);
		if (status != NV_OF_SUCCESS)
		{
			char text[256] = {};
			uint32_t size = sizeof(text);
			List(functions)->nvOFGetLastError(static_cast<NvOFHandle>(handle), text, &size);
			error = std::string("optical flow (hinted) failed: ") + text;
			return false;
		}
		return true;
	}
}
