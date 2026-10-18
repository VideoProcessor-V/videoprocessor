#pragma once

#include <cstdint>
#include <string>

namespace ConfigurationLiveApply
{
	struct SessionPresentationState
	{
		bool videoOnly = false;
		bool fullscreen = false;
	};

	enum class FullscreenToggleAction
	{
		CancelPending,
		ExitFullscreen,
		EnterFullscreen
	};

	enum class FullscreenTransitionDirection
	{
		None,
		Entering,
		Exiting
	};

	enum class ConfigurationEditorToggleAction
	{
		HideForegroundEditor,
		RevealOrActivate,
		CoalescePendingReveal
	};

	struct PresentationFocusDecision
	{
		bool mayActivatePresentation = false;
		bool preserveForeground = true;
		bool consumeKeyboardMessage = false;
	};

	enum class RendererPublicationSource
	{
		SavedConfiguration,
		SessionOverride,
		AcceptedRuntime,
		None
	};

	struct RendererPublicationDecision
	{
		std::wstring renderer;
		RendererPublicationSource source = RendererPublicationSource::None;
	};

	enum class ConfigurationEditorRevealOutcome
	{
		Idle,
		Pending,
		Complete,
		Expired
	};

	static const wchar_t ChangedEventName[] =
		L"Local\\VideoProcessor.ConfigurationChanged.v1";

	inline std::wstring ConfigurationEditorRevealEventName(uint32_t processId)
	{
		return L"Local\\VideoProcessor.ConfigEditor.Reveal." +
			std::to_wstring(processId) + L".v1";
	}
	constexpr uint16_t VideoOnlyToggleDefaultKey = 'U';
	constexpr uint8_t VideoOnlyToggleDefaultModifiers = 0x0c; // Ctrl+Shift
	constexpr uint16_t ViewToggleDefaultKey = 0x0d; // Enter
	constexpr uint8_t ViewToggleDefaultModifiers = 0x10; // Alt

	// Delayed fullscreen placement may restore focus only while VideoProcessor
	// already owns the foreground. This prevents a renderer transition from
	// taking keyboard focus from the separately owned configuration editor (or
	// from any other application the operator has selected).
	inline bool MayActivateFullscreen(uint32_t videoProcessorProcessId,
		uint32_t foregroundProcessId, bool hasForegroundWindow)
	{
		return !hasForegroundWindow ||
			foregroundProcessId == videoProcessorProcessId;
	}

	inline PresentationFocusDecision ResolvePresentationFocus(
		uint32_t videoProcessorProcessId, uint32_t foregroundProcessId,
		bool hasForegroundWindow)
	{
		const bool mayActivate = MayActivateFullscreen(
			videoProcessorProcessId, foregroundProcessId,
			hasForegroundWindow);
		// Presentation lifecycle code never owns keyboard dispatch. In particular,
		// preserving an editor popup or another application's foreground must not
		// consume or reroute VP accelerators/background shortcuts.
		return { mayActivate, !mayActivate, false };
	}

	inline ConfigurationEditorToggleAction ResolveConfigurationEditorToggle(
		bool, bool, bool)
	{
		// The configuration shortcut is an explicit reveal command.  It must
		// never hide an editor or coalesce behind a stale launch attempt: every
		// press is expected to recover it from renderer/fullscreen transitions.
		return ConfigurationEditorToggleAction::RevealOrActivate;
	}

	inline bool ConfigurationEditorRevealAcknowledged(bool delivered,
		uint64_t acknowledgement)
	{
		return delivered && acknowledgement == 1;
	}

	inline bool ShouldRediscoverConfigurationEditor(bool delivered,
		uint64_t acknowledgement, bool alreadyRetried)
	{
		return delivered && acknowledgement != 1 && !alreadyRetried;
	}

	inline ConfigurationEditorRevealOutcome ResolveConfigurationEditorReveal(
		bool pending, bool activationAcknowledged, bool editorVisible,
		uint64_t elapsedMilliseconds, uint64_t timeoutMilliseconds,
		bool unrecoverableLaunchFailure = false)
	{
		if (!pending)
			return ConfigurationEditorRevealOutcome::Idle;
		if (activationAcknowledged && editorVisible)
			return ConfigurationEditorRevealOutcome::Complete;
		if (unrecoverableLaunchFailure ||
			elapsedMilliseconds >= timeoutMilliseconds)
			return ConfigurationEditorRevealOutcome::Expired;
		return ConfigurationEditorRevealOutcome::Pending;
	}

	inline bool ShouldRetryRevealForAssociation(bool revealPending,
		bool validAssociation)
	{
		return revealPending && validAssociation;
	}

	inline bool ShouldRetryConfigurationEditorActivate(bool revealPending,
		bool activationAcknowledged, uint64_t elapsedSinceAcknowledgement,
		uint64_t acknowledgementGraceMilliseconds, bool retryIntervalReady)
	{
		return revealPending && retryIntervalReady &&
			(!activationAcknowledged ||
				elapsedSinceAcknowledgement >=
					acknowledgementGraceMilliseconds);
	}

	inline bool AssociationPublicationMayBlockActivateHandler()
	{
		// Association is an advertisement, not a request/response operation. It
		// must be posted after Activate has returned to prevent a cross-process
		// SendMessage cycle between the two UI threads.
		return false;
	}

	inline RendererPublicationDecision ResolveRendererPublication(
		const std::wstring& savedRenderer,
		const std::wstring& sessionOverride,
		const std::wstring& acceptedRuntime,
		bool editorApply, bool savedRendererChanged)
	{
		if (editorApply && savedRendererChanged && !savedRenderer.empty())
			return { savedRenderer,
				RendererPublicationSource::SavedConfiguration };
		if (!sessionOverride.empty())
			return { sessionOverride,
				RendererPublicationSource::SessionOverride };
		if (!acceptedRuntime.empty())
			return { acceptedRuntime,
				RendererPublicationSource::AcceptedRuntime };
		if (!savedRenderer.empty())
			return { savedRenderer,
				RendererPublicationSource::SavedConfiguration };
		return {};
	}

	inline bool MayDispatchGlobalShortcut(uint32_t videoProcessorProcessId,
		uint32_t foregroundProcessId, bool configurationModal)
	{
		return !configurationModal && foregroundProcessId != 0 &&
			foregroundProcessId != videoProcessorProcessId;
	}

	inline bool MayDispatchBackgroundAccelerator(bool expectedControl,
		bool expectedAlt, bool expectedShift)
	{
		// VP shortcuts are intentionally global whenever Config does not own
		// focus.  Bare configured keys are just as valid as modified chords.
		(void)expectedControl;
		(void)expectedAlt;
		(void)expectedShift;
		return true;
	}

	// Delta 6 (planner, global_hotkeys): with [shortcuts] global_hotkeys off
	// (the default) only chords that cannot be ordinary typing act while
	// another application has focus: Ctrl or Alt combinations and the
	// function keys. A bare key or Shift+key (a capital letter) acts only
	// while VP has focus. On, every configured key is global (the old rule).
	inline bool IsGlobalSafeShortcut(bool control, bool alt, bool shift,
		unsigned int virtualKey)
	{
		(void)shift;
		if (control || alt)
			return true;
		return virtualKey >= 0x70 && virtualKey <= 0x87;   // VK_F1..VK_F24
	}

	inline bool MayDispatchInjectedShortcut(bool injected,
		uint32_t videoProcessorProcessId, uint32_t foregroundProcessId)
	{
		return injected && MayDispatchGlobalShortcut(videoProcessorProcessId,
			foregroundProcessId, false);
	}

	inline bool ShortcutModifiersMatch(bool expectedControl,
		bool expectedAlt, bool expectedShift, bool control, bool alt,
		bool shift)
	{
		return expectedControl == control && expectedAlt == alt &&
			expectedShift == shift;
	}

	// Windows reports Right Alt (AltGr) as Ctrl+Alt.  Fullscreen is the one
	// standard Alt shortcut where operators reasonably use either Alt key, so
	// accept the synthetic Ctrl only after exact accelerator matching has had
	// priority.  Callers must not use this relaxation for arbitrary shortcuts.
	inline bool FullscreenShortcutModifiersMatch(bool expectedControl,
		bool expectedAlt, bool expectedShift, bool control, bool alt,
		bool shift, bool rightAlt)
	{
		return ShortcutModifiersMatch(expectedControl, expectedAlt,
			expectedShift, control, alt, shift) ||
			(rightAlt && !expectedControl && expectedAlt && control && alt &&
				expectedShift == shift);
	}

	// Bug hunt 3 (SH6): AltGr (Right Alt on layouts that have it) reaches
	// Windows as Ctrl+Alt (a synthetic Left Ctrl plus Right Alt). It is not a
	// Ctrl+Alt chord: AltGr+letter types a character (AltGr+W, AltGr+V, ...),
	// so a Ctrl+Alt shortcut must never fire from it. Detected as Right Alt
	// held with Ctrl and without Left Alt; a real Ctrl+Alt chord therefore
	// uses Left Alt. The one documented relaxation, AltGr+Enter for the
	// fullscreen toggle, stays in FullscreenShortcutModifiersMatch.
	inline bool IsAltGr(bool control, bool leftAlt, bool rightAlt)
	{
		return rightAlt && control && !leftAlt;
	}

	// Exact chord match from the physical key state, AltGr excluded.
	inline bool KeyboardShortcutMatches(bool expectedControl, bool expectedAlt,
		bool expectedShift, bool control, bool leftAlt, bool rightAlt, bool shift)
	{
		if (IsAltGr(control, leftAlt, rightAlt))
			return false;
		return ShortcutModifiersMatch(expectedControl, expectedAlt, expectedShift,
			control, leftAlt || rightAlt, shift);
	}

	// Bug hunt 3 (SH6): what the background (low-level) keyboard observer
	// does with a key press. VP acts only while its own window or its
	// fullscreen output has focus - those keys reach it through the dialog
	// and the fullscreen host, not through the observer - unless the
	// matched shortcut is explicitly global ([shortcuts] global_actions, or
	// global_hotkeys on). A key VP does not act on is never swallowed.
	enum class BackgroundShortcutAction
	{
		PassThrough,         // not VP's: the foreground application gets it untouched
		DispatchAndPass,     // VP acts; the foreground application still gets the key
		DispatchAndConsume   // VP acts and takes the key
	};

	struct BackgroundShortcutContext
	{
		bool matched = false;             // the key matched an observed binding (AltGr excluded)
		bool vpForeground = false;        // a VP window (main or fullscreen) is foreground
		bool configurationForeground = false; // VP's Config owns the foreground
		bool explicitlyGlobal = false;    // the user marked this action global
		bool sameProcessConfigRoute = false; // foreground_only: the Config chord from VP's fullscreen
		bool consumeWhenActing = false;   // the command owns its key (config_editor, toggle_noui)
	};

	inline BackgroundShortcutAction DecideBackgroundShortcut(const BackgroundShortcutContext& c)
	{
		if (!c.matched || c.configurationForeground)
			return BackgroundShortcutAction::PassThrough;
		if (c.vpForeground)
			return c.sameProcessConfigRoute ? BackgroundShortcutAction::DispatchAndConsume :
				BackgroundShortcutAction::PassThrough;   // the dialog / fullscreen host handles it
		if (!c.explicitlyGlobal)
			return BackgroundShortcutAction::PassThrough;
		return c.consumeWhenActing ? BackgroundShortcutAction::DispatchAndConsume :
			BackgroundShortcutAction::DispatchAndPass;
	}

	// Bug hunt 3 (SH9): Config's window title is "VideoProcessor
	// Configuration", or "VideoProcessor Configuration <U+00B7> <target>" while it
	// edits a remote VP. VP's focus checks used to accept only the first.
	inline bool IsConfigurationEditorTitle(const std::wstring& title)
	{
		static const std::wstring base = L"VideoProcessor Configuration";
		if (title == base)
			return true;
		static const std::wstring remotePrefix = base + L" " + std::wstring(1, static_cast<wchar_t>(0x00B7)) + L" ";
		return title.size() > remotePrefix.size() &&
			title.compare(0, remotePrefix.size(), remotePrefix) == 0;
	}

	inline bool ShouldConsumeUnmatchedModifiedEnter(bool keyDown,
		uint16_t virtualKey, bool control, bool alt, bool shift)
	{
		return keyDown && virtualKey == ViewToggleDefaultKey &&
			(control || alt || shift);
	}

	inline bool ShouldEnableBackgroundShortcuts(bool modernInterface,
		bool noUi, bool foregroundOnly)
	{
		return modernInterface && !noUi && !foregroundOnly;
	}

	inline bool ShouldRequestPresentationFocus(bool foregroundOnly,
		bool configurationVisible, bool validTarget)
	{
		return foregroundOnly && !configurationVisible && validTarget;
	}

	inline bool ShouldReturnPresentationFocus(bool foregroundOnly,
		bool configurationWasForeground, bool validTarget)
	{
		return foregroundOnly && configurationWasForeground && validTarget;
	}

	// Renderer construction must consume the same capture-state generation that
	// UpdateState admitted. Configuration staging is synchronous and may widen
	// that boundary long enough for capture ingress to publish a newer state.
	inline bool MayConstructRendererAfterConfigurationBoundary(
		uint64_t appliedCaptureSequence, uint64_t latestCaptureSequence)
	{
		return appliedCaptureSequence == latestCaptureSequence;
	}

	// A restart request can be latched while no renderer exists (for example,
	// when an editor apply and a capture-device transition overlap). Building a
	// fresh renderer already fulfills that request; carrying it into the new
	// generation would immediately stop the graph that was just constructed.
	inline bool ShouldConsumeRestartForFreshRenderer(
		bool hasRenderer, bool restartRequested)
	{
		return !hasRenderer && restartRequested;
	}

	// Capture-device changes are published before the old capture run is
	// stopped. A no-signal device can legitimately have no video state (or no
	// display mode), so automatic timing must not dereference either while the
	// new selection is being applied.
    constexpr int DefaultDirectShowFrameOffsetMs = 90;

    inline int AutomaticFrameOffsetMs(bool builtInRenderer)
    {
        return builtInRenderer ? 0 : DefaultDirectShowFrameOffsetMs;
    }

    inline bool ParseFrameOffset(const std::string& normalized, bool& automatic, int& milliseconds)
    {
        if (normalized.empty() || normalized == "auto")
        {
            automatic = true;
            milliseconds = DefaultDirectShowFrameOffsetMs;
            return true;
        }
        try
        {
            size_t consumed = 0;
            const int parsed = std::stoi(normalized, &consumed);
            if (consumed != normalized.size() || parsed < 0) return false;
            automatic = false;
            milliseconds = parsed;
            return true;
        }
        catch (const std::exception&) { return false; }
    }

	inline bool ShouldSelectFirstDiscoveredValue(
		bool hasConfiguredValue, int currentSelection, int availableCount)
	{
		return !hasConfiguredValue && currentSelection < 0 &&
			availableCount > 0;
	}

	inline bool ShouldRestoreAcceptedRendererAfterReload(
		bool reloadSucceeded, bool hasAcceptedRenderer,
		bool selectionDiffers)
	{
		return !reloadSucceeded && hasAcceptedRenderer && selectionDiffers;
	}

	inline bool ShouldStageShortcutTable(bool shortcutsChanged,
		bool rendererRestartAction)
	{
		return shortcutsChanged || rendererRestartAction;
	}

	// These controls are immediate session overrides. Configuration publication
	// may update startup defaults, but must not change an already-running view.
	inline SessionPresentationState PreserveSessionPresentation(
		bool videoOnly, bool fullscreen)
	{
		return { videoOnly, fullscreen };
	}

	inline bool SessionPresentationControlIsPersistent()
	{
		return false;
	}

	inline FullscreenToggleAction ResolveFullscreenToggle(
		bool fullscreenRequested, bool fullscreenActive,
		FullscreenTransitionDirection transitionDirection)
	{
		if (transitionDirection == FullscreenTransitionDirection::Entering)
			return FullscreenToggleAction::CancelPending;
		if (transitionDirection == FullscreenTransitionDirection::Exiting)
			return FullscreenToggleAction::EnterFullscreen;
		if (fullscreenActive)
			return FullscreenToggleAction::ExitFullscreen;
		// A pending request is also the "on" state in the operator UI.
		if (fullscreenRequested)
			return FullscreenToggleAction::ExitFullscreen;
		return FullscreenToggleAction::EnterFullscreen;
	}

	inline bool FullscreenRequestedAfterToggle(FullscreenToggleAction action)
	{
		return action == FullscreenToggleAction::EnterFullscreen;
	}

	inline bool EffectiveFullscreenToggleActive(bool fullscreenRequested,
		bool fullscreenActive)
	{
		(void)fullscreenRequested;
		return fullscreenActive;
	}

	inline bool MayDispatchForegroundPresentationShortcut(
		bool hasAcceleratorTable, bool)
	{
		// Video Only removes chrome, not the dialog's message pump. Its foreground
		// accelerator table remains the authoritative path back to visible UI.
		return hasAcceleratorTable;
	}

	inline bool ShouldSuppressFullscreenTopmost(bool activationPending,
		bool configurationModal, bool editorVisible)
	{
		return activationPending || configurationModal || editorVisible;
	}

	inline bool ShouldPromoteFullscreenAfterLiveFrame(bool exclusiveFullscreen,
		bool fullscreenRequested, bool liveFrameVerified,
		bool suppressTopmost)
	{
		return exclusiveFullscreen && fullscreenRequested &&
			liveFrameVerified && !suppressTopmost;
	}

	inline bool ShouldExecuteDeferredPresentationActivation(
		bool activationPending, bool configurationEditorVisible,
		bool fullscreenRequested, bool hostValid, bool hostIconic,
		bool foregroundPolicyAllowsActivation)
	{
		return activationPending && !configurationEditorVisible &&
			fullscreenRequested && hostValid && hostIconic &&
			foregroundPolicyAllowsActivation;
	}

	inline bool ShouldRestoreConfigurationEditorForeground(
		bool editorOwnedForegroundBeforePresentation,
		uint32_t videoProcessorProcessId, uint32_t currentForegroundProcessId)
	{
		// Restore only from VP itself. If the user moved to another application
		// during the transition, that newer foreground choice wins.
		return editorOwnedForegroundBeforePresentation &&
			videoProcessorProcessId != 0 &&
			currentForegroundProcessId == videoProcessorProcessId;
	}

	inline uintptr_t SelectConfigurationEditorPresentationTarget(
		bool fullscreenRequested, uintptr_t fullscreenTarget,
		uintptr_t currentRendererTarget, uintptr_t windowedVideoTarget)
	{
		if (fullscreenRequested && fullscreenTarget != 0)
			return fullscreenTarget;
		if (currentRendererTarget != 0)
			return currentRendererTarget;
		return windowedVideoTarget;
	}

	// Config must remain owned by the stable VP host. Renderer and fullscreen
	// HWNDs are transient presentation targets used only for monitor placement;
	// making either one the native owner recreates Qt's top-level HWND during a
	// renderer/capture transition and interrupts active controls and popups.
	inline uintptr_t SelectConfigurationEditorNativeOwner(
		uintptr_t /*videoProcessorHost*/, uintptr_t /*presentationTarget*/)
	{
		// Config is a separate process with a tray lifetime. Cross-process native
		// ownership makes Qt recreate its HWND after show/renderer transitions,
		// dropping focus and invalidating reveal routing. Communication and monitor
		// placement are carried separately, so no native owner is required.
		return 0;
	}

	inline bool IsValidatedPresentationTargetProcess(
		uint32_t videoProcessorProcessId, uint32_t targetProcessId,
		bool targetWindowValid)
	{
		return targetWindowValid && videoProcessorProcessId != 0 &&
			targetProcessId == videoProcessorProcessId;
	}

	inline bool ConfigurationEditorRuntimeUsesRecurringLease()
	{
		return false;
	}

	inline bool ShouldRequestConfigurationEditorOneShotReassert(
		bool presentationTargetAccepted, bool editorWindowValid)
	{
		return presentationTargetAccepted && editorWindowValid;
	}

	// Renderer reconstruction owns an explicit reveal obligation. Do not use
	// visibility of an optional transition HWND as that obligation: transitions
	// may deliberately run without the popup to avoid stealing focus.
	inline bool ShouldRetireWaitingSurfaceAfterLiveFrame(
		uint32_t evidenceGeneration, uintptr_t evidenceTarget,
		uint32_t pendingGeneration, uintptr_t pendingTarget,
		uint32_t currentGeneration, uintptr_t currentTarget,
		bool rendererAvailable, bool rendererRunning,
		bool liveFramePresented, bool resetBlocksReveal,
		bool /*configurationEditorVisible*/)
	{
		return pendingGeneration != 0 && pendingTarget != 0 &&
			evidenceGeneration == pendingGeneration &&
			evidenceGeneration == currentGeneration &&
			evidenceTarget == pendingTarget &&
			evidenceTarget == currentTarget && rendererAvailable &&
			rendererRunning && liveFramePresented && !resetBlocksReveal;
	}

	inline bool ShouldRecreateFullscreenHostForBackendHandoff(
		bool hasPreviousRenderer, bool previousWasDirectShow,
		bool nextIsDirectShow, bool fullscreenHostExists,
		bool preserveHostForProfileRestart)
	{
		return hasPreviousRenderer && !previousWasDirectShow &&
			nextIsDirectShow && fullscreenHostExists &&
			!preserveHostForProfileRestart;
	}

	inline bool MayEnterConfigurationModal(bool editorVisible,
		bool editorIconic, uint32_t editorProcessId,
		uint32_t foregroundProcessId)
	{
		return editorVisible && !editorIconic && editorProcessId != 0 &&
			editorProcessId == foregroundProcessId;
	}

	inline bool IsDuplicateBackgroundShortcut(uint16_t lastCommand,
		uint16_t candidateCommand, uint64_t elapsedMilliseconds,
		uint64_t duplicateWindowMilliseconds)
	{
		return lastCommand != 0 && lastCommand == candidateCommand &&
			elapsedMilliseconds <= duplicateWindowMilliseconds;
	}

	inline bool MayDispatchWhileConfigurationModal(bool,
		bool)
	{
	// Config disables VP's foreground controls while it is visible, but VP's
	// configured background shortcuts must remain available.
		return true;
	}
}
