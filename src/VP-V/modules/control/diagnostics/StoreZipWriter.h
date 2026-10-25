#pragma once

// A minimal store-only (uncompressed) ZIP writer for the report bundle
// (StateReport.h). Files are added from memory; Finish() returns the whole
// archive. No ZIP64: entries and the archive must stay below 4 GiB (a report
// is a few MB). Names are UTF-8 with '/' separators (general-purpose flag bit
// 11 set). Every reader handles method 0; Windows Explorer opens it directly.
//
// C++14, header-only, no Windows dependency.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class StoreZipWriter
{
public:
	struct DosTime
	{
		uint16_t time = 0;   // hours<<11 | minutes<<5 | seconds/2
		uint16_t date = 0x21; // (year-1980)<<9 | month<<5 | day (1980-01-01)
	};

	static DosTime MakeDosTime(int year, int month, int day, int hour, int minute, int second)
	{
		DosTime result;
		if (year < 1980) year = 1980;
		if (year > 2107) year = 2107;
		result.date = static_cast<uint16_t>(((year - 1980) << 9) | ((month & 0x0f) << 5) | (day & 0x1f));
		result.time = static_cast<uint16_t>(((hour & 0x1f) << 11) | ((minute & 0x3f) << 5) | ((second / 2) & 0x1f));
		return result;
	}

	static uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0)
	{
		static const uint32_t* table = []
		{
			static uint32_t values[256];
			for (uint32_t index = 0; index < 256; ++index)
			{
				uint32_t value = index;
				for (int bit = 0; bit < 8; ++bit)
					value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
				values[index] = value;
			}
			return values;
		}();
		crc = ~crc;
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (size_t index = 0; index < size; ++index)
			crc = table[(crc ^ bytes[index]) & 0xff] ^ (crc >> 8);
		return ~crc;
	}

	explicit StoreZipWriter(DosTime time = DosTime()) : time_(time) {}

	// False (and nothing added) for an empty or duplicate name, a name with
	// '\\', a leading '/', a ".." component, or once Finish() has run.
	bool Add(const std::string& name, const void* data, size_t size)
	{
		if (finished_ || !ValidName(name) || size >= 0xFFFFFFFFull ||
			archive_.size() + size + name.size() + 64 >= 0xFFFFFFFFull)
			return false;
		for (const Entry& entry : entries_)
			if (entry.name == name)
				return false;
		Entry entry;
		entry.name = name;
		entry.crc = Crc32(data, size);
		entry.size = static_cast<uint32_t>(size);
		entry.offset = static_cast<uint32_t>(archive_.size());

		Put32(0x04034b50);           // local file header
		Put16(10);                   // version needed: 1.0 (stored)
		Put16(kUtf8Flag);
		Put16(0);                    // method: stored
		Put16(time_.time);
		Put16(time_.date);
		Put32(entry.crc);
		Put32(entry.size);           // compressed
		Put32(entry.size);           // uncompressed
		Put16(static_cast<uint16_t>(name.size()));
		Put16(0);                    // extra length
		archive_.insert(archive_.end(), name.begin(), name.end());
		if (size)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			archive_.insert(archive_.end(), bytes, bytes + size);
		}
		entries_.push_back(entry);
		return true;
	}

	bool Add(const std::string& name, const std::string& text)
	{
		return Add(name, text.data(), text.size());
	}

	size_t EntryCount() const { return entries_.size(); }

	// Appends the central directory and end record; returns the archive.
	const std::vector<unsigned char>& Finish()
	{
		if (finished_)
			return archive_;
		finished_ = true;
		const uint32_t directoryOffset = static_cast<uint32_t>(archive_.size());
		for (const Entry& entry : entries_)
		{
			Put32(0x02014b50);       // central directory header
			Put16(0x0314);           // made by: Unix (3), spec 2.0 - keeps attributes readable
			Put16(10);
			Put16(kUtf8Flag);
			Put16(0);
			Put16(time_.time);
			Put16(time_.date);
			Put32(entry.crc);
			Put32(entry.size);
			Put32(entry.size);
			Put16(static_cast<uint16_t>(entry.name.size()));
			Put16(0);                // extra
			Put16(0);                // comment
			Put16(0);                // disk
			Put16(0);                // internal attributes
			Put32(0100644u << 16);   // external: regular file rw-r--r--
			Put32(entry.offset);
			archive_.insert(archive_.end(), entry.name.begin(), entry.name.end());
		}
		const uint32_t directorySize = static_cast<uint32_t>(archive_.size()) - directoryOffset;
		Put32(0x06054b50);           // end of central directory
		Put16(0);
		Put16(0);
		Put16(static_cast<uint16_t>(entries_.size()));
		Put16(static_cast<uint16_t>(entries_.size()));
		Put32(directorySize);
		Put32(directoryOffset);
		Put16(0);                    // comment length
		return archive_;
	}

	// ---- Reading (tests and self-check) ----------------------------------------

	struct ReadEntry
	{
		std::string name;
		std::string data;
		uint32_t crc = 0;
	};

	// Parses a stored archive through its central directory, checking every
	// local header, size and CRC. False on any inconsistency.
	static bool Read(const std::vector<unsigned char>& zip, std::vector<ReadEntry>& out, std::string* error = nullptr)
	{
		out.clear();
		auto fail = [error](const char* message) { if (error) *error = message; return false; };
		if (zip.size() < 22)
			return fail("too small");
		const size_t end = zip.size() - 22;   // no archive comment is written
		if (Get32(zip, end) != 0x06054b50)
			return fail("no end record");
		const uint16_t count = Get16(zip, end + 10);
		const uint32_t directorySize = Get32(zip, end + 12);
		const uint32_t directoryOffset = Get32(zip, end + 16);
		if (static_cast<uint64_t>(directoryOffset) + directorySize != end)
			return fail("directory bounds");
		size_t position = directoryOffset;
		for (uint16_t index = 0; index < count; ++index)
		{
			if (position + 46 > end || Get32(zip, position) != 0x02014b50)
				return fail("directory header");
			const uint16_t method = Get16(zip, position + 10);
			const uint32_t crc = Get32(zip, position + 16);
			const uint32_t compressed = Get32(zip, position + 20);
			const uint32_t size = Get32(zip, position + 24);
			const uint16_t nameLength = Get16(zip, position + 28);
			const uint16_t extraLength = Get16(zip, position + 30);
			const uint16_t commentLength = Get16(zip, position + 32);
			const uint32_t offset = Get32(zip, position + 42);
			if (method != 0 || compressed != size)
				return fail("not stored");
			if (position + 46 + nameLength > end)
				return fail("name bounds");
			ReadEntry entry;
			entry.name.assign(reinterpret_cast<const char*>(&zip[position + 46]), nameLength);
			entry.crc = crc;
			if (static_cast<uint64_t>(offset) + 30 > directoryOffset || Get32(zip, offset) != 0x04034b50)
				return fail("local header");
			const uint16_t localName = Get16(zip, offset + 26);
			const uint16_t localExtra = Get16(zip, offset + 28);
			if (Get32(zip, offset + 14) != crc || Get32(zip, offset + 22) != size || localName != nameLength)
				return fail("local header mismatch");
			const uint64_t dataStart = static_cast<uint64_t>(offset) + 30 + localName + localExtra;
			if (dataStart + size > directoryOffset)
				return fail("data bounds");
			if (std::memcmp(&zip[offset + 30], entry.name.data(), nameLength) != 0)
				return fail("local name mismatch");
			entry.data.assign(reinterpret_cast<const char*>(zip.data() + dataStart), size);
			if (Crc32(entry.data.data(), entry.data.size()) != crc)
				return fail("crc");
			out.push_back(entry);
			position += 46 + nameLength + extraLength + commentLength;
		}
		return true;
	}

private:
	static constexpr uint16_t kUtf8Flag = 0x0800;

	struct Entry
	{
		std::string name;
		uint32_t crc = 0;
		uint32_t size = 0;
		uint32_t offset = 0;
	};

	static bool ValidName(const std::string& name)
	{
		if (name.empty() || name.size() > 0xFFFF || name[0] == '/' ||
			name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
			return false;
		size_t start = 0;
		while (start <= name.size())
		{
			const size_t slash = name.find('/', start);
			const std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
			if (part.empty() || part == "." || part == "..")
				return false;
			if (slash == std::string::npos)
				break;
			start = slash + 1;
		}
		return true;
	}

	void Put16(uint16_t value)
	{
		archive_.push_back(static_cast<unsigned char>(value & 0xff));
		archive_.push_back(static_cast<unsigned char>(value >> 8));
	}
	void Put32(uint32_t value)
	{
		Put16(static_cast<uint16_t>(value & 0xffff));
		Put16(static_cast<uint16_t>(value >> 16));
	}
	static uint16_t Get16(const std::vector<unsigned char>& data, size_t offset)
	{
		if (offset + 2 > data.size()) return 0;
		return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
	}
	static uint32_t Get32(const std::vector<unsigned char>& data, size_t offset)
	{
		if (offset + 4 > data.size()) return 0;
		return static_cast<uint32_t>(Get16(data, offset)) |
			(static_cast<uint32_t>(Get16(data, offset + 2)) << 16);
	}

	DosTime time_;
	std::vector<unsigned char> archive_;
	std::vector<Entry> entries_;
	bool finished_ = false;
};
