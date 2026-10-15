// VideoProcessorStreamRecorder: records what a DeckLink input actually delivers
// (stream robustness, owner 2026-10-14) - every frame's hardware timestamp,
// stream time, callback time, flags, buffered count and a hash of its pixels,
// plus a small luma thumbnail - so the de-judder's handling of real streamed
// input (repeats, pulldown cadences, missed frames, jitter, drift, resyncs)
// can be inspected and replayed in the tests. It does not process or display
// video, and VP must not be using the card at the same time.

#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#include <DeckLinkAPI_h.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	struct Options
	{
		int device = 0;
		bool list = false;
		BMDVideoConnection input = bmdVideoConnectionHDMI;
		double seconds = 60.0;
		std::wstring outDir;
		int thumbWidth = 240;
		int rawFrames = 0;
	};

	std::atomic<bool> g_stop{false};

	BOOL WINAPI OnConsoleControl(DWORD)
	{
		g_stop.store(true);
		return TRUE;
	}

	int64_t QpcNow()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	const char* PixelFormatName(BMDPixelFormat format)
	{
		switch (format)
		{
		case bmdFormat8BitYUV: return "8BitYUV";
		case bmdFormat10BitYUV: return "10BitYUV";
		case bmdFormat8BitARGB: return "8BitARGB";
		case bmdFormat8BitBGRA: return "8BitBGRA";
		case bmdFormat10BitRGB: return "10BitRGB";
		case bmdFormat12BitRGB: return "12BitRGB";
		default: return "other";
		}
	}

	// The same choice as VP's canonical capture format.
	BMDPixelFormat CapturePixelFormat(BMDDetectedVideoInputFormatFlags flags)
	{
		if (flags & bmdDetectedVideoInputRGB444)
		{
			if (flags & bmdDetectedVideoInput8BitDepth) return bmdFormat8BitARGB;
			if (flags & bmdDetectedVideoInput10BitDepth) return bmdFormat10BitRGB;
			if (flags & bmdDetectedVideoInput12BitDepth) return bmdFormat12BitRGB;
		}
		if (flags & bmdDetectedVideoInputYCbCr422)
		{
			if (flags & bmdDetectedVideoInput8BitDepth) return bmdFormat8BitYUV;
			if (flags & bmdDetectedVideoInput10BitDepth) return bmdFormat10BitYUV;
		}
		return static_cast<BMDPixelFormat>(0);
	}

	// An approximate 8-bit luma of pixel x in a row, for the thumbnail only.
	uint8_t SampleLuma(const uint8_t* row, BMDPixelFormat format, int x)
	{
		switch (format)
		{
		case bmdFormat8BitYUV:   // UYVY
			return row[x * 2 + 1];
		case bmdFormat10BitYUV:  // v210: 6 pixels in 4 little-endian words
		{
			const uint32_t* words = reinterpret_cast<const uint32_t*>(row + (x / 6) * 16);
			uint32_t y = 0;
			switch (x % 6)
			{
			case 0: y = (words[0] >> 10) & 0x3ff; break;
			case 1: y = words[1] & 0x3ff; break;
			case 2: y = (words[1] >> 20) & 0x3ff; break;
			case 3: y = (words[2] >> 10) & 0x3ff; break;
			case 4: y = words[3] & 0x3ff; break;
			default: y = (words[3] >> 20) & 0x3ff; break;
			}
			return static_cast<uint8_t>(y >> 2);
		}
		case bmdFormat8BitARGB:
		{
			const uint8_t* p = row + x * 4;
			return static_cast<uint8_t>((p[1] * 2 + p[2] * 5 + p[3]) / 8);
		}
		case bmdFormat8BitBGRA:
		{
			const uint8_t* p = row + x * 4;
			return static_cast<uint8_t>((p[2] * 2 + p[1] * 5 + p[0]) / 8);
		}
		case bmdFormat10BitRGB:  // r210: big-endian, R 29-20, G 19-10, B 9-0
		{
			const uint8_t* p = row + x * 4;
			const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
			const uint32_t r = (v >> 20) & 0x3ff, g = (v >> 10) & 0x3ff, b = v & 0x3ff;
			return static_cast<uint8_t>(((r * 2 + g * 5 + b) / 8) >> 2);
		}
		default:
			return 0;
		}
	}

	// A fast 64-bit hash of every byte: identical hashes = a bit-exact repeat.
	uint64_t HashBytes(const uint8_t* data, size_t size)
	{
		uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
		const size_t words = size / 8;
		const uint64_t* p = reinterpret_cast<const uint64_t*>(data);
		for (size_t i = 0; i < words; ++i)
		{
			uint64_t k;
			memcpy(&k, p + i, 8);
			h = (h ^ (k * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
			h ^= h >> 29;
		}
		for (size_t i = words * 8; i < size; ++i)
			h = (h ^ data[i]) * 0x100000001B3ull;
		return h;
	}

	class Recorder : public IDeckLinkInputCallback
	{
	public:
		Recorder(IDeckLinkInput* input, const Options& options, FILE* frames, FILE* thumbs, FILE* events)
			: m_input(input), m_options(options), m_frames(frames), m_thumbs(thumbs), m_events(events)
		{
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			m_qpcFrequency = frequency.QuadPart;
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID* ppv) override { *ppv = nullptr; return E_NOINTERFACE; }
		ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
		ULONG STDMETHODCALLTYPE Release() override { return --m_refs; }

		void SetMode(IDeckLinkDisplayMode* mode, BMDPixelFormat format)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			mode->GetFrameRate(&m_frameDuration, &m_timeScale);
			m_pixelFormat = format;
			BSTR name = nullptr;
			char modeName[128] = "?";
			if (mode->GetName(&name) == S_OK && name)
			{
				WideCharToMultiByte(CP_UTF8, 0, name, -1, modeName, sizeof(modeName), nullptr, nullptr);
				SysFreeString(name);
			}
			fprintf(m_events, "frame %llu: mode %s %ldx%ld %.5f Hz %s\n",
				static_cast<unsigned long long>(m_index), modeName, mode->GetWidth(), mode->GetHeight(),
				m_frameDuration > 0 ? double(m_timeScale) / double(m_frameDuration) : 0.0, PixelFormatName(format));
			fflush(m_events);
			printf("Mode: %s %ldx%ld %.5f Hz %s\n", modeName, mode->GetWidth(), mode->GetHeight(),
				m_frameDuration > 0 ? double(m_timeScale) / double(m_frameDuration) : 0.0, PixelFormatName(format));
			m_previousHw = 0;
			m_previousHash = 0;
		}

		HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events,
			IDeckLinkDisplayMode* mode, BMDDetectedVideoInputFormatFlags flags) override
		{
			if (!mode)
				return S_OK;
			const BMDPixelFormat format = CapturePixelFormat(flags);
			if (format == 0)
			{
				fprintf(m_events, "frame %llu: format change to an unsupported signal (flags 0x%x)\n",
					static_cast<unsigned long long>(m_index), static_cast<unsigned>(flags));
				fflush(m_events);
				return S_OK;
			}
			fprintf(m_events, "frame %llu: format changed (events 0x%x, flags 0x%x) - restarting the stream as VP does\n",
				static_cast<unsigned long long>(m_index), static_cast<unsigned>(events), static_cast<unsigned>(flags));
			m_input->PauseStreams();
			m_input->EnableVideoInput(mode->GetDisplayMode(), format, bmdVideoInputEnableFormatDetection);
			m_input->FlushStreams();
			SetMode(mode, format);
			m_input->StartStreams();
			++m_resyncs;
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket*) override
		{
			const int64_t qpc = QpcNow();
			if (!frame || g_stop.load())
				return S_OK;
			std::lock_guard<std::mutex> lock(m_mutex);

			BMDTimeValue hw = 0, hwDuration = 0, stream = 0, streamDuration = 0;
			frame->GetHardwareReferenceTimestamp(10000000, &hw, &hwDuration);
			if (m_timeScale > 0)
				frame->GetStreamTime(&stream, &streamDuration, m_timeScale);
			uint32_t buffered = 0;
			m_input->GetAvailableVideoFrameCount(&buffered);
			const BMDFrameFlags flags = frame->GetFlags();
			const long width = frame->GetWidth(), height = frame->GetHeight(), rowBytes = frame->GetRowBytes();
			void* bytes = nullptr;
			frame->GetBytes(&bytes);
			const uint8_t* data = static_cast<const uint8_t*>(bytes);
			const bool noInput = (flags & bmdFrameHasNoInputSource) != 0;

			uint64_t hash = 0;
			if (data && !noInput)
				hash = HashBytes(data, size_t(rowBytes) * size_t(height));
			const bool repeat = hash != 0 && hash == m_previousHash;

			// Whole periods since the previous frame (1 = contiguous).
			int periods = 0;
			if (m_previousHw > 0 && hwDuration > 0)
				periods = static_cast<int>((hw - m_previousHw + hwDuration / 2) / hwDuration);
			if (periods > 1)
				m_missed += periods - 1;

			// Thumbnail: fixed width, height in proportion, nearest sample.
			const int tw = m_options.thumbWidth;
			const int th = std::max(1, static_cast<int>(int64_t(tw) * height / std::max(1L, width)));
			if (m_thumbHeight == 0)
			{
				m_thumbHeight = th;
				fprintf(m_events, "thumbnails: %dx%d 8-bit luma per frame, in frame order\n", tw, th);
			}
			m_thumb.assign(size_t(tw) * size_t(m_thumbHeight), 0);
			if (data && !noInput)
				for (int y = 0; y < m_thumbHeight; ++y)
				{
					const uint8_t* row = data + size_t(int64_t(y) * height / m_thumbHeight) * size_t(rowBytes);
					for (int x = 0; x < tw; ++x)
						m_thumb[size_t(y) * tw + x] = SampleLuma(row, m_pixelFormat, static_cast<int>(int64_t(x) * width / tw));
				}
			fwrite(m_thumb.data(), 1, m_thumb.size(), m_thumbs);

			if (data && !noInput && m_rawWritten < m_options.rawFrames)
			{
				wchar_t path[MAX_PATH];
				swprintf_s(path, L"%s\\raw_%05llu.bin", m_options.outDir.c_str(), static_cast<unsigned long long>(m_index));
				FILE* raw = nullptr;
				if (_wfopen_s(&raw, path, L"wb") == 0 && raw)
				{
					fwrite(data, 1, size_t(rowBytes) * size_t(height), raw);
					fclose(raw);
					fprintf(m_events, "frame %llu: raw frame saved (%ldx%ld, %ld bytes per row, %s)\n",
						static_cast<unsigned long long>(m_index), width, height, rowBytes, PixelFormatName(m_pixelFormat));
					++m_rawWritten;
				}
			}

			const int64_t callbackUs = (QpcNow() - qpc) * 1000000 / m_qpcFrequency;
			fprintf(m_frames, "%llu,%lld,%lld,%lld,%lld,%lld,%d,%u,0x%x,%d,%016llx,%d,%lld\n",
				static_cast<unsigned long long>(m_index), static_cast<long long>(hw), static_cast<long long>(hwDuration),
				static_cast<long long>(stream), static_cast<long long>(streamDuration), static_cast<long long>(qpc),
				periods, buffered, static_cast<unsigned>(flags), noInput ? 1 : 0,
				static_cast<unsigned long long>(hash), repeat ? 1 : 0, static_cast<long long>(callbackUs));

			m_cadence.push_back(repeat ? 'R' : (periods > 1 ? 'G' : '.'));
			if (m_cadence.size() > 20)
				m_cadence.pop_front();
			if (repeat)
				++m_repeatsThisSecond;
			++m_framesThisSecond;
			m_previousHw = hw;
			if (hash != 0)
				m_previousHash = hash;
			++m_index;
			return S_OK;
		}

		// Once a second: frames, repeats, missed, resyncs and the last 20 frames
		// ('.' new, 'R' a bit-exact repeat, 'G' after missed frames).
		void PrintStatus(double elapsed)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const std::string cadence(m_cadence.begin(), m_cadence.end());
			printf("%6.1f s  %3d fps  repeats %2d  missed %llu  resyncs %d  [%s]\n", elapsed, m_framesThisSecond,
				m_repeatsThisSecond, static_cast<unsigned long long>(m_missed), m_resyncs, cadence.c_str());
			m_framesThisSecond = 0;
			m_repeatsThisSecond = 0;
			fflush(m_frames);
			fflush(m_thumbs);
		}

		uint64_t Frames() { std::lock_guard<std::mutex> lock(m_mutex); return m_index; }

	private:
		std::atomic<ULONG> m_refs{1};
		IDeckLinkInput* m_input;
		const Options& m_options;
		FILE* m_frames;
		FILE* m_thumbs;
		FILE* m_events;
		std::mutex m_mutex;
		int64_t m_qpcFrequency = 1;
		BMDTimeValue m_frameDuration = 0;
		BMDTimeScale m_timeScale = 0;
		BMDPixelFormat m_pixelFormat = bmdFormat8BitYUV;
		uint64_t m_index = 0;
		BMDTimeValue m_previousHw = 0;
		uint64_t m_previousHash = 0;
		uint64_t m_missed = 0;
		int m_resyncs = 0;
		int m_thumbHeight = 0;
		int m_rawWritten = 0;
		int m_framesThisSecond = 0;
		int m_repeatsThisSecond = 0;
		std::vector<uint8_t> m_thumb;
		std::deque<char> m_cadence;
	};

	void PrintUsage()
	{
		printf(
			"VideoProcessorStreamRecorder - records what a DeckLink input delivers (timing, repeats, gaps).\n"
			"Close VP first: the card can only be used by one program.\n\n"
			"  --list              list the DeckLink devices\n"
			"  --device N          device number from --list (default 0)\n"
			"  --input hdmi|sdi    input connector (default hdmi)\n"
			"  --seconds S         how long to record (default 60; Ctrl+C stops early)\n"
			"  --out DIR           output folder (default .\\stream-YYYYMMDD-HHMMSS)\n"
			"  --thumb W           thumbnail width in pixels (default 240, 0 = none)\n"
			"  --raw N             also save the first N full frames (about 20 MB each at 4K)\n\n"
			"Writes frames.csv (one line per frame), thumbs.y8 (8-bit luma thumbnails) and events.txt.\n");
	}
}

int wmain(int argc, wchar_t** argv)
{
	Options options;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if (arg == L"--list") options.list = true;
		else if (arg == L"--device" && hasValue) options.device = _wtoi(argv[++i]);
		else if (arg == L"--input" && hasValue) { const std::wstring v = argv[++i]; options.input = v == L"sdi" ? bmdVideoConnectionSDI : bmdVideoConnectionHDMI; }
		else if (arg == L"--seconds" && hasValue) options.seconds = _wtof(argv[++i]);
		else if (arg == L"--out" && hasValue) options.outDir = argv[++i];
		else if (arg == L"--thumb" && hasValue) options.thumbWidth = std::max(0, _wtoi(argv[++i]));
		else if (arg == L"--raw" && hasValue) options.rawFrames = std::max(0, _wtoi(argv[++i]));
		else { PrintUsage(); return arg == L"--help" || arg == L"-h" ? 0 : 2; }
	}
	if (options.thumbWidth == 0)
		options.thumbWidth = 1;   // keeps the record layout; a 1-pixel column

	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
	{
		printf("COM initialisation failed\n");
		return 1;
	}
	int result = 0;
	{
		using Microsoft::WRL::ComPtr;
		ComPtr<IDeckLinkIterator> iterator;
		if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&iterator))))
		{
			printf("The DeckLink driver is not installed (no DeckLink iterator).\n");
			CoUninitialize();
			return 1;
		}
		ComPtr<IDeckLink> chosen;
		ComPtr<IDeckLink> deckLink;
		for (int index = 0; iterator->Next(deckLink.ReleaseAndGetAddressOf()) == S_OK; ++index)
		{
			BSTR name = nullptr;
			deckLink->GetDisplayName(&name);
			if (options.list)
				printf("%d: %ls\n", index, name ? name : L"DeckLink");
			if (name)
				SysFreeString(name);
			if (index == options.device)
				chosen = deckLink;
		}
		if (options.list)
		{
			CoUninitialize();
			return 0;
		}
		if (!chosen)
		{
			printf("No DeckLink device %d (use --list).\n", options.device);
			CoUninitialize();
			return 1;
		}

		ComPtr<IDeckLinkInput> input;
		ComPtr<IDeckLinkConfiguration> configuration;
		chosen.As(&input);
		chosen.As(&configuration);
		if (!input)
		{
			printf("Device %d cannot capture.\n", options.device);
			CoUninitialize();
			return 1;
		}
		if (configuration)
			configuration->SetInt(bmdDeckLinkConfigVideoInputConnection, options.input);

		if (options.outDir.empty())
		{
			SYSTEMTIME now{};
			GetLocalTime(&now);
			wchar_t name[64];
			swprintf_s(name, L"stream-%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			options.outDir = name;
		}
		CreateDirectoryW(options.outDir.c_str(), nullptr);
		FILE* frames = nullptr;
		FILE* thumbs = nullptr;
		FILE* events = nullptr;
		_wfopen_s(&frames, (options.outDir + L"\\frames.csv").c_str(), L"w");
		_wfopen_s(&thumbs, (options.outDir + L"\\thumbs.y8").c_str(), L"wb");
		_wfopen_s(&events, (options.outDir + L"\\events.txt").c_str(), L"w");
		if (!frames || !thumbs || !events)
		{
			printf("Cannot write to %ls\n", options.outDir.c_str());
			CoUninitialize();
			return 1;
		}
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		fprintf(events, "recorder start; QPC frequency %lld; hardware timestamps in 100 ns units\n",
			static_cast<long long>(frequency.QuadPart));
		fprintf(frames, "index,hw_time_100ns,hw_duration_100ns,stream_time,stream_duration,callback_qpc,"
			"periods_since_previous,buffered,flags,no_input,hash,exact_repeat,callback_us\n");

		Recorder recorder(input.Get(), options, frames, thumbs, events);
		input->SetCallback(&recorder);

		// Start in any mode with format detection; the first format change
		// switches to the signal's mode and pixel format.
		ComPtr<IDeckLinkDisplayModeIterator> modes;
		ComPtr<IDeckLinkDisplayMode> mode;
		if (FAILED(input->GetDisplayModeIterator(&modes)) || modes->Next(mode.GetAddressOf()) != S_OK ||
			FAILED(input->EnableVideoInput(mode->GetDisplayMode(), bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection)))
		{
			printf("Cannot open the input - is VP (or another capture program) still running?\n");
			result = 1;
		}
		else
		{
			recorder.SetMode(mode.Get(), bmdFormat8BitYUV);
			SetConsoleCtrlHandler(OnConsoleControl, TRUE);
			if (FAILED(input->StartStreams()))
			{
				printf("Cannot start the stream.\n");
				result = 1;
			}
			else
			{
				printf("Recording to %ls (Ctrl+C stops)\n", options.outDir.c_str());
				const ULONGLONG start = GetTickCount64();
				ULONGLONG nextStatus = start + 1000;
				while (!g_stop.load() && (GetTickCount64() - start) < static_cast<ULONGLONG>(options.seconds * 1000.0))
				{
					Sleep(50);
					if (GetTickCount64() >= nextStatus)
					{
						recorder.PrintStatus((GetTickCount64() - start) / 1000.0);
						nextStatus += 1000;
					}
				}
				g_stop.store(true);
				input->StopStreams();
			}
			input->DisableVideoInput();
		}
		input->SetCallback(nullptr);
		fprintf(events, "recorder stop; %llu frames\n", static_cast<unsigned long long>(recorder.Frames()));
		printf("%llu frames recorded in %ls\n", static_cast<unsigned long long>(recorder.Frames()), options.outDir.c_str());
		fclose(frames);
		fclose(thumbs);
		fclose(events);
	}
	CoUninitialize();
	return result;
}
