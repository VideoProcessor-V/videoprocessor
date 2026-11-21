// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Projector RS-232 connection test and automatic calibration RPC handlers,
// moved out of Bill's VideoProcessorDlg.cpp into compiled VP-V GUI code.
#include "pch.h"

#include "VideoProcessorDlg.h"

#include <ActiveProfileStatus.h>
#include <ConfigFile.h>
#include <VP-V/abi/VpvHostCalls.h>
#include <VP-V/modules/drive/projector_control/ConnectionTest.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>
#include <led/LedDriveConfig.h>

namespace
{
	ConfigurationRpcProtocol::Frame ConfigurationRpcError(uint16_t operation, const std::string& message)
	{
		ConfigurationRpcProtocol::Frame response;
		response.operation = static_cast<uint16_t>(operation |
			ConfigurationRpcProtocol::ResponseFlag |
			ConfigurationRpcProtocol::ErrorFlag);
		ConfigurationRpcProtocol::WriteString(response.payload, message);
		return response;
	}
}
namespace
{
	ConfigurationRpcProtocol::Frame ProjectorTestResponse(bool pass, bool refused,
		const std::string& summary, const std::string& details)
	{
		using namespace ConfigurationRpcProtocol;
		Frame response;
		response.operation = static_cast<uint16_t>(
			static_cast<uint16_t>(Operation::ProjectorTest) | ResponseFlag);
		response.payload.push_back(pass ? 1 : 0);
		response.payload.push_back(refused ? 1 : 0);
		WriteString(response.payload, summary.substr(0, 400));
		WriteString(response.payload, details.substr(0, 4000));
		return response;
	}
}

// UI thread: the renderer frees the port (refused while video plays), then a
// worker runs the test on a read-back connection; OnProjectorTestDone
// restarts the LED drive and answers Config.
void CVideoProcessorDlg::StartProjectorTest(
	const std::shared_ptr<ConfigurationRpcServer::Pending>& pending)
{
	using namespace ConfigurationRpcProtocol;
	if (m_projectorTestPending)
	{
		pending->response = ConfigurationRpcError(pending->request.operation,
			"A projector test is already running.");
		m_configurationRpcServer.Complete(pending);
		return;
	}
	CString reason;
	if (m_videoRenderer && !VpvCalls::SuspendLedDriveForTest(m_videoRenderer.get(), reason)) // VP-V: extension interface (src/VP-V/abi)
	{
		DebugLog::Log("RS-232 connection test refused: %S", static_cast<LPCTSTR>(reason));
		pending->response = ProjectorTestResponse(false, true, std::string(CStringA(reason)), std::string());
		m_configurationRpcServer.Complete(pending);
		return;
	}
	m_projectorTestSuspendedRenderer = m_videoRenderer != nullptr;
	m_projectorTestPending = pending;
	const HWND window = GetSafeHwnd();
	// The result goes through a shared object, never through `this`, so VP
	// closing during the test cannot leave the worker writing a destroyed window.
	auto shared = std::make_shared<ProjectorTestShared>();
	std::atomic_store_explicit(&m_projectorTestShared, shared, std::memory_order_release);
	// Bug hunt 3 (SH5): the worker stays detached (it never touches `this`),
	// but VP's close paths cancel it through shared->cancel - the 5 s hold
	// ends and the light goes back up - and wait (bounded) for shared->done.
	// It used to be killed at exit mid-hold, leaving the LED at 60.
	std::thread([shared, window]()
	{
		ConfigFile config;
		LedDrive::ConnectionTestResult result;
		if (!config.Load(ConfigFile::DEFAULT_FILENAME, ConfigFile::ReadPolicy::Fresh))
			result.summary = "FAIL: VideoProcessor could not read its configuration";
		else
		{
			LedDrive::ConnectionTestPlan plan;
			plan.cancel = shared->cancel;
			result = LedDrive::RunConnectionTestOnNewConnection(LedDrive::ReadProjectorConfig(config),
				[] { return LedDrive::CreateWin32SerialPort(); }, plan);
		}
		std::string details;
		for (const std::string& line : result.lines)
			details += line + "\n";
		DebugLog::Log("RS-232 connection test: %s", result.summary.c_str());
		{
			std::lock_guard<std::mutex> guard(shared->mutex);
			shared->response = ProjectorTestResponse(result.pass, false, result.summary, details);
			shared->done = true;
		}
		shared->doneChanged.notify_all();
		::PostMessageW(window, WM_MESSAGE_PROJECTOR_TEST_DONE, 0, 0);
	}).detach();
}

// Automatic calibration: the wizard's commands. The controller answers at
// once; measurements run on its worker thread.
ConfigurationRpcProtocol::Frame CVideoProcessorDlg::HandleCalibrationRpc(
	const ConfigurationRpcProtocol::Frame& request)
{
	using namespace ConfigurationRpcProtocol;
	size_t cursor = 2;
	std::string command, arguments;
	if (!ReadString(request.payload, cursor, command) || command.empty() || command.size() > 32 ||
		!ReadString(request.payload, cursor, arguments) || arguments.size() > 4096 ||
		cursor != request.payload.size())
		return ConfigurationRpcError(request.operation, "Invalid calibration request.");
	if (!m_calibration)
	{
		Calibration::CalibrationController::Hooks hooks;
		hooks.renderer = [this]()
		{
			return std::atomic_load_explicit(&m_videoRenderer, std::memory_order_acquire);
		};
		hooks.activeColorProfile = []()
		{
			ActiveProfileStatus::Snapshot snapshot;
			if (!ActiveProfileStatus::Read(GetCurrentProcessId(), snapshot)) return std::string();
			return std::string(snapshot.color, strnlen_s(snapshot.color, sizeof(snapshot.color)));
		};
		hooks.displayName = []()
		{
			ConfigFile config;
			std::string name;
			if (config.Load(ConfigFile::DEFAULT_FILENAME, ConfigFile::ReadPolicy::Fresh))
				config.TryGetString("general", "fullscreen_monitor_name", name);
			return ConfigFile::Trim(name);
		};
		const HWND window = GetSafeHwnd();
		hooks.applied = [window]() { ::PostMessageW(window, WM_MESSAGE_CALIBRATION_APPLIED, 0, 0); };
		// WP-12 (module findings item 7): the configuration file VP itself resolved (next to the
		// executable, or --config), as an absolute path; never a name relative to the working folder.
		hooks.configPath = []()
		{
			ConfigFile active;
			return active.Load(ConfigFile::DEFAULT_FILENAME, ConfigFile::ReadPolicy::Fresh) ?
				active.GetLoadedPath() : std::string();
		};
		// Phase 2 (ambient-light check): the room light sensor's newest reading.
		hooks.roomLux = [this](double& lux)
		{
			uint64_t sequence = 0;
			return m_ambientSensor.Newest(sequence, lux);
		};
		m_calibration.reset(new Calibration::CalibrationController(hooks));
	}
	const std::string text = m_calibration->Handle(command, arguments);
	Frame response;
	response.operation = static_cast<uint16_t>(request.operation | ResponseFlag);
	if (!WriteString(response.payload, text))
		return ConfigurationRpcError(request.operation, "The calibration status exceeded the RPC limit.");
	return response;
}

LRESULT CVideoProcessorDlg::OnCalibrationApplied(WPARAM, LPARAM)
{
	// The session saved the calibration LUT keys (after its backups); publish
	// them like a Config Apply.
	DebugLog::Log("Calibration: applying the saved configuration (calibration LUTs)");
	if (!ApplySavedConfiguration(false))
		DebugLog::Log("Calibration: the saved configuration could not be applied at once; it applies on the next restart");
	return 0;
}

LRESULT CVideoProcessorDlg::OnProjectorTestDone(WPARAM, LPARAM)
{
	// Also when the renderer was created during the test (audit stage8-rs232
	// item 1): its LED drive could not open the port the test held.
	// Bug hunt 3 (SH5): not while closing - the close path cancelled the test
	// so the light stays restored; resuming would start the drive again.
	if (m_videoRenderer && !m_wantToTerminate)
		VpvCalls::ResumeLedDriveAfterTest(m_videoRenderer.get());
	m_projectorTestSuspendedRenderer = false;
	const auto pending = std::move(m_projectorTestPending);
	m_projectorTestPending.reset();
	const auto shared = std::atomic_exchange_explicit(&m_projectorTestShared,
		std::shared_ptr<ProjectorTestShared>(), std::memory_order_acq_rel);
	if (pending && shared)
	{
		std::lock_guard<std::mutex> guard(shared->mutex);
		pending->response = shared->response;
		m_configurationRpcServer.Complete(pending);
	}
	return 0;
}
