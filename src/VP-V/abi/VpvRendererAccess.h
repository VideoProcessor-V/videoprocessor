// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Host-side access to the VP-V extension interfaces (M2 P2, D-P2c).
//
// An interface pointer lives in the renderer DLL for the renderer's lifetime. Host code therefore NEVER
// caches one: Query<T> asks the renderer on every use, and the result is used in the same expression or
// block. Storing a Query result (a member, a static, a lambda capture) is a defect:
// src/VP-V/tests/QueryNoCacheTests.cpp scans the host sources for it.
//
// A renderer without the interface (a DirectShow renderer, a Bill DLL, a null renderer) gives nullptr; the
// callers in VpvHostCalls.h turn that into the defaults the old IVideoRenderer virtuals had.
// C++14.

#include <IRenderer.h>

#include <memory>

namespace Vpv
{
	template <class Interface>
	Interface* Query(IVideoRenderer* renderer)
	{
		return renderer ? static_cast<Interface*>(renderer->QueryExtension(Interface::Iid())) : nullptr;
	}

	template <class Interface>
	Interface* Query(const std::shared_ptr<IVideoRenderer>& renderer)
	{
		return Query<Interface>(renderer.get());
	}
}
