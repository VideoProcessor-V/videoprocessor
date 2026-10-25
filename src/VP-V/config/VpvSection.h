#pragma once

// The VP-V Config section (settings agent, 2026-10-24; owner decisions 2026-10-24).
//
// ONE master page for everything VP-V adds: the pathway switch (VP-V / Original), the global tier,
// "Fallbacks engaged", and one sub-page per unit (Input, Motion, Repair, Scaling, Colour, Dynamic
// Drive, Output) with the unit's tier slider, its toggles listed beneath, its planned settings
// shown as "not yet implemented" and its slots with their fallback entries.
//
// Organisation pass (2026-10-24, VpvOrganisation.h, docs/settings/organisation.md): each unit tab is
// a column of small labelled cards; VP's own Rendering-page rows and sections MOVE onto them (one
// live control per key, still bound to the Rendering page's selected profile, which the VP-V
// profile picker follows), the candidates for retirement move to the Proposed retirement tab, and
// what is left of VP's on the Rendering page sits in a collapsed "All raw settings (expert)" group.
//
// SELF-CONTAINED: this module never includes ConfigEditorWindow. The host editor reaches it only
// through Host (below) and two calls, so the hook can be re-applied to a fresh upstream
// ConfigEditorWindow (M2 rebuild). See src/VP-V/config/README.md for the exact hook.
//
// While the VP-V pathway is on, upstream controls for the keys VP-V manages are greyed with
// "Managed by VP-V" (never removed). In Original pathway mode VP-V's tiers and toggles are disabled and the
// upstream controls behave exactly as upstream.

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace Vpv
{
	struct Host
	{
		// The stored value of key in section ("" when absent). Sections are the editor's names:
		// "vprenderer", "vprenderer.<profile>", "queue", "vpv".
		std::function<QString(const QString& section, const QString& key)> value;
		// Writes the value; an empty value removes the key. The host marks the document dirty.
		std::function<void(const QString& section, const QString& key, const QString& value)> setValue;
		// The profile sections under a root in file order, the default profile first, e.g.
		// profileSections("vprenderer") = { "vprenderer.Main", "vprenderer.Cinema" }; "queue" likewise.
		std::function<QStringList(const QString& root)> profileSections;
		// The object name the upstream editor gives the control of a renderer key (greying), e.g.
		// "config.vprenderer.deband_vp". May be empty: then nothing upstream is greyed.
		std::function<QString(const QString& key)> upstreamControlName;
		// The upstream widget tree the managed controls live in (greying); may be null.
		QWidget* upstreamRoot = nullptr;
	};

	// The VP-V page body (the host wraps it in its own page frame).
	QWidget* CreateSection(const Host& host);

	// Greys (vpvOn) or restores the upstream controls of the VP-V-managed keys. CreateSection
	// calls it itself; the host calls it again after it rebuilds upstream pages.
	void ApplyManagedState(const Host& host, bool vpvOn);
	// The same for the managed controls under another root (the upstream rows the VP-V page hosts).
	void ApplyManagedIn(const Host& host, QWidget* root, bool vpvOn);

	// Appends "proposed retirement" (or "proposed: hide as diagnostic") to the tooltip of every
	// upstream control of a guide section-12 candidate (coverage audit 2026-10-24). Idempotent.
	void MarkRetirements(const Host& host);

	// The pathway stored in [vpv] (default VP-V when absent).
	bool PathwayIsVpv(const Host& host);

	// Object names (tests and automation).
	inline QString ObjectName(const QString& part) { return QStringLiteral("vpv.") + part; }
}
