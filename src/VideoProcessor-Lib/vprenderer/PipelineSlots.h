#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

// The pipeline skeleton's ONE slot registry (design freeze 2026-10-24; barrier scan R2).
//
// docs/design/slots.json is the authoritative slot list; this table mirrors its ids, order,
// hook point and status, and tools/check_module_manifests.py fails when the two disagree. Each
// slot's module doc is src/modules/<unit>/<slot>/README.md.
//
// Why a registry: the order of VP's stages used to be implied by where code happened to run
// (three copies of the front-end chain, the output hook list assembled in four places). With
// one table, the order is data, the re-base/repair order can be swapped in one place once the
// round-2 A diagnostic decides it (ADR-0005), and "off" has one meaning everywhere:
//
//   A slot that is off, or that has no component yet (a placeholder), RUNS NO PASS and
//   REGISTERS NO HOOK. Its output is byte-identical to the slot not existing, at zero cost.
//   (A registered no-op libplacebo hook still forces pass boundaries, so it is not free.)
//
// Header-only and pure (no libplacebo, no Windows), C++14 (the test project): shared by the
// renderer and the tests.
namespace PipelineSlots
{
	enum class Side : uint8_t { Input, Processing, Output, Control };

	// Where a slot runs. Prepass = VP's own GPU passes on the source planes before libplacebo
	// samples them; PreOutput/Output = VP hooks handed to libplacebo at those stages.
	enum class HookPoint : uint8_t { Capture, Host, Prepass, Libplacebo, PreOutput, Output, Present };

	// Built = a real component exists; Partial = part of it exists; Placeholder = no component.
	enum class Status : uint8_t { Built, Partial, Placeholder };

	// Where the slot comes from, for the ORIGINAL PATHWAY baseline (ADR-0019): Upstream = Bill's
	// pathway as it was; Mixed = the slot exists upstream but VP added options inside it (it runs
	// in its UPSTREAM FORM in the baseline); Vp = a VP addition (bypassed in the baseline).
	enum class Origin : uint8_t { Upstream, Mixed, Vp };

	enum class Id : uint8_t
	{
		InputCapture, InputSignal, InputQueueTiming, InputSourceAnalysis, MotionEstimation,
		ScalingRebase, RepairEncoding, RepairDeband, RepairTextureRestore, ScalingRoute4k,
		ScalingRefine, ScalingChromaUpscale, MotionVectorDelivery, MotionConsumers, ScalingGeometry,
		ScalingOutput, ColourToneGamut, ColourExtendedRange, ColourSdrToneShape, ColourAppearance,
		DriveLedMeasure, DriveLedCompensation, DriveBlackLevel, OutputCalibrationLut, OutputDither,
		OutputPresentation, DriveProjectorControl, ControlNetRuntime, ControlDiagnostics, ControlConfig,
		Count
	};

	struct Info
	{
		Id id;
		const char* name;   // slots.json id, "<unit>.<slot>"
		Side side;
		int order;          // pixel-path order; gaps of 10; >= 900 = outside the pixel path
		HookPoint hook;
		Status status;
		Origin origin;
	};

	// Keep one entry per line in this exact form: the drift check parses it.
	constexpr size_t kCount = static_cast<size_t>(Id::Count);

	inline const std::array<Info, kCount>& Slots()
	{
		static const std::array<Info, kCount> kSlots = { {
		{ Id::InputCapture, "input.capture", Side::Input, 10, HookPoint::Capture, Status::Built, Origin::Mixed },
		{ Id::InputSignal, "input.signal", Side::Input, 20, HookPoint::Host, Status::Built, Origin::Mixed },
		{ Id::InputQueueTiming, "input.queue_timing", Side::Input, 30, HookPoint::Host, Status::Built, Origin::Mixed },
		{ Id::InputSourceAnalysis, "input.source_analysis", Side::Input, 40, HookPoint::Host, Status::Partial, Origin::Mixed },
		{ Id::MotionEstimation, "motion.estimation", Side::Input, 50, HookPoint::Prepass, Status::Built, Origin::Vp },
		{ Id::ScalingRebase, "scaling.rebase", Side::Input, 60, HookPoint::Prepass, Status::Built, Origin::Vp },
		{ Id::RepairEncoding, "repair.encoding", Side::Input, 70, HookPoint::Prepass, Status::Placeholder, Origin::Vp },
		{ Id::RepairDeband, "repair.deband", Side::Input, 80, HookPoint::Prepass, Status::Built, Origin::Vp },
		{ Id::RepairTextureRestore, "repair.texture_restore", Side::Input, 90, HookPoint::Prepass, Status::Built, Origin::Vp },
		{ Id::ScalingRoute4k, "scaling.route_4k", Side::Input, 100, HookPoint::Prepass, Status::Placeholder, Origin::Vp },
		{ Id::ScalingRefine, "scaling.refine", Side::Input, 110, HookPoint::Prepass, Status::Placeholder, Origin::Vp },
		{ Id::ScalingChromaUpscale, "scaling.chroma_upscale", Side::Input, 120, HookPoint::Prepass, Status::Placeholder, Origin::Vp },
		{ Id::MotionVectorDelivery, "motion.vector_delivery", Side::Processing, 130, HookPoint::Host, Status::Partial, Origin::Vp },
		{ Id::MotionConsumers, "motion.consumers", Side::Processing, 140, HookPoint::Prepass, Status::Built, Origin::Vp },
		{ Id::ScalingGeometry, "scaling.geometry", Side::Processing, 150, HookPoint::Libplacebo, Status::Built, Origin::Mixed },
		{ Id::ScalingOutput, "scaling.output", Side::Processing, 160, HookPoint::Libplacebo, Status::Partial, Origin::Mixed },
		{ Id::ColourToneGamut, "colour.tone_gamut", Side::Processing, 170, HookPoint::Libplacebo, Status::Built, Origin::Upstream },
		{ Id::ColourExtendedRange, "colour.extended_range", Side::Processing, 175, HookPoint::Libplacebo, Status::Built, Origin::Vp },
		{ Id::ColourSdrToneShape, "colour.sdr_tone_shape", Side::Output, 200, HookPoint::PreOutput, Status::Built, Origin::Vp },
		{ Id::ColourAppearance, "colour.appearance", Side::Output, 210, HookPoint::PreOutput, Status::Placeholder, Origin::Vp },
		{ Id::DriveLedMeasure, "drive.led_measure", Side::Output, 220, HookPoint::PreOutput, Status::Built, Origin::Vp },
		{ Id::DriveLedCompensation, "drive.led_compensation", Side::Output, 230, HookPoint::PreOutput, Status::Built, Origin::Vp },
		{ Id::DriveBlackLevel, "drive.black_level", Side::Output, 240, HookPoint::PreOutput, Status::Placeholder, Origin::Vp },
		{ Id::OutputCalibrationLut, "output.calibration_lut", Side::Output, 250, HookPoint::PreOutput, Status::Partial, Origin::Mixed },
		{ Id::OutputDither, "output.dither", Side::Output, 260, HookPoint::Output, Status::Built, Origin::Mixed },
		{ Id::OutputPresentation, "output.presentation", Side::Output, 270, HookPoint::Present, Status::Built, Origin::Mixed },
		{ Id::DriveProjectorControl, "drive.projector_control", Side::Control, 900, HookPoint::Host, Status::Built, Origin::Vp },
		{ Id::ControlNetRuntime, "control.net_runtime", Side::Control, 910, HookPoint::Host, Status::Placeholder, Origin::Vp },
		{ Id::ControlDiagnostics, "control.diagnostics", Side::Control, 920, HookPoint::Host, Status::Partial, Origin::Mixed },
		{ Id::ControlConfig, "control.config", Side::Control, 930, HookPoint::Host, Status::Partial, Origin::Mixed },
		} };
		return kSlots;
	}

	// ADR-0005 (PENDING the round-2 A diagnostic): re-base first (order 60) is proposed; the
	// alternative puts it after repair (order 75: between repair 70 and deband 80).
	constexpr int kRebaseAfterRepairOrder = 75;

	inline const Info& Get(Id id) { return Slots()[static_cast<size_t>(id)]; }

	// Id::Count when the name is not a slot.
	inline Id FromName(const char* name)
	{
		for (const Info& info : Slots())
			if (name && std::strcmp(name, info.name) == 0)
				return info.id;
		return Id::Count;
	}

	// The input-side slots that transform the picture once per source frame (barrier B3): the
	// entrance stage list runs exactly these, in effective order.
	inline bool IsEntranceStage(Id id)
	{
		const Info& info = Get(id);
		return info.side == Side::Input && info.hook == HookPoint::Prepass && id != Id::MotionEstimation;
	}

	// Slot-level enable/disable plus which slots have a component. Defaults: every slot with a
	// component is enabled (its own settings still decide whether it does anything); every
	// placeholder is disabled and has no component.
	class Registry
	{
	public:
		Registry()
		{
			for (const Info& info : Slots())
			{
				const size_t i = static_cast<size_t>(info.id);
				component[i] = info.status != Status::Placeholder;
				enabled[i] = component[i];
			}
		}

		void SetEnabled(Id id, bool on) { enabled[static_cast<size_t>(id)] = on; }
		bool Enabled(Id id) const { return enabled[static_cast<size_t>(id)]; }

		// A real component registers itself when it lands in its slot (or is unavailable, e.g.
		// missing net weights: then the slot reports its "Fallbacks engaged" entry and stays inert).
		void SetComponent(Id id, bool present) { component[static_cast<size_t>(id)] = present; }
		bool HasComponent(Id id) const { return component[static_cast<size_t>(id)]; }

		// THE zero-cost rule: only an enabled slot with a component may run a pass or register
		// a hook. Everything else is skipped entirely.
		bool Runs(Id id) const { return Enabled(id) && HasComponent(id); }

		// ORIGINAL PATHWAY (owner 2026-10-24, ADR-0019): every VP addition off, every mixed slot
		// in its upstream form, so the pipeline reproduces Bill's original pathway. Components
		// read UpstreamForm() to drop their VP options (the settings overrides are listed per slot
		// in slots.json "original_pathway").
		void ApplyOriginalPathway()
		{
			for (const Info& info : Slots())
			{
				const size_t i = static_cast<size_t>(info.id);
				if (info.origin == Origin::Vp)
					enabled[i] = false;
				upstreamForm[i] = info.origin != Origin::Vp;
			}
			original = true;
		}
		bool OriginalPathway() const { return original; }
		bool UpstreamForm(Id id) const { return upstreamForm[static_cast<size_t>(id)]; }

		void SetRebaseFirst(bool first) { rebaseFirst = first; }
		bool RebaseFirst() const { return rebaseFirst; }

		int Order(Id id) const
		{
			return id == Id::ScalingRebase && !rebaseFirst ? kRebaseAfterRepairOrder : Get(id).order;
		}

		// Slots that run, in effective order (all of them, or those at one hook point).
		std::vector<Id> Active() const { return Collect(false, HookPoint::Host); }
		std::vector<Id> Active(HookPoint hook) const { return Collect(true, hook); }
		// The entrance stages that run this frame, in effective order.
		std::vector<Id> ActiveEntrance() const
		{
			std::vector<Id> ids = Active(HookPoint::Prepass);
			ids.erase(std::remove_if(ids.begin(), ids.end(), [](Id id) { return !IsEntranceStage(id); }), ids.end());
			return ids;
		}

	private:
		std::vector<Id> Collect(bool filter, HookPoint hook) const
		{
			std::vector<Id> ids;
			for (const Info& info : Slots())
				if (Runs(info.id) && (!filter || info.hook == hook))
					ids.push_back(info.id);
			std::stable_sort(ids.begin(), ids.end(), [this](Id a, Id b) { return Order(a) < Order(b); });
			return ids;
		}

		std::array<bool, kCount> enabled{};
		std::array<bool, kCount> component{};
		std::array<bool, kCount> upstreamForm{};
		bool rebaseFirst = true;
		bool original = false;
	};
}
