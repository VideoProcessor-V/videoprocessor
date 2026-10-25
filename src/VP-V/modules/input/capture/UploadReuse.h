#pragma once

#include <cstddef>
#include <cstdint>

// Efficiency round 2026-10-18 (efficiency_fixes): the de-judder uploads the next
// frame B (and, estimated ahead, C) for its motion estimate. When B becomes the
// frame shown, the renderer used to upload the same payload again into its own
// planes. With the fix the planes uploaded for the estimate are taken as A's
// planes instead (the textures are swapped): the same bytes, through the same
// upload or GPU unpack, into the same formats, so the picture is bit-identical
// and one source upload (and GPU unpack) per new frame is saved.
//
// Pure: the decision only. The renderer keeps one Record per plane pair (next,
// ahead), swapped with the textures they describe.
namespace UploadReuse
{
	// How a pair of planes was filled.
	struct Record
	{
		bool valid = false;
		const void* payload = nullptr;   // identity of the converted payload uploaded
		uint64_t sequence = 0;           // its source frame
		int width = 0, height = 0, chromaHeight = 0;
		size_t rowBytes = 0;
		bool raw = false;                // the GPU unpack route (capture_path gpu / DV tunnel)
		bool rgbLumaOnly = false;        // native RGB: the matcher's luma only (never reusable)
	};

	// The upload the renderer is about to make for the frame shown.
	struct Request
	{
		bool enabled = false;            // efficiency_fixes
		const void* payload = nullptr;
		uint64_t sequence = 0;
		int width = 0, height = 0, chromaHeight = 0;
		size_t rowBytes = 0;
		bool raw = false;
		bool texturesPresent = false;    // both planes of the pair exist
		bool uploadedSequenceMatches = false;   // the renderer's own record of the pair's frame agrees
	};

	// True when the pair already holds exactly the planes the upload would make.
	inline bool CanReuse(const Record& r, const Request& q)
	{
		return q.enabled && r.valid && !r.rgbLumaOnly && q.texturesPresent && q.uploadedSequenceMatches &&
			q.payload != nullptr && r.payload == q.payload &&
			q.sequence != 0 && r.sequence == q.sequence &&
			r.width == q.width && r.height == q.height && r.chromaHeight == q.chromaHeight &&
			r.rowBytes == q.rowBytes && r.raw == q.raw;
	}
}
