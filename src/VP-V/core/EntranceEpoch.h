#pragma once

#include <cstdint>
#include <cstring>

// The key of a cached entrance result (barrier B3; skeleton audit blocker 1, 2026-10-24).
//
// The entrance cache (EntranceStages::Cache) may hand back a stage result instead of re-running
// the stages, so its key must cover EVERYTHING the result depends on. The first version keyed
// the input textures and the stage parameters but not the frame's crop, representation (range,
// bit depth) or colour, nor the detail stages' vector field and bitrate quality: a repeat could
// show an old crop or old levels (start-up crop hold, range re-detection). The frame part is
// now one templated function over the frame's fields (pl_frame in the renderer, a small struct
// in the CPU tests), so the tested code is the code the renderer runs.
//
// Header-only, C++14, no libplacebo.
namespace EntranceEpoch
{
	// FNV-1a style mixing; collisions are irrelevant at one cached entry, the point is that any
	// changed input changes the key.
	class Key
	{
	public:
		Key& Add(uint64_t value) { h = (h ^ value) * 0x100000001B3ull; return *this; }
		Key& Add(int value) { return Add(static_cast<uint64_t>(static_cast<int64_t>(value))); }
		Key& Add(double value)
		{
			uint64_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return Add(bits);
		}
		Key& Add(float value) { return Add(static_cast<double>(value)); }
		Key& Add(const void* pointer) { return Add(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer))); }
		uint64_t Value() const { return h; }

	private:
		uint64_t h = 0xCBF29CE484222325ull;
	};

	// Every field of the frame the stages read or pass on: its planes (texture identity and
	// component mapping), crop, representation and colour.
	template <class Frame>
	void AddFrame(Key& key, const Frame& frame)
	{
		const int planeCapacity = static_cast<int>(sizeof(frame.planes) / sizeof(frame.planes[0]));
		key.Add(frame.num_planes);
		for (int i = 0; i < frame.num_planes && i < planeCapacity; ++i)
		{
			key.Add(static_cast<const void*>(frame.planes[i].texture));
			key.Add(frame.planes[i].components);
			for (int c = 0; c < 4; ++c)
				key.Add(frame.planes[i].component_mapping[c]);
		}
		key.Add(frame.crop.x0).Add(frame.crop.y0).Add(frame.crop.x1).Add(frame.crop.y1);
		key.Add(static_cast<int>(frame.repr.sys)).Add(static_cast<int>(frame.repr.levels)).Add(static_cast<int>(frame.repr.alpha));
		key.Add(frame.repr.bits.sample_depth).Add(frame.repr.bits.color_depth).Add(frame.repr.bits.bit_shift);
		key.Add(static_cast<int>(frame.color.primaries)).Add(static_cast<int>(frame.color.transfer));
		key.Add(frame.color.hdr.min_luma).Add(frame.color.hdr.max_luma);
	}
}
