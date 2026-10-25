// VP-V forwarding header (WP-17 move; remove in M2 step 0): RawCaptureRecorder.h moved to src/VP-V/modules/control/diagnostics/.
// It stays only so host files (Bill's) and files on active branches that still include the old path build unchanged;
// new code includes <VP-V/modules/control/diagnostics/RawCaptureRecorder.h>. Listed in the module's MANIFEST.json "forwarding_headers".
#pragma once
#include <VP-V/modules/control/diagnostics/RawCaptureRecorder.h>
