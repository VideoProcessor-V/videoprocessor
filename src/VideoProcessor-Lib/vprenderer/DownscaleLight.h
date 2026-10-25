// VP-V forwarding header (M2): DownscaleLight.h moved to src/VP-V/modules/scaling/output/.
// It stays only so busy or upstream files that still include the old path build unchanged
// (LibplaceboVideoRenderer.cpp, ConfigFileTests.cpp);
// new code includes <VP-V/modules/scaling/output/DownscaleLight.h>. Listed in the module's MANIFEST.json "forwarding_headers".
#pragma once
#include <VP-V/modules/scaling/output/DownscaleLight.h>
