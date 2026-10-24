#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// DRAFT (2026-10-24): the pure parts of HookSegment (see HookSegment.h): the intermediate
// texture pool and the 1-in-N timing sampler. No libplacebo, no Windows, C++14, so the reuse
// and sampling rules are tested on the CPU. Self-contained: no VP-V dependency.
namespace HookSegment
{
	// A run's intermediates. Within one run a texture is handed out once; across runs the
	// same (w, h, format) textures are reused, so a steady picture allocates nothing.
	// Generic over the texture handle: Create(w, h, fmt) -> Tex (null on failure), Destroy(Tex).
	template <class Tex, class Fmt>
	class TexturePool
	{
	public:
		using Create = std::function<Tex(int, int, Fmt)>;
		using Destroy = std::function<void(Tex)>;

		TexturePool(Create c, Destroy d) : create(std::move(c)), destroy(std::move(d)) {}
		~TexturePool() { Clear(); }
		TexturePool(const TexturePool&) = delete;
		TexturePool& operator=(const TexturePool&) = delete;

		// Starts a run: every texture becomes available again (the GPU orders the reuse).
		void BeginRun()
		{
			for (Entry& e : entries)
				e.inUse = false;
		}
		Tex Get(int w, int h, Fmt fmt)
		{
			for (Entry& e : entries)
				if (!e.inUse && e.w == w && e.h == h && e.fmt == fmt)
				{
					e.inUse = true;
					++reused;
					return e.tex;
				}
			Tex tex = create(w, h, fmt);
			if (!tex)
			{
				++failed;
				return tex;
			}
			++created;
			entries.push_back({ tex, w, h, fmt, true });
			return tex;
		}
		// Frees the entries the last run did not use (after a size change), keeping the rest.
		void Trim()
		{
			for (size_t i = 0; i < entries.size();)
				if (!entries[i].inUse)
				{
					destroy(entries[i].tex);
					entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
				}
				else
					++i;
		}
		void Clear()
		{
			for (Entry& e : entries)
				destroy(e.tex);
			entries.clear();
		}
		size_t Size() const { return entries.size(); }
		uint64_t Created() const { return created; }
		uint64_t Reused() const { return reused; }
		uint64_t Failed() const { return failed; }

	private:
		struct Entry { Tex tex; int w; int h; Fmt fmt; bool inUse; };
		Create create;
		Destroy destroy;
		std::vector<Entry> entries;
		uint64_t created = 0, reused = 0, failed = 0;
	};

	// 1-in-N sampling: timers are attached on sampled runs only and polled every run WITHOUT
	// waiting (a result of 0 = not ready yet). Never drains or blocks the GPU queue.
	class Sampler
	{
	public:
		explicit Sampler(uint32_t everyN = 30) : n(everyN ? everyN : 1) {}
		void SetEvery(uint32_t everyN) { n = everyN ? everyN : 1; }
		uint32_t Every() const { return n; }
		bool BeginRun() { return (runs++ % n) == 0; }   // true = attach timers this run
		uint64_t Runs() const { return runs; }

	private:
		uint32_t n;
		uint64_t runs = 0;
	};

	// Which member hooks get merged (pure form of Runner::Fuse's grouping, for the tests).
	// stages[i] = the hook's stage bits, member[i] = may join a merged run, disabled[i] = the
	// renderer disabled it (left alone). Returns, per input index, the run id it belongs to
	// (-1 = left as is); runs with fewer than two hooks are not merged. Hooks of other stages
	// between two members do not split a run (they do not run at that stage); a non-member hook
	// AT the stage does (it must still run between them).
	inline std::vector<int> GroupRuns(const std::vector<uint32_t>& stages, const std::vector<bool>& member,
		const std::vector<bool>& disabled)
	{
		const size_t n = stages.size();
		std::vector<int> run(n, -1);
		int next = 0;
		for (uint32_t bit = 1; bit != 0; bit <<= 1)
		{
			std::vector<size_t> current;
			auto close = [&]()
			{
				if (current.size() >= 2)
				{
					for (size_t i : current)
						run[i] = next;
					++next;
				}
				current.clear();
			};
			for (size_t i = 0; i < n; ++i)
			{
				if (!(stages[i] & bit) || disabled[i])
					continue;
				const bool single = stages[i] == bit;   // a member must run at exactly one stage
				if (member[i] && single && run[i] < 0)
					current.push_back(i);
				else
					close();
			}
			close();
		}
		return run;
	}
}
