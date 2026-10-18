This directory contains the following files from LAVFilters.
 - IMediaSideData.h

Originally retrieved from https://github.com/Nevcairiel/LAVFilters on 2021-05-06 (partially).
Updated 2026-10-17 to upstream include/IMediaSideData.h at commit
0e6b8133a8b30ddd5a97c99d6b20ffbe246a0656 (2026-05-26, "Add the version identification fields to
the HDR10+ metadata"), which adds the Dolby Vision RPU / metadata V2 side data, the new HDR10+ GUID
(the old one is kept upstream as IID_MediaSideDataHDR10PlusOld) and the control flags.
Local change: #include <guiddef.h> and <unknwn.h> so the header is self-contained.

URL: https://raw.githubusercontent.com/Nevcairiel/LAVFilters/0e6b8133a8b30ddd5a97c99d6b20ffbe246a0656/include/IMediaSideData.h

Licence: LAV Filters is GPL-2.0-or-later (its sources say "either version 2 of the License, or (at
your option) any later version"; COPYING is GPLv2). The header carries no notice of its own, so the
project licence applies; GPL-2.0-or-later is compatible with VideoProcessor's GPL-3.0.
For the license text see LICENSE.txt
