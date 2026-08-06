#include <external/commonItems/external/zip/src/zip.h>
#include "gtest/gtest.h"
#include "src/ck3_world/SaveRepack.h"
#include <cstdlib>

namespace
{
// A minimal kind-03 save: header, a binary metadata block, then a zip holding the gamestate.
std::string makeSplitBinarySave(const std::string& meta, const std::string& gamestate)
{
	char header[25];
	snprintf(header, sizeof(header), "SAV0103deadbeef%08zx\n", meta.size());

	auto* zip = zip_stream_open(nullptr, 0, 0, 'w');
	zip_entry_open(zip, "gamestate");
	zip_entry_write(zip, gamestate.data(), gamestate.size());
	zip_entry_close(zip);
	char* buffer = nullptr;
	size_t bufferSize = 0;
	zip_stream_copy(zip, reinterpret_cast<void**>(&buffer), &bufferSize);
	zip_stream_close(zip);

	auto save = std::string(header) + meta + std::string(buffer, bufferSize);
	free(buffer);
	return save;
}
} // namespace

TEST(CK3World_SaveRepackTests, compressedAutosavesUnpackIntoThePlainBinaryLayout)
{
	// The zipped gamestate is the complete save document - it opens with the metadata block again.
	const std::string meta = "\x01\x02\x03meta";
	const std::string gamestate = meta + "\x04\x05the actual game";
	const auto repacked = CK3::repackSplitBinarySave(makeSplitBinarySave(meta, gamestate));

	EXPECT_EQ("SAV0101deadbeef00000007\n" + gamestate, repacked);
}

TEST(CK3World_SaveRepackTests, otherSaveKindsPassThroughUntouched)
{
	const std::string textual = "SAV0102deadbeef00000004\nmetaPK\x03\x04whatever";
	EXPECT_EQ(textual, CK3::repackSplitBinarySave(textual));

	const std::string plainBinary = "SAV0101deadbeef00000004\nmeta binary gamestate";
	EXPECT_EQ(plainBinary, CK3::repackSplitBinarySave(plainBinary));

	EXPECT_EQ("not a save", CK3::repackSplitBinarySave("not a save"));
	EXPECT_TRUE(CK3::repackSplitBinarySave("").empty());
}

TEST(CK3World_SaveRepackTests, aBrokenArchivePassesThroughForTheMelterToReject)
{
	// Kind 03 header but garbage where the zip should be - repacking backs off rather than throwing.
	const std::string broken = "SAV0103deadbeef00000004\nmeta this is not a zip archive";
	EXPECT_EQ(broken, CK3::repackSplitBinarySave(broken));
}
