/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>


class CVideoProcessorApp:
	public CWinAppEx
{
public:
	virtual BOOL InitInstance();
	virtual int ExitInstance();
	BOOL PreTranslateMessage(MSG* message) override;
	bool RestoreDisplayTopology(const char* reason);
	// Bug hunt 3 (SH4): the forced exit and the session end must not block on
	// the restore mutex while another thread (the UI thread) is stuck inside
	// SetDisplayConfig: wait at most lockTimeoutMs, then skip the step (the
	// pending record stays for the next start). skipped reports that case.
	bool RestoreDisplayTopologyBounded(const char* reason, uint32_t lockTimeoutMs, bool* skipped = nullptr);
private:
	bool RestoreDisplayTopologyLocked(const char* reason);
public:
	// Bug hunt 2 (2026-10-17, L10): (re-)enters the target-only display
	// session when fullscreen_monitor_session_mode asks for it and none is
	// active (startup, and fullscreen on again after a fullscreen-off restore).
	bool BeginTargetOnlyDisplaySession(const CString& target, const char* reason);

	// DirectShow startup priming remains automatic. The public queue controls
	// are the timestamp lead and maintained VP queue target, both expressed as
	// whole frames in the inclusive range [0, 16].
	size_t GetQueueStartupPrerollFrames() const { return m_queueStartupPrerollFrames; }
	void SetQueueStartupPrerollFrames(size_t value) { m_queueStartupPrerollFrames = value; }
	size_t GetQueueSteadyReserveFrames() const { return m_queueSteadyReserveFrames; }
	bool HasQueueSteadyReserveFrames() const { return m_hasQueueSteadyReserveFrames; }
	void SetQueueSteadyReserveFrames(size_t value)
	{
		m_queueSteadyReserveFrames = value;
		m_hasQueueSteadyReserveFrames = true;
	}
	bool HasPresentationLeadFrames() const { return m_hasPresentationLeadFrames; }
	size_t GetPresentationLeadFrames() const { return m_presentationLeadFrames; }
	void SetPresentationLeadFrames(size_t value)
	{
		m_presentationLeadFrames = value;
		m_hasPresentationLeadFrames = true;
	}
	// Stage 5: the default queue's target as time (0 = not set). It is turned
	// into target_frames once the source rate is known.
	int GetQueueTargetLatencyMs() const { return m_queueTargetLatencyMs; }
	void SetQueueTargetLatencyMs(int value) { m_queueTargetLatencyMs = value; }
	size_t GetActivePictureLookaheadFrames() const
	{
		return m_activePictureLookaheadFrames;
	}
	void SetActivePictureLookaheadFrames(size_t value)
	{
		m_activePictureLookaheadFrames = value > 8 ? 8 : value;
	}
	// The default queue section (or command line) set active_picture_lookahead_frames
	// explicitly (owner 2026-10-17: absent = it follows the queue, QueueLookahead.h).
	bool IsActivePictureLookaheadConfigured() const { return m_activePictureLookaheadConfigured; }
	void SetActivePictureLookaheadConfigured(bool value) { m_activePictureLookaheadConfigured = value; }

private:
	size_t m_queueStartupPrerollFrames = 0;
	size_t m_queueSteadyReserveFrames = 4;
	bool m_hasQueueSteadyReserveFrames = true;
	size_t m_presentationLeadFrames = 1;
	bool m_hasPresentationLeadFrames = true;
	size_t m_activePictureLookaheadFrames = 0;
	bool m_activePictureLookaheadConfigured = false;
	int m_queueTargetLatencyMs = 0;
	std::string m_displayRecoveryStatePath;
	bool m_targetOnlyDisplaySessionActive = false;
	int m_startupExitCode = 0;

	DECLARE_MESSAGE_MAP()
};

extern CVideoProcessorApp videoProcessorApp;
