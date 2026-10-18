#include <pch.h>
#include "TextureRestore.h"

// Skeleton (lead, 2026-10-17): the implementation lands on feat/detail-restore.
namespace TextureRestore
{
	struct Module::Impl {};

	bool Module::Apply(pl_gpu, pl_dispatch, const pl_frame&, const Params&, pl_frame&)
	{
		error = "not built";
		return false;
	}
	void Module::Forget() {}
	void Module::Release(pl_gpu) { delete impl; impl = nullptr; }
}
