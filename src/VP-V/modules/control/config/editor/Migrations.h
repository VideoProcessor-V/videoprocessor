// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Config editor: the load-time migrations that exist only in VP-V (no Bill counterpart in a591ad3b5), moved
// byte-for-byte out of ConfigEditorWindow.cpp (P9 row a). Their declarations stay in the class; runLoadMigrations
// (mixed: Bill's calls, the VPV-HOOK line) keeps its order in the .cpp; testLoadMigrationsMoved (VpvConfigTests.cpp) covers the moved steps.
// Included by ConfigEditorWindow.cpp ONLY (through VpvRenderingDefaults.h): the member definitions are not
// inline, so a second including translation unit would be a duplicate definition.

#include <QDir>
#include <QStringList>
#include <QString>
#include <RefreshKey.h>
#include "QualityProfileMigration.h"
#include <RendererProfileConfig.h>
#include "ConfigEditorWindow.h"

void ConfigEditorWindow::migrateRefreshKeys()
{
    if (!configurationLoaded_ || !document_) return;
    bool migrated = false;
    for (const auto& section : document_->SectionNames())
    {
        if (!RefreshKey::IsKeyedSection(ConfigFile::NormalizeName(section))) continue;
        const auto settings = document_->SectionSettings(section);
        std::vector<std::string> keys;
        for (const auto& setting : settings) keys.push_back(ConfigFile::NormalizeName(setting.first));
        for (const auto& rename : RefreshKey::PlanSection(keys))
        {
            std::string value;
            for (const auto& setting : settings)
                if (ConfigFile::NormalizeName(setting.first) == rename.from) value = setting.second;
            if (!rename.drop)
                document_->SetKnown(section, rename.to.c_str(), value);
            document_->RemoveKnown(section, rename.from.c_str());
            migrated = true;
        }
    }
    if (migrated)
    {
        dirty_ = true;
        hasPendingMigrations_ = true;
    }
}

void ConfigEditorWindow::migrateLegacyQueueOwnership()
{
    // Fold legacy queue ownership into the first ordered queue profile in the
    // pending document. Compatibility readers remain in place for old files,
    // but every save from this editor uses [queue.*] exclusively. A real
    // migration (hunt 2, M1): it runs at load, inside runLoadMigrations'
    // before/after comparison, and keeps a backup of the original file.
    if (!configurationLoaded_ || !document_) return;
    const QStringList queues = profileSections(QStringLiteral("queue"));
    if (queues.isEmpty()) return;
    // Hunt 3 (CFG6): the section VP reads the start-up queue values from
    // (QueueConfiguration::ResolveDefaultSection): [queue] when present, else
    // the first named queue profile - not simply the first in the file.
    QString baseline = queues.front();
    for (const QString& queueSection : queues)
        if (queueSection.compare(QStringLiteral("queue"), Qt::CaseInsensitive) == 0)
        {
            baseline = queueSection;
            break;
        }
    bool migrated = false;
    auto migrate = [this, &baseline, &migrated](const QString& oldSection,
        const QString& oldKey, const QString& newKey)
    {
        const QString legacy = value(oldSection, oldKey);
        if (legacy.isEmpty()) return;
        if (value(baseline, newKey).isEmpty())
            document_->SetKnown(baseline.toStdString(),
                newKey.toStdString().c_str(), legacy.toLocal8Bit().constData());
        document_->RemoveKnown(oldSection.toStdString(),
            oldKey.toStdString().c_str());
        migrated = true;
    };
    migrate(QStringLiteral("queue_recovery"),
        QStringLiteral("reset_after_render_restart_seconds"),
        QStringLiteral("reset_after_render_restart_seconds"));
    migrate(QStringLiteral("queue_recovery"),
        QStringLiteral("reset_queue_too_large_percent"),
        QStringLiteral("reset_queue_too_large_percent"));
    migrate(QStringLiteral("directshow"),
        QStringLiteral("presentation_lead_frames"),
        QStringLiteral("lead_frames"));
    migrate(QStringLiteral("command_line"),
        QStringLiteral("queue_size"), QStringLiteral("queue_size"));
    // Hunt 3 (CFG9): VP reads [general] queue_size too (after [command_line]).
    migrate(QStringLiteral("general"),
        QStringLiteral("queue_size"), QStringLiteral("queue_size"));
    for (const QString& queueSection : queues)
    {
        const QString legacy = value(queueSection,
            QStringLiteral("steady_reserve_frames"));
        if (legacy.isEmpty()) continue;
        if (value(queueSection, QStringLiteral("target_frames")).isEmpty())
            document_->SetKnown(queueSection.toStdString(), "target_frames",
                legacy.toLocal8Bit().constData());
        document_->RemoveKnown(queueSection.toStdString(),
            "steady_reserve_frames");
        migrated = true;
    }
    if (!migrated) return;
    dirty_ = true;
    hasPendingMigrations_ = true;
    document_->requiresMigrationBackup = document_->existedAtLoad;
}

void ConfigEditorWindow::migrateLegacyRendererName()
{
    // Keep old configurations loadable, but migrate the former public display
    // name in the pending document so the next editor save uses the final name.
    // A real migration at load (hunt 2): dirty, and the original is backed up.
    // Bill's v1.4.02 list of former names ("VP Renderer (Beta)", "libplacebo")
    // is covered too.
    const QString configuredRenderer = value(QStringLiteral("general"), QStringLiteral("renderer")).trimmed();
    if (!configurationLoaded_ || !document_ ||
        (configuredRenderer.compare(QStringLiteral("VideoProcessor Renderer (Alpha)"), Qt::CaseInsensitive) != 0 &&
         configuredRenderer.compare(QStringLiteral("VP Renderer (Beta)"), Qt::CaseInsensitive) != 0 &&
         configuredRenderer.compare(QStringLiteral("libplacebo"), Qt::CaseInsensitive) != 0))
        return;
    document_->SetKnown("general", "renderer", "VP Renderer");
    dirty_ = true;
    hasPendingMigrations_ = true;
    document_->requiresMigrationBackup = document_->existedAtLoad;
}

void ConfigEditorWindow::migrateProfileValueNormalisations()
{
    // Rewrites that loadDetails used to make each time a profile was shown
    // (hunt 2, M2). They run once at load as real migrations: the editor is
    // dirty, the original file is backed up on save, and displaying a
    // profile never changes the document.
    if (!configurationLoaded_ || !document_) return;
    bool migrated = false;
    // Calibration LUT paths are stored with forward slashes.
    for (const QString& section : profileSections(QStringLiteral("vprenderer.color")))
        for (const char* key : { "calibration_lut_bt709", "calibration_lut_p3_d65",
            "calibration_lut_bt2020" })
        {
            const QString raw = value(section, QString::fromLatin1(key));
            if (raw.isEmpty()) continue;
            const QString normalized = QDir::fromNativeSeparators(raw);
            if (normalized == raw) continue;
            document_->SetKnown(section.toStdString(), key, normalized.toStdString());
            migrated = true;
        }
    // The retired downscaler spellings resolve to the quality preset. Keep
    // the preset's filter explicit instead of inheriting a parent's named
    // filter (Bill's v1.4.02 value; Scaling no longer offers Auto).
    for (const QString& section : profileSections(QStringLiteral("vprenderer.scaling")))
    {
        const QString raw = value(section, QStringLiteral("downscaler")).trimmed().toLower();
        if (raw != QStringLiteral("none") && raw != QStringLiteral("ewa_lanczos")) continue;
        document_->SetKnown(section.toStdString(), "downscaler",
            QualityProfileMigration::PresetValue("high", "downscaler"));
        migrated = true;
    }
    // Optional crop aspect limits are best-effort: a malformed value is
    // removed (a visible, backed-up migration) rather than carried on.
    for (const QString& section : profileSections(QStringLiteral("vprenderer.zoom")))
        for (const char* key : { "crop_narrower_content_aspect_limit",
            "crop_wider_content_aspect_limit" })
        {
            const QString raw = value(section, QString::fromLatin1(key));
            if (raw.isEmpty()) continue;
            AspectRatio ignored;
            std::string parseError;
            if (AspectRatioParser::Parse(raw.toStdString(), 1.0, 4.0, ignored, parseError))
                continue;
            document_->RemoveKnown(section.toStdString(), key);
            migrated = true;
        }
    if (!migrated) return;
    dirty_ = true;
    hasPendingMigrations_ = true;
    document_->requiresMigrationBackup = document_->existedAtLoad;
}

bool ConfigEditorWindow::foldLegacyInputPolicy()
{
	// The input policy keys move from [vprenderer.input] / [vprenderer] to
	// [vprenderer.input_processing], and the emptied headers are removed. Run
	// by every load path (hunt 3, CFG7: a remote Discard skipped it).
	bool migratedLegacyInputPolicy = false;
	for (const char* inputKey : { "video_conversion", "container_colorspace",
		"hdr_colorspace", "hdr_luminance" })
	{
		const std::string vpRendererInput =
			document_->Get("vprenderer.input_processing", inputKey);
		const std::string interimVpRendererInput =
			document_->Get("vprenderer.input", inputKey);
		const std::string legacyVpRenderer =
			document_->Get("vprenderer", inputKey);
		if (!interimVpRendererInput.empty())
		{
			if (vpRendererInput.empty())
			{
				document_->AddSection("vprenderer.input_processing");
				document_->SetKnown("vprenderer.input_processing", inputKey,
					interimVpRendererInput);
			}
			document_->RemoveKnown("vprenderer.input", inputKey);
			migratedLegacyInputPolicy = true;
		}
		if (!legacyVpRenderer.empty())
		{
			if (vpRendererInput.empty() && interimVpRendererInput.empty())
			{
				document_->AddSection("vprenderer.input_processing");
				document_->SetKnown("vprenderer.input_processing", inputKey,
					legacyVpRenderer);
			}
			document_->RemoveKnown("vprenderer", inputKey);
			migratedLegacyInputPolicy = true;
		}
	}
	// Removing an empty header is a visible migration (hunt 2, L9): the
	// presence of [vprenderer] alone selects the [general] validation.
	for (const char* emptied : { "vprenderer.input", "vprenderer" })
		if (document_->SectionSettings(emptied).empty() &&
			document_->RemoveSection(emptied))
			migratedLegacyInputPolicy = true;
	return migratedLegacyInputPolicy;
}
