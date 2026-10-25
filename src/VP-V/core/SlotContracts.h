#pragma once

#include "PipelineSlots.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// What each pipeline slot NEEDS and GIVES, for reorderable modules (owner 2026-10-24, ADR-0021).
// Read with OrderValidator.h (which uses these contracts to accept or reject an order) and
// SlotRegistry.h (which holds the order the renderer follows).
namespace PipelineSlots
{
	// ---------------------------------------------------------------------------------------
	// REORDERABLE MODULES (owner 2026-10-24, ADR-0021). The order is DATA: the designed order
	// (the table above) is the default; a custom order permutes slots within their ZONE. Each
	// slot declares what it needs and what it gives (resolution domain, light/colour domain,
	// plane layout, vectors, "must see the original frame") plus hard precede rules; Validate()
	// accepts or rejects a whole order with a plain reason, and an invalid order never runs.
	// Net fingerprints include UpstreamKey(), so moving a module changes the key of every
	// module after it and a dependent net bypasses VISIBLY ("Fallbacks engaged").
	// ---------------------------------------------------------------------------------------

	// Zones a slot may never leave. Host slots are outside the pixel path (fixed). Libplacebo-
	// internal slots run where libplacebo's stages put them (fixed: its hooks allow no move).
	enum class Zone : uint8_t { Host, FrontEnd, Libplacebo, OutputHooks, Quantise, Present };

	inline Zone ZoneOf(Id id)
	{
		switch (Get(id).hook)
		{
		case HookPoint::Prepass: return Zone::FrontEnd;
		case HookPoint::Libplacebo: return Zone::Libplacebo;
		case HookPoint::PreOutput: return Zone::OutputHooks;
		case HookPoint::Output: return Zone::Quantise;
		case HookPoint::Present: return Zone::Present;
		default: return Zone::Host;
		}
	}

	// Domains as bit sets: a slot NEEDS one of the set bits and GIVES one value (0 = unchanged).
	namespace Res { constexpr uint8_t Delivered = 1, Encoded = 2, Processing = 4, Output = 8, Any = 15; }
	namespace Light { constexpr uint8_t Source = 1, Target = 2, Codes = 4, Any = 7; }
	namespace Layout { constexpr uint8_t Subsampled = 1, Full = 2, Any = 3; }

	struct Contract
	{
		uint8_t needRes = Res::Any, needLight = Light::Any, needLayout = Layout::Any;
		uint8_t giveRes = 0, giveLight = 0, giveLayout = 0;
		bool needsVectors = false;   // reads motion vectors (motion.estimation must come first)
		bool givesVectors = false;
		bool needsOriginal = false;  // must see the untouched delivered frame
		bool modifies = false;       // changes the picture
		bool movable = true;         // may move within its zone
	};

	inline Contract ContractOf(Id id)
	{
		Contract c;
		const uint8_t inputRes = Res::Delivered | Res::Encoded;
		switch (id)
		{
		case Id::MotionEstimation: c.needsOriginal = true; c.givesVectors = true; break;
		case Id::ScalingRebase: c.needRes = Res::Delivered; c.giveRes = Res::Encoded; c.modifies = true; break;
		case Id::RepairEncoding: c.needRes = inputRes; c.modifies = true; break;   // A_motion's vectors are optional (A_single)
		case Id::RepairDeband: c.needRes = inputRes; c.modifies = true; break;
		case Id::RepairTextureRestore: c.needRes = inputRes; c.needsVectors = true; c.modifies = true; break;
		case Id::ScalingRoute4k: c.needRes = inputRes; c.giveRes = Res::Processing; c.modifies = true; break;
		case Id::ScalingRefine: c.needRes = Res::Processing; c.modifies = true; break;
		case Id::ScalingChromaUpscale: c.needLayout = Layout::Subsampled; c.giveLayout = Layout::Full; c.modifies = true; break;
		case Id::MotionConsumers: c.needsVectors = true; c.modifies = true; c.movable = false; break;
		case Id::ColourSdrToneShape: case Id::ColourAppearance: case Id::DriveLedMeasure:
		case Id::DriveLedCompensation: case Id::DriveBlackLevel:
			c.needLight = Light::Target;
			c.modifies = id != Id::DriveLedMeasure;
			break;
		case Id::OutputCalibrationLut: c.needLight = Light::Target; c.giveLight = Light::Codes; c.modifies = true; break;
		default:
			// Host, libplacebo-internal, quantiser and presentation slots stay where they are.
			c.movable = false;
			// Fail safe (net-runner audit 2026-10-24): a slot changes the picture unless it is known
			// not to. Net fingerprints compare only picture-changing upstream slots, so a wrong "no"
			// could hide a changed pipeline; a wrong "yes" only bypasses a net visibly.
			switch (id)
			{
			case Id::InputCapture: case Id::InputSignal: case Id::InputQueueTiming: case Id::InputSourceAnalysis:
			case Id::MotionVectorDelivery: case Id::OutputPresentation: case Id::DriveProjectorControl:
			case Id::ControlNetRuntime: case Id::ControlDiagnostics: case Id::ControlConfig:
				break;
			default:
				c.modifies = true;
				break;
			}
			break;
		}
		return c;
	}

	// Hard precede rules beyond the domains: first must run before second.
	struct Precede { Id first, second; const char* why; };
	inline const std::array<Precede, 3>& HardRules()
	{
		static const std::array<Precede, 3> rules = { {
			{ Id::DriveLedMeasure, Id::DriveLedCompensation, "the LED compensation uses the measurement of the same picture" },
			{ Id::DriveLedCompensation, Id::DriveBlackLevel, "black level corrects the floor after the LED compensation" },
			{ Id::ColourSdrToneShape, Id::ColourAppearance, "Stage 13 adapts the SDR-shaped picture (delta 13 first)" },
		} };
		return rules;
	}
}
