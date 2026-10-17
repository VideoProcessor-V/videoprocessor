#pragma once

#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <Windows.h>
#include <cstdint>
#include <array>

// DeckLink r210 carries the HDMI 8-bit RGB tunnel. Each pixel contains one
// 12-bit I sample and one alternating 12-bit chroma sample.
class CR210DolbyVisionTunnelFormatter final : public IVideoFrameFormatter
{
public:
	// Explicit dispatch also permits the scalar and SIMD paths to be qualified.
	enum class ConversionMethod { AUTO, SCALAR, AVX2 };
	void SetConversionMethod(ConversionMethod method) { m_conversionMethod = method; }
	// dv_tunnel_fixes (bug hunt 2026-10-17, restore switch, default on): the
	// whole-raster SDR alignment probe, the grey SDR exit, the loss limit in
	// source pictures and L2 trims before L8. Off = the formatter before them.
	void SetBugHuntFixes(bool on) { m_bugHuntFixes = on; }
	bool BugHuntFixes() const { return m_bugHuntFixes; }
	CR210DolbyVisionTunnelFormatter();
	~CR210DolbyVisionTunnelFormatter() override;
	CR210DolbyVisionTunnelFormatter(const CR210DolbyVisionTunnelFormatter&) = delete;
	CR210DolbyVisionTunnelFormatter& operator=(const CR210DolbyVisionTunnelFormatter&) = delete;
	void OnVideoState(VideoStateComPtr& state) override;
	bool FormatVideoFrame(const VideoFrame& frame, BYTE* output) override;
	// dv_tunnel_gpu_unpack: everything FormatVideoFrame does except writing
	// the planes. The DM packet is found and CRC-checked reading only its own
	// pixels (plus a sparse whole-picture alignment probe while the placement
	// is unknown, Bug hunt 2026-10-17); the metadata and
	// contract are then current exactly as after FormatVideoFrame.
	//   REJECTED  FormatVideoFrame would return false
	//   BLACK     FormatVideoFrame would write zeros (no lock / no valid DM)
	//   PICTURE   FormatVideoFrame would unpack every pixel: the caller unpacks
	//             the raw frame itself (GpuDolbyVisionTunnelUnpack) with
	//             PlanesHighAligned()
	enum class FrameResult { REJECTED, BLACK, PICTURE };
	FrameResult AnalyzeFrame(const VideoFrame& frame);
	// The RGB8 placement inside the r210 words the planes use: true = the high
	// eight bits of each 10-bit channel, false = the low eight.
	bool PlanesHighAligned() const { return m_alignment == ByteAlignment::LEFT; }
	// The planes FormatVideoFrame writes for a picture AnalyzeFrame returned
	// PICTURE for, from that picture's raw frame and placement (the GPU
	// unpack's fallback for a frame already handed on raw), with the same row
	// workers. Not concurrent with FormatVideoFrame (callers hold one lock).
	bool UnpackPicture(const void* source, BYTE* output, bool highAligned);
	LONG GetOutFrameSize() const override { return m_outputSize; }
	VideoFrameFormatterOutputContract GetOutputContract() const override
	{
		return { m_metadata.fullRange ? VideoFrameSampleRange::FULL :
			VideoFrameSampleRange::LIMITED, 12, 4 };
	}
	DolbyVisionTunnelFrameMetadata GetDolbyVisionTunnelMetadata() const override
	{
		auto metadata = m_metadata;
		metadata.allowSdrFallback = !m_tunnelConfirmed;
		metadata.requiresBlackFrame = m_tunnelConfirmed &&
			(!m_currentPictureHasValidHeader || !metadata.valid);
		return metadata;
	}

private:
	static constexpr uint32_t MAX_WORKERS = 2;
	enum class ByteAlignment { UNKNOWN, LEFT, RIGHT };
	// audit 2026-10-16: DeckLink 10-bit RGB containers that can carry the
	// RGB8 tunnel. R10b/R10l words are normalised to r210's [00 R G B]
	// layout (word >> 2) for packet parsing; the unpack kernels use the
	// equivalent constant shift (+2). Only CRC-valid packets prove that the
	// card stored the tunnel bytes untouched in these containers.
	enum class WordLayout { R210, R10_BIG_ENDIAN, R10_LITTLE_ENDIAN };
	uint32_t ReadTunnelWord(const uint8_t* source, size_t pixel) const;
	bool ReadPacketCopies(const uint8_t* source, size_t index, bool leftAligned,
		uint8_t* packet);
	bool LooksLikeTunnel(const uint8_t* source) const;
	void BeginTitle();
	void LogTitleLevels(const char* reason);
	// Every row, split over the row workers at 1080p and above.
	void ConvertPicture(const uint8_t* source, uint16_t* output, bool leftAligned);
	void ConvertRows(const uint8_t* source, uint16_t* output,
		uint32_t firstRow, uint32_t rowCount, bool leftAligned) const;
	void ConvertRowsAVX2(const uint8_t* source, uint16_t* output,
		uint32_t firstRow, uint32_t rowCount, bool leftAligned) const;
	static void CALLBACK ConversionWorkCallback(
		PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work);
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_stride = 0;
	LONG m_outputSize = 0;
	ByteAlignment m_alignment = ByteAlignment::UNKNOWN;
	uint32_t m_missingPacketFrames = 0;   // source pictures since the first loss
	bool m_missingPacketReported = false;
	// Bug hunt 2026-10-17: source counter of the first picture of the current
	// loss run (the loss limit counts source pictures, not analysed ones).
	bool m_hasPacketLoss = false;
	bool m_bugHuntFixes = true;   // dv_tunnel_fixes
	uint64_t m_packetLossStartCounter = 0;
	bool m_tunnelConfirmed = false;
	bool m_currentPictureHasValidHeader = false;
	uint32_t m_diagnosticFrames = 0;
	uint32_t m_crcFailedCopies = 0;
	uint32_t m_recoveredPackets = 0;
	uint32_t m_rejectedFrames = 0;
	uint32_t m_noMetadataFrames = 0;
	size_t m_lastPacketCount = 0;
	size_t m_lastMetadataLength = 0;
	uint32_t m_metadataIdTransitions = 0;
	uint8_t m_lastCurrentId = 0;
	uint8_t m_lastAffectedId = 0;
	bool m_haveDiagnosticIds = false;
	std::array<bool, 256> m_seenExtensionLevels{};
	DolbyVisionTunnelFrameMetadata m_metadata;
	DolbyVisionTunnelFrameMetadata m_pendingMetadata;
	bool m_hasPendingMetadata = false;
	uint8_t m_pendingAffectedId = 0;
	uint8_t m_pendingCurrentId = 0;
	uint64_t m_pendingAfterCounter = 0;
	uint8_t m_activeMetadataId = 0;
	bool m_hasActiveMetadataId = false;
	uint64_t m_endOfStreamAfterCounter = 0;
	bool m_hasEndOfStream = false;
	// audit 2026-10-16: robustness / coverage state (see .cpp).
	WordLayout m_wordLayout = WordLayout::R210;
	bool m_tunnelCapable = true;          // false for interlaced modes
	double m_frameRate = 24000.0 / 1001.0;
	uint32_t m_packetLossLimit = 24;      // ~1.001 s, derived from frame rate
	uint64_t m_sdrFallbackFrames = 48;    // ~2 s, derived from frame rate
	bool m_sdrSuspect = false;
	uint64_t m_sdrSuspectStartCounter = 0;
	uint64_t m_lastHeaderCounter = 0;
	bool m_hasLastHeaderCounter = false;
	uint32_t m_idAmbiguityResets = 0;
	uint32_t m_majorityRecoveries = 0;
	std::array<uint16_t, 256> m_titleLevelCounts{};
	bool m_titleLevelsLogged = false;
	bool m_titleTrimLogged[2] = {};       // [0]=L2, [1]=L8
	bool m_hasAVX2 = false;
	ConversionMethod m_conversionMethod = ConversionMethod::AUTO;
	PTP_WORK m_conversionWork[MAX_WORKERS] = {};
	uint32_t m_workerCount = 0;
	const uint8_t* m_workerSource = nullptr;
	uint16_t* m_workerOutput = nullptr;
	bool m_workerLeftAligned = false;
	uint32_t m_workerFirstRow[MAX_WORKERS] = {};
	uint32_t m_workerRowCount[MAX_WORKERS] = {};
};
