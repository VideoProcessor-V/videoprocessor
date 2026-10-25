#include "pch.h"
#include "CppUnitTest.h"

#include <ChangeKey.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Efficiency audit 2026-10-24 #10: ChangeKey produces exactly the text a default
// std::ostringstream produced at the render thread's change-signature sites, so every
// "log on change" line fires on exactly the same refreshes as before.
namespace VideoProcessorTest
{
	namespace
	{
		enum Unscoped { kZero = 0, kSeven = 7, kBig = 100000 };

		template <typename T>
		void Same(const T& value)
		{
			std::ostringstream o;
			o << value;
			ChangeKey k;
			k << value;
			Assert::AreEqual(o.str(), k.str());
		}
	}

	TEST_CLASS(ChangeKeyTests)
	{
	public:
		TEST_METHOD(MatchesOstringstreamPerType)
		{
			for (bool b : { false, true }) Same(b);
			for (int v : { 0, 1, -1, 42, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)() }) Same(v);
			for (long long v : { 0LL, -9000000000LL, (std::numeric_limits<long long>::max)() }) Same(v);
			for (unsigned long long v : { 0ULL, 18446744073709551615ULL }) Same(v);
			Same(size_t{ 123456789 });
			Same(uint64_t{ 99 });
			Same(static_cast<unsigned>(4000000000u));
			Same(kZero); Same(kSeven); Same(kBig);
			Same('|'); Same(',');
			Same("text"); Same(std::string("rgba16f"));
			const double doubles[] = { 0.0, -0.0, 1.0, 0.1, 100.0, 100.0000001, 1e-7, 123456.0, 1234567.0, 1e300,
				-2.5, 0.000123456789, 203.0, 1.0 / 3.0, std::numeric_limits<double>::infinity(),
				-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::denorm_min() };
			for (double d : doubles) Same(d);
			for (double d : doubles) Same(static_cast<float>(d));
			for (float f : { 0.5f, 1919.5f, 3840.25f, 0.0001f, 1.0f / 3.0f }) Same(f);
		}

		TEST_METHOD(MatchesAWholeSignatureAndDetectsTheSameChanges)
		{
			auto build = [](auto& s, double nits, bool sdr, float crop) {
				s << 3 << '|' << sdr << '|' << kSeven << '|' << nits << '|' << crop << ',' << "rgba16f" << '|' << 1080u;
			};
			std::string lastOld, lastNew;
			int changesOld = 0, changesNew = 0;
			// Steps below %g's 6 digits must not count as changes; real ones must.
			const double nits[] = { 100.0, 100.0000001, 100.0000002, 100.001, 100.001, 203.0, 203.0 };
			for (int i = 0; i < 7; ++i)
			{
				std::ostringstream o;
				build(o, nits[i], i % 3 == 0, 0.5f * i);
				ChangeKey k;
				build(k, nits[i], i % 3 == 0, 0.5f * i);
				Assert::AreEqual(o.str(), k.str());
				if (o.str() != lastOld) { lastOld = o.str(); ++changesOld; }
				if (k.Changed(lastNew)) ++changesNew;
				Assert::AreEqual(lastOld, lastNew);
			}
			Assert::AreEqual(changesOld, changesNew);
		}
	};
}
