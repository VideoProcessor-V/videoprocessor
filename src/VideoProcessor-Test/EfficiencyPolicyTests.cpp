#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/EarlyUploadPolicy.h>
#include <vprenderer/FeatureTextureIdle.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Efficiency audit 2026-10-24: #3 (early upload in all modes, VRAM guard, visible fallback) and
// #8b (feature textures freed after idle, with hysteresis). Byte identity of both on the GPU:
// docs/design/efficiency-byte-identical-tests.md.
namespace VideoProcessorTest
{
	namespace
	{
		constexpr uint64_t MB = uint64_t(1) << 20;
		constexpr uint64_t GB = uint64_t(1) << 30;
	}

	TEST_CLASS(EfficiencyPolicyTests)
	{
	public:
		TEST_METHOD(EarlyUploadAutoIsEarlyWithoutLowLatency)
		{
			EarlyUploadPolicy::Input in;
			in.mode = EarlyUploadPolicy::Mode::Auto;
			in.slots = 8;
			in.frameBytes = 33 * MB;          // 4K r210
			in.budgetBytes = 22 * GB;         // RTX 3090
			in.usageBytes = 3 * GB;
			const EarlyUploadPolicy::Decision d = EarlyUploadPolicy::Decide(in);
			Assert::IsTrue(d.early);
			Assert::IsTrue(d.reason == EarlyUploadPolicy::Reason::Active);
			Assert::IsTrue(d.FallbackReason().empty());
			Assert::AreEqual(static_cast<unsigned long long>(8 * 33 * MB), static_cast<unsigned long long>(d.needBytes));
		}

		TEST_METHOD(EarlyUploadMemoryShortIsAVisibleFallback)
		{
			EarlyUploadPolicy::Input in;
			in.slots = 10;
			in.frameBytes = 33 * MB;          // 330 MB by need
			in.budgetBytes = 3584 * MB;       // 4 GB laptop budget
			in.usageBytes = 2800 * MB;        // reserve 896 MB -> nothing free
			EarlyUploadPolicy::Decision d = EarlyUploadPolicy::Decide(in);
			Assert::IsFalse(d.early);
			Assert::IsTrue(d.reason == EarlyUploadPolicy::Reason::MemoryShort);
			const std::string why = d.FallbackReason();
			Assert::IsTrue(why.find("render upload: video memory short, needs 330 MB") == 0);
			// An open pool's own bytes are in the usage: it is not counted against itself.
			in.usageBytes = 2200 * MB + 330 * MB;
			in.poolBytesHeld = 330 * MB;
			d = EarlyUploadPolicy::Decide(in);
			Assert::IsTrue(d.early);
			// An explicit "early" is guarded too; "render" is never a fallback.
			in.mode = EarlyUploadPolicy::Mode::Early;
			in.usageBytes = 3500 * MB;
			in.poolBytesHeld = 0;
			Assert::IsTrue(EarlyUploadPolicy::Decide(in).reason == EarlyUploadPolicy::Reason::MemoryShort);
			in.mode = EarlyUploadPolicy::Mode::Render;
			d = EarlyUploadPolicy::Decide(in);
			Assert::IsFalse(d.early);
			Assert::IsTrue(d.FallbackReason().empty());
		}

		TEST_METHOD(EarlyUploadWithoutABudgetKeepsTheByteCap)
		{
			EarlyUploadPolicy::Input in;
			in.slots = 6;
			in.frameBytes = 12 * MB;
			EarlyUploadPolicy::Decision d = EarlyUploadPolicy::Decide(in);
			Assert::IsTrue(d.early && d.reason == EarlyUploadPolicy::Reason::NoBudget);
			in.openFailed = true;
			d = EarlyUploadPolicy::Decide(in);
			Assert::IsFalse(d.early);
			Assert::IsFalse(d.FallbackReason().empty());
			in.openFailed = false;
			in.device = false;
			Assert::IsTrue(EarlyUploadPolicy::Decide(in).reason == EarlyUploadPolicy::Reason::NoDevice);
		}

		TEST_METHOD(IdleTexturesAreReleasedAfterTheIdleTime)
		{
			FeatureTextureIdle::Releaser r;
			Assert::IsFalse(r.Note(false, true, 0.0), L"never active: not this releaser's textures");
			Assert::IsFalse(r.Note(true, true, 1.0));
			Assert::IsFalse(r.Note(false, true, 5.0));
			Assert::IsFalse(r.Note(false, true, 10.9));
			Assert::IsTrue(r.Note(false, true, 11.0));
			Assert::IsFalse(r.Note(false, false, 12.0), L"released once");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(r.Releases()));
		}

		TEST_METHOD(ThrashingDoublesTheIdleTime)
		{
			FeatureTextureIdle::Releaser r;
			double t = 0.0;
			// A feature that toggles every 12 s: released, back within 30 s -> idle doubles.
			for (int cycle = 0; cycle < 6; ++cycle)
			{
				r.Note(true, true, t);
				for (double s = 1.0; s <= 12.0; s += 1.0)
					r.Note(false, true, t + s);
				t += 13.0;
			}
			Assert::IsTrue(r.IdleSeconds() > 12.0, L"backed off past the toggle period");
			Assert::IsTrue(r.Releases() <= 2ull);
			Assert::IsTrue(r.ThrashBackoffs() >= 1ull);
			// Memory pressure uses the minimum idle time.
			FeatureTextureIdle::Releaser p;
			p.Note(true, true, 0.0);
			Assert::IsTrue(p.Note(false, true, 10.0, true));
		}
	};
}
