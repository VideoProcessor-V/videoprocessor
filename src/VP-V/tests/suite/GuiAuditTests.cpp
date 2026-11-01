#include "pch.h"
#include "CppUnitTest.h"

// audit 2026-10-16: host-GUI audit helpers - general N*1000/1001 refresh
// canonicalisation and startup display-recovery quarantine.

#include <DisplayTopologySession.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(GuiAuditTests)
	{
	public:
		TEST_METHOD(CanonicalRefreshSnapsEveryFractionalFamily)
		{
			const double tolerance = 1e-9;
			Assert::AreEqual(23.976, DisplayTopologySession::CanonicalRefreshRateHz(24000.0 / 1001.0), tolerance);
			Assert::AreEqual(29.97, DisplayTopologySession::CanonicalRefreshRateHz(30000.0 / 1001.0), tolerance);
			Assert::AreEqual(59.94, DisplayTopologySession::CanonicalRefreshRateHz(60000.0 / 1001.0), tolerance);
			Assert::AreEqual(47.952, DisplayTopologySession::CanonicalRefreshRateHz(48000.0 / 1001.0), tolerance);
			Assert::AreEqual(119.88, DisplayTopologySession::CanonicalRefreshRateHz(120000.0 / 1001.0), tolerance);
			Assert::AreEqual(239.76, DisplayTopologySession::CanonicalRefreshRateHz(240000.0 / 1001.0), tolerance);
			// Measured rates inside the 0.01 Hz window snap too.
			Assert::AreEqual(23.976, DisplayTopologySession::CanonicalRefreshRateHz(23.971), tolerance);
			Assert::AreEqual(59.94, DisplayTopologySession::CanonicalRefreshRateHz(59.948), tolerance);
		}

		TEST_METHOD(CanonicalRefreshLeavesIntegerAndOtherRatesUnchanged)
		{
			const double tolerance = 1e-12;
			for (const double rate : { 24.0, 25.0, 30.0, 48.0, 50.0, 60.0, 100.0, 120.0, 144.0, 240.0,
				23.95, 59.9, 74.97, 0.0, -1.0, 5.0 })
			{
				Assert::AreEqual(rate, DisplayTopologySession::CanonicalRefreshRateHz(rate), tolerance);
			}
		}

		TEST_METHOD(StartupRecoveryQuarantinesUnreadableRecordAndKeepsOtherKeys)
		{
			// The record cannot decode, so no SetDisplayConfig call is made.
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-gui-audit-display-recovery.state";
			DeleteFileA(path.c_str());
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "# Managed by VideoProcessor.\n"
					"profile.viewport: scope\n"
					"display_recovery.v1: DEADBEEF\n";
			}
			Assert::IsTrue(DisplayTopologySession::HasPendingRecovery(path));

			bool restored = true;
			bool quarantined = false;
			std::string error;
			Assert::IsFalse(DisplayTopologySession::RecoverPendingAtStartup(
				path, "test", restored, quarantined, error));
			Assert::IsFalse(restored);
			Assert::IsTrue(quarantined);
			Assert::IsFalse(error.empty());
			Assert::IsFalse(DisplayTopologySession::HasPendingRecovery(path));

			std::ifstream input(path);
			std::stringstream contents;
			contents << input.rdbuf();
			input.close();
			const std::string text = contents.str();
			Assert::IsTrue(text.find("profile.viewport: scope") != std::string::npos);
			const size_t quarantinedKey = text.find("display_recovery.v1.quarantined-");
			Assert::IsTrue(quarantinedKey != std::string::npos);
			Assert::IsTrue(text.find(": DEADBEEF", quarantinedKey) != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(StartupRecoveryWithoutRecordIsANoOp)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-gui-audit-display-recovery-empty.state";
			DeleteFileA(path.c_str());
			bool restored = true;
			bool quarantined = true;
			std::string error;
			Assert::IsTrue(DisplayTopologySession::RecoverPendingAtStartup(
				path, "test", restored, quarantined, error));
			Assert::IsFalse(restored);
			Assert::IsFalse(quarantined);
		}
	};
}
