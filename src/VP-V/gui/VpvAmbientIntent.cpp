// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage 9 ambient, content intent, intent interpreter and startup active-picture look-ahead
// helpers, moved out of Bill's VideoProcessorDlg.cpp into compiled VP-V GUI code.
#include "pch.h"

#include "VideoProcessorDlg.h"
#include "VideoProcessorApp.h"

#include <IntentInterpreterSupport.h>
#include <QueueLookahead.h>
#include <VP-V/abi/VpvHostCalls.h>
#include <VP-V/gui/VpStatusAdapter.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>
// Stage 9: the room as the rules' `ambient` variable ("" when off).
std::string CVideoProcessorDlg::CurrentAmbientValue() const
{
	if (m_ambientConfigured.mode == Ambient::Mode::Off)
		return std::string();
	if (m_ambientOverride)
		return Ambient::Name(m_ambientOverrideRoom);
	Ambient::Room room = Ambient::Room::Dark;
	if (!m_ambientState.Current(static_cast<double>(GetTickCount64()) / 1000.0, room))
		return std::string();
	return Ambient::Name(room);
}

// Re-evaluates the profile rules when the room changed (deferred by the
// runtime while there is no capture signal, as for every rules change).
void CVideoProcessorDlg::UpdateAmbient(const char* reason)
{
	const std::string value = CurrentAmbientValue();
	if (value == m_ambientValue)
		return;
	DebugLog::Log("Room lights: %s -> %s (%s)", m_ambientValue.empty() ? "off" : m_ambientValue.c_str(),
		value.empty() ? "off" : value.c_str(), reason);
	m_ambientValue = value;
	RefreshUnifiedProfilesForRuleContext(reason);
}

// 9b: the sensor reader runs while mode is sensor and a port is set; it is
// restarted when the port or speed changes.
void CVideoProcessorDlg::ConfigureAmbientSensor()
{
	const Ambient::Settings& s = m_ambientConfigured;
	if (s.mode != Ambient::Mode::Sensor || s.port.empty())
	{
		if (m_ambientSensor.Running())
		{
			m_ambientSensor.Stop();
			DebugLog::Log("Room light sensor: stopped");
		}
		return;
	}
	if (m_ambientSensor.Running() && m_ambientSensor.Port() == s.port && m_ambientSensor.Baud() == s.baud)
		return;
	m_ambientSensorSequence = 0;
	m_ambientSensor.Start(s.port, s.baud, [] { return LedDrive::CreateWin32SerialPort(); });
}

// Once a second: the newest reading into the state; a detector that went
// quiet falls back to the saved room, and its next reading decides at once.
void CVideoProcessorDlg::PollAmbientSensor()
{
	const double now = static_cast<double>(GetTickCount64()) / 1000.0;
	double lux = 0.0;
	if (m_ambientSensor.Newest(m_ambientSensorSequence, lux))
		m_ambientState.AddReading(lux, now);
	else if (m_ambientState.Sensed() && m_ambientState.DetectorLost(now))
	{
		DebugLog::Log("Room light sensor: no reading for %.0f s; using the saved room (%s)",
			m_ambientConfigured.lostSeconds, Ambient::Name(m_ambientConfigured.room));
		m_ambientState.ResetSensor();
	}
}

// Content intent (intent interpreter item 1, 2026-10-17): the renderer's
// content class as the rules' `content` variable ("" while intent_content is
// off or nothing is classified). Re-evaluates the profile rules on a change.
//
// Bug hunt 2 (2026-10-17, H2): the value used to become "" whenever the
// renderer was not RENDERING, and every new renderer reclassifies from
// Unknown, so each renderer stop/restart flipped content-ruled profiles to
// their fallback and back (an endless loop when that profile needs a renderer
// restart). The last classification is now held across stops and restarts
// and changes only on a real classification change (ContentRuleHold).
// Correctness fix, no switch: intent_content off still gives "".
void CVideoProcessorDlg::UpdateContentIntent(const char* reason)
{
	ContentIntentStatus status;
	const bool rendering = m_videoRenderer &&
		m_rendererState == RendererState::RENDERSTATE_RENDERING;
	const bool statusAvailable = rendering &&
		VpvCalls::GetContentIntentStatus(m_videoRenderer.get(), status);
	if (!statusAvailable)
		status = ContentIntentStatus();
	if (rendering)
		m_contentStatus = status;   // the overlay line shows the live state
	const std::string value =
		m_contentRuleHold.Update(rendering, statusAvailable, status);
	if (value == m_contentValue)
		return;
	DebugLog::Log("Content intent: rules see content=%s (was %s; %s, %s)",
		value.empty() ? "(empty)" : value.c_str(), m_contentValue.empty() ? "(empty)" : m_contentValue.c_str(),
		DisplayName(status.source), reason);
	m_contentValue = value;
	RefreshUnifiedProfilesForRuleContext(reason);
}

// Owner 2026-10-17: the active-picture look-ahead for a renderer start. After a
// queue profile was applied, the value it resolved; otherwise, absent from the
// default queue section, it follows the queue (QueueLookahead.h; the
// low-latency path is a queue-profile property, so normal here).
size_t CVideoProcessorDlg::StartupActivePictureLookaheadFrames()
{
	if (m_profileQueueDefaultsCaptured || videoProcessorApp.IsActivePictureLookaheadConfigured())
	{
		m_activePictureLookaheadAutomatic = m_profileQueueDefaultsCaptured ?
			!m_activePictureLookaheadLoggedConfigured : false;
		return videoProcessorApp.GetActivePictureLookaheadFrames();
	}
	m_activePictureLookaheadAutomatic = true;
	const size_t frames = QueueLookahead::ResolveActivePictureLookahead(false, 0, false,
		videoProcessorApp.GetQueueSteadyReserveFrames(), GetRendererVideoFrameQueueSizeMax());
	if (frames != videoProcessorApp.GetActivePictureLookaheadFrames())
	{
		DebugLog::Log("Queue: active_picture_lookahead_frames=%zu (auto; no queue profile, target=%zu queue_size=%zu)",
			frames, videoProcessorApp.GetQueueSteadyReserveFrames(), GetRendererVideoFrameQueueSizeMax());
		videoProcessorApp.SetActivePictureLookaheadFrames(frames);
	}
	return frames;
}

// Bug hunt 3 F2: the startup look-ahead and whether it is automatic (the locked-film
// queue scaling takes no floor from an automatic look-ahead).
void CVideoProcessorDlg::ApplyStartupActivePictureLookahead()
{
	if (!m_videoRenderer)
		return;
	const size_t frames = StartupActivePictureLookaheadFrames();
	VpvCalls::SetActivePictureLookaheadAutomatic(m_videoRenderer.get(), m_activePictureLookaheadAutomatic);
	m_videoRenderer->SetActivePictureLookaheadFrames(frames);
}

// Intent interpreter items 2-5 (2026-10-17): once a second, the room reading for
// ambient adaptation (sensor: the newest reading; manual: lit = lux_lit, dark =
// 0 lux; off or detector lost: invalid) and the renderer's intent status for
// the stats overlay (toggled only) and the crash state; each change is logged.
void CVideoProcessorDlg::UpdateIntentInterpreter()
{
	const bool rendering = m_videoRenderer &&
		m_rendererState == RendererState::RENDERSTATE_RENDERING;
	IntentStatus status;
	if (rendering)
	{
		const double now = static_cast<double>(GetTickCount64()) / 1000.0;
		double lux = 0.0;
		const Ambient::Room overrideRoom = m_ambientOverrideRoom;
		const bool valid = IntentInterpreter::AmbientLuxFromDetector(m_ambientState, now, lux,
			m_ambientOverride ? &overrideRoom : nullptr);
		VpvCalls::SetAmbientLux(m_videoRenderer.get(), lux, valid, now);
		if (!VpvCalls::GetIntentStatus(m_videoRenderer.get(), status))
			status = IntentStatus();
	}
	m_intentStatus = status;
	const std::string keys[4] = {
		IntentInterpreter::MotionLogKey(status.motion), IntentInterpreter::AmbientLogKey(status.ambient),
		IntentInterpreter::BlackLogKey(status.black), IntentInterpreter::DisplayIntentLine(status.display) };
	static const char* const kNames[4] = { "Motion intent", "Ambient", "Black", "Display" };
	for (int i = 0; i < 4; ++i)
	{
		if (keys[i] == m_intentLogKeys[i])
			continue;
		if (rendering || keys[i].empty())
			DebugLog::Log("Intent interpreter: %s: %s", kNames[i], keys[i].empty() ? "off" : keys[i].c_str());
		m_intentLogKeys[i] = keys[i];
	}
}

// "sports (inferred)" / "film (from metadata)"; empty while the switch is off.
CString CVideoProcessorDlg::ContentIntentLine() const
{
	return VpvGui::ContentIntentLine(m_contentStatus);   // wording: src/VP-V/core/StatusText.h (WP-11)
}

CString CVideoProcessorDlg::AmbientOverlayLine() const
{
	// Wording: src/VP-V/core/StatusText.h (WP-11); the facts are the dialog's ambient state.
	VpStatusText::AmbientFacts facts;
	facts.enabled = m_ambientConfigured.mode != Ambient::Mode::Off;
	if (facts.enabled)
	{
		facts.value = CurrentAmbientValue();
		facts.keyOverride = m_ambientOverride;
		facts.manual = m_ambientConfigured.mode == Ambient::Mode::Manual;
		facts.portSet = !m_ambientConfigured.port.empty();
		facts.sensed = facts.portSet && m_ambientState.Sensed();
		facts.lux = facts.sensed ? m_ambientState.LastLux() : 0.0;
	}
	return VpvGui::AmbientLine(facts);
}

// Backup key (settings first): flips dark / lit until restart or a Config
// change of [ambient]; a second press ends the override.
void CVideoProcessorDlg::OnCommandRoomLights()
{
	if (m_ambientConfigured.mode == Ambient::Mode::Off)
	{
		if (!m_ambientKeyOffLogged)
			DebugLog::Log("Room lights key ignored: [ambient] mode is off");
		m_ambientKeyOffLogged = true;
		return;
	}
	if (m_ambientOverride)
		m_ambientOverride = false;
	else
	{
		m_ambientOverrideRoom = CurrentAmbientValue() == "lit" ? Ambient::Room::Dark : Ambient::Room::Lit;
		m_ambientOverride = true;
	}
	ToggleKeyOverride("room lights", CString(Ambient::Name(m_ambientOverrideRoom)));
	UpdateAmbient("ambient-key");
}
