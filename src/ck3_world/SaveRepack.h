#ifndef CK3_SAVE_REPACK_H
#define CK3_SAVE_REPACK_H
#include <string>

namespace CK3
{
// CK3 writes exit autosaves in a layout the bundled melter rejects as an invalid header:
// header kind 03, a binary metadata block, then the whole gamestate in a zip. That zipped
// gamestate is a complete uncompressed-binary save document (it opens with the same metadata
// block), so unpacking it and relabeling the header as the long-supported uncompressed-binary
// kind 01 is a lossless translation into a save the melter has always understood.
// Anything that isn't a kind-03 save passes through untouched.
std::string repackSplitBinarySave(const std::string& raw);
} // namespace CK3

#endif // CK3_SAVE_REPACK_H
