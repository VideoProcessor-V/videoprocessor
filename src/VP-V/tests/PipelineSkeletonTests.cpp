#include "CppUnitTest.h"

#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/EntranceStages.h>
#include <VP-V/core/OutputHookOrder.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/core/SlotTiming.h>
#include <VP-V/core/VectorScale.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pipeline skeleton (design freeze 2026-10-24, docs/design/VP-pipeline-final.md): the slot
// registry, the zero-cost placeholder rule, the once-per-source-frame entrance stage list, the
// corrected output-hook order and per-axis vector scaling. CPU only; the frame type is a small
// pixel buffer so byte-identity is checked exactly.
namespace PipelineSkeletonTestsNs
{
	using PipelineSlots::Id;

	struct Frame
	{
		std::vector<uint16_t> pixels;
		int width = 0;
		int height = 0;
		std::string trail;   // which stages touched it, in order
		bool operator==(const Frame& o) const
		{
			return pixels == o.pixels && width == o.width && height == o.height && trail == o.trail;
		}
	};

	inline Frame Ramp()
	{
		Frame f;
		f.width = 8;
		f.height = 2;
		for (int i = 0; i < f.width * f.height; ++i)
			f.pixels.push_back(static_cast<uint16_t>(64 + i * 50));
		return f;
	}

	// A stage that changes the picture and records itself.
	inline EntranceStages::StageList<Frame>::Stage Marker(const char* tag, int* calls = nullptr)
	{
		return [tag, calls](const Frame& in, Frame& out)
		{
			if (calls) ++*calls;
			out = in;
			for (uint16_t& p : out.pixels) p = static_cast<uint16_t>(p + 1);
			out.trail += tag;
			return true;
		};
	}

	struct Hook { int id; };

	inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	TEST_CLASS(PipelineSkeletonTests)
	{
	public:
		TEST_METHOD(SlotTableIsInIdOrderWithUniqueOrders)
		{
			std::set<int> orders;
			std::set<std::string> names;
			for (size_t i = 0; i < PipelineSlots::Slots().size(); ++i)
			{
				const PipelineSlots::Info& info = PipelineSlots::Slots()[i];
				Assert::AreEqual(i, static_cast<size_t>(info.id), L"kSlots must be indexed by Id");
				Assert::IsTrue(orders.insert(info.order).second, L"duplicate order");
				Assert::IsTrue(names.insert(info.name).second, L"duplicate name");
				Assert::IsTrue(PipelineSlots::FromName(info.name) == info.id);
			}
			// Pixel-path slots are ordered by side: input < processing < output < control.
			int previous = 0;
			for (const auto& info : PipelineSlots::Slots())
			{
				Assert::IsTrue(info.order > previous, L"table not in pipeline order");
				previous = info.order;
			}
		}

		TEST_METHOD(PlaceholdersDefaultOffAndNeverRun)
		{
			PipelineSlots::Registry registry;
			for (const auto& info : PipelineSlots::Slots())
			{
				const bool placeholder = info.status == PipelineSlots::Status::Placeholder;
				Assert::AreEqual(!placeholder, registry.Runs(info.id), std::wstring(info.name, info.name + strlen(info.name)).c_str());
				// Enabling a placeholder alone never makes it run: it has no component.
				registry.SetEnabled(info.id, true);
				Assert::AreEqual(!placeholder, registry.Runs(info.id));
			}
		}

		TEST_METHOD(EntranceOrderIsTheAgreedInputSide)
		{
			PipelineSlots::Registry registry;
			for (Id id : { Id::RepairEncoding, Id::ScalingRoute4k, Id::ScalingRefine, Id::ScalingChromaUpscale })
			{
				registry.SetComponent(id, true);
				registry.SetEnabled(id, true);
			}
			const std::vector<Id> expected = { Id::ScalingRebase, Id::RepairEncoding, Id::RepairDeband,
				Id::RepairTextureRestore, Id::ScalingRoute4k, Id::ScalingRefine, Id::ScalingChromaUpscale };
			Assert::IsTrue(registry.ActiveEntrance() == expected);
			// ADR-0005 alternative: re-base after repair, before deband.
			registry.SetRebaseFirst(false);
			const std::vector<Id> swapped = { Id::RepairEncoding, Id::ScalingRebase, Id::RepairDeband,
				Id::RepairTextureRestore, Id::ScalingRoute4k, Id::ScalingRefine, Id::ScalingChromaUpscale };
			Assert::IsTrue(registry.ActiveEntrance() == swapped);
			// Motion estimation is a prepass but never an entrance stage (it reads the original).
			Assert::IsFalse(PipelineSlots::IsEntranceStage(Id::MotionEstimation));
		}

		TEST_METHOD(OffSlotsAreByteIdenticalAndCostNoCall)
		{
			PipelineSlots::Registry registry;
			EntranceStages::StageList<Frame> list;
			int calls = 0;
			// Placeholders wired with a callable still never run: no component.
			list.Set(Id::RepairEncoding, Marker("A", &calls));
			list.Set(Id::ScalingRoute4k, Marker("4", &calls));
			list.Set(Id::ScalingRefine, Marker("R", &calls));
			list.Set(Id::ScalingChromaUpscale, Marker("C", &calls));
			const Frame in = Ramp();
			Frame out;
			auto result = list.Run(registry, in, out);
			Assert::AreEqual(0, calls);
			Assert::AreEqual(0, result.applied);
			Assert::IsTrue(out == in);

			// A built slot disabled at slot level: skipped as well.
			list.Set(Id::RepairDeband, Marker("D", &calls));
			registry.SetEnabled(Id::RepairDeband, false);
			result = list.Run(registry, in, out);
			Assert::AreEqual(0, calls);
			Assert::IsTrue(out == in);
		}

		TEST_METHOD(ActiveStagesRunInOrderAndDeclinesPassThrough)
		{
			PipelineSlots::Registry registry;
			registry.SetComponent(Id::RepairEncoding, true);
			registry.SetEnabled(Id::RepairEncoding, true);
			EntranceStages::StageList<Frame> list;
			list.Set(Id::ScalingRebase, Marker("B"));
			list.Set(Id::RepairEncoding, [](const Frame&, Frame&) { return false; });   // e.g. fingerprint bypass
			list.Set(Id::RepairDeband, Marker("D"));
			list.Set(Id::RepairTextureRestore, Marker("T"));
			Frame out;
			const auto result = list.Run(registry, Ramp(), out);
			Assert::AreEqual(std::string("BDT"), out.trail);
			Assert::AreEqual(3, result.applied);
			Assert::AreEqual(size_t(1), result.declined.size());
			Assert::IsTrue(result.declined[0] == Id::RepairEncoding);
		}

		TEST_METHOD(CacheRunsTheInputSideOncePerSourceFrame)
		{
			PipelineSlots::Registry registry;
			EntranceStages::StageList<Frame> list;
			int calls = 0;
			list.Set(Id::ScalingRebase, Marker("B", &calls));
			EntranceStages::Cache<Frame> cache;
			const Frame source = Ramp();
			bool computed = false;
			// 24p in 60: 2-3 refreshes (and extras/crossfade reads) per source frame.
			for (int refresh = 0; refresh < 5; ++refresh)
			{
				const uint64_t sequence = 1 + refresh / 3;
				cache.Get(list, registry, sequence, 7, source, &computed);
				Assert::AreEqual(refresh % 3 == 0, computed);
			}
			Assert::AreEqual(2, calls);
			// A settings change (new epoch) recomputes the same sequence.
			cache.Get(list, registry, 2, 8, source, &computed);
			Assert::IsTrue(computed);
			// Sequence 0 (unknown) is never served from the cache.
			cache.Store(0, 8, source);
			Assert::IsNull(cache.Find(0, 8));
		}

		TEST_METHOD(OutputHooksTakeTheCorrectedOrder)
		{
			using OutputHookOrder::Kind;
			using T = OutputHookOrder::Tagged<Hook>;
			// Collected in today's order, with the calibration LUT added and Stage 13 + black level present.
			std::vector<T> tagged = {
				{ { 9 }, Kind::Quantiser },
				{ { 1 }, Kind::Slot, Id::ColourSdrToneShape },
				{ { 5 }, Kind::Slot, Id::OutputCalibrationLut },
				{ { 3 }, Kind::Slot, Id::DriveLedMeasure },
				{ { 4 }, Kind::Slot, Id::DriveLedCompensation },
				{ { 6 }, Kind::UserPreOutput },
				{ { 2 }, Kind::Slot, Id::ColourAppearance },
				{ { 7 }, Kind::DejudderCache },
				{ { 41 }, Kind::Slot, Id::DriveBlackLevel },
				{ { 0 }, Kind::EncodingRepair },
				{ { 99 }, Kind::Other },
				{ { 8 }, Kind::OutputStage },
			};
			Assert::IsFalse(OutputHookOrder::InOrder(tagged));
			const std::vector<Hook> ordered = OutputHookOrder::Order(tagged);
			const std::vector<int> expected = { 99, 0, 1, 2, 3, 4, 41, 6, 5, 7, 8, 9 };
			Assert::AreEqual(expected.size(), ordered.size());
			for (size_t i = 0; i < expected.size(); ++i)
				Assert::AreEqual(expected[i], ordered[i].id);
		}

		TEST_METHOD(OutputHookOrderIsStableForTodaysChain)
		{
			using OutputHookOrder::Kind;
			// Today's chain (no Stage 13 / black level / VP LUT): already in order, so the
			// skeleton's ordering leaves the rendered output unchanged.
			std::vector<OutputHookOrder::Tagged<Hook>> tagged = {
				{ { 0 }, Kind::EncodingRepair },
				{ { 1 }, Kind::Slot, Id::ColourSdrToneShape },
				{ { 2 }, Kind::Slot, Id::ColourSdrToneShape },   // DV trims share the slot
				{ { 3 }, Kind::Slot, Id::DriveLedMeasure },
				{ { 4 }, Kind::Slot, Id::DriveLedCompensation },
				{ { 5 }, Kind::UserPreOutput },
				{ { 6 }, Kind::DejudderCache },
				{ { 7 }, Kind::Quantiser },
			};
			Assert::IsTrue(OutputHookOrder::InOrder(tagged));
			const auto ordered = OutputHookOrder::Order(tagged);
			for (int i = 0; i < 8; ++i)
				Assert::AreEqual(i, ordered[static_cast<size_t>(i)].id);
		}

		TEST_METHOD(SlotHooksReorderInPlaceOnly)
		{
			using OutputHookOrder::Kind;
			using T = OutputHookOrder::Tagged<Hook>;
			// Plumbing and user hooks keep their positions; slot hooks take the registry order.
			std::vector<T> tagged = {
				{ { 0 }, Kind::Other },
				{ { 2 }, Kind::Slot, Id::DriveLedMeasure },
				{ { 1 }, Kind::Slot, Id::ColourSdrToneShape },
				{ { 7 }, Kind::Other },
				{ { 3 }, Kind::Slot, Id::DriveLedCompensation },
			};
			PipelineSlots::Registry registry;
			const auto rank = [&registry](Id id) { return registry.Order(id); };
			const std::vector<Hook> ordered = OutputHookOrder::OrderSlotsInPlace(tagged, rank);
			const std::vector<int> expected = { 0, 1, 2, 7, 3 };
			for (size_t i = 0; i < expected.size(); ++i)
				Assert::AreEqual(expected[i], ordered[i].id);
			// Today's chain (already in order) comes back unchanged.
			std::vector<T> today = { { { 0 }, Kind::Other }, { { 1 }, Kind::Slot, Id::ColourSdrToneShape },
				{ { 2 }, Kind::Slot, Id::DriveLedMeasure }, { { 3 }, Kind::Slot, Id::DriveLedCompensation }, { { 4 }, Kind::Other } };
			const std::vector<Hook> same = OutputHookOrder::OrderSlotsInPlace(today, rank);
			for (int i = 0; i < 5; ++i)
				Assert::AreEqual(i, same[static_cast<size_t>(i)].id);
		}

		TEST_METHOD(VectorsScalePerAxis)
		{
			// Single-axis re-base: 1920x1080 delivered from a 1440x1080 encode (x only).
			const VectorScale rebase = VectorScale::FromSizes(1920, 1080, 1440, 1080);
			Assert::IsTrue(rebase.Anisotropic());
			// 4K route from the encoded grid: 1440 -> 3840, 1080 -> 2160.
			const VectorScale route = VectorScale::FromSizes(1440, 1080, 3840, 2160);
			const VectorScale chain = rebase.Then(route);
			Assert::AreEqual(2.0, chain.sx, 1e-12);
			Assert::AreEqual(2.0, chain.sy, 1e-12);
			float dx = 3.0f, dy = -1.5f;
			rebase.Apply(dx, dy);
			Assert::AreEqual(2.25f, dx);
			Assert::AreEqual(-1.5f, dy);
			Assert::IsTrue(rebase.Then(rebase.Inverse()).Identity());
			struct V { float x, y; };
			const V v = VectorScale{ 0.25, 0.5 }.Apply(V{ 8.0f, 8.0f });
			Assert::AreEqual(2.0f, v.x);
			Assert::AreEqual(4.0f, v.y);
			Assert::IsTrue(VectorScale::FromSizes(0, 0, 10, 10).Identity());
		}

		TEST_METHOD(DelayBudgetRoundsDownInsideTheBudget)
		{
			// 24p (23.976) at 200 ms on a 60 Hz output with libplacebo's 2-frame swapchain, the display
			// phase and a 20 ms processing allowance: 200 - 50 - 20 = 130 ms -> 3 frames (degraded).
			DelayBudget::Inputs in;
			in.budgetMs = 200.0; in.sourceFps = 24000.0 / 1001.0; in.refreshHz = 60.0;
			in.swapchainFrames = 2; in.processingMs = 20.0;
			DelayBudget::Plan plan = DelayBudget::Compute(in);
			Assert::IsTrue(plan.feasible);
			Assert::AreEqual(3, plan.queueFrames);
			Assert::IsTrue(plan.degraded);
			Assert::AreEqual(200.0, plan.fixedMs + plan.queueMs + plan.paddingMs, 1e-9);
			Assert::IsTrue(plan.paddingMs >= 0.0);
			// The 250 ms margin fits the design depth of 4.
			in.budgetMs = 250.0;
			plan = DelayBudget::Compute(in);
			Assert::AreEqual(4, plan.queueFrames);
			Assert::IsFalse(plan.degraded);
			// A 1-frame swapchain, no display phase (an exact-present display) and 15 ms processing
			// fit 4 frames at 200 ms.
			in.budgetMs = 200.0; in.swapchainFrames = 1; in.displayPhaseRefreshes = 0; in.processingMs = 15.0;
			Assert::AreEqual(4, DelayBudget::Compute(in).queueFrames);
			// Never more than the design depth (60p).
			in.sourceFps = 60.0;
			Assert::AreEqual(4, DelayBudget::Compute(in).queueFrames);
			// Render-ahead is inside the budget: it costs queue frames, never extra delay.
			in.sourceFps = 24.0; in.renderAheadMs = 50.0;
			plan = DelayBudget::Compute(in);
			Assert::AreEqual(2, plan.queueFrames);
			Assert::IsTrue(plan.fixedMs + plan.queueMs <= 200.0);
			// Below the 2-frame floor: infeasible, reported rather than silently over budget.
			in.budgetMs = 100.0;
			plan = DelayBudget::Compute(in);
			Assert::IsFalse(plan.feasible);
			Assert::AreEqual(0, plan.queueFrames);
			// ADR-0020: the recommended single total delay gives 4 frames at the slowest rate and
			// the SAME total at every rate (faster rates padded).
			DelayBudget::Inputs base;
			base.refreshHz = 60.0; base.swapchainFrames = 2; base.processingMs = 30.0;
			const double rates[] = { 24000.0 / 1001.0, 25.0, 30000.0 / 1001.0, 50.0, 60000.0 / 1001.0 };
			const double recommended = DelayBudget::RecommendedBudgetMs(base, rates);
			Assert::IsTrue(recommended >= 230.0 && recommended <= 260.0);   // ~230-250 ms at 24p
			for (double fps : rates)
			{
				DelayBudget::Inputs at = base;
				at.budgetMs = recommended; at.sourceFps = fps;
				const DelayBudget::Plan p = DelayBudget::Compute(at);
				Assert::AreEqual(4, p.queueFrames);
				Assert::AreEqual(recommended, p.fixedMs + p.queueMs + p.paddingMs, 1e-9);
			}
			// Unknown rate: no plan.
			in.sourceFps = 0.0;
			Assert::IsFalse(DelayBudget::Compute(in).feasible);
		}

		TEST_METHOD(OriginalPathwayTurnsEveryVpAdditionOff)
		{
			// ADR-0019: the baseline reproduces Bill's original pathway: no VP slot runs, every
			// slot that exists upstream runs in its upstream form, and the entrance is identity.
			PipelineSlots::Registry registry;
			for (const auto& info : PipelineSlots::Slots())
			{
				registry.SetComponent(info.id, true);
				registry.SetEnabled(info.id, true);
			}
			registry.ApplyOriginalPathway();
			Assert::IsTrue(registry.OriginalPathway());
			for (const auto& info : PipelineSlots::Slots())
			{
				const bool vp = info.origin == PipelineSlots::Origin::Vp;
				Assert::AreEqual(!vp, registry.Runs(info.id));
				Assert::AreEqual(!vp, registry.UpstreamForm(info.id));
			}
			Assert::IsTrue(registry.ActiveEntrance().empty());
			EntranceStages::StageList<Frame> list;
			list.Set(Id::ScalingRebase, Marker("B"));
			list.Set(Id::RepairDeband, Marker("D"));
			list.Set(Id::RepairTextureRestore, Marker("T"));
			const Frame in = Ramp();
			Frame out;
			Assert::AreEqual(0, list.Run(registry, in, out).applied);
			Assert::IsTrue(out == in);
			// Bill's pathway had no VP output hooks: the only upstream-origin output-side slots are
			// the calibration LUT (libplacebo target LUT upstream), dither and presentation.
			for (Id id : registry.Active(PipelineSlots::HookPoint::PreOutput))
				Assert::IsTrue(id == Id::OutputCalibrationLut);
		}

		TEST_METHOD(SlotTimingProjectsFromMeasuredRatios)
		{
			SlotTiming::Accumulator acc;
			acc.Record(Id::ScalingRebase, 2.0);
			acc.Record(Id::ScalingRebase, 4.0);
			acc.Record(Id::RepairDeband, 6.0);
			std::array<double, SlotTiming::kSlots> ratio{};
			ratio[static_cast<size_t>(Id::ScalingRebase)] = 0.25;
			const SlotTiming::Projection p = SlotTiming::Project(acc, ratio, 0.4, 41.7);
			Assert::AreEqual(9.0, p.laptopMs, 1e-12);
			Assert::AreEqual(3.0 * 0.25 + 6.0 * 0.4, p.projectedMs, 1e-12);
			Assert::AreEqual(1, p.unmeasuredRatios);
			Assert::IsTrue(p.fitsFrame);
			Assert::AreEqual(4.0, acc.MaxMs(Id::ScalingRebase));
		}

		TEST_METHOD(DesignedOrderIsValid)
		{
			std::string reason;
			Assert::IsTrue(PipelineSlots::Validate(PipelineSlots::DefaultOrder(), &reason), W(reason).c_str());
			PipelineSlots::Registry registry;
			Assert::IsTrue(registry.IsDefaultOrder());
		}

		TEST_METHOD(ValidReorderMovesTheModule)
		{
			// ADR-0021: the re-base/repair experiment and a deband move are valid orders.
			PipelineSlots::Registry registry;
			for (Id id : { Id::RepairEncoding, Id::ScalingRoute4k })
			{
				registry.SetComponent(id, true);
				registry.SetEnabled(id, true);
			}
			std::string reason;
			Assert::IsTrue(registry.SetOrderByNames("repair.encoding, scaling.rebase", &reason), W(reason).c_str());
			Assert::IsFalse(registry.RebaseFirst());
			Assert::IsFalse(registry.IsDefaultOrder());
			// Deband before repair (an experiment the owner named) is valid too.
			Assert::IsTrue(registry.SetOrderByNames("repair.deband > repair.encoding", &reason), W(reason).c_str());
			const std::vector<Id> expected = { Id::ScalingRebase, Id::RepairDeband, Id::RepairEncoding,
				Id::RepairTextureRestore, Id::ScalingRoute4k };
			Assert::IsTrue(registry.ActiveEntrance() == expected);
			// The stage list follows the data.
			EntranceStages::StageList<Frame> list;
			list.Set(Id::ScalingRebase, Marker("B"));
			list.Set(Id::RepairDeband, Marker("D"));
			list.Set(Id::RepairEncoding, Marker("A"));
			list.Set(Id::RepairTextureRestore, Marker("T"));
			list.Set(Id::ScalingRoute4k, Marker("4"));
			Frame out;
			list.Run(registry, Ramp(), out);
			Assert::AreEqual(std::string("BDAT4"), out.trail);
			// An empty setting returns to the designed order.
			Assert::IsTrue(registry.SetOrderByNames("", &reason));
			Assert::IsTrue(registry.IsDefaultOrder());
		}

		TEST_METHOD(InvalidReordersAreRejectedWithAPlainReason)
		{
			PipelineSlots::Registry registry;
			std::string reason;
			auto rejected = [&](const char* order, const char* expect)
			{
				const std::vector<Id> before = registry.CurrentOrder();
				Assert::IsFalse(registry.SetOrderByNames(order, &reason), W(order).c_str());
				Assert::IsTrue(reason.find(expect) != std::string::npos, W(reason).c_str());
				Assert::IsTrue(registry.CurrentOrder() == before, L"a rejected order must change nothing");
			};
			// Deband would see the processing resolution after the 4K route.
			rejected("scaling.route_4k, repair.deband", "processing resolution");
			// Re-base needs the delivered resolution.
			rejected("scaling.route_4k, scaling.rebase", "needs the delivered");
			// Motion estimation must see the original frame.
			rejected("scaling.rebase, motion.estimation", "original delivered frame");
			// Texture restore needs vectors.
			rejected("repair.texture_restore, motion.estimation", "motion vectors");
			// The calibration LUT gives projector codes: nothing that needs target light may follow it.
			rejected("output.calibration_lut, drive.black_level", "target light");
			// Hard rule: LED measurement before compensation.
			rejected("drive.led_compensation, drive.led_measure", "must run before");
			// Zones: a front-end module cannot move into the output hooks.
			rejected("colour.appearance, repair.deband", "out of its zone");
			// Libplacebo-internal slots are fixed.
			rejected("colour.tone_gamut, scaling.output", "fixed in its zone");
			rejected("scaling.nonexistent", "unknown slot");
		}

		TEST_METHOD(MovingAModuleChangesDownstreamFingerprints)
		{
			// Net fingerprints include the upstream order: a move changes the key of the nets
			// after it (they bypass visibly), and leaves the keys before it unchanged.
			PipelineSlots::Registry registry;
			for (Id id : { Id::RepairEncoding, Id::ScalingRoute4k, Id::ScalingRefine })
			{
				registry.SetComponent(id, true);
				registry.SetEnabled(id, true);
			}
			const std::string refineKey = registry.UpstreamKey(Id::ScalingRefine);
			const std::string motionKey = registry.UpstreamKey(Id::MotionEstimation);
			Assert::IsTrue(refineKey.find("scaling.rebase>repair.encoding>repair.deband") != std::string::npos, W(refineKey).c_str());
			std::string reason;
			Assert::IsTrue(registry.SetOrderByNames("repair.deband, repair.encoding", &reason));
			Assert::AreNotEqual(refineKey, registry.UpstreamKey(Id::ScalingRefine));
			Assert::AreEqual(motionKey, registry.UpstreamKey(Id::MotionEstimation));
			// Switching an upstream module off changes the key too.
			registry.ResetOrder();
			registry.SetEnabled(Id::RepairDeband, false);
			Assert::AreNotEqual(refineKey, registry.UpstreamKey(Id::ScalingRefine));
		}

		TEST_METHOD(OriginalPathwayRestoresTheDesignedOrder)
		{
			PipelineSlots::Registry registry;
			std::string reason;
			Assert::IsTrue(registry.SetOrderByNames("repair.encoding, scaling.rebase", &reason));
			registry.ApplyOriginalPathway();
			Assert::IsTrue(registry.IsDefaultOrder());
		}
	};
}
