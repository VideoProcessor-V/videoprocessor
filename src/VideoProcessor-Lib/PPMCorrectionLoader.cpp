/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#include <pch.h>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <DebugLog.h>
#include <ConfigFile.h>

#include "PPMCorrectionLoader.h"
#include "GeneralRate.h"
#include "RefreshKey.h"

PPMCorrectionLoader::PPMCorrectionLoader()
{
}

bool PPMCorrectionLoader::LoadCorrectionFile()
{
    ConfigFile config;
    config.Load();
    return LoadCorrections(config);
}

bool PPMCorrectionLoader::IsAutomatic(double refreshRate) const
{
    return m_automaticRates.count(FindBestMatch(refreshRate)) != 0;
}

bool PPMCorrectionLoader::LoadCorrections(const ConfigFile& unifiedConfig)
{
    Clear();
    if (unifiedConfig.IsLoaded())
    {
        if (!unifiedConfig.GetWarnings().empty())
        {
            for (const auto& warning : unifiedConfig.GetWarnings())
            {
                DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: Invalid VideoProcessor.cfg syntax: %S"), warning.c_str()));
            }

            DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: VideoProcessor.cfg has syntax errors - using default PPM values")));
            return false;
        }

        if (unifiedConfig.HasSection("directshow.ppm"))
        {
            std::string rawPpm;
			if (!unifiedConfig.TryGetString("directshow.ppm", "ppm", rawPpm))
            {
                DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: VideoProcessor.cfg [directshow.ppm] requires ppm")));
				return false;
			}
			if (ConfigFile::NormalizeName(rawPpm) == "auto")
			{
				// Keep automatic mode separate from all valid numeric corrections.
                for (int rate = 1; rate <= 1000; ++rate)
                {
                    m_corrections[rate] = 0;
                    m_automaticRates.insert(rate);
                }
				DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: Loaded [directshow.ppm] ppm=AUTO for all source rates")));
				return true;
			}
			try
            {
                size_t consumed = 0;
                const int ppm = std::stoi(ConfigFile::Trim(rawPpm), &consumed);
                if (consumed != ConfigFile::Trim(rawPpm).size() ||
                    ppm < -1000000 || ppm > 1000000)
                    throw std::out_of_range("ppm");
                // The VP-0079 form is source-timing policy, not a brittle
                // nominal-refresh lookup. Populate the legacy lookup domain
                // so every actual cadence receives this one correction.
                for (int rate = 1; rate <= 1000; ++rate)
                    m_corrections[rate] = ppm;
                DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: Loaded [directshow.ppm] ppm=%d for all source rates"), ppm));
                return true;
            }
            catch (const std::exception&)
            {
                DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: Invalid [directshow.ppm] ppm: %S"), rawPpm.c_str()));
                return false;
            }
        }

        if (!unifiedConfig.HasSection("ppm_correction"))
        {
            DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: VideoProcessor.cfg [ppm_correction] not found - using default PPM values")));
            return false;
        }

        DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: Loading VideoProcessor.cfg [ppm_correction]")));

        int validEntries = 0;
        const auto* ppmCorrections = unifiedConfig.GetSectionValues("ppm_correction");
        if (ppmCorrections)
        {
            for (const auto& correction : *ppmCorrections)
            {
                if (ParseConfigLine(correction.first + "=" + correction.second))
                    validEntries++;
                else
                    DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: Invalid VideoProcessor.cfg ppm_correction entry: %S=%S"),
                        correction.first.c_str(), correction.second.c_str()));
            }
        }

        if (validEntries > 0)
        {
            DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: Loaded %d PPM corrections from VideoProcessor.cfg"), validEntries));

            for (const auto& correction : m_corrections)
            {
                DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: %.3f Hz = %d PPM"),
                    correction.first, correction.second));
            }

            return true;
        }

        Clear();
        DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: VideoProcessor.cfg [ppm_correction] had no valid entries - using default PPM values")));
        return false;
    }

    DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: VideoProcessor.cfg not found - using default PPM values")));
    return false;
}

bool PPMCorrectionLoader::ParseConfigLine(const std::string& line)
{
    // Remove any whitespace
    std::string cleanLine = line;
    cleanLine.erase(std::remove_if(cleanLine.begin(), cleanLine.end(), ::isspace), cleanLine.end());
    
    if (cleanLine.empty())
        return true; // Empty line after cleanup is OK

    // Split the line into tokens by space (for multiple entries on one line)
    std::istringstream lineStream(line);
    std::string token;
    bool parsedAny = false;

    while (lineStream >> token)
    {
        // Find the '=' separator
        size_t equalPos = token.find('=');
        if (equalPos == std::string::npos)
            continue;

        // Extract refresh rate and PPM value
        std::string refreshRateStr = token.substr(0, equalPos);
        std::string ppmStr = token.substr(equalPos + 1);

        try
        {
            // Parse the refresh key (fixed-constant batch B7 2026-10-18: RefreshKey - an integer,
            // decimal or ratio rate; saved band keys were migrated at load).
            double refreshRate = 0.0;
            if (!RefreshKey::Parse(refreshRateStr, refreshRate) || refreshRate > 1000.0)
            {
                DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: Invalid refresh rate %S (must be a rate up to 1000)"), refreshRateStr.c_str()));
                continue;
            }

            // Check for AUTO keyword
            if (ConfigFile::NormalizeName(ppmStr) == "auto")
            {
                // Auto has its own mode; 999999 remains a valid fixed correction.
                m_corrections[refreshRate] = 0;
                m_automaticRates.insert(refreshRate);
                parsedAny = true;
                DbgLog((LOG_TRACE, 2, TEXT("PPMCorrectionLoader: Parsed %.3f Hz = AUTO (auto-calibration)"), refreshRate));
                continue;
            }

            // Parse PPM value (can be negative)
            int ppmValue = std::stoi(ppmStr);
            if (ppmValue < -1000000 || ppmValue > 1000000)
            {
                DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: Invalid PPM value %d (must be -1000000 to 1000000)"), ppmValue));
                continue;
            }

            // Store the correction
            m_corrections[refreshRate] = ppmValue;
            m_automaticRates.erase(refreshRate);
            parsedAny = true;

            DbgLog((LOG_TRACE, 2, TEXT("PPMCorrectionLoader: Parsed %.3f Hz = %d PPM"), refreshRate, ppmValue));
        }
        catch (const std::exception&)
        {
            DbgLog((LOG_WARNING, 1, TEXT("PPMCorrectionLoader: Failed to parse token: %S"), token.c_str()));
        }
    }

    return parsedAny;
}

int PPMCorrectionLoader::GetPPMCorrection(double refreshRate) const
{
    if (m_corrections.empty())
        return 0;

    // Find the best matching refresh rate
    const double bestMatch = FindBestMatch(refreshRate);
    
    if (bestMatch == 0)
        return 0;

    auto it = m_corrections.find(bestMatch);
    if (it != m_corrections.end())
    {
        DbgLog((LOG_TRACE, 1, TEXT("PPMCorrectionLoader: Using %d PPM correction for %.3f Hz (matched %.3f Hz)"), 
            it->second, refreshRate, bestMatch));
        return it->second;
    }

    return 0;
}

double PPMCorrectionLoader::FindBestMatch(double targetRate) const
{
    if (m_corrections.empty())
        return 0.0;

    // Fixed-constant batch B7 (2026-10-18): the shared refresh-key lookup (RefreshKey::Find).
    // general_rate_fixes: an exact key (59.94, 60), then an integer key's band (59 for 59.94: a
    // file the migration has not seen), then the nearest within REFRESH_RATE_TOLERANCE. Off: the
    // truncated integer key (59.94 -> 59), then the nearest within the tolerance, as before.
    std::vector<double> keys;
    keys.reserve(m_corrections.size());
    for (const auto& correction : m_corrections)
        keys.push_back(correction.first);
    const int index = RefreshKey::Find(keys, targetRate, GeneralRate::ProcessEnabled());
    return index < 0 ? 0.0 : keys[static_cast<size_t>(index)];
}
