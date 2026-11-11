// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Beta test host only (WAIVER-MV-04): the host links the production MotionAheadProducer.cpp, which needs AnalysisLumaSource::IsValid()
// (and, for formats the report never submits, Sample()). The production AnalysisLumaSource.cpp cannot be compiled here: it includes the
// Lib pch (MFC + DirectShow base classes), which this host does not have. The report submits ONLY P010 frames, so this stub implements the
// production IsValid() for the two-plane 16-bit formats (the same checks, verbatim) and refuses every other format and every sampler call
// (the producer then fails visibly instead of reading garbage). Not part of any runtime project.
#include "AnalysisLumaSource.h"

#include <limits>

bool AnalysisLumaSource::IsValid() const
{
	if (!data || width < 1 || height < 1 || rowBytes == 0)
		return false;
	if (format != AnalysisLumaFormat::P010 && format != AnalysisLumaFormat::P210)
		return false;
	const size_t requiredBytes = rowBytes * static_cast<size_t>(height);
	if (requiredBytes / static_cast<size_t>(height) != rowBytes || requiredBytes > dataBytes)
		return false;
	if ((width & 1) != 0 || (format == AnalysisLumaFormat::P010 && (height & 1) != 0) ||
		rowBytes < static_cast<size_t>(width) * 2 || chromaRowBytes < static_cast<size_t>(width) * 2)
		return false;
	const size_t chromaHeight = format == AnalysisLumaFormat::P210 ? static_cast<size_t>(height) : static_cast<size_t>(height / 2);
	const size_t chromaBytes = chromaRowBytes * chromaHeight;
	return requiredBytes <= (std::numeric_limits<size_t>::max)() - chromaBytes && requiredBytes + chromaBytes <= dataBytes;
}

bool AnalysisLumaSource::Sample(int, int, AnalysisLumaSample&) const { return false; }

// No shared luma plane is ever published for the report's frames: production returns null in that case too.
const uint16_t* AnalysisLumaSource::SharedLumaRow(int) const { return nullptr; }
