#include "pch.h"
#include "CppUnitTest.h"

#include <blackmagic_decklink/BlackMagicDeckLinkTranslate.h>
#include <DebugLogRetention.h>
#include <DisplayTopologySession.h>
#include <RendererResetPolicy.h>
#include <VideoState.h>

#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Bug hunt 2026-10-17: CPU checks for the capture/application-lifecycle
// fixes - VideoState reference count start, display mode ranking (area, then
// refresh, no interlaced), DeckLink Rec.601 line-count mapping, in-session
// log rollover and the renderer failure retry schedule.
namespace VideoProcessorTest
{
	namespace
	{
		std::string MakeRolloverDirectory()
		{
			char temporaryRoot[MAX_PATH] = {};
			GetTempPathA(ARRAYSIZE(temporaryRoot), temporaryRoot);
			const std::string path = std::string(temporaryRoot) +
				"VideoProcessor-log-rollover-" +
				std::to_string(GetCurrentProcessId()) + "-" +
				std::to_string(GetTickCount64());
			CreateDirectoryA(path.c_str(), nullptr);
			return path;
		}

		void WriteText(const std::string& path, const std::string& text)
		{
			std::ofstream file(path, std::ios::out | std::ios::trunc | std::ios::binary);
			file << text;
		}

		std::string ReadText(const std::string& path)
		{
			std::ifstream file(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(file),
				std::istreambuf_iterator<char>());
		}

		bool Exists(const std::string& path)
		{
			return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		}

		DisplayTopologySession::DisplayModeCandidate Mode(
			uint32_t width, uint32_t height, uint32_t refreshHz,
			bool interlaced = false, uint32_t bitsPerPixel = 32)
		{
			DisplayTopologySession::DisplayModeCandidate mode;
			mode.width = width;
			mode.height = height;
			mode.refreshHz = refreshHz;
			mode.bitsPerPixel = bitsPerPixel;
			mode.interlaced = interlaced;
			return mode;
		}

		// The ranking loop of ApplyMaximumMode, over plain candidates.
		DisplayTopologySession::DisplayModeCandidate Best(
			const std::vector<DisplayTopologySession::DisplayModeCandidate>& modes, bool fixes = true)
		{
			DisplayTopologySession::DisplayModeCandidate best;
			bool found = false;
			for (const auto& mode : modes)
			{
				if (!DisplayTopologySession::IsMaximumModeCandidate(mode, fixes))
					continue;
				if (!found || DisplayTopologySession::DisplayModeRanksAbove(mode, best, fixes))
				{
					best = mode;
					found = true;
				}
			}
			return best;
		}
	}

	TEST_CLASS(BugHunt20261017CaptureTests)
	{
	public:

		TEST_METHOD(VideoStateReferenceCountStartsAtZero)
		{
			VideoState* state = new VideoState();
			Assert::AreEqual<ULONG>(1, state->AddRef());
			Assert::AreEqual<ULONG>(2, state->AddRef());
			Assert::AreEqual<ULONG>(1, state->Release());
			Assert::AreEqual<ULONG>(0, state->Release());  // deletes
		}

		TEST_METHOD(VideoStateCopyHasItsOwnReferenceCount)
		{
			VideoState* original = new VideoState();
			original->valid = true;
			original->AddRef();
			original->AddRef();
			VideoState* copy = new VideoState(*original);
			Assert::IsTrue(copy->valid);
			Assert::AreEqual<ULONG>(1, copy->AddRef());
			Assert::AreEqual<ULONG>(0, copy->Release());
			Assert::AreEqual<ULONG>(1, original->Release());
			Assert::AreEqual<ULONG>(0, original->Release());
		}

		TEST_METHOD(DisplayModeRankingPrefersAreaThenRefresh)
		{
			// A low-resolution high-refresh mode used to beat the native one.
			const auto best = Best({
				Mode(1920, 1080, 60),
				Mode(1280, 720, 144),
				Mode(3840, 2160, 30),
				Mode(3840, 2160, 60),
				Mode(2560, 1440, 120) });
			Assert::AreEqual<uint32_t>(3840, best.width);
			Assert::AreEqual<uint32_t>(2160, best.height);
			Assert::AreEqual<uint32_t>(60, best.refreshHz);
		}

		TEST_METHOD(DisplayModeRankingSkipsInterlacedAndLowDepth)
		{
			const auto best = Best({
				Mode(1920, 1080, 60, true),       // 1080i
				Mode(4096, 2160, 24, false, 16),  // below 32 bpp
				Mode(1920, 1080, 50),
				Mode(1920, 1080, 30) });
			Assert::AreEqual<uint32_t>(1920, best.width);
			Assert::AreEqual<uint32_t>(50, best.refreshHz);
			Assert::IsFalse(best.interlaced);
			Assert::IsFalse(DisplayTopologySession::IsMaximumModeCandidate(Mode(0, 1080, 60)));
			Assert::IsFalse(DisplayTopologySession::IsMaximumModeCandidate(Mode(1920, 1080, 0)));
		}

		TEST_METHOD(DeckLinkRec601MapsActiveLinesToTheStandard)
		{
			Assert::IsTrue(Translate(bmdColorspaceRec601, 486) == ColorSpace::REC_601_525);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 480) == ColorSpace::REC_601_525);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 525) == ColorSpace::REC_601_525);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 576) == ColorSpace::REC_601_625);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 625) == ColorSpace::REC_601_625);
			Assert::IsTrue(Translate(bmdColorspaceRec709, 1080) == ColorSpace::REC_709);
			// Hunt 2 (08 #4): an HD source flagged BT.601 keeps the BT.601 matrix
			// (SMPTE 170M) instead of throwing (no picture, an error box).
			Assert::IsTrue(Translate(bmdColorspaceRec601, 720) == ColorSpace::REC_601_525);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 1080) == ColorSpace::REC_601_525);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 2160) == ColorSpace::REC_601_525);
		}

		// Hunt 2 (08 #4): the flag decides; the line count only picks the BT.601 entry.
		TEST_METHOD(Rec601FlagAndAviColorimetryDecideTheMatrix)
		{
			Assert::IsTrue(Rec601ColorSpace(576) == ColorSpace::REC_601_625);
			Assert::IsTrue(Rec601ColorSpace(625) == ColorSpace::REC_601_625);
			Assert::IsTrue(Rec601ColorSpace(480) == ColorSpace::REC_601_525);
			Assert::IsTrue(Rec601ColorSpace(1080) == ColorSpace::REC_601_525);
			Assert::IsTrue(DecodeAviColorimetry(0, 0) == AviColorimetry::NoData);
			Assert::IsTrue(DecodeAviColorimetry(1, 6) == AviColorimetry::Bt601, L"EC is read only for C = 11");
			Assert::IsTrue(DecodeAviColorimetry(2, 0) == AviColorimetry::Bt709);
			Assert::IsTrue(DecodeAviColorimetry(3, 0) == AviColorimetry::Bt601);   // xvYCC601
			Assert::IsTrue(DecodeAviColorimetry(3, 1) == AviColorimetry::Bt709);   // xvYCC709
			Assert::IsTrue(DecodeAviColorimetry(3, 2) == AviColorimetry::Bt601);   // sYCC601
			Assert::IsTrue(DecodeAviColorimetry(3, 3) == AviColorimetry::Bt601);   // opYCC601
			Assert::IsTrue(DecodeAviColorimetry(3, 4) == AviColorimetry::Other);   // opRGB
			Assert::IsTrue(DecodeAviColorimetry(3, 5) == AviColorimetry::Bt2020);
			Assert::IsTrue(DecodeAviColorimetry(3, 6) == AviColorimetry::Bt2020);
			Assert::IsTrue(DecodeAviColorimetry(3, 7) == AviColorimetry::Other);
		}

		// Post-merge audit 2026-10-17: capture_display_fixes off restores Bill's earlier
		// ranking (refresh first, interlaced accepted) and Rec.601 mapping.
		TEST_METHOD(CaptureDisplayFixesOffRestoresTheEarlierBehaviour)
		{
			const auto best = Best({
				Mode(1920, 1080, 60),
				Mode(1280, 720, 144),
				Mode(3840, 2160, 60) }, false);
			Assert::AreEqual<uint32_t>(1280, best.width, L"off: the highest refresh first");
			Assert::AreEqual<uint32_t>(144, best.refreshHz);
			const auto interlaced = Best({ Mode(1920, 1080, 60, true), Mode(1920, 1080, 50) }, false);
			Assert::IsTrue(interlaced.interlaced, L"off: interlaced accepted");
			Assert::IsTrue(Translate(bmdColorspaceRec601, 486, false) == ColorSpace::UNKNOWN,
				L"off: the card's brief 486-line mode has no colour space (no exception)");
			Assert::IsTrue(Translate(bmdColorspaceRec601, 525, false) == ColorSpace::REC_601_525);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 576, false) == ColorSpace::REC_601_576);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 625, false) == ColorSpace::REC_601_625);
			Assert::ExpectException<std::runtime_error>([]() { Translate(bmdColorspaceRec601, 480, false); });
			// On: the 486 guard's protection is kept - the brief mode never throws.
			Assert::IsTrue(Translate(bmdColorspaceRec601, 486, true) == ColorSpace::REC_601_525);
		}

		TEST_METHOD(LogRolloverKeepsThreePartsApartFromSessionArchives)
		{
			const std::string directory = MakeRolloverDirectory();
			const std::string active = directory + "\\vp.log";
			const std::string sessionArchive = active + ".1";
			WriteText(sessionArchive, "previous session");
			std::vector<std::string> diagnostics;
			for (int part = 1; part <= 5; ++part)
			{
				WriteText(active, "part " + std::to_string(part));
				Assert::IsTrue(DebugLogRetention::RollOver(active, 3, diagnostics));
				Assert::IsFalse(Exists(active));
			}
			// Newest first: .r1 = part 5, .r2 = part 4, .r3 = part 3.
			Assert::AreEqual(std::string("part 5"), ReadText(active + ".r1"));
			Assert::AreEqual(std::string("part 4"), ReadText(active + ".r2"));
			Assert::AreEqual(std::string("part 3"), ReadText(active + ".r3"));
			Assert::IsFalse(Exists(active + ".r4"));
			// The indexed session archive is untouched and the parts are not
			// mistaken for archives by startup retention.
			Assert::AreEqual(std::string("previous session"), ReadText(sessionArchive));
			Assert::IsFalse(DebugLogRetention::IsArchiveFileName("vp.log", "vp.log.r1"));
			Assert::IsTrue(DebugLogRetention::ROLLOVER_BYTES >= 100ULL * 1024 * 1024);

			for (const char* name : { ".r1", ".r2", ".r3", ".1" })
				DeleteFileA((active + name).c_str());
			RemoveDirectoryA(directory.c_str());
		}

		TEST_METHOD(RendererFailureRetryBacksOffToThirtySeconds)
		{
			Assert::AreEqual<uint32_t>(2000, RendererFailureRetryDelayMs(0));
			Assert::AreEqual<uint32_t>(5000, RendererFailureRetryDelayMs(1));
			Assert::AreEqual<uint32_t>(10000, RendererFailureRetryDelayMs(2));
			Assert::AreEqual<uint32_t>(30000, RendererFailureRetryDelayMs(3));
			Assert::AreEqual<uint32_t>(30000, RendererFailureRetryDelayMs(1000));
			Assert::AreEqual<uint32_t>(10000, ShutdownBlockedRetirementDeadlineMs);
		}

		// Post-merge audit 2026-10-17: at most 10 automatic attempts, then manual Restart.
		TEST_METHOD(RendererFailureRetryIsCappedAtTenAttempts)
		{
			Assert::AreEqual<uint32_t>(10, RendererFailureRetryMaxAttempts);
			for (uint32_t made = 0; made < 10; ++made)
				Assert::IsTrue(RendererFailureRetryAllowed(made));
			Assert::IsFalse(RendererFailureRetryAllowed(10));
			Assert::IsFalse(RendererFailureRetryAllowed(11));
		}
	};
}
