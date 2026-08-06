#include "SaveRepack.h"
#include "Log.h"
#include <external/commonItems/external/zip/src/zip.h>
#include <charconv>
#include <cstdlib>
#include <system_error>

std::string CK3::repackSplitBinarySave(const std::string& raw)
{
	// The header line: "SAV" + header version (2 chars) + kind (2 chars) + two 8-hex fields,
	// the second being the size of the metadata block that sits between header and zip.
	constexpr size_t headerSize = 24;
	if (raw.size() <= headerSize || !raw.starts_with("SAV") || raw.compare(5, 2, "03") != 0 || raw[headerSize - 1] != '\n')
		return raw;

	size_t metaSize = 0;
	const auto* metaField = raw.data() + 15;
	if (const auto [end, error] = std::from_chars(metaField, metaField + 8, metaSize, 16); error != std::errc() || headerSize + metaSize >= raw.size())
	{
		Log(LogLevel::Warning) << "Save has a compressed-autosave header the converter can't read; handing it to the melter as-is.";
		return raw;
	}

	auto* zip = zip_stream_open(raw.data() + headerSize + metaSize, raw.size() - headerSize - metaSize, 0, 'r');
	if (!zip)
	{
		Log(LogLevel::Warning) << "Compressed autosave carries no readable archive where one belongs; handing it to the melter as-is.";
		return raw;
	}
	std::string gamestate;
	if (zip_entry_open(zip, "gamestate") == 0)
	{
		void* buffer = nullptr;
		size_t bufferSize = 0;
		if (zip_entry_read(zip, &buffer, &bufferSize) >= 0 && buffer)
			gamestate.assign(static_cast<char*>(buffer), bufferSize);
		free(buffer);
		zip_entry_close(zip);
	}
	zip_stream_close(zip);
	if (gamestate.empty())
	{
		Log(LogLevel::Warning) << "Compressed autosave's archive holds no gamestate; handing it to the melter as-is.";
		return raw;
	}

	auto header = raw.substr(0, headerSize);
	header[5] = '0';
	header[6] = '1';
	Log(LogLevel::Info) << "Save is a compressed autosave; unpacked into the plain binary layout for melting.";
	return header + gamestate;
}
