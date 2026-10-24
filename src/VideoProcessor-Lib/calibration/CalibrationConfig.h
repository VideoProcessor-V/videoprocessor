// VP-V forwarding header (early migration; remove in M2): CalibrationConfig.h moved to src/VP-V/modules/output/calibration_lut/calibration/.
// It stays only so files on active branches that still include the old path build unchanged;
// new code includes <VP-V/modules/output/calibration_lut/calibration/CalibrationConfig.h>. Listed in the module's MANIFEST.json "forwarding_headers".
#pragma once
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationConfig.h>
