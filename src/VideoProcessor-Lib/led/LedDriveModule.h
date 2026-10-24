// VP-V forwarding header (early migration; remove in M2): LedDriveModule.h moved to src/VP-V/modules/drive/led_compensation/.
// It stays only so files on active branches that still include the old path build unchanged;
// new code includes <VP-V/modules/drive/led_compensation/LedDriveModule.h>. Listed in the module's MANIFEST.json "forwarding_headers".
#pragma once
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
