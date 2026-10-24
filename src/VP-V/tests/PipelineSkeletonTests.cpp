#include "CppUnitTest.h"

#include <VP-V/core/ChromaSiting.h>
#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/EntranceEpoch.h>
#include <VP-V/core/OriginalPathway.h>
#include <RendererProfileConfig.h>
#include <vprenderer/AlphaQueuePolicy.h>
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

		TEST_METHOD(HeldQueueShedsABacklogBeyondItsHold)
		{
			// Gate 5 (2026-10-24): a held queue's head waits the hold by design; only a head older than
			// the hold by more than two frames (and a queue above target) is a backlog to shed.
			const double hold = 192.5;
			Assert::IsFalse(AlphaQueuePolicy::ShouldShedHeldBacklog(14, 11, hold + 10.0, hold, 60.0));    // jitter
			Assert::IsTrue(AlphaQueuePolicy::ShouldShedHeldBacklog(16, 11, hold + 40.0, hold, 60.0));     // 2.4 frames late
			Assert::IsFalse(AlphaQueuePolicy::ShouldShedHeldBacklog(11, 11, hold + 400.0, hold, 60.0));   // at target
			Assert::IsFalse(AlphaQueuePolicy::ShouldShedHeldBacklog(16, 11, hold + 400.0, 0.0, 60.0));    // no hold
			Assert::IsFalse(AlphaQueuePolicy::ShouldShedHeldBacklog(8, 4, 167.0 + 80.0, 167.0, 23.976));  // < 2 film frames
			Assert::IsTrue(AlphaQueuePolicy::ShouldShedHeldBacklog(8, 4, 167.0 + 90.0, 167.0, 23.976));
			// One frame behind for good: shed after half a second of late heads, never on jitter.
			unsigned run = 0;
			for (int i = 0; i < 29; ++i)
				Assert::IsFalse(AlphaQueuePolicy::ShouldShedHeldExcess(14, 11, hold + 22.0, hold, 60.0, run));
			Assert::IsTrue(AlphaQueuePolicy::ShouldShedHeldExcess(14, 11, hold + 22.0, hold, 60.0, run));
			Assert::AreEqual(0u, run);
			for (int i = 0; i < 100; ++i)   // jitter: late every other frame
				Assert::IsFalse(AlphaQueuePolicy::ShouldShedHeldExcess(14, 11, hold + (i % 2 ? 22.0 : 5.0), hold, 60.0, run));
		}

		TEST_METHOD(DelayBudgetQueueCapacityCoversTheHold)
		{
			// Gate 4 (2026-10-24): the FIFO must hold every frame that arrives during the hold plus two
			// slots; a queue_size below that gets the hold cut to HoldForCapacity (visibly, in the renderer).
			const double p60 = 1000.0 / 60.0;
			Assert::AreEqual(static_cast<size_t>(14), DelayBudget::QueueCapacityFor(192.5, p60));   // 11.55 -> 12 + 2
			Assert::AreEqual(static_cast<size_t>(5), DelayBudget::QueueCapacityFor(3.0 * p60, p60)); // exact fit: 3 + 2
			Assert::AreEqual(static_cast<size_t>(1), DelayBudget::QueueCapacityFor(0.0, p60));
			Assert::AreEqual(6.0 * p60, DelayBudget::HoldForCapacity(8, p60), 1e-9);
			Assert::AreEqual(0.0, DelayBudget::HoldForCapacity(2, p60), 1e-9);
			// The inverse never asks for more slots than it was given.
			for (size_t capacity = 2; capacity <= 32; ++capacity)
				Assert::IsTrue(DelayBudget::QueueCapacityFor(DelayBudget::HoldForCapacity(capacity, p60), p60) <= capacity);
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

		TEST_METHOD(EntranceEpochCoversCropRangeAndColour)
		{
			// Audit blocker 1: a cached entrance result must never come back for a frame with another
			// crop, range, bit depth or colour (the renderer keys its cache with this function).
			struct Plane { const void* texture; int components; int component_mapping[4]; };
			struct Rect { float x0, y0, x1, y1; };
			struct Bits { int sample_depth, color_depth, bit_shift; };
			struct Repr { int sys, levels, alpha; Bits bits; };
			struct Hdr { float min_luma, max_luma; };
			struct Color { int primaries, transfer; Hdr hdr; };
			struct FakeFrame { int num_planes; Plane planes[4]; Rect crop; Repr repr; Color color; };
			int textureA = 0, textureB = 0;
			FakeFrame base{};
			base.num_planes = 2;
			base.planes[0] = { &textureA, 1, { 0, -1, -1, -1 } };
			base.planes[1] = { &textureB, 2, { 1, 2, -1, -1 } };
			base.crop = { 0.0f, 0.0f, 1920.0f, 1080.0f };
			base.repr = { 1, 1, 0, { 16, 10, 6 } };
			base.color = { 1, 1, { 0.0f, 100.0f } };
			auto key = [](const FakeFrame& f) { EntranceEpoch::Key k; EntranceEpoch::AddFrame(k, f); return k.Value(); };
			const uint64_t same = key(base);
			Assert::AreEqual(same, key(base));
			FakeFrame crop = base; crop.crop.y0 = 132.0f;          // a start-up crop hold releasing
			FakeFrame levels = base; levels.repr.levels = 2;       // range re-detection
			FakeFrame depth = base; depth.repr.bits.color_depth = 12;
			FakeFrame color = base; color.color.transfer = 2;
			FakeFrame texture = base; texture.planes[1].texture = &textureA;
			for (const FakeFrame* f : { &crop, &levels, &depth, &color, &texture })
				Assert::AreNotEqual(same, key(*f));
			// B3's ring (owned copies keyed by the unique source sequence): the texture a frame was
			// uploaded into is left out, everything else still counts.
			auto ringKey = [](const FakeFrame& f) { EntranceEpoch::Key k; EntranceEpoch::AddFrame(k, f, false); return k.Value(); };
			Assert::AreEqual(ringKey(base), ringKey(texture));
			for (const FakeFrame* f : { &crop, &levels, &depth, &color })
				Assert::AreNotEqual(ringKey(base), ringKey(*f));
		}

		TEST_METHOD(EntranceRingKeepsAAndBByRoleAndReplacesTheOldest)
		{
			// B3 (ADR-0024): the processed A and B are kept as the ring's own copies, by sequence, key
			// and role; a claimed entry is valid only once committed; the least recently used goes.
			EntranceStages::Ring<int, 3> ring;
			using EntranceStages::Role;
			Assert::IsNull(ring.Find(1, 7, Role::A));
			ring.Claim(1, 7, Role::A) = 101;
			Assert::IsNull(ring.Find(1, 7, Role::A));           // not committed yet
			ring.Commit();
			Assert::AreEqual(101, *ring.Find(1, 7, Role::A));
			Assert::IsNull(ring.Find(1, 7, Role::B));           // another role
			Assert::IsNull(ring.Find(1, 8, Role::A));           // another key (parameters changed)
			Assert::IsNull(ring.Find(0, 7, Role::A));           // sequence 0 is never cached
			ring.Claim(2, 7, Role::B) = 202; ring.Commit();
			ring.Claim(2, 7, Role::A) = 203; ring.Commit();
			Assert::AreEqual(static_cast<size_t>(3), ring.ValidCount());
			(void)ring.Find(1, 7, Role::A);                     // A of 1 used again: 2/B is the oldest now
			ring.Claim(3, 7, Role::A) = 303; ring.Commit();
			Assert::IsNull(ring.Find(2, 7, Role::B));
			Assert::AreEqual(101, *ring.Find(1, 7, Role::A));
			Assert::AreEqual(203, *ring.Find(2, 7, Role::A));
			Assert::AreEqual(303, *ring.Find(3, 7, Role::A));
			// The same sequence and role refills its own entry (new parameters), never a second one.
			ring.Claim(3, 9, Role::A) = 304; ring.Commit();
			Assert::IsNull(ring.Find(3, 7, Role::A));
			Assert::AreEqual(304, *ring.Find(3, 9, Role::A));
			Assert::AreEqual(static_cast<size_t>(3), ring.ValidCount());
			// A failed copy (claimed, never committed) leaves nothing behind.
			ring.Claim(4, 7, Role::B) = 404;
			Assert::IsNull(ring.Find(4, 7, Role::B));
			ring.Invalidate();
			Assert::AreEqual(static_cast<size_t>(0), ring.ValidCount());
		}

		TEST_METHOD(NothingThatChangesThePictureBetweenLedMeasureAndCompensation)
		{
			// Audit item 8: the compensation must act on exactly the measured picture.
			PipelineSlots::Registry registry;
			std::string reason;
			// Moving Stage 13 between them (measure, appearance, compensation) is refused.
			Assert::IsFalse(registry.SetOrderByNames("drive.led_measure, colour.appearance", &reason));
			Assert::IsTrue(reason.find("between drive.led_measure and drive.led_compensation") != std::string::npos, W(reason).c_str());
		}

		TEST_METHOD(OnlyHonouredSlotSwitchesAreAccepted)
		{
			// Audit item 4: a switch for a slot the renderer never consults would be silently ignored.
			Assert::IsTrue(PipelineSlots::SwitchHonoured(Id::RepairDeband));
			Assert::IsTrue(PipelineSlots::SwitchHonoured(Id::DriveProjectorControl));
			Assert::IsFalse(PipelineSlots::SwitchHonoured(Id::ScalingGeometry));
			Assert::IsFalse(PipelineSlots::SwitchHonoured(Id::ColourToneGamut));
		}

		TEST_METHOD(OriginalPathwayOverridesAreRealKeysAndValues)
		{
			// Audit item 7: every override names a real renderer key with a value the configuration
			// validator accepts, and belongs to a slot that is not upstream-only.
			std::set<std::string> keys;
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
			{
				Assert::IsTrue(keys.insert(o.key).second, W(std::string("duplicate ") + o.key).c_str());
				Assert::IsTrue(RendererProfileConfig::ValidateBaseSetting(o.key, o.value),
					W(std::string(o.key) + ": " + o.value + " is not a valid renderer setting").c_str());
				const Id slot = PipelineSlots::FromName(o.slot);
				Assert::IsTrue(slot != Id::Count, W(std::string(o.key) + ": unknown slot " + o.slot).c_str());
				Assert::IsTrue(PipelineSlots::Get(slot).origin != PipelineSlots::Origin::Upstream,
					W(std::string(o.key) + ": an upstream-only slot has no VP additions to remove").c_str());
			}
		}

		TEST_METHOD(UpstreamKeyLeavesTheMotionConsumersOutUntilB3)
		{
			// Audit item 3: the consumers still run before the entrance in the renderer.
			PipelineSlots::Registry registry;
			const std::string key = registry.UpstreamKey(Id::ScalingOutput);
			Assert::IsTrue(key.find("motion.consumers") == std::string::npos, W(key).c_str());
			Assert::IsTrue(key.find("repair.deband") != std::string::npos, W(key).c_str());
		}

		TEST_METHOD(ChromaShiftScalesWithAnEdgeAlignedResample)
		{
			// research/chroma 2026-10-24: a 2:1 re-base of LEFT-sited 4:2:x keeps the colour in place
			// only when the plane shift scales with it (-0.5 -> -0.25); 1:1 leaves it unchanged.
			Assert::AreEqual(-0.25f, ChromaSiting::ShiftAfterResample(-0.5f, 0.5));
			Assert::AreEqual(-0.5f, ChromaSiting::ShiftAfterResample(-0.5f, 1.0));
			Assert::AreEqual(0.0f, ChromaSiting::ShiftAfterResample(0.0f, 0.75));
			Assert::AreEqual(-0.375f, ChromaSiting::ShiftAfterResample(-0.5f, 0.75));   // 1440 -> 1080 class
		}
	};
}
