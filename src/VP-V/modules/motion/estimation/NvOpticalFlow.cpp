#include <pch.h>
#include "NvOpticalFlow.h"

#include "../../../../../3rdparty/nvidia-optical-flow/nvOpticalFlowD3D11.h"

#include <memory>
#include <vector>

namespace NvOpticalFlow
{
	namespace
	{
		using CreateInstance = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
		NV_OF_D3D11_API_FUNCTION_LIST* List(void* p) { return static_cast<NV_OF_D3D11_API_FUNCTION_LIST*>(p); }
	}

	Engine::~Engine()
	{
		Close();
		// nvofapi64.dll stays loaded for the process: unloading the driver's library
		// while its threads may still run is unsafe, and it is loaded once anyway.
		delete List(functions);
	}

	void Engine::Close()
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

	bool Engine::Open(ID3D11Device* deviceIn, ID3D11DeviceContext* contextIn, int w, int h, Preset preset, bool wantCosts)
	{
		Close();
		error.clear();
		if (!deviceIn || !contextIn || w <= 0 || h <= 0)
		{
			error = "no device or size";
			return false;
		}
		if (!module)
		{
			module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!module)
			{
				error = "nvofapi64.dll not found (no NVIDIA driver with the optical flow engine)";
				return false;
			}
		}
		if (!functions)
		{
			const auto create = reinterpret_cast<CreateInstance>(
				GetProcAddress(static_cast<HMODULE>(module), "NvOFAPICreateInstanceD3D11"));
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
		if (f->nvCreateOpticalFlowD3D11(deviceIn, contextIn, &h0) != NV_OF_SUCCESS || !h0)
		{
			error = "this GPU has no optical flow engine (or it is busy)";
			return false;
		}
		handle = h0;
		device = deviceIn;
		context = contextIn;
		width = w;
		height = h;
		costs = wantCosts;
		NV_OF_INIT_PARAMS init{};
		init.width = static_cast<uint32_t>(w);
		init.height = static_cast<uint32_t>(h);
		init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
		init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
		init.mode = NV_OF_MODE_OPTICALFLOW;
		init.perfLevel = preset == Preset::Slow ? NV_OF_PERF_LEVEL_SLOW : preset == Preset::Fast ? NV_OF_PERF_LEVEL_FAST :
			NV_OF_PERF_LEVEL_MEDIUM;
		init.enableExternalHints = NV_OF_FALSE;
		init.enableOutputCost = wantCosts ? NV_OF_TRUE : NV_OF_FALSE;
		init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
		init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
		const NV_OF_STATUS status = f->nvOFInit(static_cast<NvOFHandle>(handle), &init);
		if (status != NV_OF_SUCCESS)
		{
			char text[256] = {};
			uint32_t size = sizeof(text);
			f->nvOFGetLastError(static_cast<NvOFHandle>(handle), text, &size);
			error = std::string("optical flow init failed: ") + text;
			Close();
			return false;
		}
		D3D11_QUERY_DESC q{};
		q.Query = D3D11_QUERY_EVENT;
		queryStart.Release();
		device->CreateQuery(&q, &queryStart);
		return true;
	}

	void* Engine::Registered(ID3D11Texture2D* texture)
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

	bool Engine::Run(ID3D11Texture2D* from, ID3D11Texture2D* to, ID3D11Texture2D* forward, ID3D11Texture2D* backward,
		ID3D11Texture2D* forwardCost, ID3D11Texture2D* backwardCost, Result* result, bool continuous)
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
		void* cost = costs && forwardCost ? Registered(forwardCost) : nullptr;
		void* bwdCost = costs && backwardCost ? Registered(backwardCost) : nullptr;
		if (!in || !ref || !fwd || !bwd || (costs && (!cost || !bwdCost)))
		{
			error = "a texture was not accepted by the optical flow engine";
			return false;
		}
		NV_OF_EXECUTE_INPUT_PARAMS input{};
		input.inputFrame = static_cast<NvOFGPUBufferHandle>(in);
		input.referenceFrame = static_cast<NvOFGPUBufferHandle>(ref);
		input.disableTemporalHints = continuous ? NV_OF_FALSE : NV_OF_TRUE;
		NV_OF_EXECUTE_OUTPUT_PARAMS output{};
		output.outputBuffer = static_cast<NvOFGPUBufferHandle>(fwd);
		output.bwdOutputBuffer = static_cast<NvOFGPUBufferHandle>(bwd);
		output.outputCostBuffer = static_cast<NvOFGPUBufferHandle>(cost);
		output.bwdOutputCostBuffer = static_cast<NvOFGPUBufferHandle>(bwdCost);
		// Timed (tests, the self-check): the GPU idle first, then the wall
		// time to the engine's completion (it runs beside the graphics queue,
		// so graphics timestamps would not see it).
		const bool timed = result && queryStart;
		LARGE_INTEGER t0{}, t1{}, frequency{};
		if (timed)
		{
			context->End(queryStart);
			context->Flush();
			while (context->GetData(queryStart, nullptr, 0, 0) == S_FALSE) {}
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&t0);
		}
		const NV_OF_STATUS status = List(functions)->nvOFExecute(static_cast<NvOFHandle>(handle), &input, &output);
		if (status != NV_OF_SUCCESS)
		{
			char text[256] = {};
			uint32_t size = sizeof(text);
			List(functions)->nvOFGetLastError(static_cast<NvOFHandle>(handle), text, &size);
			error = std::string("optical flow failed: ") + text;
			return false;
		}
		if (timed)
		{
			context->End(queryStart);
			context->Flush();
			while (context->GetData(queryStart, nullptr, 0, 0) == S_FALSE) {}
			QueryPerformanceCounter(&t1);
			result->gpuMs = frequency.QuadPart > 0 ?
				static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
		}
		return true;
	}
}
