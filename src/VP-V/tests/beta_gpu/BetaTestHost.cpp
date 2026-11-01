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
		// "w+": the suite's output is read back below through this stream (freopen_s opens the file
		// without sharing, so a second open of the path would fail).
		Assert::AreEqual(0, static_cast<int>(freopen_s(&out, path.c_str(), "w+", stdout)),
			L"cannot open the output file (VP_TEST_OUT)");
		_dup2(_fileno(stdout), _fileno(stderr));
		std::ios::sync_with_stdio(true);
		const int result = RunBetaTestMain();
		std::cout.flush();
		std::fflush(stdout);
		std::fflush(stderr);
		// The gate passes only when the suite reached its last stage: a run that stopped early (a
		// report mode, an early return) must not read as a pass. VP_TEST_REPORT_MODE=1 accepts a
		// report run (exit code 3, "MODE <name>: gate not run") without the whole chain.
		char* reportValue = nullptr;
		size_t reportLength = 0;
		const bool reportMode = _dupenv_s(&reportValue, &reportLength, "VP_TEST_REPORT_MODE") == 0 && reportValue &&
			std::string(reportValue) == "1";
		std::free(reportValue);
		std::string output;
		if (std::fseek(stdout, 0, SEEK_SET) == 0)
		{
			char buffer[4096];
			for (size_t got; (got = std::fread(buffer, 1, sizeof(buffer), stdout)) > 0;)
				output.append(buffer, got);
		}
		std::fseek(stdout, 0, SEEK_END);
		const bool wholeChain = output.find("PASS Beta whole chain") != std::string::npos;
		Logger::WriteMessage(("Beta suite exit code " + std::to_string(result) + (wholeChain ? "; whole chain passed" :
			"; whole chain NOT reached") + (reportMode ? " (VP_TEST_REPORT_MODE=1)" : "") + "\n").c_str());
		if (result == 3)
		{
			Assert::IsTrue(reportMode, L"a Beta report mode ran (MODE <name>: gate not run in the output file): "
				L"the gate did not run; set VP_TEST_REPORT_MODE=1 for a report run");
			return;
		}
		Assert::AreEqual(0, result, L"the Beta suite failed: see the output file");
		Assert::IsTrue(wholeChain || reportMode,
			L"the Beta suite returned 0 without \"PASS Beta whole chain\": the gate did not complete (see the output file)");
	}
};
