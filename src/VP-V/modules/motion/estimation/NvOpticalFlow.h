#pragma once

// NVIDIA optical flow engine (NVOFA) as a motion source (owner 2026-10-14:
// an alternative, parallel path to VP's own matcher, feeding the same trust
// checks; auto only where it proves itself). This wraps the engine's D3D11
// interface: it loads nvofapi64.dll from the driver at run time (nothing is
// shipped), runs a frame pair in both directions with a confidence (cost)
// per vector, and reports why it is unavailable otherwise. The interface
// headers are NVIDIA's MIT-licensed ones (3rdparty/nvidia-optical-flow).

#include <d3d11.h>
#include <atlbase.h>

#include <cstdint>
#include <map>
#include <string>

namespace NvOpticalFlow
{
	enum class Preset { Slow, Medium, Fast };

	// SK-MV (doc 83, section 5): the runtime ONE-ENGINE GUARD. The vector engine owns exactly one NVOF session (Engine) per GPU: the
	// look-ahead producer's. Every Engine and HintedEngine counts itself here while its driver session is open, keyed by the GPU
	// (the adapter's LUID, so a second device on the same GPU is the same GPU). Opening a second Engine on a GPU that already has
	// one is counted as a violation (the vector engine logs a rise, VectorEngine::PlanProducer); nothing is refused, so the guard changes no behaviour, and a test
	// asserts it stays at zero across the producer, the de-judder and the long-pair service. The long-pair HintedEngine is a
	// second session by design (hints on) and is counted apart; it is a governor decision, not a violation, until the service is wired.
	enum class SessionKind { Engine, Hinted };
	struct Sessions
	{
		// The adapter LUID as one word. A device whose adapter cannot be queried gets kUnknownAdapterKey (its own key, never 0: 0 means
		// "all GPUs" in Open / Peak, so an unknown GPU is counted as a GPU of its own and cannot hide a second session).
		static constexpr uint64_t kUnknownAdapterKey = ~uint64_t(0);
		static uint64_t AdapterKey(ID3D11Device* device);
		static void Opened(SessionKind kind, uint64_t adapter);
		static void Closed(SessionKind kind, uint64_t adapter);
		static int Open(SessionKind kind, uint64_t adapter);    // sessions of `kind` open on that GPU now (adapter 0: all GPUs)
		static int MostOpenOnOneGpu(SessionKind kind);          // the highest open count on any single GPU (the guard's "exceeds 1")
		static int Peak(SessionKind kind, uint64_t adapter);    // the most open at once since the process started (adapter 0: the highest of any GPU)
		static unsigned long long Violations();                  // second Engine sessions opened on a GPU that had one
		static void ResetForTest();                              // zeroes the table (tests only, nothing may be open)
	};

	struct Result
	{
		double gpuMs = 0.0;   // wall time from an idle GPU to the engine's completion (both directions)
	};

	class Engine
	{
	public:
		Engine() = default;
		Engine(const Engine&) = delete;
		Engine& operator=(const Engine&) = delete;
		~Engine();

		// The engine for frames of width x height on this device; false (and
		// LastError) when the driver, the GPU or the size is not supported.
		bool Open(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, Preset preset,
			bool costs = true);
		bool IsOpen() const { return handle != nullptr; }

		// Flow from `from` to `to` (forward) and back, one vector per 4x4
		// pixels (S10.5 fixed point, R16G16_SINT textures of width/4 x
		// height/4), and both costs (R8_UINT, higher = less sure; required when
		// opened with costs).
		// Inputs are R8_UNORM textures of the opened size.
		bool Run(ID3D11Texture2D* from, ID3D11Texture2D* to, ID3D11Texture2D* forward, ID3D11Texture2D* backward,
			ID3D11Texture2D* forwardCost, ID3D11Texture2D* backwardCost, Result* result = nullptr, bool continuous = true);

		// The texture formats the engine takes (for creating the textures).
		static DXGI_FORMAT InputFormat() { return DXGI_FORMAT_R8_UNORM; }
		static DXGI_FORMAT FlowFormat() { return DXGI_FORMAT_R16G16_SINT; }
		static DXGI_FORMAT CostFormat() { return DXGI_FORMAT_R8_UINT; }
		static constexpr int Grid = 4;

		const std::string& LastError() const { return error; }

	private:
		void* Registered(ID3D11Texture2D* texture);
		void Close();

		void* module = nullptr;      // nvofapi64.dll
		void* functions = nullptr;   // NV_OF_D3D11_API_FUNCTION_LIST
		void* handle = nullptr;      // NvOFHandle
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11DeviceContext> context;
		CComPtr<ID3D11Query> queryStart;   // an event query (timing)
		std::map<ID3D11Texture2D*, void*> registered;
		uint64_t sessionAdapter = 0;   // the GPU this session is counted under while its handle is open (Sessions)
		int width = 0, height = 0;
		bool costs = false;
		std::string error;
	};

	// MV7: bytes of the ring textures one engine's user holds (the sizes follow the formats above, one flow and one cost texel per
	// Grid x Grid pixels): two sets of input (R8, w x h), flow (R16G16_SINT, 4 B) and cost (R8_UINT, 1 B); `hint` one more flow-sized
	// texture (the long pair's temporal hint). `readback` adds the two CPU-readback staging copies of flow and cost (D3D11 STAGING
	// textures): those live in SYSTEM RAM, not video memory, so the VRAM ledger passes false (the producer and the long pair do). The
	// driver's own session memory is not visible to the application and is NOT counted (the governor line says "textures").
	inline uint64_t RingBytes(int w, int h, bool readback, bool hint)
	{
		if (w < 1 || h < 1)
			return 0;
		const uint64_t gw = static_cast<uint64_t>((w + Engine::Grid - 1) / Engine::Grid);
		const uint64_t gh = static_cast<uint64_t>((h + Engine::Grid - 1) / Engine::Grid);
		const uint64_t grid = gw * gh;
		uint64_t perSet = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) + grid * 4 + grid * 1;
		if (readback)
			perSet += grid * 4 + grid * 1;
		return 2 * perSet + (hint ? grid * 4 : 0);
	}
}
