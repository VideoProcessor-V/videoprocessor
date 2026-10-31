// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Source-ID consensus, part 2: the consensus with hysteresis, and the Indicator port
// (input.source_analysis (identity/)).
//
// The voters (SourceIdVote.h) each hold at most ONE current vote (a later Submit of the same voter
// replaces it; the slots are indexed by VoterId so the order of Submit calls within a tick never
// matters). Evaluate() decides, per field, what the source is:
//   - votes older than the voter's maximum evidence age are stale and ignored (reported);
//   - fresh votes are grouped by equal value (ratios within ratioTolerance, other fields exactly);
//     a group's weight is the sum of confidence x voter trust, summed in VoterId order;
//   - the top group is the candidate (ties: the group that equals the current value, else the one
//     whose first voter has the lower VoterId);
//   - candidate == current: refresh. Candidate != current: the change needs support (group weight /
//     total weight) >= enterSupport, weight >= enterMinWeight, enough distinct voters, to stay the
//     candidate for the dwell time (shortened right after an ABR boundary), and minHoldAfterChange
//     since this field last changed. Anything else HOLDS the current value (disagreement = hold).
//     Once a candidate is pending it is kept while its dissent stays <= exitDissent (lower bar than
//     entering: hysteresis).
//   - a safety veto (ClassicalResolution only: detail above the re-based size) sets the voted axes
//     native at once, bypassing dwell and hold.
// Time is always SOURCE time passed by the caller; there is no clock, no thread, no randomness.
//
// The ONLY way consensus moves a re-base stamp is IndicatorPort::AdvanceRebase, once per changed
// axis (see the README: the central Indicator does not exist yet). Consensus keeps no private
// epochs or flags that other stages could read.
//
// Discontinuity() is a FULL reset (new title / new source): everything returns to native/unknown
// and the votes are dropped. It emits NO stamp itself, because the Indicator's FULL event already
// bumps both axes; stamping again would double-count the re-base.
//
// Latch / crossfade: consensus decides WHAT the source is; the source-resolution latch (cut, black,
// pan, crossfade in source time) still decides WHEN the switch is shown.
#include <VP-V/modules/input/source_analysis/identity/AbrStepDetector.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdVote.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace SourceIdentity
{
	// The central Indicator (resync design 25 A2/A4, package I0) implements this. axis: 0 = H, 1 = V.
	struct IndicatorPort
	{
		virtual ~IndicatorPort() {}
		virtual void AdvanceRebase(int axis, uint64_t fromSeq, const char* cause) = 0;
	};

	struct Settings
	{
		// Trust per voter, indexed by VoterId: ClassicalResolution, ClassicalKernel, BitrateQuality, IdNet.
		double trust[kVoterCount] = { 1.0, 1.0, 0.5, 1.0 };
		// Maximum evidence age (microseconds of source time) per voter. The classical voters are
		// refreshed every few analysis intervals; the net is event-driven, so its vote may be old.
		int64_t maxEvidenceAgeUs[kVoterCount] = { 4000000, 6000000, 4000000, 30000000 };
		double enterSupport = 0.67;          // weighted share a candidate needs to start
		double enterMinWeight = 0.25;        // absolute weighted confidence a candidate needs
		double exitDissent = 0.5;            // a pending candidate is dropped when dissent exceeds this
		int64_t dwellUs = 3000000;           // a candidate must hold this long
		int64_t minHoldAfterChangeUs = 10000000; // no second change of a field soon after a change
		int minDistinctVoters = 1;           // classical-only phase: one voter may change a field
		int minDistinctVotersMulti = 2;      // when two or more voters are fresh on the field
		int64_t abrWindowUs = 8000000;       // after an ABR boundary ...
		int64_t abrDwellUs = 500000;         // ... the dwell is this long (a rendition switch is sudden)
		double ratioTolerance = 0.03;        // relative; ratios within it are the same value
	};

	struct FieldResult
	{
		double value = 0.0;
		bool changed = false;
		bool held = false;
		std::string reason;
		double support = 0.0;
		double weight = 0.0;
		int voters = 0;
		int stale = 0;                       // stale votes ignored for this field
	};

	struct RebaseRequest
	{
		int axis = 0;
		uint64_t fromSeq = 0;
		std::string cause;                   // "ratio", "kernel" or "veto"
	};

	struct Result
	{
		int64_t timeUs = 0;
		Hypothesis before;                   // the state before this Evaluate
		Hypothesis value;                    // the state after it
		FieldResult field[kFieldCount];
		uint32_t changedMask = 0;
		std::vector<RebaseRequest> rebase;

		bool Changed(Field f) const { return (changedMask & FieldBit(f)) != 0; }

		// A fixed-format trace line (used by the determinism tests and Ctrl+Alt+D).
		std::string Serialize() const
		{
			std::ostringstream o;
			o.setf(std::ios::fixed);
			o.precision(4);
			o << "t=" << timeUs << " rH=" << value.ratioH << " rV=" << value.ratioV << " k=" << value.kernel
				<< " c=" << static_cast<int>(value.codec) << " q=" << value.qualityTier << " mask=" << changedMask;
			for (int i = 0; i < kFieldCount; ++i)
				o << " [" << field[i].reason << '|' << field[i].support << '|' << field[i].voters << ']';
			for (size_t i = 0; i < rebase.size(); ++i)
				o << " rebase(" << rebase[i].axis << ',' << rebase[i].fromSeq << ',' << rebase[i].cause << ')';
			return o.str();
		}
	};

	struct Stats
	{
		uint64_t evaluations = 0;
		uint64_t changes = 0;                // field changes committed
		uint64_t holds = 0;                  // field evaluations that held a value for a reason
		uint64_t vetoes = 0;                 // safety vetoes that moved an axis to native
		uint64_t discontinuities = 0;
	};

	class Consensus
	{
	public:
		Consensus() { ResetState(); }
		explicit Consensus(const Settings& s) : s_(s) { ResetState(); }

		void Submit(const Vote& vote)
		{
			const int vi = static_cast<int>(vote.voter);
			if (vi < 0 || vi >= kVoterCount)
				return;
			slot_[vi].present = true;
			slot_[vi].vote = vote;
			if (vote.voter != VoterId::ClassicalResolution)
				slot_[vi].vote.safetyVeto = false;     // only the classical resolution voter may veto
		}

		void NoteAbrBoundary(const AbrEvent& e)
		{
			abrSeen_ = true;
			abrTimeUs_ = e.timeUs;
		}

		// FULL reset (new title / source). No stamp is emitted: the Indicator's FULL already bumps both axes.
		void Discontinuity(uint64_t seq)
		{
			lastDiscontinuitySeq_ = seq;
			ResetState();
			++stats_.discontinuities;
		}

		Result Evaluate(uint64_t seq, int64_t timeUs, IndicatorPort* port = nullptr)
		{
			Result r;
			r.timeUs = timeUs;
			r.before = cur_;
			++stats_.evaluations;
			const char* axisCause[2] = { nullptr, nullptr };
			for (int fi = 0; fi < kFieldCount; ++fi)
				EvaluateField(static_cast<Field>(fi), timeUs, r, axisCause);

			// A kernel change re-bases every axis that is currently re-based (ratio below native).
			if (r.Changed(Field::Kernel))
			{
				if (cur_.ratioH < 1.0 - s_.ratioTolerance && !axisCause[0]) axisCause[0] = "kernel";
				if (cur_.ratioV < 1.0 - s_.ratioTolerance && !axisCause[1]) axisCause[1] = "kernel";
			}
			for (int axis = 0; axis < 2; ++axis)
			{
				if (!axisCause[axis])
					continue;
				RebaseRequest q;
				q.axis = axis;
				q.fromSeq = seq;
				q.cause = axisCause[axis];
				r.rebase.push_back(q);
				if (port)
					port->AdvanceRebase(axis, seq, axisCause[axis]);
			}
			r.value = cur_;
			return r;
		}

		const Hypothesis& Current() const { return cur_; }
		const Stats& GetStats() const { return stats_; }
		const Settings& GetSettings() const { return s_; }
		uint64_t LastDiscontinuitySeq() const { return lastDiscontinuitySeq_; }

	private:
		struct Slot
		{
			bool present = false;
			Vote vote;
		};
		struct FieldState
		{
			bool hasCandidate = false;
			double candValue = 0.0;
			int64_t candSinceUs = 0;
			bool hasChanged = false;
			int64_t lastChangeUs = 0;
		};
		struct Group
		{
			double value = 0.0;              // the first member's value, then the weighted mean for ratios
			double weightedSum = 0.0;
			double weight = 0.0;
			int voters = 0;
		};

		void ResetState()
		{
			cur_ = Hypothesis();
			for (int i = 0; i < kVoterCount; ++i)
				slot_[i] = Slot();
			for (int i = 0; i < kFieldCount; ++i)
				fs_[i] = FieldState();
			abrSeen_ = false;
			abrTimeUs_ = 0;
		}

		bool Same(Field f, double a, double b) const
		{
			if (IsAxis(f))
			{
				const double m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
				return std::fabs(a - b) <= s_.ratioTolerance * m;
			}
			return a == b;
		}

		bool Fresh(int vi, int64_t now) const
		{
			const int64_t age = now - slot_[vi].vote.evidenceTimeUs;
			return age <= s_.maxEvidenceAgeUs[vi];
		}

		void EvaluateField(Field f, int64_t now, Result& r, const char** axisCause)
		{
			const int fi = static_cast<int>(f);
			FieldResult& fr = r.field[fi];
			FieldState& st = fs_[fi];
			const double cur = FieldValue(cur_, f);
			fr.value = cur;

			// Safety veto (axes only): detail above the re-based size means native, at once.
			if (IsAxis(f))
			{
				bool vetoed = false;
				for (int vi = 0; vi < kVoterCount; ++vi)
				{
					const Slot& sl = slot_[vi];
					if (sl.present && sl.vote.safetyVeto && (sl.vote.fieldMask & FieldBit(f)) && Fresh(vi, now))
						vetoed = true;
				}
				if (vetoed)
				{
					st.hasCandidate = false;
					fr.reason = "safety veto: detail above the re-based size";
					fr.support = 1.0;
					if (cur != 1.0)
					{
						SetFieldValue(cur_, f, 1.0);
						fr.value = 1.0;
						fr.changed = true;
						r.changedMask |= FieldBit(f);
						st.hasChanged = true;
						st.lastChangeUs = now;
						++stats_.vetoes;
						++stats_.changes;
						axisCause[fi] = "veto";
					}
					return;
				}
			}

			// Collect the fresh votes, grouped by equal value, in VoterId order.
			Group groups[kVoterCount];
			int groupCount = 0;
			double total = 0.0;
			int freshVoters = 0;
			for (int vi = 0; vi < kVoterCount; ++vi)
			{
				const Slot& sl = slot_[vi];
				if (!sl.present || !(sl.vote.fieldMask & FieldBit(f)))
					continue;
				if (!Fresh(vi, now))
				{
					++fr.stale;
					continue;
				}
				const double w = sl.vote.confidence[fi] * s_.trust[vi];
				if (!(w > 0.0))
					continue;
				const double v = FieldValue(sl.vote.h, f);
				total += w;
				++freshVoters;
				int g = -1;
				for (int k = 0; k < groupCount; ++k)
					if (Same(f, groups[k].value, v)) { g = k; break; }
				if (g < 0)
				{
					g = groupCount++;
					groups[g].value = v;
					groups[g].weightedSum = 0.0;
					groups[g].weight = 0.0;
					groups[g].voters = 0;
				}
				groups[g].weight += w;
				groups[g].weightedSum += w * v;
				groups[g].voters += 1;
				if (IsAxis(f))
					groups[g].value = groups[g].weightedSum / groups[g].weight;
			}

			if (groupCount == 0)
			{
				st.hasCandidate = false;
				if (fr.stale > 0)
				{
					fr.held = true;
					fr.reason = "held: stale";
					++stats_.holds;
				}
				else
					fr.reason = "no votes";
				return;
			}

			// Top group; ties go to the group equal to the current value, then the lower VoterId.
			int best = 0;
			for (int k = 1; k < groupCount; ++k)
				if (groups[k].weight > groups[best].weight + 1e-12)
					best = k;
			for (int k = 0; k < groupCount; ++k)
				if (Same(f, groups[k].value, cur) && groups[k].weight >= groups[best].weight - 1e-12)
				{
					best = k;
					break;
				}
			const Group& cand = groups[best];
			fr.support = cand.weight / total;
			fr.weight = cand.weight;
			fr.voters = cand.voters;

			if (Same(f, cand.value, cur))
			{
				st.hasCandidate = false;
				fr.reason = "agrees";
				return;
			}

			const bool pending = st.hasCandidate && Same(f, st.candValue, cand.value);
			const double needSupport = pending ? 1.0 - s_.exitDissent : s_.enterSupport;
			int needVoters = s_.minDistinctVoters;
			if (freshVoters >= 2 && s_.minDistinctVotersMulti > needVoters)
				needVoters = s_.minDistinctVotersMulti;

			if (fr.support < needSupport)
				return Hold(fr, st, "held: disagreement");
			if (fr.weight < s_.enterMinWeight)
				return Hold(fr, st, "held: weak evidence");
			if (fr.voters < needVoters)
				return Hold(fr, st, "held: too few voters");

			if (!pending)
			{
				st.hasCandidate = true;
				st.candSinceUs = now;
			}
			st.candValue = cand.value;

			int64_t dwell = s_.dwellUs;
			if (abrSeen_ && now >= abrTimeUs_ && now - abrTimeUs_ <= s_.abrWindowUs && s_.abrDwellUs < dwell)
				dwell = s_.abrDwellUs;
			if (now - st.candSinceUs < dwell)
			{
				fr.held = true;
				fr.reason = "held: dwell";
				++stats_.holds;
				return;
			}
			if (st.hasChanged && now - st.lastChangeUs < s_.minHoldAfterChangeUs)
			{
				fr.held = true;
				fr.reason = "held: min hold";
				++stats_.holds;
				return;
			}

			SetFieldValue(cur_, f, cand.value);
			fr.value = FieldValue(cur_, f);
			fr.changed = true;
			fr.reason = "changed";
			r.changedMask |= FieldBit(f);
			st.hasCandidate = false;
			st.hasChanged = true;
			st.lastChangeUs = now;
			++stats_.changes;
			if (IsAxis(f))
				axisCause[fi] = "ratio";
		}

		void Hold(FieldResult& fr, FieldState& st, const char* reason)
		{
			st.hasCandidate = false;
			fr.held = true;
			fr.reason = reason;
			++stats_.holds;
		}

		Settings s_;
		Hypothesis cur_;
		Slot slot_[kVoterCount];
		FieldState fs_[kFieldCount];
		bool abrSeen_ = false;
		int64_t abrTimeUs_ = 0;
		uint64_t lastDiscontinuitySeq_ = 0;
		Stats stats_;
	};
}
