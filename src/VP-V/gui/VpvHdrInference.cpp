// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// HDR inference, SDR picture veto, and Dolby Vision tunnel hand-off probing/handlers,
// moved out of Bill's VideoProcessorDlg.cpp into compiled VP-V GUI code.
#include "pch.h"

#include "VideoProcessorDlg.h"

#include <AnalysisFrame.h>
#include <DolbyVisionTunnelGate.h>
#include <DolbyVisionTunnelHandoff.h>
#include <HdrInference.h>
#include <HdrInferenceSignalBuilder.h>
#include <HdrVetoCaptureGate.h>
#include <RendererId.h>
#include <VP-V/modules/control/skeleton/SceneRecordReader.h>
#include <VP-V/modules/input/signal/HdrInferenceChoice.h>
bool CVideoProcessorDlg::HdrModesPermitInference()
{
	// Owner decision 2026-10-17: newlldv no longer requires both "Follow input
	// (LLDV)" modes. Every follow mode lets the inference supply what it
	// follows; explicit primaries/luminance still win for the fields they set.
	// An explicit Rec.709 hdr_colorspace contradicts a BT.2020 HDR reading, and
	// (bug hunt 3 C6) so does a container override to a non-BT.2020 colorspace.
	const bool hdrRec709 = m_hdrColorspaceCombo.GetCurSel() >= 0 &&
		static_cast<HdrColorspaceOptions>(m_hdrColorspaceCombo.GetItemData(
			m_hdrColorspaceCombo.GetCurSel())) == HdrColorspaceOptions::HDR_COLORSPACE_REC709;
	const ColorSpace container = m_colorspaceContainerCombo.GetCurSel() >= 0 ?
		static_cast<ColorSpace>(m_colorspaceContainerCombo.GetItemData(
			m_colorspaceContainerCombo.GetCurSel())) : ColorSpace::UNKNOWN;
	// F-8: the original heuristic uses its candidate only with both modes Follow input (LLDV), exactly
	// as upstream's isHDFuryLLDV (HdrInferenceChoice::ModesPermit).
	return HdrInferenceChoice::ModesPermit(m_hdrInference.Options().originalHeuristic, LldvModesSelected(),
		HdrSettingsPermitInference(hdrRec709, container));
}

HdrInferenceSignal CVideoProcessorDlg::BuildHdrInferenceSignal()
{
	// HdrInferenceSignalBuilder.h (2026-10-17: moved unchanged so the
	// incoming-HDR-mode tests build the same signal).
	const bool enableDolbyVisionTunnel = m_profileDolbyVisionTunnelOverride >= 0
		? m_profileDolbyVisionTunnelOverride != 0 : m_enableDolbyVisionTunnel;
	const int rendererIndex = m_rendererCombo.GetCurSel();
	const RendererId* rendererId = rendererIndex >= 0 ?
		reinterpret_cast<const RendererId*>(
			m_rendererCombo.GetItemData(rendererIndex)) : nullptr;
	DolbyVisionTunnelGateInput tunnelGate;
	tunnelGate.tunnelSwitch = enableDolbyVisionTunnel;
	tunnelGate.profileForcesTunnel = m_profileDolbyVisionTunnelOverride == 1;
	tunnelGate.libplaceboRenderer = rendererId &&
		rendererId->backend == RendererBackend::LIBPLACEBO;
	tunnelGate.handedOff = DvTunnelHandedOff();   // DV input fixes 2026-10-18
	tunnelGate.declineYCbCr = DvTunnelHandoffActive();
	const VideoState* state = m_captureDeviceVideoState;
	return BuildHdrInferenceSignalFromState(state, m_useNewLldvHeuristic,
		m_captureInputBitDepth, tunnelGate,
		state && state->valid ? !HdrModesPermitInference() : false);
}

HdrInferenceUpdate CVideoProcessorDlg::UpdateNewLldvCandidate()
{
	const HdrInferenceSignal signal = BuildHdrInferenceSignal();
	const HdrInferenceUpdate update =
		m_hdrInference.OnSignal(signal, GetTickCount64());
	switch (update.event)
	{
	case HdrInferenceEvent::CandidateSeen:
		if (m_hdrInference.Options().originalHeuristic)   // F-8: no bit-depth gate in Bill's decision
			DebugLog::Log("newlldv (original): BT.2020 / SDR / no HDR InfoFrame candidate seen; waiting %ums before treating it as PQ",
				m_hdrInference.Options().confirmMs);
		else
			DebugLog::Log(
				"newlldv: BT.2020 / %d-bit / no HDR InfoFrame candidate seen (epoch %llu); waiting %ums before treating it as PQ",
				signal.bitDepth,
				static_cast<unsigned long long>(m_hdrInference.Epoch()),
				m_hdrInference.Options().confirmMs);
		break;
	case HdrInferenceEvent::Inferred:
		DebugLog::Log(
			"newlldv: candidate confirmed after stabilization (epoch %llu); treating it as PQ / BT.2020",
			static_cast<unsigned long long>(m_hdrInference.Epoch()));
		break;
	case HdrInferenceEvent::CandidateCleared:
		DebugLog::Log(
			"newlldv: candidate cleared before confirmation: %s",
			m_hdrInference.Reason());
		break;
	case HdrInferenceEvent::Ended:
		DebugLog::Log(
			"newlldv: inference epoch %llu ended: %s; capture state is now %s / %s",
			static_cast<unsigned long long>(m_hdrInference.Epoch()),
			m_hdrInference.Reason(),
			signal.valid ? CStringA(ToString(signal.eotf)).GetString() : "invalid",
			signal.valid ? CStringA(ToString(signal.colorspace)).GetString() : "invalid");
		break;
	case HdrInferenceEvent::NewEpoch:
		DebugLog::Log(
			"newlldv: signal changed while eligible; new epoch %llu state=%s",
			static_cast<unsigned long long>(m_hdrInference.Epoch()),
			ToString(m_hdrInference.State()));
		break;
	default:
		break;
	}
	if (signal.enabled && !m_hdrInference.IsCandidate())
	{
		const std::string eligibility = m_hdrInference.Reason();
		if (eligibility != m_lastHdrInferenceEligibilityLog)
		{
			DebugLog::Log("newlldv: not inferring HDR: %s", eligibility.c_str());
			m_lastHdrInferenceEligibilityLog = eligibility;
		}
	}
	else
		m_lastHdrInferenceEligibilityLog.clear();
	m_newLldvCandidateActive = m_hdrInference.IsCandidate();
	m_newLldvCandidateConfirmed = m_hdrInference.IsInferredPq();
	UpdateHdrVetoMeasurement();
	return update;
}

void CVideoProcessorDlg::UpdateHdrVetoMeasurement()
{
	const bool measure = m_hdrInference.WantsPictureStatistics() &&
		!ProfileTunnelClaimsSignal() && HdrModesPermitInference();
	const bool previous = m_hdrVetoMeasure.load(std::memory_order_acquire);
	if (!measure || !m_hdrInference.VetoArmed())
		m_hdrVetoGate.Disarm();   // bug hunt 3 C2: no switch outside an armed veto
	if (measure == previous)
		return;
	if (measure)
		m_hdrVetoGeneration.fetch_add(1, std::memory_order_acq_rel);
	m_hdrVetoMeasure.store(measure, std::memory_order_release);
	DebugLog::Log("newlldv SDR veto: picture statistics %s (epoch %llu)",
		measure ? "started" : "stopped",
		static_cast<unsigned long long>(m_hdrInference.Epoch()));
}

namespace
{
	// Capture thread -> UI thread: one analysed picture for the SDR veto.
	struct HdrVetoPictureMessage
	{
		uint64_t generation = 0;
		uint64_t tickMs = 0;
		bool supported = false;
		VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;
		HdrPictureObservation observation;
		float gridDifference = 0.0f;
		float histogramDistance = 0.0f;
		uint32_t measureUs = 0;
		// Bug hunt 3 (C2): the capture thread queued this (cut) frame as SDR.
		bool switchedAtCut = false;
		bool cutUsedFallback = false;
		const char* cutFallbackReason = nullptr;
	};
}

VideoStateComPtr CVideoProcessorDlg::MeasureHdrVetoPicture(const VideoStateComPtr& state,
	const VideoFrame& frame, uint64_t generation)
{
	// WARNING: capture thread, only while the veto is measuring. 160 x 90
	// samples per picture. Not armed: after the frame was handed to the
	// renderer (no added latency to it), at most every 50 ms. Armed (bug hunt 3
	// C2): every frame, before the renderer has it, so the cut frame itself
	// can be queued as SDR.
	const ULONGLONG now = GetTickCount64();
	if (!m_hdrVetoGate.WantsSample(generation, now))
		return nullptr;
	if (!state || !state->valid || !state->displayMode || !frame.GetData())
		return nullptr;
	VideoStateComPtr switchState;

	const auto start = std::chrono::steady_clock::now();
	std::unique_ptr<HdrVetoPictureMessage> message(new HdrVetoPictureMessage());
	message->generation = generation;
	message->tickMs = now;
	message->encoding = state->videoFrameEncoding;
	AnalysisFrameInput input;
	input.nativeData = reinterpret_cast<const uint8_t*>(frame.GetData());
	input.nativeBytes = static_cast<size_t>(state->BytesPerFrame());
	input.nativeRowBytes = static_cast<size_t>(state->BytesPerRow());
	// The frame stays valid for the duration of this callback.
	input.nativeOwner = std::shared_ptr<const void>(frame.GetData(), [](const void*) {});
	input.width = static_cast<int>(state->displayMode->FrameWidth());
	input.height = static_cast<int>(state->displayMode->FrameHeight());
	input.encoding = state->videoFrameEncoding;
	input.colorspace = state->colorspace;
	input.transferKey = static_cast<uint32_t>(state->eotf);
	input.vpLimitedDomain = true;   // VP-only HDR veto grid: its thresholds are in the VP-V (limited) domain
	input.sourceGeneration = generation;
	const PreparedAnalysisFrame prepared = PrepareAnalysisFrame(input);
	if (prepared.IsValid() &&
		SampleHdrVetoGrid(prepared.source, 160, 90, m_hdrVetoGrid))
	{
		const HdrVetoFrameStats stats =
			MeasureHdrVetoGrid(m_hdrVetoGrid.data(), 160, 90);
		message->supported = true;
		message->observation.measured = stats.measured;
		message->observation.activePicture = stats.activePicture;
		message->observation.sdrEvidence = stats.sdrEvidence;
		message->observation.fractionAt880 = stats.fractionAtHigh;
		message->observation.p999 = stats.p999;
		// BLOCKER-2: NEVER pass raw capture driver frame.GetCounter() as renderer sourceSequence.
		// When armed, MeasureHdrVetoPicture runs BEFORE renderer->OnVideoFrame(), so U7 cannot be
		// sealed yet (pass 0 -> visible GridHistCut fallback). When unarmed (after OnVideoFrame),
		// translate (counter, timestamp) to the renderer-local sourceSequence recorded at Ingress.
		uint64_t rendererSequence = 0;
		if (!m_hdrVetoGate.Armed(generation))
			VpSkeleton::SceneRecordReader::ResolveCaptureSequence(
				frame.GetCounter(), static_cast<int64_t>(frame.GetTimingTimestamp()), rendererSequence);
		const HdrVetoCaptureGate::Observation observed = m_hdrVetoGate.Observe(generation, stats, rendererSequence);
		message->observation.cut = observed.cut;
		message->gridDifference = observed.gridDifference;
		message->histogramDistance = m_hdrVetoGate.LastHistogramDistance();
		message->cutUsedFallback = observed.usedFallback;
		message->cutFallbackReason = observed.fallbackReason;
		switchState = observed.switchState;
		message->switchedAtCut = switchState != nullptr;
	}
	message->measureUs = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count());
	if (PostMessage(WM_MESSAGE_HDR_INFERENCE_PICTURE,
		reinterpret_cast<WPARAM>(message.get()), 0))
		message.release();
	return switchState;
}

VideoStateComPtr CVideoProcessorDlg::BuildHdrVetoSdrState()
{
	if (!m_captureDeviceVideoState || !m_captureDeviceVideoState->valid)
		return nullptr;
	// The state BuildPushVideoState builds once the veto ends the inference:
	// the same alterations with the inferred PQ taken away. Nothing is pushed,
	// published or logged as applied.
	const bool confirmed = m_newLldvCandidateConfirmed;
	m_newLldvCandidateConfirmed = false;
	m_hdrVetoPreviewing = true;
	m_hdrVetoPreview = nullptr;
	try
	{
		BuildPushVideoStateCore();
	}
	catch (const std::exception& e)
	{
		DebugLog::Log("newlldv SDR veto: the SDR state could not be prepared: %s", e.what());
		m_hdrVetoPreview = nullptr;
	}
	m_hdrVetoPreviewing = false;
	m_newLldvCandidateConfirmed = confirmed;
	VideoStateComPtr result = m_hdrVetoPreview;
	m_hdrVetoPreview = nullptr;
	return result;
}

void CVideoProcessorDlg::RefreshHdrVetoGate()
{
	if (!m_hdrVetoMeasure.load(std::memory_order_acquire) || !m_hdrInference.VetoArmed() ||
		!m_hdrInference.IsInferredPq())
	{
		m_hdrVetoGate.Disarm();
		return;
	}
	m_hdrVetoGate.Arm(m_hdrVetoGeneration.load(std::memory_order_acquire), BuildHdrVetoSdrState());
}

LRESULT CVideoProcessorDlg::OnMessageHdrInferencePicture(WPARAM wParam, LPARAM)
{
	std::unique_ptr<HdrVetoPictureMessage> message(
		reinterpret_cast<HdrVetoPictureMessage*>(wParam));
	if (!message ||
		message->generation != m_hdrVetoGeneration.load(std::memory_order_acquire) ||
		!m_hdrVetoMeasure.load(std::memory_order_acquire))
	{
		// Bug hunt 3 (C2): the capture thread queued SDR for a veto this thread
		// no longer commits (the measuring period ended meanwhile): the renderer
		// gets the effective state again.
		if (message && message->switchedAtCut)
			BuildPushVideoState();
		return 0;
	}
	if (!message->supported)
	{
		if (m_hdrVetoUnsupportedLoggedGeneration != message->generation)
		{
			DebugLog::Log( // VPV-FALLBACK gui-hdr-veto: MFC dialog unsupported capture format diagnostic, no renderer FallbackStatus
				"newlldv SDR veto: picture statistics unavailable for capture format %s; the veto cannot act",
				CStringA(ToString(message->encoding)).GetString());
			m_hdrVetoUnsupportedLoggedGeneration = message->generation;
		}
		return 0;
	}
	const HdrPictureObservation& picture = message->observation;
	if (picture.cut && DebugLog::IsEnhancedLoggingEnabled())
	{
		const char* src = message->cutUsedFallback
			? (message->cutFallbackReason ? message->cutFallbackReason : "fallback")
			: "shared U7";
		DebugLog::Log(
			"newlldv SDR veto: cut grid=%.1f histogram=%.3f measure_us=%u source=%s",
			message->gridDifference, message->histogramDistance,
			message->measureUs, src);
	}
	const HdrInferenceUpdate update =
		m_hdrInference.OnPicture(picture, message->tickMs);
	switch (update.event)
	{
	case HdrInferenceEvent::ShotEvidence:
		DebugLog::Log(
			"newlldv SDR veto: SDR evidence in shot %u of %u (codes>=880 %.3f%%, p99.9=%u, measure_us=%u)",
			m_hdrInference.ShotsWithEvidence(), m_hdrInference.Options().shotsRequired,
			picture.fractionAt880 * 100.0, static_cast<unsigned>(picture.p999),
			message->measureUs);
		break;
	case HdrInferenceEvent::VetoArmed:
		DebugLog::Log(
			"newlldv SDR veto: armed after SDR evidence in %u shots (codes>=880 %.3f%%, p99.9=%u); returning to SDR at the next cut (or in %u s without one)",
			m_hdrInference.ShotsWithEvidence(), picture.fractionAt880 * 100.0,
			static_cast<unsigned>(picture.p999),
			m_hdrInference.Options().noCutFallbackMs / 1000);
		// Bug hunt 3 (C2): from now on the capture thread switches on the cut frame.
		RefreshHdrVetoGate();
		break;
	case HdrInferenceEvent::VetoAtCut:
	case HdrInferenceEvent::VetoWithoutCut:
		ApplyHdrInferenceVeto(update, "picture");
		break;
	default:
		if (message->switchedAtCut)
		{
			// The capture thread switched but the tracker did not flip (it can
			// only disagree if the veto was disarmed meanwhile): resynchronise.
			DebugLog::Log("newlldv SDR veto: capture-side switch not confirmed; renderer state rebuilt");
			BuildPushVideoState();
		}
		break;
	}
	return 0;
}

namespace
{
	// Capture thread -> UI thread: the DV tunnel hand-off probe's decision.
	struct DvTunnelHandoffMessage
	{
		uint64_t generation = 0;
		DolbyVisionTunnelHandoffEvent event = DolbyVisionTunnelHandoffEvent::None;
		uint64_t counter = 0;
		uint64_t windowPictures = 0;
		std::string reason;
		std::string content;
	};
}

DolbyVisionTunnelHandoffEvent CVideoProcessorDlg::ProbeDvTunnelHandoff(const VideoStateComPtr& state,
	const VideoFrame& frame, uint64_t generation)
{
	// WARNING: capture thread. DolbyVisionTunnelHandoff.h: a CRC check of the
	// first packet and two sparse grids per picture (well under a millisecond
	// at 2160p), after the renderer has the frame.
	if (!state || !state->valid || !state->displayMode || !frame.GetData())
		return DolbyVisionTunnelHandoffEvent::None;
	if (!m_dvTunnelHandoffCaptureActive || generation != m_dvTunnelHandoffCaptureGeneration)
	{
		// A new signal: the UI pushes its state through the ordinary path.
		m_dvTunnelTakeBackPending.store(false, std::memory_order_release);
		DolbyVisionTunnelHandoffSignal signal;
		signal.colorspace = state->colorspace;
		signal.hdrInfoFrame = state->hdrData != nullptr;
		signal.vsif = state->dolbyVisionVsif;
		signal.frameRate = state->displayMode->RefreshRateHz();
		m_dvTunnelHandoffCaptureProbe.Reset(signal);
		m_dvTunnelHandoffCaptureGeneration = generation;
		m_dvTunnelHandoffCaptureActive = true;
	}
	VpvDolbyVisionTunnelFormatter::RasterView raster;
	raster.data = frame.GetData();
	raster.width = static_cast<uint32_t>(state->displayMode->FrameWidth());
	raster.height = static_cast<uint32_t>(state->displayMode->FrameHeight());
	raster.stride = state->BytesPerRow();
	raster.encoding = state->videoFrameEncoding;
	const DolbyVisionTunnelHandoffEvent event =
		m_dvTunnelHandoffCaptureProbe.OnFrame(raster, frame.GetCounter());
	if (event == DolbyVisionTunnelHandoffEvent::None)
		return event;
	if (event == DolbyVisionTunnelHandoffEvent::TakeBack)
		m_dvTunnelTakeBackPending.store(true, std::memory_order_release);
	std::unique_ptr<DvTunnelHandoffMessage> message(new DvTunnelHandoffMessage());
	message->generation = generation;
	message->event = event;
	message->counter = frame.GetCounter();
	message->windowPictures = m_dvTunnelHandoffCaptureProbe.WindowPictures();
	message->reason = m_dvTunnelHandoffCaptureProbe.Reason();
	message->content = ToString(m_dvTunnelHandoffCaptureProbe.LastContent());
	if (PostMessage(WM_MESSAGE_DV_TUNNEL_HANDOFF,
		reinterpret_cast<WPARAM>(message.get()), 0))
		message.release();
	else
		m_dvTunnelTakeBackPending.store(false, std::memory_order_release);
	return event;
}

LRESULT CVideoProcessorDlg::OnMessageDvTunnelHandoff(WPARAM wParam, LPARAM)
{
	std::unique_ptr<DvTunnelHandoffMessage> message(
		reinterpret_cast<DvTunnelHandoffMessage*>(wParam));
	// A take-back holds frames back on the capture thread until a renderer
	// with the tunnel state takes them (a restarted renderer, or this one if
	// it accepts the state in place). A take-back that changes nothing here
	// releases them at once.
	const bool takeBack = message && message->event == DolbyVisionTunnelHandoffEvent::TakeBack;
	if (!message || !DvTunnelHandoffActive() ||
		message->generation != m_dvTunnelHandoffGeneration.load(std::memory_order_acquire))
	{
		if (takeBack)
			m_dvTunnelTakeBackPending.store(false, std::memory_order_release);
		return 0;
	}
	const bool handOff = message->event == DolbyVisionTunnelHandoffEvent::HandOff;
	if (handOff == DvTunnelHandedOff())
	{
		if (takeBack)
			m_dvTunnelTakeBackPending.store(false, std::memory_order_release);
		return 0;
	}
	m_dvTunnelHandedOff = handOff;
	m_dvTunnelHandedOffGeneration = message->generation;
	m_dvTunnelHandoffReason = message->reason + " (content " + message->content + ")";
	DebugLog::Log(handOff ?
		"Dolby Vision RGB tunnel hand-off: picture %llu: %s; no tunnel evidence for %llu pictures; the normal path takes the signal (newlldv may infer PQ)" :
		"Dolby Vision RGB tunnel hand-off: picture %llu: %s; the tunnel takes the signal back (%llu-picture window)",
		static_cast<unsigned long long>(message->counter), m_dvTunnelHandoffReason.c_str(),
		static_cast<unsigned long long>(message->windowPictures));
	UpdateNewLldvCandidate();
	const bool rendererAcceptedState = BuildPushVideoState();
	if (m_videoRenderer && m_rendererState == RendererState::RENDERSTATE_RENDERING &&
		(!m_videoRenderer->SupportsDynamicVideoState() || !rendererAcceptedState))
	{
		DebugLog::Log("Dolby Vision RGB tunnel hand-off: renderer cannot change the tunnel path in place; restarting renderer");
		m_wantToRestartRenderer = true;
	}
	else if (takeBack)
		m_dvTunnelTakeBackPending.store(false, std::memory_order_release);
	UpdateState();
	return 0;
}

void CVideoProcessorDlg::ApplyHdrInferenceVeto(const HdrInferenceUpdate& update,
	const char* source)
{
	DebugLog::Log(
		"newlldv SDR veto: returning epoch %llu to SDR %s (source=%s); it stays SDR until the signal changes",
		static_cast<unsigned long long>(m_hdrInference.Epoch()),
		update.event == HdrInferenceEvent::VetoAtCut ? "at a cut" :
			"without a cut (none within the fallback interval; switched in place, no crossfade)",
		source);
	m_newLldvCandidateConfirmed = m_hdrInference.IsInferredPq();
	m_newLldvCandidateActive = m_hdrInference.IsCandidate();
	UpdateHdrVetoMeasurement();
	if (!update.effectiveChanged)
		return;
	KillTimer(LLDV_CHANGE_RESTART_TIMER_ID);
	m_lldvChangeRestartDelaySeconds = -1;
	m_lldvRestartPending = false;
	const bool rendererAcceptedState = BuildPushVideoState();
	if (m_videoRenderer && m_rendererState == RendererState::RENDERSTATE_RENDERING &&
		(!m_videoRenderer->SupportsDynamicVideoState() || !rendererAcceptedState))
	{
		DebugLog::Log(
			"newlldv SDR veto: renderer cannot take PQ -> SDR in place; restarting renderer");
		m_wantToRestartRenderer = true;
		UpdateState();
	}
}
