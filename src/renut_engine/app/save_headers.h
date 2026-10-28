#pragma once
// Saves brought over from Xenia.
//
// Put a Xenia save's title folder (4D5307ED) in reNut's save location (the
// user data folder's profile folder) by hand. Some Xenia builds write a
// content item's header (<title>\Headers\<type>\<name>.header) as the whole
// Xbox 360 package header (CON / LIVE / PIRS, 40 KB) where reNut reads
// XCONTENT_AGGREGATE_DATA (0x148 bytes): the game then finds no such save. At
// startup every such header in the user data folder is rewritten as the
// aggregate data, from the package header's content type, title and display
// name and the header's own file name.

#include <filesystem>

namespace renut::saves {

// Call before the game starts, with the resolved user data folder.
void FixPackageHeaders(const std::filesystem::path &user_data_root);

} // namespace renut::saves
