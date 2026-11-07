// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The SECOND NVOF session of the vector engine: external hints on (NV_OF_INIT_PARAMS::enableExternalHints = TRUE, hint
// grid 4x4 = the output grid), for the I-to-I long pair. NvOpticalFlow::Engine (the first session: hints off, temporal
// hints on, used by the look-ahead producer and the render-time engine) is untouched. This class has its own handle
// and registered-texture map; the driver library is loaded once per process either way.
//
// Every pair here is independent (disableTemporalHints = TRUE): the long pair's distance is the measured GOP length and
// the previous Execute is not a predictor for it. A pair may run without a hint buffer (externalHints = null) when TD
// has no composed trail for it.
//
// GPU: needs an NVIDIA driver with the optical flow engine. CPU-only machines report LastError and the service takes the
// visible seeded-search fallback.
#pragma once

#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>

#include <d3d11.h>
#include <atlbase.h>

#include <map>
#include <string>

namespace NvOpticalFlow
{
	class HintedEngine
	{
	public:
		HintedEngine() = default;
		HintedEngine(const HintedEngine&) = delete;
		HintedEngine& operator=(const HintedEngine&) = delete;
		~HintedEngine();

		// Opens a session for width x height (8-bit grayscale inputs) with external hints; false + LastError when the
		// driver, the GPU, the size or the hint capability is not available.
		bool Open(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, Preset preset);
		bool IsOpen() const { return handle != nullptr; }

		// Flow from -> to (forward) and to -> from (backward), costs R8_UINT. `hint` is an R16G16_SINT texture of
		// ceil(w/4) x ceil(h/4) S10.5 vectors (forward A -> B, half-luma px) or null. All textures: the formats of
		// Engine::InputFormat/FlowFormat/CostFormat (FlowFormat also for the hint).
		bool Run(ID3D11Texture2D* from, ID3D11Texture2D* to, ID3D11Texture2D* hint, ID3D11Texture2D* forward,
			ID3D11Texture2D* backward, ID3D11Texture2D* forwardCost, ID3D11Texture2D* backwardCost);

		const std::string& LastError() const { return error; }

	private:
		void* Registered(ID3D11Texture2D* texture);
		void Close();

		void* functions = nullptr;   // NV_OF_D3D11_API_FUNCTION_LIST (own copy)
		void* handle = nullptr;
		std::map<ID3D11Texture2D*, void*> registered;
		int width = 0, height = 0;
		std::string error;
	};
}
