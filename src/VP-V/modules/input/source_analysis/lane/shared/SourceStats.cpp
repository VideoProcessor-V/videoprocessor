// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Out-of-line CPU bodies of VpUnit::SourceStats (see SourceStats.h).

#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

namespace VpUnit
{
namespace SourceStats
{

double NormalisedMean(const Record& rec)
{
    return rec.pixelCount > 0
        ? (std::min)(1.0, static_cast<double>(rec.mean) / static_cast<double>(Vpv::AnalysisDomain::kAxisQ6Max))
        : 0.0;
}

} // namespace SourceStats
} // namespace VpUnit
