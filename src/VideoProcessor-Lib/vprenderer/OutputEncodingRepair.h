// VP-V forwarding header (M2): OutputEncodingRepair.h moved to src/VP-V/modules/output/dither/.
// It stays only so busy or upstream files that still include the old path build unchanged
// (LibplaceboVideoRenderer.cpp, LibplaceboLutParserTests.cpp);
// new code includes <VP-V/modules/output/dither/OutputEncodingRepair.h>. Listed in the module's MANIFEST.json "forwarding_headers".
#pragma once
#include <VP-V/modules/output/dither/OutputEncodingRepair.h>
