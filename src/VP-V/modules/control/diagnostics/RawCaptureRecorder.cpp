/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <pch.h>
#include "RawCaptureRecorder.h"
#include <DebugLog.h>
#include <EOTF.h>
#include <ColorSpace.h>
#include <VideoFrameEncoding.h>

#include <atlstr.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace
{
	std::string Env(const char* name)
	{
		char* value = nullptr;
		size_t size = 0;
		std::string out;
		if (_dupenv_s(&value, &size, name) == 0 && value)
			out = value;
		std::free(value);
		return out;
	}

	double EnvNumber(const char* name, double fallback)
	{
		const std::string text = Env(name);
		if (text.empty())
			return fallback;
		char* end = nullptr;
		const double v = std::strtod(text.c_str(), &end);
		return end != text.c_str() ? v : fallback;
	}

	bool FileExists(const std::string& path)
	{
		const DWORD a = GetFileAttributesA(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::string Timestamp()
	{
		std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char text[32];
		std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
		return text;
	}

	double FreeGb(const std::string& path)
	{
		ULARGE_INTEGER available{};
		if (!GetDiskFreeSpaceExA(path.c_str(), &available, nullptr, nullptr))
			return 1e9;
		return static_cast<double>(available.QuadPart) / (1024.0 * 1024.0 * 1024.0);
	}

	// CreateDirectory for every missing level (the Lib builds as C++14: no
	// std::filesystem).
	void CreateDirectories(const std::string& path)
	{
		for (size_t at = path.find_first_of("\\/", 3); ; at = path.find_first_of("\\/", at + 1))
		{
			CreateDirectoryA(path.substr(0, at).c_str(), nullptr);
			if (at == std::string::npos)
				break;
		}
	}

	int64_t QpcNow()
	{
		LARGE_INTEGER v{};
		QueryPerformanceCounter(&v);
		return v.QuadPart;
	}

	int64_t QpcFrequency()
	{
		LARGE_INTEGER v{};
		QueryPerformanceFrequency(&v);
		return v.QuadPart;
	}

	std::string Narrow(const TCHAR* text)
	{
		return text ? std::string(CStringA(text).GetString()) : std::string();
	}
}

RawCaptureOptions RawCaptureOptions::FromEnvironment()
{
	RawCaptureOptions o;
	o.root = Env("VP_RAW_CAPTURE_PATH");
	if (o.root.empty())
		return o;
	o.burstFrames = static_cast<uint32_t>((std::max)(0.0, EnvNumber("VP_RAW_CAPTURE_BURST", 0)));
	o.burstSkipFrames = static_cast<int64_t>(EnvNumber("VP_RAW_CAPTURE_SKIP", -1));
	o.every = static_cast<uint32_t>((std::max)(1.0, EnvNumber("VP_RAW_CAPTURE_EVERY", 1)));
	o.maxSeconds = (std::max)(0.5, EnvNumber("VP_RAW_CAPTURE_MAX_SECONDS", 30.0));
	o.minFreeGb = (std::max)(0.0, EnvNumber("VP_RAW_CAPTURE_MIN_FREE_GB", 30.0));
	o.queueBytes = static_cast<size_t>((std::max)(64.0, EnvNumber("VP_RAW_CAPTURE_QUEUE_MB", 2048))) << 20;
	o.note = Env("VP_RAW_CAPTURE_NOTE");
	return o;
}

RawCaptureRecorder& RawCaptureRecorder::Instance()
{
	static RawCaptureRecorder recorder(RawCaptureOptions::FromEnvironment());
	return recorder;
}

RawCaptureRecorder::RawCaptureRecorder(const RawCaptureOptions& options) : m_options(options)
{
	if (!Enabled())
		return;
	CreateDirectories(m_options.root);
	m_thread = std::thread(&RawCaptureRecorder::WriterThread, this);
	DebugLog::Log("Raw capture: armed, folder=%s burst=%u every=%u max=%.0f s min_free=%.0f GB (toggle: raw_capture / RECORD file)",
		m_options.root.c_str(), m_options.burstFrames, m_options.every, m_options.maxSeconds, m_options.minFreeGb);
}

RawCaptureRecorder::~RawCaptureRecorder()
{
	if (!m_thread.joinable())
		return;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		m_stopThread = true;
	}
	m_wake.notify_all();
	m_thread.join();
}

void RawCaptureRecorder::OnCardState(const std::string& card, const std::string& inputEncoding,
	const std::string& inputBitDepth, const std::vector<std::string>& other)
{
	if (!Enabled())
		return;
	std::lock_guard<std::mutex> guard(m_mutex);
	// "(unchanged)" keeps what the card-state callback reported.
	if (!card.empty())
		m_card = card;
	if (inputEncoding != "(unchanged)")
	{
		m_cardEncoding = inputEncoding;
		m_cardBitDepth = inputBitDepth;
		m_cardOther = other;
	}
}

void RawCaptureRecorder::SetProcessingNote(const std::string& text)
{
	if (!Enabled())
		return;
	std::lock_guard<std::mutex> guard(m_mutex);
	if (text == m_processingNote)
		return;
	m_processingNote = text;
	if (!m_segmentFolder.empty())
	{
		Job job;
		job.folder = m_segmentFolder;
		job.sidecarOnly = true;
		job.sidecar = SidecarLocked();
		m_queue.push_back(std::move(job));
		m_wake.notify_one();
	}
}

std::string RawCaptureRecorder::SidecarLocked() const
{
	std::ostringstream s;
	s << "format=vp-raw-capture-1\n";
	s << "card=" << m_card << "\n";
	s << "card_input_encoding=" << m_cardEncoding << "\n";
	s << "card_input_bit_depth=" << m_cardBitDepth << "\n";
	for (const std::string& line : m_cardOther)
		s << "card_other=" << line << "\n";
	if (m_state && m_state->displayMode)
	{
		const DisplayMode& m = *m_state->displayMode;
		s << "width=" << m.FrameWidth() << "\nheight=" << m.FrameHeight() << "\n";
		s << "interlaced=" << (m.IsInterlaced() ? 1 : 0) << "\n";
		s << "rate_numerator=" << m.TimeScale() << "\nrate_denominator=" << m.FrameDuration() << "\n";
		char rate[32];
		snprintf(rate, sizeof(rate), "%.6f", m.RefreshRateHz());
		s << "rate_hz=" << rate << "\n";
		s << "encoding=" << Narrow(ToString(m_state->videoFrameEncoding)) << "\n";
		s << "encoding_id=" << static_cast<int>(m_state->videoFrameEncoding) << "\n";
		try
		{
			s << "bytes_per_row=" << m_state->BytesPerRow() << "\n";
			s << "bytes_per_frame=" << m_state->BytesPerFrame() << "\n";
		}
		catch (...) {}
		s << "eotf=" << Narrow(ToString(m_state->eotf)) << "\n";
		s << "eotf_id=" << static_cast<int>(m_state->eotf) << "\n";
		s << "colorspace=" << Narrow(ToString(m_state->colorspace)) << "\n";
		s << "colorspace_id=" << static_cast<int>(m_state->colorspace) << "\n";
		s << "hdr_infoframe=" << (m_state->hdrData ? 1 : 0) << "\n";
		if (m_state->hdrData)
		{
			const HDRData& h = *m_state->hdrData;
			s << "hdr_primaries=" << h.displayPrimaryRedX << "," << h.displayPrimaryRedY << ","
				<< h.displayPrimaryGreenX << "," << h.displayPrimaryGreenY << ","
				<< h.displayPrimaryBlueX << "," << h.displayPrimaryBlueY << "\n";
			s << "hdr_white=" << h.whitePointX << "," << h.whitePointY << "\n";
			s << "hdr_mastering=" << h.masteringDisplayMinLuminance << "," << h.masteringDisplayMaxLuminance << "\n";
			s << "hdr_maxcll=" << h.maxCll << "\nhdr_maxfall=" << h.maxFall << "\n";
		}
		s << "capture_dolby_vision_tunnel_flag=" << (m_state->dolbyVisionTunnel ? 1 : 0) << "\n";
	}
	s << "qpc_frequency=" << QpcFrequency() << "\n";
	s << "vp_processing=" << m_processingNote << "\n";
	s << "note=" << m_options.note << "\n";
	s << "never_commit=captured pixels; keep this folder out of source checkouts\n";
	return s.str();
}

void RawCaptureRecorder::BeginSegmentLocked(const char* kind)
{
	if (m_session.empty())
	{
		m_session = m_options.root + "\\" + Timestamp() + "-" + kind;
		m_segment = 0;
	}
	char name[16];
	snprintf(name, sizeof(name), "seg%02u", ++m_segment);
	m_segmentFolder = m_session + "\\" + name;
	CreateDirectories(m_segmentFolder);
	m_segmentFrames = 0;
	Job sidecar;
	sidecar.folder = m_segmentFolder;
	sidecar.sidecarOnly = true;
	sidecar.sidecar = SidecarLocked();
	m_queue.push_back(std::move(sidecar));
	Job header;
	header.folder = m_segmentFolder;
	header.csvLine = "sequence,file,counter,timing_timestamp,capture_timestamp,arrival_qpc,source_discontinuity";
	m_queue.push_back(std::move(header));
	m_wake.notify_one();
}

void RawCaptureRecorder::OnVideoState(const VideoState& state)
{
	if (!Enabled())
		return;
	std::lock_guard<std::mutex> guard(m_mutex);
	m_state = new VideoState(state);
	m_frameInState = 0;
	if (!state.valid || !state.displayMode)
		return;
	++m_statesSeen;
	if (m_continuous.load())
	{
		if (m_segmentFrames == 0 && !m_segmentFolder.empty())
		{
			// Nothing recorded under the previous state: describe this one.
			Job sidecar;
			sidecar.folder = m_segmentFolder;
			sidecar.sidecarOnly = true;
			sidecar.sidecar = SidecarLocked();
			m_queue.push_back(std::move(sidecar));
			m_wake.notify_one();
		}
		else
		{
			BeginSegmentLocked("rec");
			DebugLog::Log("Raw capture: capture state changed while recording; new segment %s", m_segmentFolder.c_str());
		}
	}
	else if (m_options.burstFrames > 0 && m_statesSeen <= m_options.burstStates)
	{
		m_burstActive = true;
		m_burstWritten = 0;
		BeginSegmentLocked("burst");
	}
}

bool RawCaptureRecorder::QueueFrameLocked(const VideoFrame& frame, const char*)
{
	if (!m_state || !m_state->displayMode || !frame.GetData() || m_segmentFolder.empty())
		return false;
	size_t bytes = 0;
	try { bytes = m_state->BytesPerFrame(); }
	catch (...) { return false; }
	if (m_queuedBytes + bytes > m_options.queueBytes)
	{
		const uint64_t skipped = ++m_framesSkipped;
		if ((skipped & (skipped - 1)) == 0)
			DebugLog::Log("Raw capture: the disk is behind; %llu frame(s) not recorded so far (queue %zu MB)",
				static_cast<unsigned long long>(skipped), m_queuedBytes >> 20);
		return false;
	}
	Job job;
	job.folder = m_segmentFolder;
	if (!m_pool.empty())
	{
		job.bytes = std::move(m_pool.back());
		m_pool.pop_back();
	}
	job.bytes.resize(bytes);
	memcpy(job.bytes.data(), frame.GetData(), bytes);
	char file[32];
	snprintf(file, sizeof(file), "frame_%06llu.bin", static_cast<unsigned long long>(++m_segmentFrames));
	job.file = file;
	char line[256];
	snprintf(line, sizeof(line), "%llu,%s,%llu,%lld,%lld,%lld,%d",
		static_cast<unsigned long long>(++m_sequence), file, static_cast<unsigned long long>(frame.GetCounter()),
		static_cast<long long>(frame.GetTimingTimestamp()), static_cast<long long>(frame.GetCaptureTimingTimestamp()),
		static_cast<long long>(QpcNow()), frame.IsSourceDiscontinuity() ? 1 : 0);
	job.csvLine = line;
	m_queuedBytes += bytes;
	m_queue.push_back(std::move(job));
	m_wake.notify_one();
	return true;
}

void RawCaptureRecorder::CheckFiles()
{
	if (!m_options.watchFiles)
		return;
	const std::string recordFile = m_options.root + "\\RECORD";
	const std::string stopFile = m_options.root + "\\STOP";
	const bool record = FileExists(recordFile);
	const bool stop = FileExists(stopFile);
	if (!record)
		m_recordArmed = true;   // a new RECORD file may start the next recording
	if (m_continuous.load())
	{
		if (stop)
		{
			DeleteFileA(stopFile.c_str());
			Stop("STOP file");
		}
		else if (!record && m_startedByFile)
			Stop("RECORD file removed");
	}
	else if (record && !stop && m_recordArmed)
		Start("RECORD file");
	else if (stop)
		DeleteFileA(stopFile.c_str());
}

void RawCaptureRecorder::OnFrame(const VideoFrame& frame)
{
	if (!Enabled())
		return;
	double rate = 24.0;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		++m_frameInState;
		if (m_state && m_state->displayMode && m_state->displayMode->RefreshRateHz() > 0)
			rate = m_state->displayMode->RefreshRateHz();
	}
	const uint64_t checkEvery = static_cast<uint64_t>((std::max)(1.0, std::floor(rate / 2.0)));
	if (m_filesCheckedAt == 0 || frame.GetCounter() >= m_filesCheckedAt + checkEvery ||
		frame.GetCounter() < m_filesCheckedAt)
	{
		CheckFiles();
		m_filesCheckedAt = (std::max)(uint64_t(1), frame.GetCounter());
	}
	bool stopForTime = false, stopForDisk = false;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_continuous.load())
		{
			if (!m_continuousHaveStart)
			{
				m_continuousHaveStart = true;
				m_continuousStartCounter = frame.GetCounter();
				if (m_segmentFolder.empty() || m_session.find("-rec") == std::string::npos)
					BeginSegmentLocked("rec");
			}
			const uint64_t elapsedFrames = frame.GetCounter() >= m_continuousStartCounter ?
				frame.GetCounter() - m_continuousStartCounter : 0;
			if (rate > 0 && static_cast<double>(elapsedFrames) / rate >= m_options.maxSeconds)
				stopForTime = true;
			else if (elapsedFrames % m_options.every == 0)
			{
				if (QueueFrameLocked(frame, "rec") && (m_segmentFrames % 48) == 1 &&
					FreeGb(m_options.root) < m_options.minFreeGb)
					stopForDisk = true;
			}
		}
		else if (m_burstActive)
		{
			const int64_t skip = m_options.burstSkipFrames >= 0 ? m_options.burstSkipFrames :
				static_cast<int64_t>(std::llround(rate));
			if (static_cast<int64_t>(m_frameInState) > skip)
			{
				if (m_burstWritten == 0 && FreeGb(m_options.root) < m_options.minFreeGb)
				{
					m_burstActive = false;
					DebugLog::Log("Raw capture: burst skipped, less than %.0f GB free", m_options.minFreeGb);
				}
				else if (QueueFrameLocked(frame, "burst") && ++m_burstWritten >= m_options.burstFrames)
				{
					m_burstActive = false;
					DebugLog::Log("Raw capture: burst of %llu frames recorded in %s",
						static_cast<unsigned long long>(m_burstWritten), m_segmentFolder.c_str());
				}
			}
		}
	}
	if (stopForTime)
		Stop("time limit (VP_RAW_CAPTURE_MAX_SECONDS)");
	else if (stopForDisk)
		Stop("low disk space (VP_RAW_CAPTURE_MIN_FREE_GB)");
}

std::string RawCaptureRecorder::Start(const char* reason)
{
	if (!Enabled())
	{
		const std::string text = "Raw capture: off - set VP_RAW_CAPTURE_PATH to a folder on a large drive and restart VP";
		DebugLog::Log("%s", text.c_str());
		return text;
	}
	std::string folder;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_continuous.load())
			return "Raw capture: already recording in " + m_session;
		if (FreeGb(m_options.root) < m_options.minFreeGb)
		{
			const std::string text = "Raw capture: not started, less than the minimum free space on the drive";
			DebugLog::Log("%s", text.c_str());
			return text;
		}
		m_session.clear();
		m_segmentFolder.clear();
		m_continuousHaveStart = false;
		m_burstActive = false;
		BeginSegmentLocked("rec");
		folder = m_session;
		m_continuous = true;
		m_startedByFile = std::string(reason) == "RECORD file";
	}
	char text[512];
	snprintf(text, sizeof(text), "Raw capture: recording started (%s) -> %s; every %u frame(s); stops after %.0f s, or press the raw_capture key",
		reason, folder.c_str(), m_options.every, m_options.maxSeconds);
	DebugLog::Log("%s", text);
	return text;
}

std::string RawCaptureRecorder::Stop(const char* reason)
{
	if (!m_continuous.exchange(false))
		return "Raw capture: not recording";
	// A RECORD file still present must be deleted and created again to start
	// the next recording (so a time limit does not restart it at once).
	m_recordArmed = false;
	m_startedByFile = false;
	std::string folder;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		folder = m_session;
	}
	char text[512];
	snprintf(text, sizeof(text), "Raw capture: recording stopped (%s): %llu frame(s) written so far, %llu not recorded (disk behind); folder %s",
		reason, static_cast<unsigned long long>(m_framesWritten.load()),
		static_cast<unsigned long long>(m_framesSkipped.load()), folder.c_str());
	DebugLog::Log("%s", text);
	return text;
}

std::string RawCaptureRecorder::Toggle()
{
	return m_continuous.load() ? Stop("shortcut") : Start("shortcut");
}

std::string RawCaptureRecorder::SessionFolder() const
{
	std::lock_guard<std::mutex> guard(m_mutex);
	return m_session;
}

void RawCaptureRecorder::Flush()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_drained.wait(lock, [this] { return (m_queue.empty() && !m_writing) || !m_thread.joinable(); });
}

void RawCaptureRecorder::WriterThread()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	for (;;)
	{
		m_wake.wait(lock, [this] { return m_stopThread || !m_queue.empty(); });
		if (m_queue.empty())
		{
			if (m_stopThread)
				break;
			continue;
		}
		Job job = std::move(m_queue.front());
		m_queue.pop_front();
		m_writing = true;
		lock.unlock();
		if (job.sidecarOnly)
		{
			std::ofstream out(job.folder + "\\capture.txt", std::ios::trunc);
			out << job.sidecar;
		}
		else
		{
			bool ok = true;
			if (!job.file.empty())
			{
				std::ofstream out(job.folder + "\\" + job.file, std::ios::binary | std::ios::trunc);
				out.write(reinterpret_cast<const char*>(job.bytes.data()), static_cast<std::streamsize>(job.bytes.size()));
				ok = static_cast<bool>(out);
			}
			if (ok && !job.csvLine.empty())
			{
				std::ofstream csv(job.folder + "\\frames.csv", std::ios::app);
				csv << job.csvLine << "\n";
			}
			if (!job.file.empty())
			{
				if (ok)
					++m_framesWritten;
				else
					DebugLog::Log("Raw capture: writing %s failed", job.file.c_str());
			}
		}
		lock.lock();
		if (!job.file.empty())
		{
			m_queuedBytes -= (std::min)(m_queuedBytes, job.bytes.size());
			if (m_pool.size() < 8)
				m_pool.push_back(std::move(job.bytes));
		}
		m_writing = false;
		if (m_queue.empty())
			m_drained.notify_all();
	}
	m_writing = false;
	m_drained.notify_all();
}
