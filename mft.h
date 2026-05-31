#ifndef SPACEMAP_MFT_H
#define SPACEMAP_MFT_H

#include "types.h"
#include <string>
#include <vector>

// Full MFT scan result including per-subdirectory breakdown
struct MftScanResult {
    ScanResult scan;               // totals, ext_sizes, top_files
    std::vector<DirEntry> folders; // immediate children of scan root with sizes
};

// MFT fast-path scan for NTFS volumes.
// Returns true on success, false on failure (caller should fall back to get_dir_size).
bool get_dir_size_mft(const std::wstring& root, int top_n, MftScanResult& out);

// Check if a volume is NTFS by drive root path (e.g. L"C:\\")
bool is_ntfs_volume(const std::wstring& drive_root);

#endif
