// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// SID diagnostic dump v4 (input.source_analysis (identity/); doc 100 s4 P0, E2): a DECISION-NEUTRAL, opt-in CSV writer for the classical
// detector (witness A, SourceResolutionMonitor) and the chain estimator (witness B, ChainEstimator). Gated by the environment variable
//   VP_SID_DUMP_V4_DIR = <absolute scratch directory>   (unset or empty = OFF: nothing is opened, nothing is written)
// read by the WRITER thread when it first writes and again whenever the owner's tag changes (SetDiagnosticTag). Each owner writes its own
// files <dir>\<tag>.<kind>.csv (truncated when opened): kinds a_frames / a_abr (the monitor), b_evals / b_sizes (the estimator); the
// column contract is in identity/README.md "Dump v4". Without a tag the files are named live<k> (k = a process-wide instance number).
// Writers only READ decision state (const accessors) and own their files; no decision, record, counter, log line or timing input of the
// detectors changes with the dump on or off (tests: ChainEvalRegistrationTests "DumpV4..."). Header-only, C++14, no Windows headers.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace SourceIdentity
{
	namespace Diag
	{
		// The dump directory from the environment ("" = off).
		inline std::string DumpDirV4()
		{
			std::string dir;
#ifdef _MSC_VER
			char* v = nullptr;
			size_t len = 0;
			if (_dupenv_s(&v, &len, "VP_SID_DUMP_V4_DIR") == 0 && v != nullptr)
			{
				dir = v;
				std::free(v);
			}
#else
			const char* v = std::getenv("VP_SID_DUMP_V4_DIR");
			if (v != nullptr)
				dir = v;
#endif
			return dir;
		}

		// A process-wide instance number for owners without a tag.
		inline int NextInstance()
		{
			static std::atomic<int> next{ 0 };
			return next.fetch_add(1, std::memory_order_relaxed);
		}

		// "%.6g" (the number style of the cal dump); NaN / inf are written as they print.
		inline std::string Num(double v)
		{
			char b[48];
			std::snprintf(b, sizeof(b), "%.6g", v);
			return b;
		}

		// The owner's file-name tag (any thread sets it; the writer thread reads it when its generation moved).
		class Tag
		{
		public:
			void Set(const std::string& tag)
			{
				std::lock_guard<std::mutex> g(m_);
				tag_ = tag;
				gen_.fetch_add(1, std::memory_order_acq_rel);
			}
			uint64_t Gen() const { return gen_.load(std::memory_order_acquire); }
			std::string Get() const
			{
				std::lock_guard<std::mutex> g(m_);
				return tag_;
			}

		private:
			mutable std::mutex m_;
			std::string tag_;
			std::atomic<uint64_t> gen_{ 0 };
		};

		// One CSV file of one kind. Writer thread only.
		class File
		{
		public:
			File() {}
			File(const File&) = delete;
			File& operator=(const File&) = delete;
			~File() { Close(); }

			// True when rows are to be written now (the dump is on for the current tag). Re-reads the environment and reopens the file only
			// when the tag's generation moved (or on the first call).
			bool Ready(const Tag& tag, const char* kind, const char* header)
			{
				const uint64_t g = tag.Gen();
				if (checked_ && g == seenGen_)
					return open_;
				checked_ = true;
				seenGen_ = g;
				Close();
				const std::string dir = DumpDirV4();
				if (dir.empty())
					return false;
				std::string t = tag.Get();
				if (t.empty())
				{
					if (instance_ < 0)
						instance_ = NextInstance();
					t = "live" + std::to_string(instance_);
				}
				for (char& c : t)
					if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
						c = '_';
				const char last = dir[dir.size() - 1];
				const std::string sep = (last == '\\' || last == '/') ? "" : "\\";
				f_.open((dir + sep + t + "." + kind + ".csv").c_str(), std::ios::out | std::ios::trunc);
				open_ = f_.is_open();
				if (open_)
					f_ << header << '\n';
				return open_;
			}

			std::ofstream& Out() { return f_; }

			// After each row: flushed every 64 rows (a partial run stays readable) and at Close.
			void EndRow()
			{
				if (++rows_ % 64 == 0)
					f_.flush();
			}

			uint64_t Rows() const { return rows_; }

			void Close()
			{
				if (f_.is_open())
				{
					f_.flush();
					f_.close();
				}
				open_ = false;
				rows_ = 0;
			}

		private:
			std::ofstream f_;
			bool open_ = false;
			bool checked_ = false;
			uint64_t seenGen_ = 0;
			uint64_t rows_ = 0;
			int instance_ = -1;
		};

		// Column headers (the contract; identity/README.md "Dump v4" explains every column).
		inline const char* FramesHeaderV4()
		{
			return "seq,t_s,mode,coded_bits,output_bits,informative,informative_tiles,tiles,mixed,blank,black,cut,cuts,shots,ratio,ratio_h,ratio_v,coherence_h,coherence_v,"
				"trk_ratio,trk_cand,trk_ratio_h,trk_cand_h,trk_ratio_v,trk_cand_v,declared,conf,axis_ratio_x,axis_ratio_y,axis_conf,axis_conf_h,"
				"axis_conf_v,detail_above_raw,detail_above,chain_hold_mask,frame_quality,noise,apl,abr_events,abr_confounded,evidence_s";
		}
		inline const char* AbrHeaderV4()
		{
			return "type,report_seq,report_t_s,boundary_seq,boundary_t_s,direction,quality_step,ratio_step,confidence,on_grid,cut_confound,period_s";
		}
		inline const char* EvalsHeaderV4()
		{
			return "seq,t_s,shot_id,flags,cut,blank,fade,clear_window,menu_hold,abr_step,area_changed,window_cleared,step_codes,floor_codes,coded_bits,output_bits,"
				"classical_ok,classical_h,classical_v,safety,state_x,state_y,meas_n_x,meas_n_y,app_state_x,app_state_y,app_n_x,app_n_y,N_x,N_y,"
				"llr_x,llr_y,informative_x,informative_y,hold_x,hold_y,probe_seen_x,probe_seen_y,latched,latch_nx,latch_ny,latch_event,"
				"latch_confirmed,latch_deferred_now,latch_defer_cancelled,latch_held_back,clock_s,deferred,dwelling,defer_cause,defer_since_s,"
				"last_confirm_s,newest_credit_x_s,newest_credit_y_s,acc_x,acc_y,bar,c1,cost_ms";
		}
		inline const char* SizesHeaderV4()
		{
			return "seq,t_s,shot_id,axis,N,n,geometry,parked,valid,stat,kernel,lines,credit_now,raw_now,newest_credit_s,llr_window,"
				"shot_open,shot_open_id,shot_open_newest_s,closed_shots,closed_sum,pending,shot_key,shot_start_s,shot_met";
		}
	}
}
