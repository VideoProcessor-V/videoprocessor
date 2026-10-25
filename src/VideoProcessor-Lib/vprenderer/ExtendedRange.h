// VP-V forwarding header (M2): ExtendedRange.h moved to src/VP-V/modules/colour/extended_range/.
// It stays only so busy or upstream files that still include the old path build unchanged
// (LibplaceboVideoRenderer.cpp, LibplaceboLutParserTests.cpp, MotionRendererTests.cpp);
// new code includes <VP-V/modules/colour/extended_range/ExtendedRange.h>. Listed in the module's MANIFEST.json "forwarding_headers".
#pragma once
#include <VP-V/modules/colour/extended_range/ExtendedRange.h>
