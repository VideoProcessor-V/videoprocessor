// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "VpvSettingsModule.h"

#include "VpvRendererExtension.h"
#include "VpvRendererSettings.h"

#include <VP-V/core/hub/ExtensionHub.h>

#include <memory>

namespace Vpv
{
	namespace Settings
	{
		namespace
		{
			namespace RE = RendererExtension;

			RendererSettingsExtension& Mutable(void* settings) { return *static_cast<RendererSettingsExtension*>(settings); }
			const RendererSettingsExtension& Const(const void* settings) { return *static_cast<const RendererSettingsExtension*>(settings); }

			class SettingsModule : public Hub::ISkeletonModule
			{
			public:
				const char* Name() const override { return "settings"; }

				void OnSettingsLoad(RE::SettingsLoadContext& c) override
				{
					switch (c.phase)
					{
					case RE::SettingsPhase::ReadBase:
						VpvRendererSettings::ReadBase(*c.config, Mutable(c.settings));
						break;
					case RE::SettingsPhase::ReadProfile:
						VpvRendererSettings::ReadProfile(*c.config, *c.section, *c.rule, c.flag, Mutable(c.settings));
						break;
					case RE::SettingsPhase::ReadGlobal:
						VpvRendererSettings::ReadGlobal(*c.config, Mutable(c.settings));
						break;
					case RE::SettingsPhase::PostLoad:
					{
						const VpvRendererSettings::Overrides decided = VpvRendererSettings::PostLoad(Mutable(c.settings), c.flag);
						if (RE::SettingsOverrides* out = static_cast<RE::SettingsOverrides*>(c.result))
						{
							out->hasPresentEveryVsync = true;
							out->presentEveryVsync = decided.presentEveryVsync;
							out->chromaUpscaler = decided.chromaUpscaler;
						}
						break;
					}
					case RE::SettingsPhase::Fingerprint:
						if (c.out) *c.out += VpvRendererSettings::Fingerprint(Const(c.other), c.includeLive);
						break;
					case RE::SettingsPhase::Delta:
						if (c.changed) VpvRendererSettings::AppendDelta(Mutable(c.settings), Const(c.other), *c.changed);
						break;
					case RE::SettingsPhase::CopyLive:
						VpvRendererSettings::CopyLive(Mutable(c.settings), Const(c.other));
						break;
					}
				}
			};
		}

		void RegisterSettingsModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new SettingsModule()));
		}
	}
}
