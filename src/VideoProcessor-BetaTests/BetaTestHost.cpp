// The Beta suite as a native test DLL (VideoProcessor-BetaTestsHost): one test
// method runs the suite's entry, with its output written to VP_TEST_OUT (or
// beta-test-output.txt in the working directory). Environment switches
// (VP_TEST_GPU, VP_TEST_MATRIX, VP_TEST_TIMING, VP_TEST_SCENARIO, ...) pass
// through vstest to the test host. Smart App Control on the development laptop
// blocks new builds of the suite's program, not DLLs loaded by Microsoft's
// signed test host (owner 2026-10-13).
#include <CppUnitTest.h>

#include <cstdio>
#include <cstdlib>
#include <io.h>
#include <iostream>
#include <string>

int RunBetaTestMain();

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(BetaSuite)
{
public:
	TEST_METHOD(RunBetaSuite)
	{
		char* value = nullptr;
		size_t length = 0;
		std::string path = "beta-test-output.txt";
		if (_dupenv_s(&value, &length, "VP_TEST_OUT") == 0 && value && *value)
			path = value;
		std::free(value);
		FILE* out = nullptr;
		Assert::AreEqual(0, static_cast<int>(freopen_s(&out, path.c_str(), "w", stdout)),
			L"cannot open the output file (VP_TEST_OUT)");
		_dup2(_fileno(stdout), _fileno(stderr));
		std::ios::sync_with_stdio(true);
		const int result = RunBetaTestMain();
		std::cout.flush();
		std::fflush(stdout);
		std::fflush(stderr);
		Assert::AreEqual(0, result, L"the Beta suite failed: see the output file");
	}
};
