#include <pch.h>
#include "DynamicDriveHost.h"

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/RunningPathway.h>
#include <VP-V/core/RunningPathwayLatch.h>
#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <VP-V/modules/drive/led_compensation/LedDriveHoldover.h>
#include <VP-V/modules/drive/projector_control/LedDriveArbiter.h>
#include <filesource/FileHostSettings.h>
#include <ConfigFile.h>
#include <DebugLog.h>
#include <RendererConfigView.h>
#include <ShortcutDefaults.h>

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace LedDrive
{
	namespace
	{
		bool TryGetPipelineString(const ConfigFile& config, const char* key, std::string& value)
		{
			return config.TryGetString("vpv", key, value) || RendererConfigView(config).TryGetDisplayString(key, value);
		}

		// Mirrors VpvRendererSettings ReadGlobal ([vpv] first, then [vprenderer] / display section):
		// slot drive.projector_control is off on the Original pathway or when listed in pipeline_slots_off.
		bool ConfigAllowsProjectorControl(const ConfigFile& config)
		{
			std::string raw;
			if (TryGetPipelineString(config, "pipeline_original_pathway", raw) &&
				ConfigFile::NormalizeName(raw) == "on")
				return false;
			if (!TryGetPipelineString(config, "pipeline_slots_off", raw))
				return true;
			const char* const slotName = PipelineSlots::Get(PipelineSlots::Id::DriveProjectorControl).name;
			std::string token;
			auto tokenDisables = [&]() -> bool
			{
				if (token.empty())
					return false;
				const bool match = ConfigFile::NormalizeName(token) == slotName;
				token.clear();
				return match;
			};
			for (char ch : raw)
			{
				if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t')
				{
					if (tokenDisables())
						return false;
				}
				else
					token += ch;
			}
			return !tokenDisables();
		}
	}

	DynamicDriveShared::DynamicDriveShared(std::shared_ptr<QueuedFrameStatistics::IObserver>& walkObserverIn) :
		walkObserver(walkObserverIn)
	{
	}

	void DynamicDriveShared::PublishModule(const std::shared_ptr<LedDriveModule>& next)
	{
		std::atomic_store(&module, next);
	}

	void DynamicDriveShared::PublishObserver(const std::shared_ptr<LedDriveModule>& next, bool onlyWhenWanted)
	{
		if (next && next->WantsPredictions())
			std::atomic_store(&walkObserver, std::shared_ptr<QueuedFrameStatistics::IObserver>(next, &next->Lookahead()));
		else if (!onlyWhenWanted)
			std::atomic_store(&walkObserver, std::shared_ptr<QueuedFrameStatistics::IObserver>());
	}

	void DynamicDriveShared::Clear()
	{
		std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
		std::atomic_store(&walkObserver, std::shared_ptr<QueuedFrameStatistics::IObserver>());
	}

	void DynamicDriveShared::PublishParams(const DynamicDriveParams& p)
	{
		gammaBoost.store(p.gammaBoost, std::memory_order_release);
		compensationEotf.store(p.compensationEotf, std::memory_order_release);
		knee.store(p.knee, std::memory_order_release);
		boostKnee.store(p.boostKnee, std::memory_order_release);
		boostKneeWidth.store(p.boostKneeWidth, std::memory_order_release);
		shadowLift.store(p.shadowLift, std::memory_order_release);
		wtwPassthrough.store(p.wtwPassthrough, std::memory_order_release);
		btbPassthrough.store(p.btbPassthrough, std::memory_order_release);
	}

	void DynamicDriveShared::PublishRejection(const std::string& reason)
	{
		std::lock_guard<std::mutex> lock(rejectionMutex);
		if (reason == rejection)
			return;
		rejection = reason;
		rejectionGeneration.fetch_add(1, std::memory_order_acq_rel);
	}

	std::string DynamicDriveShared::PublishedRejection() const
	{
		std::lock_guard<std::mutex> lock(rejectionMutex);
		return rejection;
	}

	std::shared_ptr<LedDriveModule> DynamicDriveShared::Module() const
	{
		return std::atomic_load(&module);
	}

	size_t DynamicDriveShared::WantedLookaheadFrames() const
	{
		if (!std::atomic_load(&walkObserver))
			return 0;
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		return led ? led->WantedLookaheadFrames() : 0;
	}

	DynamicDriveParams DynamicDriveShared::PublishedParams() const
	{
		DynamicDriveParams p;
		p.gammaBoost = gammaBoost.load(std::memory_order_acquire);
		p.compensationEotf = compensationEotf.load(std::memory_order_acquire);
		p.knee = knee.load(std::memory_order_acquire);
		p.boostKnee = boostKnee.load(std::memory_order_acquire);
		p.boostKneeWidth = boostKneeWidth.load(std::memory_order_acquire);
		p.wtwPassthrough = wtwPassthrough.load(std::memory_order_acquire);
		p.btbPassthrough = btbPassthrough.load(std::memory_order_acquire);
		p.shadowLift = shadowLift.load(std::memory_order_acquire);
		return p;
	}

	// GEM-190: single background lifecycle worker so Update() (called under renderer stateMutex)
	// never blocks on LedDriveModule::Stop() while still guaranteeing ordered serial-port handoff
	// (previous->Stop() always completes before a replacement module->Start() opens the port).
	// Lock ordering: stateMutex -> LifecycleWorker::mutex (NEVER reverse). Stop()/Start() and final
	// shared_ptr<LedDriveModule> destruction always run strictly outside LifecycleWorker::mutex.
	struct DynamicDriveHost::LifecycleWorker
	{
		enum class TaskKind { Stop, Start };

		struct Task
		{
			TaskKind kind;
			std::shared_ptr<LedDriveModule> module;
			uint64_t seq;
		};

		std::mutex mutex;
		std::condition_variable workCv;
		std::condition_variable idleCv;
		std::deque<Task> queue;
		std::shared_ptr<LedDriveModule> activeModule;
		TaskKind activeKind = TaskKind::Stop;
		uint64_t activeSeq = 0;
		uint64_t nextSeq = 1;
		bool shuttingDown = false;
		std::thread thread;

		void EnsureStartedLocked()
		{
			if (!thread.joinable() && !shuttingDown)
				thread = std::thread(&LifecycleWorker::Run, this);
		}

		// Non-blocking (safe with stateMutex held): if `target` has a queued Start that has not run
		// yet, cancels it instead of enqueueing a redundant Stop; otherwise queues Stop(target).
		void EnqueueStop(std::shared_ptr<LedDriveModule> target)
		{
			if (!target)
				return;
			std::shared_ptr<LedDriveModule> cancelled;
			{
				std::lock_guard<std::mutex> lock(mutex);
				for (auto it = queue.begin(); it != queue.end(); ++it)
				{
					if (it->kind == TaskKind::Start && it->module == target)
					{
						cancelled = std::move(it->module);
						queue.erase(it);
						idleCv.notify_all();
						break;
					}
				}
				if (!cancelled && !shuttingDown)
				{
					EnsureStartedLocked();
					queue.push_back(Task{ TaskKind::Stop, std::move(target), nextSeq++ });
					workCv.notify_one();
				}
			}
			cancelled.reset();
			target.reset();
		}

		// Non-blocking (safe with stateMutex held): when prior Stop/Start work is queued or in
		// flight, queues Start(target) strictly behind it and returns true; returns false when idle
		// so the caller may call target->Start() synchronously.
		bool EnqueueStartIfBusy(const std::shared_ptr<LedDriveModule>& target)
		{
			if (!target)
				return false;
			std::lock_guard<std::mutex> lock(mutex);
			if (shuttingDown || (!activeModule && queue.empty()))
				return false;
			EnsureStartedLocked();
			queue.push_back(Task{ TaskKind::Start, target, nextSeq++ });
			workCv.notify_one();
			return true;
		}

		// Called OUTSIDE stateMutex before target->Stop(): cancels a queued Start (returning true so
		// the caller skips Stop()) or waits out an in-flight Start() so Stop() cannot race ahead of it.
		bool CancelOrWaitStart(const std::shared_ptr<LedDriveModule>& target)
		{
			if (!target)
				return false;
			std::shared_ptr<LedDriveModule> cancelled;
			{
				std::unique_lock<std::mutex> lock(mutex);
				for (auto it = queue.begin(); it != queue.end(); ++it)
				{
					if (it->kind == TaskKind::Start && it->module == target)
					{
						cancelled = std::move(it->module);
						queue.erase(it);
						idleCv.notify_all();
						break;
					}
				}
				if (!cancelled)
				{
					idleCv.wait(lock, [this, &target]
					{
						return !(activeKind == TaskKind::Start && activeModule == target);
					});
				}
			}
			const bool wasCancelled = cancelled != nullptr;
			cancelled.reset();
			return wasCancelled;
		}

		// Called OUTSIDE stateMutex: waits until every task enqueued at or before this call finishes.
		void Flush()
		{
			std::unique_lock<std::mutex> lock(mutex);
			const uint64_t targetSeq = nextSeq - 1;
			if (targetSeq == 0)
				return;
			idleCv.wait(lock, [this, targetSeq]
			{
				const bool activePending = activeModule != nullptr && activeSeq <= targetSeq;
				const bool queuedPending = !queue.empty() && queue.front().seq <= targetSeq;
				return !activePending && !queuedPending;
			});
		}

		// Called by StopForShutdown WITHOUT stateMutex: cancels all queued Start tasks, waits out any
		// in-flight Start() (thread spawn only, no serial I/O), and returns all active/queued Stop
		// modules (plus any just-finished in-flight Start) so the caller stops them outside all locks.
		std::vector<std::shared_ptr<LedDriveModule>> CancelPendingStartsAndCollectActive()
		{
			std::vector<std::shared_ptr<LedDriveModule>> toStop;
			std::vector<std::shared_ptr<LedDriveModule>> releasedOutsideLock;
			const auto addUnique = [&toStop](const std::shared_ptr<LedDriveModule>& mod)
			{
				if (mod && std::find(toStop.begin(), toStop.end(), mod) == toStop.end())
					toStop.push_back(mod);
			};
			{
				std::unique_lock<std::mutex> lock(mutex);
				for (;;)
				{
					for (auto it = queue.begin(); it != queue.end(); )
					{
						if (it->kind == TaskKind::Start)
						{
							releasedOutsideLock.push_back(std::move(it->module));
							it = queue.erase(it);
						}
						else
							++it;
					}
					if (activeModule && activeKind == TaskKind::Start)
					{
						std::shared_ptr<LedDriveModule> starting = activeModule;
						addUnique(starting);
						idleCv.wait(lock, [this, &starting]
						{
							return !(activeKind == TaskKind::Start && activeModule == starting);
						});
						continue;
					}
					break;
				}
				if (activeModule)
					addUnique(activeModule);
				for (const Task& task : queue)
					if (task.kind == TaskKind::Stop)
						addUnique(task.module);
				idleCv.notify_all();
			}
			releasedOutsideLock.clear();
			return toStop;
		}

		// Called by ~DynamicDriveHost(): cancels queued Start tasks, drains queued/in-flight Stop
		// tasks so running modules restore full light, and joins the worker thread cleanly.
		void Shutdown()
		{
			std::thread workerToJoin;
			std::vector<std::shared_ptr<LedDriveModule>> releasedOutsideLock;
			{
				std::lock_guard<std::mutex> lock(mutex);
				shuttingDown = true;
				for (auto it = queue.begin(); it != queue.end(); )
				{
					if (it->kind == TaskKind::Start)
					{
						releasedOutsideLock.push_back(std::move(it->module));
						it = queue.erase(it);
					}
					else
						++it;
				}
				workerToJoin = std::move(thread);
				workCv.notify_all();
				idleCv.notify_all();
			}
			releasedOutsideLock.clear();
			if (workerToJoin.joinable())
				workerToJoin.join();
		}

		void Run()
		{
			std::unique_lock<std::mutex> lock(mutex);
			for (;;)
			{
				workCv.wait(lock, [this] { return !queue.empty() || shuttingDown; });
				if (queue.empty())
				{
					if (shuttingDown)
						break;
					continue;
				}
				Task task = std::move(queue.front());
				queue.pop_front();
				activeKind = task.kind;
				activeModule = task.module;
				activeSeq = task.seq;
				lock.unlock();

				if (task.kind == TaskKind::Stop)
					task.module->Stop();
				else
					task.module->Start();

				std::shared_ptr<LedDriveModule> releaseOutsideLock;
				lock.lock();
				releaseOutsideLock = std::move(activeModule);
				activeSeq = 0;
				idleCv.notify_all();
				lock.unlock();
				// Release references strictly outside mutex (~LedDriveModule calls Stop()).
				task.module.reset();
				releaseOutsideLock.reset();
				lock.lock();
			}
		}
	};

	DynamicDriveHost::DynamicDriveHost(std::mutex& rendererStateMutex) :
		stateMutex(rendererStateMutex),
		explicitOriginal(false),
		candidateProjectorAllowed(true),
		lifecycleWorker(std::make_unique<LifecycleWorker>())
	{
	}

	DynamicDriveHost::~DynamicDriveHost()
	{
		if (lifecycleWorker)
			lifecycleWorker->Shutdown();
	}

	void DynamicDriveHost::SetPortFactoryForTest(std::function<std::unique_ptr<ISerialPort>()> factory)
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		portFactoryForTest = std::move(factory);
	}

	std::shared_ptr<LedDriveModule> DynamicDriveHost::Module() const
	{
		return std::atomic_load(&module);
	}

	void DynamicDriveHost::ApplySnapshot(const std::shared_ptr<const ConfigFile>& snapshotConfiguration,
		const std::string& sourceKind, const HostContext& context, bool deferUntilAttach)
	{
		if (!snapshotConfiguration)
			return;
		// VP Renderer filter (2026-10-17): the LED hold-over is for file playback only.
		holdoverMs = sourceKind == "file" ?
			FileSource::ReadFileHostSettings(*snapshotConfiguration).ledHoldoverMs : 0;
		configuration = snapshotConfiguration;
		if (deferUntilAttach)
		{
			deferredSnapshot = true;
			return;
		}
		deferredSnapshot = false;
		if (!suspended)
			Update(*snapshotConfiguration, context);
	}

	void DynamicDriveHost::ApplyAcceptedSnapshot(const void* rendererHost, const ConfigFile* candidateConfig,
		bool hasExplicitPathway, bool explicitOriginalPathway,
		const std::shared_ptr<const ConfigFile>& snapshotConfig, const std::string& sourceKind,
		const HostContext& context, bool rebuilding, bool implInitialized, bool candidateProjectorAllowedIn)
	{
		const bool nextOriginal = hasExplicitPathway ? explicitOriginalPathway :
			(candidateConfig != nullptr && OriginalPathwayForConfig([candidateConfig](const char* section, const char* key, std::string& out)
				{ return candidateConfig->TryGetString(section, key, out); }));
		explicitOriginal.store(nextOriginal, std::memory_order_release);
		candidateProjectorAllowed.store(!nextOriginal && candidateProjectorAllowedIn, std::memory_order_release);
		Vpv::RunningPathway::Store(nextOriginal);
		Vpv::RunningPathway::NotifyHostPathway(rendererHost, nextOriginal);
		if (!snapshotConfig)
		{
			if (nextOriginal)
			{
				deferredSnapshot = false;
				if (!rejection.empty())
				{
					rejection.clear();
					std::atomic_store(&statusRejection, std::shared_ptr<const std::string>());
				}
				params = DynamicDriveParams();
				aliasConflict.clear();
				signature.clear();
				std::shared_ptr<LedDriveModule> previous = std::atomic_load(&module);
				if (previous)
					lifecycleWorker->EnqueueStop(previous);
				std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
				if (context.frame)
				{
					context.frame->Clear();
					context.frame->PublishRejection(std::string());
					context.frame->PublishParams(DynamicDriveParams());
				}
			}
			return;
		}
		// W23-REG-H3 / W23-C2-S2-H3: on Original, stop/clear immediately (never defer); on VP-V, never
		// publish onto an outgoing Original/uninitialized Impl — defer until post-Initialize Attach().
		const bool deferUntilAttach = !nextOriginal && (rebuilding || !implInitialized) && (!context.projectorAllowed || !implInitialized);
		ApplySnapshot(snapshotConfig, sourceKind, context, deferUntilAttach);
	}

	void DynamicDriveHost::Attach(const HostContext& context)
	{
		std::shared_ptr<LedDriveModule> stopped;
		std::shared_ptr<LedDriveModule> attached;
		DynamicDriveParams publishedParams;
		std::string publishedRejection;
		bool allowPublish = false;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			const bool isOriginal = explicitOriginal.load(std::memory_order_acquire) || Vpv::RunningPathway::Original();
			const bool allowAttach = context.projectorAllowed && candidateProjectorAllowed.load(std::memory_order_acquire) && !isOriginal;
			allowPublish = allowAttach;
			if (!allowAttach)
			{
				if (deferredSnapshot && configuration && !suspended)
				{
					deferredSnapshot = false;
					Update(*configuration, context);
				}
				stopped = DetachLocked(context);
				if (isOriginal)
				{
					params = DynamicDriveParams();
					if (!rejection.empty())
					{
						rejection.clear();
						std::atomic_store(&statusRejection, std::shared_ptr<const std::string>());
					}
				}
			}
			else if (!context.implInitialized)
			{
				// W23-C2-S2-H3: Build() runs before Initialize() resolves pipelineSlots; preserve an
				// already-running module across internal resets, but defer starting a new module until
				// post-Initialize Attach() confirms VP-V and drive.projector_control.
				if (configuration && !std::atomic_load(&module))
					deferredSnapshot = true;
				attached = std::atomic_load(&module);
			}
			else
			{
				if (!suspended && configuration && (deferredSnapshot || !std::atomic_load(&module)))
				{
					deferredSnapshot = false;
					Update(*configuration, context);
				}
				attached = std::atomic_load(&module);
			}
			publishedParams = params;
			publishedRejection = rejection;
		}
		if (stopped)
		{
			if (!lifecycleWorker->CancelOrWaitStart(stopped))
				stopped->Stop();
			DebugLog::Log("Dynamic Drive: stopped on attach (slot drive.projector_control off: Original pathway or pipeline_slots_off)");
		}
		if (!context.frame)
			return;
		if (allowPublish)
		{
			context.frame->PublishModule(attached);
			if (attached)
				context.frame->PublishObserver(attached, true);
		}
		context.frame->PublishParams(publishedParams);
		context.frame->PublishRejection(publishedRejection);
	}

	void DynamicDriveHost::ParkForHoldover(const HostContext& context)
	{
		// VP Renderer filter (2026-10-17): in the filter process (source=file) the running LED
		// drive is held for the next graph instead of returning the projector to full light; the
		// capture app never gets here with a hold-over.
		// POD1-M4: synchronize state fields (holdoverMs, suspended, signature, module) under stateMutex.
		lifecycleWorker->Flush();
		std::shared_ptr<LedDriveModule> held;
		std::string heldSignature;
		uint32_t heldMs = 0;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			const std::shared_ptr<LedDriveModule> current = std::atomic_load(&module);
			if (holdoverMs > 0 && current && !suspended)
			{
				held = current;
				heldSignature = signature;
				heldMs = holdoverMs;
				std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
				if (context.frame)
					context.frame->PublishModule(std::shared_ptr<LedDriveModule>());
			}
		}
		if (held)
			ProcessLedDriveHoldover().Park(held, heldSignature, heldMs);
	}

	std::shared_ptr<LedDriveModule> DynamicDriveHost::DetachLocked(const HostContext& context)
	{
		std::shared_ptr<LedDriveModule> previous = std::atomic_load(&module);
		std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
		signature.clear();
		if (context.frame)
			context.frame->Clear();
		return previous;
	}

	void DynamicDriveHost::Update(const ConfigFile& config, const HostContext& context)
	{
		// Original == Bill: inert on pipeline_original_pathway: on — no [led_drive.*] parsing,
		// no validation rejections/fallbacks, no gamma boost/shadow lift, stop any active LED module.
		if (explicitOriginal.load(std::memory_order_acquire) || Vpv::RunningPathway::Original() ||
			OriginalPathwayForConfig([&config](const char* section, const char* key, std::string& out)
			{ return config.TryGetString(section, key, out); }))
		{
			if (!rejection.empty())
			{
				rejection.clear();
				std::atomic_store(&statusRejection, std::shared_ptr<const std::string>());
			}
			params = DynamicDriveParams();
			aliasConflict.clear();
			signature.clear();
			std::shared_ptr<LedDriveModule> previous = std::atomic_load(&module);
			if (previous)
				lifecycleWorker->EnqueueStop(previous);
			std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
			if (context.frame)
			{
				context.frame->Clear();
				context.frame->PublishRejection(std::string());
				context.frame->PublishParams(DynamicDriveParams());
			}
			return;
		}
		// Only [projector.default] and [led_drive.default] (or its alias [dynamic_drive.default])
		// matter; any other configuration change leaves a running module alone.
		std::string next;
		const std::string ledSection = ResolveLedDriveSection(config);
		for (const std::string& section : { std::string(PROJECTOR_SECTION), ledSection })
			if (const auto* values = config.GetSectionValues(section))
				for (const auto& value : *values)
					next += section + "." + value.first + "=" + value.second + "\n";
		// Both spellings in one file: [led_drive.*] wins; said once per change.
		const std::string conflict = LedDriveAliasConflict(config);
		if (conflict != aliasConflict && !conflict.empty())
			DebugLog::Log("Dynamic Drive: WARNING %s", conflict.c_str());
		aliasConflict = conflict;
		// An LED profile file is part of the settings: editing it applies too.
		const std::string ledProfilePath = LedProfilePath(config);
		if (!ledProfilePath.empty())
		{
			std::ifstream profile(ledProfilePath, std::ios::binary);
			next += "profile=" + std::string((std::istreambuf_iterator<char>(profile)),
				std::istreambuf_iterator<char>());
		}
		std::string validationError;
		const bool valid = Validate(config, validationError);
		// Bug hunt 31 H1: a rejected configuration turns the module and the gamma boost off; that is
		// shown (the "Dynamic Drive" fallback entry and status line), never log-only. A valid Apply
		// releases it.
		const std::string nextRejection = valid ? std::string() :
			(validationError.empty() ? std::string("invalid settings") : validationError);
		if (nextRejection != rejection)
		{
			rejection = nextRejection;
			std::atomic_store(&statusRejection, rejection.empty() ?
				std::shared_ptr<const std::string>() : std::make_shared<const std::string>(rejection));
		}
		if (context.frame)
			context.frame->PublishRejection(rejection);
		// The gamma boost applies even with the module off or in log_only.
		const LedDriveConfig boostConfig = valid ? ReadLedDriveConfig(config) : LedDriveConfig();
		// The exponent is output gamma / gamma_boost, worked out per frame from the frame's
		// output transfer; the render side logs it when it changes.
		if (boostConfig.gammaBoost != params.gammaBoost)
			DebugLog::Log("Dynamic Drive: gamma boost setting %s (%.2f)",
				boostConfig.gammaBoost > 0.0 ? "on" : "off", boostConfig.gammaBoost);
		params.gammaBoost = boostConfig.gammaBoost;
		params.compensationEotf = boostConfig.compensationEotf;
		params.knee = boostConfig.knee;
		params.boostKnee = boostConfig.gammaBoostKnee;
		params.boostKneeWidth = boostConfig.gammaBoostKneeWidth;
		params.wtwPassthrough = boostConfig.wtwPassthrough;
		params.btbPassthrough = boostConfig.btbPassthrough;
		// The shadow lift is part of adaptive gamma (owner 2026-10-14).
		params.shadowLift = boostConfig.adaptiveGamma ? boostConfig.shadowLiftAnsi : 0.0;
		if (context.frame)
			context.frame->PublishParams(params);
		// VP-V: slot drive.projector_control (off in the Original pathway, ADR-0019, or at slot
		// level): the RS-232 LED/CNTR drive is not started, and a running module is stopped.
		// On cold startup (context.frame == nullptr, before Build creates Impl), also honour the
		// latched process pathway so an Original process stays off even if the snapshot lacks [vpv].
		const bool projectorAllowed = context.projectorAllowed && candidateProjectorAllowed &&
			ConfigAllowsProjectorControl(config) && !explicitOriginal && !Vpv::RunningPathway::Original();
		const LedDriveConfig& led = boostConfig;
		std::shared_ptr<LedDriveModule> current = std::atomic_load(&module);
		if (next == signature && (current != nullptr) == (valid && projectorAllowed && led.enabled))
			return;
		// A broken LED profile never stops VP: it is left out (built-in values plus the main
		// settings) and said once per change.
		std::string profileProblem;
		if (!ValidateLedProfile(config, profileProblem))
			DebugLog::Log("Dynamic Drive: WARNING LED profile ignored (%s); using the built-in values",
				profileProblem.c_str());
		// Tuning and script/compensate switches go to the running module, so the LED carries on
		// from its level instead of restoring full light.
		if (valid && projectorAllowed && led.enabled && current &&
			current->Reconfigure(ReadProjectorConfig(config), led))
		{
			signature = next;
			if (context.frame)
				context.frame->PublishObserver(current, false);
			return;
		}
		signature = next;
		std::shared_ptr<LedDriveModule> previous = std::move(current);
		const bool hadPrevious = previous != nullptr;
		// GEM-190: enqueue Stop() asynchronously on lifecycleWorker BEFORE clearing module/frame so
		// stateMutex is never held across blocking serial-port teardown and StopForShutdown() cannot
		// observe a window where `previous` is absent from both module/frame and lifecycleWorker.
		// Copy `previous` to keep it alive across EnqueueStop() until published references clear.
		if (previous)
			lifecycleWorker->EnqueueStop(previous);
		std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
		if (context.frame)
			context.frame->Clear();
		previous.reset();
		if (!valid)
		{
			DebugLog::Log("Dynamic Drive: configuration rejected (%s); module and gamma boost off",
				validationError.c_str());
			return;
		}
		if (!led.enabled || !projectorAllowed)
		{
			if (hadPrevious || (led.enabled && !projectorAllowed))
				DebugLog::Log("Dynamic Drive: disabled%s", projectorAllowed ? "" :
					" (slot drive.projector_control off: Original pathway or pipeline_slots_off)");
			return;
		}
		// VP Renderer filter (2026-10-17): a drive the previous file's renderer held (same LED
		// settings) carries on; nothing is held in the capture app.
		std::shared_ptr<LedDriveModule> nextModule = ProcessLedDriveHoldover().Adopt(next);
		if (!nextModule)
		{
			// The serial port goes through the LED drive arbiter: VideoProcessor.exe and the
			// filter never drive the projector at the same time.
			const SerialPortFactory portFactory = portFactoryForTest ? portFactoryForTest : ArbitratedSerialPortFactory();
			nextModule = std::make_shared<LedDriveModule>(ReadProjectorConfig(config), led, portFactory);
			// GEM-190: if a prior Stop() is still in flight on lifecycleWorker, queue Start()
			// behind it so the previous module closes the port before nextModule opens it;
			// otherwise start synchronously (cold start).
			if (!lifecycleWorker->EnqueueStartIfBusy(nextModule))
				nextModule->Start();
		}
		std::atomic_store(&module, nextModule);
		if (context.frame)
		{
			context.frame->PublishModule(nextModule);
			// The module's look-ahead watches the queued-frame walk; the aliasing pointer keeps
			// the module alive while the walk holds it.
			context.frame->PublishObserver(nextModule, true);
		}
	}

	bool DynamicDriveHost::SuspendForTest(const HostContext& context, std::string& reason)
	{
		reason.clear();
		std::shared_ptr<LedDriveModule> stopped;
		{
			std::unique_lock<std::mutex> guard(stateMutex);
			bool playing = false;
			const std::shared_ptr<LedDriveModule> current = std::atomic_load(&module);
			if (current)
				playing = current->MsSinceLastFrame() < 2000.0;
			else if (context.frame)
			{
				const uint64_t last = context.lastRenderStartedTick;
				playing = last != 0 && GetTickCount64() - last < 2000;
			}
			if (playing)
			{
				reason = "Not run: video is playing. Stop playback and try again.";
				return false;
			}
			stopped = DetachLocked(context);
			suspended = true;
		}
		const bool cancelledBeforeStart = stopped && lifecycleWorker->CancelOrWaitStart(stopped);
		lifecycleWorker->Flush();
		if (stopped)
		{
			if (!cancelledBeforeStart)
				stopped->Stop();
			DebugLog::Log("Dynamic Drive: stopped for the RS-232 connection test");
		}
		return true;
	}

	// Integration 2026-10-17: the forced shutdown's LED stop. The render state lock may be held
	// by the blocked retirement, so the module comes from the render loop's atomic copy (or the
	// host's atomic copy without waiting on stateMutex), plus any module currently stopping on
	// lifecycleWorker. Stop() restores full light.
	bool DynamicDriveHost::StopForShutdown(const HostContext& context)
	{
		// Read the published module BEFORE querying lifecycleWorker so an Update() that enqueues
		// Stop(previous) and then clears module/frame cannot slip between the two observations;
		// re-read afterwards as well to catch a newly published replacement module.
		std::shared_ptr<LedDriveModule> runningBefore;
		if (context.frame)
			runningBefore = context.frame->Module();
		if (!runningBefore)
			runningBefore = std::atomic_load(&module);
		std::vector<std::shared_ptr<LedDriveModule>> toStop =
			lifecycleWorker->CancelPendingStartsAndCollectActive();
		const auto addUnique = [&toStop](std::shared_ptr<LedDriveModule> candidate)
		{
			if (candidate && std::find(toStop.begin(), toStop.end(), candidate) == toStop.end())
				toStop.push_back(std::move(candidate));
		};
		addUnique(std::move(runningBefore));
		std::shared_ptr<LedDriveModule> runningAfter;
		if (context.frame)
			runningAfter = context.frame->Module();
		if (!runningAfter)
			runningAfter = std::atomic_load(&module);
		addUnique(std::move(runningAfter));
		if (toStop.empty())
			return true;
		DebugLog::Log("Dynamic Drive: stopping for the forced shutdown (restoring full light)");
		// Bug hunt 2 2026-10-17: the restore's result (light, and LDMM / CNTR when sent), so the
		// caller can say "not confirmed" when it was not.
		bool confirmed = true;
		for (const std::shared_ptr<LedDriveModule>& target : toStop)
			if (!target->Stop())
				confirmed = false;
		DebugLog::Log("Dynamic Drive: stopped for the forced shutdown%s",
			confirmed ? "" : " (restore not confirmed)");
		return confirmed;
	}

	void DynamicDriveHost::ResumeAfterTest(const HostContext& context)
	{
		// A renderer created during the test (a restart, a new signal) started its own LED drive
		// while the test held the port: that drive found the port busy and runs as a dry run.
		// Restart it too (audit stage8-rs232 item 1); the dialog calls this after every test.
		std::shared_ptr<LedDriveModule> stale;
		{
			std::unique_lock<std::mutex> guard(stateMutex);
			if (!suspended && std::atomic_load(&module))
				stale = DetachLocked(context);
		}
		const bool cancelledBeforeStart = stale && lifecycleWorker->CancelOrWaitStart(stale);
		lifecycleWorker->Flush();
		if (stale && !cancelledBeforeStart)
			stale->Stop();   // closes the port before the new drive opens it
		std::unique_lock<std::mutex> guard(stateMutex);
		const bool wasSuspended = suspended;
		suspended = false;
		if ((wasSuspended || stale) && configuration)
		{
			Update(*configuration, context);
			DebugLog::Log(wasSuspended ? "Dynamic Drive: restarted after the RS-232 connection test" :
				"Dynamic Drive: restarted after the RS-232 connection test (renderer created during the test)");
		}
	}

	bool DynamicDriveHost::SuspendForCalibration(const HostContext& context)
	{
		// The LED drive stops (Stop() restores full light: LDMM 0, LPW1 to the ceiling, CNTR 50)
		// so no APL command moves the light between patches.
		std::shared_ptr<LedDriveModule> stopped;
		{
			std::unique_lock<std::mutex> guard(stateMutex);
			if (std::atomic_load(&module))
			{
				stopped = DetachLocked(context);
				suspended = true;
				calibrationSuspended = true;
			}
		}
		const bool cancelledBeforeStart = stopped && lifecycleWorker->CancelOrWaitStart(stopped);
		lifecycleWorker->Flush();
		if (stopped && !cancelledBeforeStart)
			stopped->Stop();
		return stopped != nullptr;
	}

	void DynamicDriveHost::ResumeAfterCalibration(const HostContext& context)
	{
		// POD1-M4: synchronize calibrationSuspended under stateMutex before delegating.
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (!calibrationSuspended)
				return;
			calibrationSuspended = false;
		}
		ResumeAfterTest(context);
	}

	std::string DynamicDriveHost::StatusText() const
	{
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		if (!led)
		{
			const std::shared_ptr<const std::string> currentRejection = std::atomic_load(&statusRejection);
			return (!currentRejection || currentRejection->empty()) ? std::string("off") :
				"off: configuration rejected (" + *currentRejection + "); LED drive and gamma boost off";
		}
		const LedDriveConfig config = led->Config();
		const DriverStatus driver = led->ProjectorStatus();
		const CompensationGain gain = led->CurrentGain();
		const char* mode = config.mode == Mode::Compensate ? "compensate" :
			config.mode == Mode::Script ? "script" :
			config.mode == Mode::PictureOnly ? "picture_only" : "log_only";
		char text[512];
		snprintf(text, sizeof(text),
			"source %s (mode %s%s), %s, light last sent %d (confirmed %d), contrast last sent %d, commands %d, timeouts %d, "
			"picture gain %s %.3f",
			config.commandSource.empty() ? "(from mode)" : config.commandSource.c_str(), mode,
			config.mode == Mode::Script ? (config.scriptPacing ? ", script pacing" : ", tick loop") : "",
			led->DrivingProjector() ? "driving the projector" : "not driving the projector (dry run)",
			driver.lastSentLight, driver.lastLight, driver.lastSentContrast, driver.commands, driver.timeouts,
			gain.active ? "on" : "off", gain.gain);
		return text;
	}
}
