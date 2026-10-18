#include "pch.h"

#include "StateReport.h"

#include "DiagnosticHistory.h"
#include "StoreZipWriter.h"

#include <algorithm>
#include <bcrypt.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dxgi1_6.h>
#include <fstream>
#include <intrin.h>
#include <sstream>
#include <wincodec.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace StateReport
{
	namespace
	{
		std::string ToUtf8(const std::wstring& text)
		{
			if (text.empty())
				return std::string();
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
				nullptr, 0, nullptr, nullptr);
			std::string result(size > 0 ? static_cast<size_t>(size) : 0, '\0');
			if (size > 0)
				WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
					&result[0], size, nullptr, nullptr);
			return result;
		}

		std::string Lower(std::string text)
		{
			for (char& c : text)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return text;
		}

		bool StartsWithNoCase(const std::string& text, size_t offset, const std::string& prefix)
		{
			if (offset + prefix.size() > text.size())
				return false;
			for (size_t index = 0; index < prefix.size(); ++index)
			{
				char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset + index])));
				char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
				if (a == '/') a = '\\';
				if (b == '/') b = '\\';
				if (a != b)
					return false;
			}
			return true;
		}

		bool IsDriveStart(const std::string& text, size_t index)
		{
			return index + 2 < text.size() &&
				std::isalpha(static_cast<unsigned char>(text[index])) &&
				text[index + 1] == ':' && (text[index + 2] == '\\' || text[index + 2] == '/') &&
				(index == 0 || !std::isalnum(static_cast<unsigned char>(text[index - 1])));
		}

		bool IsUncStart(const std::string& text, size_t index)
		{
			return index + 2 < text.size() && text[index] == '\\' && text[index + 1] == '\\' &&
				(std::isalnum(static_cast<unsigned char>(text[index + 2])) || text[index + 2] == '?' ||
					text[index + 2] == '.') &&
				(index == 0 || text[index - 1] != '\\');
		}

		bool IsPathDelimiter(char c)
		{
			return c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '\t' ||
				c == '\r' || c == '\n' || c == ',' || c == ';' || c == ')' || c == ']' ||
				c == '}' || c == '=' || c == '*' || c == '?';
		}

		// The end of an absolute path starting at `start`: directories may
		// contain spaces; the last component ends at the first space.
		size_t PathEnd(const std::string& text, size_t start)
		{
			size_t lastSeparator = start + 2;
			size_t index = start + 2;
			for (; index < text.size(); ++index)
			{
				const char c = text[index];
				if (IsPathDelimiter(c) && !(c == '?' && index == start + 2))
					break;
				if (index > start + 2 && (IsDriveStart(text, index) || IsUncStart(text, index)))
					break;
				if (c == '\\' || c == '/')
					lastSeparator = index;
			}
			size_t end = lastSeparator + 1;
			while (end < text.size() && end < index && text[end] != ' ')
				++end;
			// A trailing sentence period or colon is not part of the name.
			while (end > lastSeparator + 1 && (text[end - 1] == '.' || text[end - 1] == ':'))
				--end;
			return end;
		}

		std::string RedactPaths(const std::string& text, const RedactionContext& context)
		{
			std::string out;
			out.reserve(text.size());
			size_t index = 0;
			while (index < text.size())
			{
				if (IsDriveStart(text, index) || IsUncStart(text, index))
				{
					const size_t end = PathEnd(text, index);
					const std::string path = text.substr(index, end - index);
					bool kept = false;
					for (const auto& root : context.keptRoots)
					{
						if (root.first.size() < 3 || !StartsWithNoCase(path, 0, root.first))
							continue;
						const size_t after = root.first.size();
						if (after == path.size() || path[after] == '\\' || path[after] == '/')
						{
							std::string rest = path.substr(after);
							// A kept root's remainder may still hold a user name
							// (rare); the word pass below handles it.
							out += root.second;
							out += rest;
							kept = true;
							break;
						}
					}
					if (!kept)
						out += "<path>";
					index = end > index ? end : index + 1;
					continue;
				}
				out.push_back(text[index]);
				++index;
			}
			return out;
		}

		bool IsEmailLocal(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '%' ||
				c == '+' || c == '-';
		}
		bool IsDomainChar(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
		}

		std::string RedactEmails(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			size_t copied = 0;
			for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + 1))
			{
				if (at < copied)
					continue;
				size_t left = at;
				while (left > copied && IsEmailLocal(text[left - 1]))
					--left;
				size_t right = at + 1;
				while (right < text.size() && IsDomainChar(text[right]))
					++right;
				while (right > at + 1 && text[right - 1] == '.')
					--right;
				const std::string domain = text.substr(at + 1, right - at - 1);
				const size_t dot = domain.find('.');
				if (left == at || dot == std::string::npos || dot == 0 || dot + 1 >= domain.size())
					continue;
				out.append(text, copied, left - copied);
				out += "<email>";
				copied = right;
				at = right - 1;
			}
			out.append(text, copied, std::string::npos);
			return out;
		}

		std::string RedactUrls(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			size_t index = 0;
			while (index < text.size())
			{
				const bool http = StartsWithNoCase(text, index, "http://");
				const bool https = StartsWithNoCase(text, index, "https://");
				const bool ftp = StartsWithNoCase(text, index, "ftp://");
				if (http || https || ftp)
				{
					size_t end = index;
					while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
						text[end] != '"' && text[end] != '\'' && text[end] != '<' && text[end] != '>')
						++end;
					out += "<url>";
					index = end;
					continue;
				}
				out.push_back(text[index]);
				++index;
			}
			return out;
		}

		bool IsKeyChar(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
		}

		// Replaces the values of secret keys anywhere in a line:
		//   key: value | key = value | "key": "value" | key=value
		std::string RedactSecretValues(const std::string& line)
		{
			std::string out;
			size_t index = 0;
			while (index < line.size())
			{
				if (!IsKeyChar(line[index]) || (index > 0 && IsKeyChar(line[index - 1])))
				{
					out.push_back(line[index]);
					++index;
					continue;
				}
				size_t keyEnd = index;
				while (keyEnd < line.size() && IsKeyChar(line[keyEnd]))
					++keyEnd;
				const std::string key = line.substr(index, keyEnd - index);
				size_t cursor = keyEnd;
				if (cursor < line.size() && line[cursor] == '"')
					++cursor;
				while (cursor < line.size() && line[cursor] == ' ')
					++cursor;
				const bool assignment = cursor < line.size() && (line[cursor] == ':' || line[cursor] == '=') &&
					!(line[cursor] == ':' && cursor + 1 < line.size() && (line[cursor + 1] == '\\' || line[cursor + 1] == '/'));
				if (!assignment || !IsSecretKey(key))
				{
					out.append(line, index, keyEnd - index);
					index = keyEnd;
					continue;
				}
				++cursor;
				while (cursor < line.size() && line[cursor] == ' ')
					++cursor;
				out.append(line, index, cursor - index);
				size_t valueEnd = cursor;
				if (cursor < line.size() && line[cursor] == '"')
				{
					valueEnd = line.find('"', cursor + 1);
					valueEnd = valueEnd == std::string::npos ? line.size() : valueEnd + 1;
					out += "\"<redacted>\"";
				}
				else
				{
					// A config value runs to the end of the line (or a comma/brace
					// in JSON-like text); an empty value stays empty.
					while (valueEnd < line.size() && line[valueEnd] != ',' && line[valueEnd] != '}' &&
						line[valueEnd] != '\r' && line[valueEnd] != '\n')
						++valueEnd;
					if (valueEnd > cursor)
						out += "<redacted>";
				}
				index = valueEnd;
			}
			return out;
		}

		std::string ReplaceWordNoCase(const std::string& text, const std::string& word, const std::string& replacement)
		{
			if (word.size() < 3)
				return text;
			const std::string lowerText = Lower(text);
			const std::string lowerWord = Lower(word);
			std::string out;
			size_t copied = 0;
			for (size_t found = lowerText.find(lowerWord); found != std::string::npos;
				found = lowerText.find(lowerWord, found + lowerWord.size()))
			{
				const bool leftOk = found == 0 || !std::isalnum(static_cast<unsigned char>(text[found - 1]));
				const size_t end = found + lowerWord.size();
				const bool rightOk = end >= text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
				if (!leftOk || !rightOk)
					continue;
				out.append(text, copied, found - copied);
				out += replacement;
				copied = end;
			}
			out.append(text, copied, std::string::npos);
			return out;
		}

		std::string JsonString(const std::string& text)
		{
			return "\"" + JsonEscape(text) + "\"";
		}

		std::string FormatLocalTime(const SYSTEMTIME& time)
		{
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
				time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
				time.wMilliseconds);
			return buffer;
		}

		std::wstring Join(const std::wstring& directory, const std::wstring& name)
		{
			if (directory.empty())
				return name;
			const wchar_t last = directory.back();
			return (last == L'\\' || last == L'/') ? directory + name : directory + L"\\" + name;
		}

		std::wstring FileNameOf(const std::wstring& path)
		{
			const size_t separator = path.find_last_of(L"\\/");
			return separator == std::wstring::npos ? path : path.substr(separator + 1);
		}

		bool CreateDirectoryTree(const std::wstring& directory)
		{
			if (directory.empty())
				return false;
			const DWORD attributes = GetFileAttributesW(directory.c_str());
			if (attributes != INVALID_FILE_ATTRIBUTES)
				return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			const size_t separator = directory.find_last_of(L"\\/");
			if (separator != std::wstring::npos && separator > 2)
				CreateDirectoryTree(directory.substr(0, separator));
			return CreateDirectoryW(directory.c_str(), nullptr) != FALSE ||
				GetLastError() == ERROR_ALREADY_EXISTS;
		}

		bool WriteFileAtomically(const std::wstring& path, const std::vector<unsigned char>& data)
		{
			const std::wstring temporary = path + L".partial";
			HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			size_t written = 0;
			bool ok = true;
			while (written < data.size())
			{
				const DWORD chunk = static_cast<DWORD>((std::min)(data.size() - written, size_t(1) << 24));
				DWORD done = 0;
				if (!WriteFile(file, data.data() + written, chunk, &done, nullptr) || done == 0)
				{
					ok = false;
					break;
				}
				written += done;
			}
			ok = ok && FlushFileBuffers(file) != FALSE;
			CloseHandle(file);
			if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileW(temporary.c_str());
				return false;
			}
			return true;
		}

		std::string RegistryString(HKEY root, const wchar_t* key, const wchar_t* value)
		{
			wchar_t buffer[256] = {};
			DWORD size = sizeof(buffer);
			if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
				return std::string();
			return ToUtf8(buffer);
		}

		DWORD RegistryDword(HKEY root, const wchar_t* key, const wchar_t* value)
		{
			DWORD data = 0;
			DWORD size = sizeof(data);
			if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS)
				return 0;
			return data;
		}

		const char* OutputTechnologyName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
		{
			switch (technology)
			{
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "DisplayPort (embedded)";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "internal";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return "VGA";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return "Miracast";
			case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return "indirect";
			default: return "other";
			}
		}

		const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE space)
		{
			switch (space)
			{
			case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "SDR (sRGB/BT.709)";
			case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "HDR10 (PQ BT.2020)";
			case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "scRGB linear";
			default: return "other";
			}
		}
	}

	std::string JsonEscape(const std::string& text)
	{
		std::string out;
		out.reserve(text.size() + 8);
		for (const char c : text)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					char buffer[8];
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
					out += buffer;
				}
				else
					out.push_back(c);
			}
		}
		return out;
	}

	bool IsSecretKey(const std::string& key)
	{
		const std::string lower = Lower(key);
		// Long, unambiguous words match anywhere in the key.
		static const char* const anywhere[] = {
			"password", "passwd", "passphrase", "secret", "token", "apikey", "credential",
			"privatekey", "hostname", "username", "cookie" };
		for (const char* word : anywhere)
			if (lower.find(word) != std::string::npos)
				return true;
		// Short words must be a whole token ("ghost_suppression" is not "host").
		std::vector<std::string> tokens;
		std::string token;
		for (const char c : lower)
		{
			if (std::isalnum(static_cast<unsigned char>(c)))
				token.push_back(c);
			else if (!token.empty())
			{
				tokens.push_back(token);
				token.clear();
			}
		}
		if (!token.empty())
			tokens.push_back(token);
		static const char* const whole[] = {
			"pwd", "auth", "authorization", "serial", "license", "licence", "host", "address",
			"ip", "url", "uri", "email", "mail", "user", "login", "account", "ssid", "session", "pin" };
		for (size_t index = 0; index < tokens.size(); ++index)
		{
			for (const char* word : whole)
				if (tokens[index] == word)
					return true;
			if (tokens[index] == "key" && index > 0 &&
				(tokens[index - 1] == "api" || tokens[index - 1] == "access" ||
					tokens[index - 1] == "private" || tokens[index - 1] == "license" ||
					tokens[index - 1] == "product"))
				return true;
		}
		return false;
	}

	std::string Redact(const std::string& text, const RedactionContext& context)
	{
		// Line by line keeps the secret-value pass bounded to one line.
		std::string out;
		out.reserve(text.size());
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			const bool hasNewline = end != std::string::npos;
			if (!hasNewline)
				end = text.size();
			std::string line = text.substr(start, end - start);
			line = RedactUrls(line);
			line = RedactEmails(line);
			line = RedactPaths(line, context);
			line = RedactSecretValues(line);
			for (const auto& word : context.words)
				line = ReplaceWordNoCase(line, word.first, word.second);
			out += line;
			if (hasNewline)
				out.push_back('\n');
			start = end + 1;
		}
		return out;
	}

	RedactionContext CurrentRedactionContext()
	{
		RedactionContext context;
		wchar_t buffer[MAX_PATH * 4] = {};
		const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
		if (length > 0 && length < _countof(buffer))
		{
			std::wstring exe(buffer, length);
			const size_t separator = exe.find_last_of(L"\\/");
			if (separator != std::wstring::npos)
				context.keptRoots.emplace_back(ToUtf8(exe.substr(0, separator)), "<vp>");
		}
		const UINT windowsLength = GetWindowsDirectoryW(buffer, static_cast<UINT>(_countof(buffer)));
		if (windowsLength > 0 && windowsLength < _countof(buffer))
			context.keptRoots.emplace_back(ToUtf8(std::wstring(buffer, windowsLength)), "<windows>");
		DWORD size = static_cast<DWORD>(_countof(buffer));
		if (GetUserNameW(buffer, &size) && size > 1)
			context.words.emplace_back(ToUtf8(buffer), "<user>");
		size = static_cast<DWORD>(_countof(buffer));
		if (GetComputerNameW(buffer, &size) && size > 0)
			context.words.emplace_back(ToUtf8(std::wstring(buffer, size)), "<host>");
		size = static_cast<DWORD>(_countof(buffer));
		if (GetComputerNameExW(ComputerNameDnsFullyQualified, buffer, &size) && size > 0)
			context.words.emplace_back(ToUtf8(std::wstring(buffer, size)), "<host>");
		return context;
	}

	std::string FileName(const SYSTEMTIME& t)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%s%04u%02u%02u-%02u%02u%02u-%03u%s", kFilePrefix,
			t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, kFileSuffix);
		return buffer;
	}

	std::vector<std::wstring> ListReports(const std::wstring& directory)
	{
		std::vector<std::wstring> names;
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(Join(directory, L"vp-report-*.zip").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return names;
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				names.push_back(data.cFileName);
		} while (FindNextFileW(find, &data));
		FindClose(find);
		std::sort(names.begin(), names.end());
		return names;
	}

	std::vector<std::wstring> RemoveStalePartials(const std::wstring& directory, uint64_t olderThanMs)
	{
		std::vector<std::wstring> removed;
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(Join(directory, L"vp-report-*.zip.partial").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return removed;
		FILETIME nowFileTime = {};
		GetSystemTimeAsFileTime(&nowFileTime);
		const uint64_t now = (static_cast<uint64_t>(nowFileTime.dwHighDateTime) << 32) | nowFileTime.dwLowDateTime;
		std::vector<std::wstring> stale;
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				continue;
			const uint64_t written = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
				data.ftLastWriteTime.dwLowDateTime;
			// FILETIME is in 100 ns units.
			if (now > written && (now - written) / 10000ull >= olderThanMs)
				stale.push_back(data.cFileName);
		} while (FindNextFileW(find, &data));
		FindClose(find);
		for (const std::wstring& name : stale)
			if (DeleteFileW(Join(directory, name).c_str()))
				removed.push_back(name);
		return removed;
	}

	std::vector<std::wstring> ApplyRetention(const std::wstring& directory, int keep)
	{
		std::vector<std::wstring> removed = RemoveStalePartials(directory);
		if (keep < 1)
			keep = 1;
		const std::vector<std::wstring> names = ListReports(directory);
		if (names.size() <= static_cast<size_t>(keep))
			return removed;
		const size_t excess = names.size() - static_cast<size_t>(keep);
		for (size_t index = 0; index < excess; ++index)
			if (DeleteFileW(Join(directory, names[index]).c_str()))
				removed.push_back(names[index]);
		return removed;
	}

	std::string Sha256Hex(const void* data, size_t size)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		unsigned char digest[32] = {};
		std::string result;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
			return result;
		if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			size_t offset = 0;
			bool ok = true;
			while (ok && offset < size)
			{
				const ULONG chunk = static_cast<ULONG>((std::min)(size - offset, size_t(1) << 30));
				ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes + offset), chunk, 0) == 0;
				offset += chunk;
			}
			if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
			{
				static const char hex[] = "0123456789abcdef";
				for (unsigned char byte : digest)
				{
					result.push_back(hex[byte >> 4]);
					result.push_back(hex[byte & 0x0f]);
				}
			}
			BCryptDestroyHash(hash);
		}
		BCryptCloseAlgorithmProvider(algorithm, 0);
		return result;
	}

	std::string Sha256File(const std::wstring& path, uint64_t* bytes)
	{
		if (bytes)
			*bytes = 0;
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return std::string();
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::string result;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
		{
			if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
			{
				std::vector<unsigned char> buffer(1 << 20);
				uint64_t total = 0;
				bool ok = true;
				for (;;)
				{
					DWORD read = 0;
					if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
					{
						ok = false;
						break;
					}
					if (read == 0)
						break;
					total += read;
					if (BCryptHashData(hash, buffer.data(), read, 0) != 0)
					{
						ok = false;
						break;
					}
				}
				unsigned char digest[32] = {};
				if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
				{
					static const char hex[] = "0123456789abcdef";
					for (unsigned char byte : digest)
					{
						result.push_back(hex[byte >> 4]);
						result.push_back(hex[byte & 0x0f]);
					}
					if (bytes)
						*bytes = total;
				}
				BCryptDestroyHash(hash);
			}
			BCryptCloseAlgorithmProvider(algorithm, 0);
		}
		CloseHandle(file);
		return result;
	}

	std::string ReadWholeFile(const std::wstring& path, size_t maxBytes)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return std::string();
		LARGE_INTEGER size = {};
		std::string text;
		if (GetFileSizeEx(file, &size) && size.QuadPart > 0)
		{
			const size_t wanted = static_cast<size_t>((std::min)(static_cast<unsigned long long>(size.QuadPart),
				static_cast<unsigned long long>(maxBytes)));
			text.resize(wanted);
			DWORD read = 0;
			if (!ReadFile(file, &text[0], static_cast<DWORD>(wanted), &read, nullptr))
				read = 0;
			text.resize(read);
		}
		CloseHandle(file);
		return text;
	}

	std::string ReadFileTail(const std::wstring& path, size_t maxBytes)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return std::string();
		LARGE_INTEGER size = {};
		std::string text;
		if (GetFileSizeEx(file, &size) && size.QuadPart > 0)
		{
			const long long start = size.QuadPart > static_cast<long long>(maxBytes) ?
				size.QuadPart - static_cast<long long>(maxBytes) : 0;
			LARGE_INTEGER position;
			position.QuadPart = start;
			if (SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
			{
				text.resize(static_cast<size_t>(size.QuadPart - start));
				DWORD read = 0;
				if (!ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &read, nullptr))
					read = 0;
				text.resize(read);
				if (start > 0)
				{
					const size_t newline = text.find('\n');
					text = newline == std::string::npos ? std::string() : text.substr(newline + 1);
				}
			}
		}
		CloseHandle(file);
		return text;
	}

	std::string NewestKeptCrashState(const std::wstring& directory, std::string* name)
	{
		if (name)
			name->clear();
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(Join(directory, L"crash state (*).txt").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return std::string();
		int best = -1;
		std::wstring bestName;
		do
		{
			const std::wstring file = data.cFileName;
			const size_t open = file.find(L'(');
			const size_t close = file.find(L')', open == std::wstring::npos ? 0 : open);
			if (open == std::wstring::npos || close == std::wstring::npos)
				continue;
			const int number = _wtoi(file.substr(open + 1, close - open - 1).c_str());
			if (number > best)
			{
				best = number;
				bestName = file;
			}
		} while (FindNextFileW(find, &data));
		FindClose(find);
		if (best < 0)
			return std::string();
		if (name)
			*name = ToUtf8(bestName);
		return ReadWholeFile(Join(directory, bestName), 1024 * 1024);
	}

	std::string CollectSystemJson()
	{
		std::ostringstream json;
		json << "{\n";

		// OS: RtlGetVersion reports the real version (no manifest shim).
		{
			RTL_OSVERSIONINFOW version = {};
			version.dwOSVersionInfoSize = sizeof(version);
			using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
			if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
				if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")))
					rtlGetVersion(&version);
			const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
			json << "  \"os\": {\"product\": " << JsonString(RegistryString(HKEY_LOCAL_MACHINE, key, L"ProductName"))
				<< ", \"display_version\": " << JsonString(RegistryString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"))
				<< ", \"major\": " << version.dwMajorVersion << ", \"minor\": " << version.dwMinorVersion
				<< ", \"build\": " << version.dwBuildNumber
				<< ", \"ubr\": " << RegistryDword(HKEY_LOCAL_MACHINE, key, L"UBR") << "},\n";
		}

		// CPU, memory, power.
		{
			int registers[4] = {};
			char brand[49] = {};
			__cpuid(registers, 0x80000000);
			if (static_cast<unsigned>(registers[0]) >= 0x80000004u)
				for (int leaf = 0; leaf < 3; ++leaf)
				{
					__cpuid(registers, 0x80000002 + leaf);
					std::memcpy(brand + leaf * 16, registers, 16);
				}
			std::string brandText(brand);
			brandText.erase(0, brandText.find_first_not_of(' '));
			SYSTEM_INFO info = {};
			GetNativeSystemInfo(&info);
			MEMORYSTATUSEX memory = {};
			memory.dwLength = sizeof(memory);
			GlobalMemoryStatusEx(&memory);
			SYSTEM_POWER_STATUS power = {};
			const bool powerKnown = GetSystemPowerStatus(&power) != FALSE;
			json << "  \"cpu\": {\"name\": " << JsonString(brandText)
				<< ", \"logical_processors\": " << info.dwNumberOfProcessors << "},\n";
			json << "  \"memory_mb\": " << (memory.ullTotalPhys / (1024ull * 1024ull))
				<< ", \"memory_available_mb\": " << (memory.ullAvailPhys / (1024ull * 1024ull)) << ",\n";
			json << "  \"power\": {\"ac\": " << (!powerKnown || power.ACLineStatus == 255 ? "null" :
				power.ACLineStatus == 1 ? "true" : "false")
				<< ", \"battery_percent\": " << (!powerKnown || power.BatteryLifePercent == 255 ? -1 :
					static_cast<int>(power.BatteryLifePercent)) << "},\n";
		}

		// GPUs and their outputs.
		json << "  \"gpus\": [";
		{
			IDXGIFactory1* factory = nullptr;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
			{
				IDXGIAdapter1* adapter = nullptr;
				for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
				{
					DXGI_ADAPTER_DESC1 desc = {};
					adapter->GetDesc1(&desc);
					LARGE_INTEGER umd = {};
					const bool hasDriver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
					char driver[64] = "unknown";
					if (hasDriver)
						std::snprintf(driver, sizeof(driver), "%u.%u.%u.%u",
							HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
					char ids[64];
					std::snprintf(ids, sizeof(ids), "%04X:%04X", desc.VendorId, desc.DeviceId);
					json << (index ? ",\n" : "\n") << "    {\"name\": " << JsonString(ToUtf8(desc.Description))
						<< ", \"pci_id\": \"" << ids << "\", \"driver\": \"" << driver << "\""
						<< ", \"dedicated_mb\": " << (desc.DedicatedVideoMemory / (1024ull * 1024ull))
						<< ", \"software\": " << ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false")
						<< ", \"outputs\": [";
					IDXGIOutput* output = nullptr;
					for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex)
					{
						DXGI_OUTPUT_DESC outputDesc = {};
						output->GetDesc(&outputDesc);
						json << (outputIndex ? ", " : "") << "{\"device\": " << JsonString(ToUtf8(outputDesc.DeviceName));
						DEVMODEW mode = {};
						mode.dmSize = sizeof(mode);
						if (EnumDisplaySettingsW(outputDesc.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
							json << ", \"mode\": \"" << mode.dmPelsWidth << "x" << mode.dmPelsHeight << "@"
								<< mode.dmDisplayFrequency << "\", \"bits_per_pixel\": " << mode.dmBitsPerPel;
						IDXGIOutput6* output6 = nullptr;
						if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))))
						{
							DXGI_OUTPUT_DESC1 desc1 = {};
							if (SUCCEEDED(output6->GetDesc1(&desc1)))
								json << ", \"bits_per_color\": " << desc1.BitsPerColor
									<< ", \"color_space\": \"" << ColorSpaceName(desc1.ColorSpace) << "\""
									<< ", \"max_luminance\": " << desc1.MaxLuminance
									<< ", \"max_full_frame_luminance\": " << desc1.MaxFullFrameLuminance
									<< ", \"min_luminance\": " << desc1.MinLuminance;
							output6->Release();
						}
						json << "}";
						output->Release();
					}
					json << "]}";
					adapter->Release();
				}
				factory->Release();
			}
		}
		json << "\n  ],\n";

		// Active display paths: monitor model and the exact refresh rate.
		json << "  \"displays\": [";
		{
			UINT32 pathCount = 0;
			UINT32 modeCount = 0;
			std::vector<DISPLAYCONFIG_PATH_INFO> paths;
			std::vector<DISPLAYCONFIG_MODE_INFO> modes;
			LONG status = ERROR_SUCCESS;
			do
			{
				if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
					break;
				paths.resize(pathCount);
				modes.resize(modeCount);
				status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
			} while (status == ERROR_INSUFFICIENT_BUFFER);
			if (status == ERROR_SUCCESS)
			{
				paths.resize(pathCount);
				for (UINT32 index = 0; index < pathCount; ++index)
				{
					const DISPLAYCONFIG_PATH_INFO& path = paths[index];
					DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
					source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					source.header.size = sizeof(source);
					source.header.adapterId = path.sourceInfo.adapterId;
					source.header.id = path.sourceInfo.id;
					DisplayConfigGetDeviceInfo(&source.header);
					DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
					target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
					target.header.size = sizeof(target);
					target.header.adapterId = path.targetInfo.adapterId;
					target.header.id = path.targetInfo.id;
					DisplayConfigGetDeviceInfo(&target.header);
					const DISPLAYCONFIG_RATIONAL rate = path.targetInfo.refreshRate;
					const double hz = rate.Denominator ? static_cast<double>(rate.Numerator) / rate.Denominator : 0.0;
					char refresh[32];
					std::snprintf(refresh, sizeof(refresh), "%.4f", hz);
					json << (index ? ",\n" : "\n") << "    {\"device\": " << JsonString(ToUtf8(source.viewGdiDeviceName))
						<< ", \"monitor\": " << JsonString(ToUtf8(target.monitorFriendlyDeviceName))
						<< ", \"connection\": \"" << OutputTechnologyName(path.targetInfo.outputTechnology) << "\""
						<< ", \"refresh_hz\": " << refresh
						<< ", \"refresh_rational\": \"" << rate.Numerator << "/" << rate.Denominator << "\"}";
				}
			}
		}
		json << "\n  ]\n}\n";
		return json.str();
	}

	bool MakeThumbnailPng(const std::wstring& imagePath, unsigned maxWidth,
		std::vector<unsigned char>& png, std::string* error)
	{
		png.clear();
		auto fail = [error](const char* message) { if (error) *error = message; return false; };
		IWICImagingFactory* factory = nullptr;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&factory))))
			return fail("WIC unavailable");
		bool ok = false;
		IWICBitmapDecoder* decoder = nullptr;
		IWICBitmapFrameDecode* frame = nullptr;
		IWICBitmapScaler* scaler = nullptr;
		IWICFormatConverter* converter = nullptr;
		IStream* stream = nullptr;
		IWICBitmapEncoder* encoder = nullptr;
		IWICBitmapFrameEncode* target = nullptr;
		do
		{
			if (FAILED(factory->CreateDecoderFromFilename(imagePath.c_str(), nullptr, GENERIC_READ,
				WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame)))
			{
				fail("cannot decode the screenshot");
				break;
			}
			UINT width = 0, height = 0;
			frame->GetSize(&width, &height);
			if (width == 0 || height == 0)
			{
				fail("empty image");
				break;
			}
			const UINT outWidth = width > maxWidth ? maxWidth : width;
			const UINT outHeight = (std::max)(1u, static_cast<UINT>(static_cast<unsigned long long>(height) * outWidth / width));
			if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
				FAILED(scaler->Initialize(frame, outWidth, outHeight, WICBitmapInterpolationModeFant)) ||
				FAILED(factory->CreateFormatConverter(&converter)) ||
				FAILED(converter->Initialize(scaler, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
					nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			{
				fail("cannot scale the screenshot");
				break;
			}
			if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
				FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
				FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) ||
				FAILED(encoder->CreateNewFrame(&target, nullptr)) ||
				FAILED(target->Initialize(nullptr)) ||
				FAILED(target->SetSize(outWidth, outHeight)))
			{
				fail("cannot create the PNG encoder");
				break;
			}
			WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
			if (FAILED(target->SetPixelFormat(&format)) || FAILED(target->WriteSource(converter, nullptr)) ||
				FAILED(target->Commit()) || FAILED(encoder->Commit()))
			{
				fail("cannot encode the PNG");
				break;
			}
			HGLOBAL memory = nullptr;
			if (FAILED(GetHGlobalFromStream(stream, &memory)))
			{
				fail("no PNG memory");
				break;
			}
			STATSTG stat = {};
			stream->Stat(&stat, STATFLAG_NONAME);
			const void* bytes = GlobalLock(memory);
			if (bytes)
			{
				const auto* begin = static_cast<const unsigned char*>(bytes);
				png.assign(begin, begin + static_cast<size_t>(stat.cbSize.QuadPart));
				GlobalUnlock(memory);
				ok = !png.empty();
			}
		} while (false);
		if (target) target->Release();
		if (encoder) encoder->Release();
		if (stream) stream->Release();
		if (converter) converter->Release();
		if (scaler) scaler->Release();
		if (frame) frame->Release();
		if (decoder) decoder->Release();
		factory->Release();
		return ok;
	}

	std::string LiveSettingsJson(const std::vector<std::pair<std::string, std::string>>& settings)
	{
		std::string json = "{\n";
		for (size_t index = 0; index < settings.size(); ++index)
		{
			json += "  " + JsonString(settings[index].first) + ": " + JsonString(settings[index].second);
			json += index + 1 < settings.size() ? ",\n" : "\n";
		}
		json += "}\n";
		return json;
	}

	std::string ManifestJson(const Inputs& inputs, const std::string& createdLocal,
		const std::vector<std::pair<std::string, std::string>>& binaryHashes,
		const std::vector<std::string>& contents, size_t exeRows, size_t rendererRows)
	{
		std::ostringstream json;
		json << "{\n";
		json << "  \"format\": \"vp-state-report\",\n";
		json << "  \"format_version\": " << kFormatVersion << ",\n";
		json << "  \"created_local\": " << JsonString(createdLocal) << ",\n";
		json << "  \"trigger\": " << JsonString(inputs.trigger) << ",\n";
		json << "  \"build_id\": " << JsonString(inputs.buildId) << ",\n";
		json << "  \"commit\": " << JsonString(inputs.commit) << ",\n";
		json << "  \"version\": " << JsonString(inputs.version) << ",\n";
		json << "  \"plugin_api\": " << inputs.pluginApi << ",\n";
		json << "  \"renderer\": " << JsonString(inputs.rendererName) << ",\n";
		json << "  \"renderer_history\": " << (inputs.rendererContributed ? "true" : "false") << ",\n";
		json << "  \"history_rows\": {\"exe\": " << exeRows << ", \"renderer\": " << rendererRows << "},\n";
		json << "  \"history_bytes_per_module\": " << DiagnosticHistory::kTotalBytes << ",\n";
		json << "  \"thumbnail\": " << (inputs.thumbnailEnabled ? "true" : "false") << ",\n";
		json << "  \"binaries\": [";
		for (size_t index = 0; index < binaryHashes.size(); ++index)
			json << (index ? ", " : "") << "{\"name\": " << JsonString(binaryHashes[index].first)
				<< ", \"sha256\": " << JsonString(binaryHashes[index].second) << "}";
		json << "],\n";
		json << "  \"contents\": [";
		for (size_t index = 0; index < contents.size(); ++index)
			json << (index ? ", " : "") << JsonString(contents[index]);
		json << "],\n";
		json << "  \"redaction\": \"paths outside the VP folder -> <path>; VP folder -> <vp>; Windows folder -> <windows>; "
			"user and computer names -> <user>/<host>; e-mail -> <email>; URLs -> <url>; secret-like keys -> <redacted>\"\n";
		json << "}\n";
		return json.str();
	}

	std::string ReadmeText()
	{
		std::ostringstream md;
		const char* const* stats = DiagnosticHistory::StatsFieldNames();
		md << "# VideoProcessor problem report\n\n"
			"Written by VideoProcessor (VP) when the user pressed the report shortcut (default Ctrl+Alt+D), chose\n"
			"\"Save problem report\" in the window's system menu, or (if `[logging] auto_report` is on) when VP\n"
			"detected a problem itself. It holds the last few seconds to minutes of VP's state so a developer or\n"
			"Claude can diagnose a problem that did not crash VP. `manifest.json` says which trigger it was.\n\n"
			"## Files\n\n"
			"| File | Contents |\n|---|---|\n"
			"| `manifest.json` | Format version, local creation time, trigger, build ID (32 hex, the EXE/DLL pairing ID), commit, plugin API, active renderer, SHA-256 of `VideoProcessor.exe` and the renderer DLLs, list of files. |\n"
			"| `state-history.jsonl` | The in-memory diagnostic rings of the executable (`src: exe`) and the renderer DLL (`src: renderer`), one JSON object per line. See below. |\n"
			"| `log-excerpt.txt` | The tail of the active `logs\\vp.log` (or the in-memory recent lines when file logging is off), redacted. Log lines carry local wall-clock time; `manifest.created_local` is `t_ms = 0`. |\n"
			"| `crash-state.txt` | The live crash-state values at the moment of the report (source, display refresh, renderer, profiles, de-judder, presentation, render stall, Dynamic Drive, source resolution, content). |\n"
			"| `crash-state-last.txt` | Only if a previous run crashed: the newest kept `crash state (N).txt`. Its time says whether it is related. |\n"
			"| `live-settings.json` | Effective runtime choices: active profile selections, renderer, window mode, overlay, shortcut and report switches. |\n"
			"| `config/*` | Each configuration file VP uses, redacted (comments kept). |\n"
			"| `system.json` | OS version, CPU, memory, power source, GPUs (PCI ID, driver version, memory), outputs (current mode, bits, HDR colour space, luminance) and active display paths (monitor model, connection, exact refresh as a rational). |\n"
			"| `thumbnail.png` | Only when `[logging] report_thumbnail` is on (default off): a small copy of the next rendered frame. |\n\n"
			"## state-history.jsonl\n\n"
			"Every row has `src` (`exe` or `renderer`), `ring`, `i` (the record's absolute index in its ring; gaps\n"
			"mean records were overwritten or skipped) and `t_ms`: milliseconds relative to the report moment\n"
			"(negative = before it). Both modules use the same QueryPerformanceCounter clock, so `t_ms` of exe and\n"
			"renderer rows are directly comparable. Rows are grouped by ring; sort by `t_ms` to interleave.\n\n"
			"- `ring_meta`: per ring and module: `name`, `capacity`, `written` (ever), `returned`, `skipped`\n"
			"  (records being written during the snapshot - normal, a few at most).\n"
			"- `capture`: capture-card events - signal/video-state changes, ingress refusals, device errors.\n"
			"  Fields `name`, `detail`, and numbers `a`, `b`, `x`, `y` whose meaning depends on `name`.\n"
			"- `decision`: VP's choices - renderer reset requests (`a` = reason), profile changes, render-ahead\n"
			"  gate/hold/open-loop transitions, motion engine and de-judder modes, source-resolution latch, content\n"
			"  intent, output/display mode changes, report triggers. Same fields as `capture`.\n"
			"- `led`: Dynamic Drive (projector LED, RS-232) commands actually sent and connection events; the\n"
			"  level/value is in `a` or `x`.\n"
			"- `stats`: one row per second from the executable: ";
		for (size_t index = 0; index < DiagnosticHistory::kStatsFields; ++index)
			md << (index ? ", " : "") << "`" << stats[index] << "`";
		md << ".\n"
			"  Totals (`*_total`) are cumulative counters; differences between rows give per-second rates.\n"
			"- `frame`: one row per frame per stage. `stage` is one of `capture_ingress` (exe: accepted from the\n"
			"  card), `capture_rejected` (exe: refused during a state change), `renderer_ingest`, `rendered`,\n"
			"  `presented`, `dropped`, `repeated` (renderer). Fields (absent = 0): `counter` (capture frame\n"
			"  counter - follow one frame through the stages with it), `source_ts`/`capture_ts` (card timestamps,\n"
			"  100 ns units), `present_t_ms` (target/actual present time, same base as `t_ms`), `queue` (frames\n"
			"  queued), `hold` (renderer hold-reason code), `render_ms`, `upload_ms`, `wait_ms`, `interval_ms`\n"
			"  (since the previous row of that stage), `lead_ms`, `latency_ms` (capture to this stage), `phase`\n"
			"  (cadence/render-ahead slot), `repeat`, `w`/`h`, `dropped_total`, `repeated_total`, `gen` (renderer\n"
			"  generation; it changes on every renderer reset), and `flags` (bit mask: 1 interpolated, 2\n"
			"  render-ahead, 4 open loop, 8 late, 16 scene cut, 32 vectors valid, 64 HDR, 128 discontinuity).\n\n"
			"The frame ring holds the newest 4096 frame rows per module (about 15-60 s of playback, less when\n"
			"several stages are recorded per frame); decisions 1024, LED 256, capture 256, stats 256 (about 4 min).\n\n"
			"## Renderer hold codes (`hold` in frame rows)\n\n"
			"`dropped` rows: 1 no video state, 2 queue full of repeats (incoming dropped), 3 queue overflow (oldest\n"
			"dropped), 4 active-picture identity rejected, 5 enqueue failed, 6 render/present failed, 7 backlog flush,\n"
			"8 conversion failed, 9 drift-correction drop, 10 queue resized below its depth.\n"
			"`repeated` rows (de-judder / render-ahead holds; the `decision` ring has the text): 20 other, 21 no\n"
			"motion estimate, 22 vectors invalid, 23 cadence repeat, 24 composed source (subtitle moving), 25 composed\n"
			"or RGB source without the picture, 26 no flip presentation, 27 timing unknown, 28 frame work too long,\n"
			"29 no swapchain/output, 30 render-ahead no plan, 31 render-ahead gate hold, 32 render-ahead extras\n"
			"dropped, 33 render-ahead cadence, 34 render-ahead phase, 35 extra failed or late, 36 planner stopped,\n"
			"37 next frame shown late, 38 cadence-correction repeat. Renderer `presented` rows: `phase` = the main\n"
			"present's SyncInterval (render-ahead), `repeat` 1 = cadence repeat, `latency_ms` = capture callback to\n"
			"Present returned. Renderer rows have no `stats` ring (the executable's stats cover both).\n\n"
			"## How to read it\n\n"
			"1. `manifest.json`: build, trigger, renderer. `system.json`: GPU, driver, display mode and refresh.\n"
			"2. `crash-state.txt` for the one-screen summary of what VP was doing.\n"
			"3. `state-history.jsonl`: look for `decision` and `capture` rows near `t_ms` 0 (the user pressed the\n"
			"   key when they saw the problem, usually 0-10 s after it), then `frame` rows around them: rising\n"
			"   `interval_ms` or `latency_ms`, `dropped`/`repeated` stages, changing `gen`, `hold` codes.\n"
			"4. `log-excerpt.txt` for the matching log lines (local time; `created_local` = `t_ms` 0).\n"
			"5. `config/` for the settings in force.\n\n"
			"## Privacy\n\n"
			"No video pixels (unless the thumbnail switch was on), no minidump, no user or computer name, no paths\n"
			"outside the VP folder (shown as `<path>`; the VP folder is `<vp>`, Windows is `<windows>`), no\n"
			"e-mail addresses or URLs, and values of secret-like keys are `<redacted>`.\n";
		return md.str();
	}

	Result Write(const Inputs& inputsIn)
	{
		Result result;
		Inputs inputs = inputsIn;
		if (inputs.localTime.wYear == 0)
			GetLocalTime(&inputs.localTime);
		const RedactionContext redaction =
			inputs.redaction.keptRoots.empty() && inputs.redaction.words.empty() ?
			CurrentRedactionContext() : inputs.redaction;

		if (inputs.reportsDirectory.empty() || !CreateDirectoryTree(inputs.reportsDirectory))
		{
			result.error = "cannot create the reports folder";
			return result;
		}

		const std::string createdLocal = FormatLocalTime(inputs.localTime);
		std::vector<std::pair<std::string, std::string>> hashes;
		for (const BinaryFile& binary : inputs.binaries)
		{
			const std::string hash = Sha256File(binary.path);
			hashes.emplace_back(binary.name, hash.empty() ? std::string("missing") : hash);
		}

		auto countRows = [](const std::string& text)
		{
			return static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
		};
		std::string history = Redact(inputs.historyExe, redaction);
		const size_t exeRows = countRows(history);
		const std::string rendererHistory = Redact(inputs.historyRenderer, redaction);
		const size_t rendererRows = countRows(rendererHistory);
		history += rendererHistory;

		std::vector<std::pair<std::string, std::string>> files;
		files.emplace_back("state-history.jsonl", history);
		files.emplace_back("log-excerpt.txt", Redact(inputs.logExcerpt, redaction));
		files.emplace_back("crash-state.txt", Redact(inputs.crashStateLive.empty() ?
			std::string("(the crash-state recorder was not running)\n") : inputs.crashStateLive, redaction));
		if (!inputs.crashStateDirectory.empty())
		{
			std::string keptName;
			const std::string kept = NewestKeptCrashState(inputs.crashStateDirectory, &keptName);
			if (!kept.empty())
				files.emplace_back("crash-state-last.txt",
					"# " + keptName + "\n" + Redact(kept, redaction));
		}
		std::vector<std::pair<std::string, std::string>> settings;
		for (const auto& setting : inputs.liveSettings)
			settings.emplace_back(setting.first, Redact(setting.second, redaction));
		files.emplace_back("live-settings.json", LiveSettingsJson(settings));
		for (const std::wstring& config : inputs.configFiles)
		{
			const std::string text = ReadWholeFile(config, 4 * 1024 * 1024);
			if (text.empty())
				continue;
			std::string name = ToUtf8(FileNameOf(config));
			for (char& c : name)
				if (c == ':' || c == '\\' || c == '/')
					c = '_';
			files.emplace_back("config/" + name, Redact(text, redaction));
		}
		files.emplace_back("system.json", Redact(inputs.systemJson.empty() ? CollectSystemJson() : inputs.systemJson,
			redaction));
		if (inputs.thumbnailEnabled && !inputs.thumbnailPng.empty())
			files.emplace_back("thumbnail.png", std::string(inputs.thumbnailPng.begin(), inputs.thumbnailPng.end()));
		else if (inputs.thumbnailEnabled)
			files.emplace_back("thumbnail-missing.txt", "No thumbnail: " +
				(inputs.thumbnailNote.empty() ? std::string("no rendered frame was available") : inputs.thumbnailNote) + "\n");

		std::vector<std::string> contents;
		contents.push_back("README.md");
		contents.push_back("manifest.json");
		for (const auto& file : files)
			contents.push_back(file.first);

		const StoreZipWriter::DosTime time = StoreZipWriter::MakeDosTime(inputs.localTime.wYear,
			inputs.localTime.wMonth, inputs.localTime.wDay, inputs.localTime.wHour,
			inputs.localTime.wMinute, inputs.localTime.wSecond);
		StoreZipWriter zip(time);
		bool added = zip.Add("README.md", ReadmeText());
		added = zip.Add("manifest.json", ManifestJson(inputs, createdLocal, hashes, contents, exeRows, rendererRows)) && added;
		for (const auto& file : files)
			added = zip.Add(file.first, file.second) && added;
		if (!added)
		{
			result.error = "cannot add a file to the archive";
			return result;
		}
		const std::vector<unsigned char>& archive = zip.Finish();

		result.fileName = FileName(inputs.localTime);
		const std::wstring wideName(result.fileName.begin(), result.fileName.end());
		result.path = Join(inputs.reportsDirectory, wideName);
		if (!WriteFileAtomically(result.path, archive))
		{
			result.error = "cannot write the report file";
			return result;
		}
		result.ok = true;
		result.bytes = archive.size();
		result.entries = contents;
		result.removed = ApplyRetention(inputs.reportsDirectory, inputs.keep);
		return result;
	}
}
