#include "mft.h"
#include "scanner.h"
#include <winioctl.h>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <vector>

// MinGW lacks USN/MFT structures, define them manually
#ifndef FSCTL_ENUM_USN_DATA
#define FSCTL_ENUM_USN_DATA CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 44, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_READ_MFT_RECORD
#define FSCTL_READ_MFT_RECORD CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 45, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

typedef struct {
    DWORDLONG StartFileReferenceNumber;
    LONGLONG LowUsn;
    LONGLONG HighUsn;
} MFT_ENUM_DATA_V0, *PMFT_ENUM_DATA_V0;

typedef struct {
    DWORD RecordLength;
    USHORT MajorVersion;
    USHORT MinorVersion;
    DWORDLONG FileReferenceNumber;
    DWORDLONG ParentFileReferenceNumber;
    USN Usn;
    LARGE_INTEGER TimeStamp;
    DWORD Reason;
    DWORD SourceInfo;
    DWORD SecurityId;
    DWORD FileAttributes;
    USHORT FileNameLength;
    USHORT FileNameOffset;
    WCHAR FileName[1];
} USN_RECORD_V2, *PUSN_RECORD_V2;

// NTFS boot sector — just enough to find the MFT
#pragma pack(push, 1)
struct NtfsBootSector {
    char     jump[3];
    char     oem[8];         // "NTFS    "
    USHORT   bytes_per_sector;
    BYTE     sectors_per_cluster;
    BYTE     reserved1[7];
    BYTE     media_desc;
    BYTE     reserved2[2];
    USHORT   sectors_per_track;
    USHORT   num_heads;
    DWORD    hidden_sectors;
    BYTE     reserved3[4];
    BYTE     reserved4[4];
    LONGLONG total_sectors;
    LONGLONG mft_start_lcn;  // logical cluster number of $MFT
    LONGLONG mftmirr_lcn;
    CHAR     clusters_per_mft_record;  // if < 0, size = 2^(-val)
    BYTE     reserved5[3];
    BYTE     clusters_per_index;
    BYTE     reserved6[3];
    LONGLONG volume_serial;
};
#pragma pack(pop)

// MFT record header (first 48 bytes of each MFT record)
#pragma pack(push, 1)
struct MftRecordHeader {
    char magic[4];           // "FILE"
    USHORT update_seq_off;
    USHORT update_seq_count;
    ULONGLONG logfile_seq;
    USHORT seq_number;
    USHORT hard_links;
    USHORT attr_off;         // offset to first attribute
    USHORT flags;            // 0x01 = in use, 0x02 = directory
    DWORD rec_size;
    DWORD alloc_size;
    ULONGLONG base_ref;
    USHORT next_attr_id;
};
#pragma pack(pop)

// Fix ①: Read $DATA size with proper name offset handling
// Fix ⑤: Use logical data_size (0x30) instead of allocated_size (0x28)
static long long parse_mft_data_size(const BYTE* rec_buf, DWORD bytes_read) {
    const MftRecordHeader* hdr = (const MftRecordHeader*)rec_buf;
    if (bytes_read < 48) return -1;
    if (memcmp(hdr->magic, "FILE", 4) != 0) return -1;
    if (hdr->attr_off >= bytes_read) return -1;

    DWORD pos = hdr->attr_off;
    while (pos + 4 <= bytes_read) {
        DWORD type = *(const DWORD*)(rec_buf + pos);
        if (type == 0xFFFFFFFF) break;
        if (type == 0) break;

        if (pos + 8 > bytes_read) break;
        DWORD attr_len = *(const DWORD*)(rec_buf + pos + 4);
        if (attr_len < 24 || attr_len > 65536) break;

        if (type == 0x80) {  // $DATA
            if (pos + 9 > bytes_read) break;
            BYTE non_res = rec_buf[pos + 8];

            if (non_res) {
                // Non-resident: fields are at fixed offsets from attr start
                // allocated_size @ 0x28, data_size (logical) @ 0x30
                // Fix ⑤: use data_size instead of allocated_size
                if (pos + 0x38 <= bytes_read) {
                    LONGLONG data_size = *(const LONGLONG*)(rec_buf + pos + 0x30);
                    if (data_size > 0) return data_size;
                    // Fallback to allocated if data_size is 0
                    return *(const LONGLONG*)(rec_buf + pos + 0x28);
                }
            } else {
                // Resident: value_offset @ 0x10, value_size @ 0x14
                // Fix ①: value_offset is relative to attr start; use it
                if (pos + 0x18 <= bytes_read) {
                    DWORD value_size = *(const DWORD*)(rec_buf + pos + 0x14);
                    if (value_size > 0 && value_size <= 65536) return value_size;
                }
            }
            break;  // found first $DATA, stop (unnamed stream = main data)
        }

        pos += attr_len;
    }
    return -1;
}

// Extract extension from wide filename (fast ASCII path)
static std::string mft_get_ext(const wchar_t* name) {
    const wchar_t* dot = NULL;
    for (const wchar_t* p = name; *p; p++) {
        if (*p == L'.') dot = p;
    }
    if (!dot || dot == name) return "";

    bool all_ascii = true;
    int len = 0;
    for (const wchar_t* p = dot; *p; p++) {
        if (*p > 127) { all_ascii = false; break; }
        len++;
    }

    if (all_ascii && len < 16) {
        char buf[16];
        for (int i = 0; i < len; i++) {
            char c = (char)dot[i];
            buf[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        buf[len] = 0;
        return std::string(buf);
    }
    // Slow path: non-ASCII extension (rare)
    int wlen = (int)wcslen(dot);
    char buf[32];
    int n = WideCharToMultiByte(CP_UTF8, 0, dot, wlen, buf, sizeof(buf) - 1, NULL, NULL);
    if (n <= 0) return "";
    buf[n] = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32;
    }
    return std::string(buf);
}

struct MftFileEntry {
    DWORDLONG parent_ref;
    std::wstring name;
    long long size;
    DWORD file_attr;  // ② store attributes for reparse point filtering
    bool is_dir;
};

// ② Check if MFT entry should be skipped (reparse points + system dirs)
static bool mft_should_skip(const std::wstring& name, DWORD attr) {
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    if (!name.empty() && name[0] == L'.') return true;
    if (name == L"$RECYCLE.BIN" || name == L"System Volume Information" ||
        name == L"$WinREAgent" || name == L"Recovery" || name == L"PerfLogs") {
        return true;
    }
    return false;
}

// Binary search helper for sorted vector of entries
static inline auto find_entry(std::vector<std::pair<DWORDLONG, MftFileEntry>>& entries, DWORDLONG key)
    -> decltype(entries.begin()) {
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
        [](const std::pair<DWORDLONG, MftFileEntry>& p, DWORDLONG k) { return p.first < k; });
    if (it != entries.end() && it->first == key) return it;
    return entries.end();
}

bool get_dir_size_mft(const std::wstring& root, int top_n, MftScanResult& out) {
    // Extract volume root: "C:\" from "\\?\C:\..."
    std::wstring vol_root;
    size_t colon = root.find(L':');
    if (colon != std::wstring::npos) {
        vol_root = root.substr(0, colon + 1) + L"\\";
    } else {
        return false;
    }

    // Fix ⑥: Bulk read MFT — read boot sector to find MFT location
    std::wstring vol_dev = L"\\\\.\\" + vol_root.substr(0, 2);
    HANDLE vol = CreateFileW(vol_dev.c_str(), FILE_READ_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) return false;

    // Read boot sector (first 512 bytes)
    BYTE boot_buf[512];
    DWORD boot_read = 0;
    OVERLAPPED boot_ov = {};
    if (!ReadFile(vol, boot_buf, 512, &boot_read, &boot_ov) || boot_read < 512) {
        CloseHandle(vol);
        return false;
    }

    const NtfsBootSector* boot = (const NtfsBootSector*)boot_buf;
    if (memcmp(boot->oem, "NTFS", 4) != 0) {
        CloseHandle(vol);
        return false;
    }

    int bytes_per_sector = boot->bytes_per_sector;
    int sectors_per_cluster = boot->sectors_per_cluster;
    int cluster_size = bytes_per_sector * sectors_per_cluster;
    LONGLONG mft_start_byte = boot->mft_start_lcn * cluster_size;

    // MFT record size: if clusters_per_mft_record < 0, size = 2^(-val)
    int mft_rec_size;
    if (boot->clusters_per_mft_record < 0) {
        mft_rec_size = 1 << (-boot->clusters_per_mft_record);
    } else {
        mft_rec_size = boot->clusters_per_mft_record * cluster_size;
    }
    if (mft_rec_size < 128 || mft_rec_size > 65536) mft_rec_size = 1024;

    // Get actual MFT size via FSCTL_GET_NTFS_VOLUME_DATA
    LONGLONG mft_data_length = 0;
    {
        NTFS_VOLUME_DATA_BUFFER vol_data = {};
        DWORD vol_ret = 0;
        if (DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                            &vol_data, sizeof(vol_data), &vol_ret, NULL) && vol_ret > 0) {
            mft_data_length = vol_data.MftValidDataLength.QuadPart;
            // Also use the record size from volume data if available
            if (vol_data.BytesPerFileRecordSegment >= 128 && vol_data.BytesPerFileRecordSegment <= 65536) {
                mft_rec_size = vol_data.BytesPerFileRecordSegment;
            }
        }
    }
    // Fallback: if we couldn't get MFT size, cap at 2GB (enough for most volumes)
    if (mft_data_length <= 0) {
        mft_data_length = 2LL * 1024 * 1024 * 1024;
    }

    // Phase 1: Enumerate all MFT entries via FSCTL_ENUM_USN_DATA
    std::vector<std::pair<DWORDLONG, MftFileEntry>> entries;

    HANDLE vol_enum = CreateFileW(vol_dev.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, 0, NULL);
    if (vol_enum == INVALID_HANDLE_VALUE) {
        CloseHandle(vol);
        return false;
    }

    MFT_ENUM_DATA_V0 med = {};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;

    const DWORD usn_buf_size = 65536;
    BYTE* usn_buf = new BYTE[usn_buf_size];
    DWORD bytes_ret = 0;

    while (!g_interrupted.load()) {
        if (!DeviceIoControl(vol_enum, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                             usn_buf, usn_buf_size, &bytes_ret, NULL)) {
            break;
        }
        if (bytes_ret <= sizeof(USN)) break;

        DWORD offset = sizeof(USN);
        while (offset + sizeof(USN_RECORD_V2) <= bytes_ret) {
            USN_RECORD_V2* rec = (USN_RECORD_V2*)(usn_buf + offset);
            if (rec->MajorVersion != 2 || rec->RecordLength < sizeof(USN_RECORD_V2) || rec->RecordLength > 65536) break;

            // Mask off sequence number (bits 48-63), keep only MFT record index
            DWORDLONG file_ref = rec->FileReferenceNumber & 0x0000FFFFFFFFFFFF;

            MftFileEntry entry;
            entry.parent_ref = rec->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFF;
            entry.name = std::wstring(rec->FileName, rec->FileNameLength / sizeof(WCHAR));
            entry.size = 0;
            entry.file_attr = rec->FileAttributes;
            entry.is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            entries.push_back(std::make_pair(file_ref, entry));
            offset += rec->RecordLength;
        }

        med.StartFileReferenceNumber = *(DWORDLONG*)usn_buf;
    }
    delete[] usn_buf;
    CloseHandle(vol_enum);

    if (entries.empty()) {
        CloseHandle(vol);
        return false;
    }

    // Sort entries by key for binary search lookups
    std::sort(entries.begin(), entries.end(),
        [](const std::pair<DWORDLONG, MftFileEntry>& a, const std::pair<DWORDLONG, MftFileEntry>& b) {
            return a.first < b.first;
        });

    // Fix ⑥: Phase 2 — Read file sizes from MFT records
    // Strategy: bulk read if MFT <= 512MB (HDD friendly), per-record otherwise (SSD fast enough)
    const LONGLONG BULK_THRESHOLD = 512LL * 1024 * 1024;  // 512MB

    if (mft_data_length <= BULK_THRESHOLD) {
        // Bulk read: sequential 1MB chunks (fast on HDD)
        const DWORD chunk_size = 1024 * 1024;
        BYTE* chunk_buf = new BYTE[chunk_size];
        LONGLONG mft_offset = mft_start_byte;
        LONGLONG mft_end = mft_start_byte + mft_data_length;

        while (mft_offset < mft_end && !g_interrupted.load()) {
            DWORD to_read = chunk_size;
            if (mft_offset + to_read > mft_end) to_read = (DWORD)(mft_end - mft_offset);

            DWORD bytes_read = 0;
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)(mft_offset & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)(mft_offset >> 32);

            if (!ReadFile(vol, chunk_buf, to_read, &bytes_read, &ov) || bytes_read < (DWORD)mft_rec_size) break;

            for (DWORD off = 0; off + mft_rec_size <= bytes_read; off += mft_rec_size) {
                const MftRecordHeader* hdr = (const MftRecordHeader*)(chunk_buf + off);
                if (memcmp(hdr->magic, "FILE", 4) != 0) continue;

                DWORDLONG file_ref = (DWORDLONG)(mft_offset + off - mft_start_byte) / mft_rec_size;
                auto it = find_entry(entries, file_ref);
                if (it == entries.end() || it->second.is_dir) continue;

                long long sz = parse_mft_data_size(chunk_buf + off, mft_rec_size);
                if (sz > 0) it->second.size = sz;
            }

            mft_offset += bytes_read;
        }
        delete[] chunk_buf;
    } else {
        // Per-record: random 1KB reads (fast on SSD with OS cache)
        BYTE* rec_buf = new BYTE[mft_rec_size];
        size_t count = 0;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (g_interrupted.load()) break;
            if (it->second.is_dir) continue;

            LONGLONG offset = (LONGLONG)it->first * mft_rec_size;
            DWORD bytes_read = 0;
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)(offset >> 32);

            if (ReadFile(vol, rec_buf, mft_rec_size, &bytes_read, &ov) && bytes_read >= 48) {
                long long sz = parse_mft_data_size(rec_buf, bytes_read);
                if (sz > 0) it->second.size = sz;
            }

            count++;
            if (count % 10000 == 0) {
                std::cerr << "\r  MFT: read " << count << " records..." << std::flush;
            }
        }
        delete[] rec_buf;
    }

    CloseHandle(vol);

    // Phase 3: Build directory tree and propagate sizes
    std::vector<DWORDLONG> dir_refs;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->second.is_dir) dir_refs.push_back(it->first);
    }

    std::unordered_map<DWORDLONG, int> dir_idx;
    dir_idx.reserve(dir_refs.size());
    for (size_t i = 0; i < dir_refs.size(); i++) {
        dir_idx[dir_refs[i]] = (int)i;
    }

    int nd = (int)dir_refs.size();
    std::vector<long long> dir_sizes(nd, 0);
    // Fix ⑧: dir_files = recursive file count (all descendants)
    std::vector<unsigned long long> dir_files(nd, 0);
    // Fix ⑦: dir_subdirs = actual subdirectory count (not file count)
    std::vector<unsigned long long> dir_subdirs(nd, 0);
    std::vector<int> dir_parent(nd, -1);

    // Fix ⑨: Build adjacency list for O(n) BFS instead of O(n²)
    std::vector<std::vector<int>> children(nd);
    for (int i = 0; i < nd; i++) {
        auto eit = find_entry(entries, dir_refs[i]);
        if (eit == entries.end()) continue;
        DWORDLONG parent = eit->second.parent_ref;
        std::unordered_map<DWORDLONG, int>::iterator pit = dir_idx.find(parent);
        if (pit != dir_idx.end() && pit->second != i) {
            dir_parent[i] = pit->second;
            children[pit->second].push_back(i);
        }
    }

    // Propagate: each file adds size to all ancestor directories
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->second.is_dir || it->second.size <= 0) continue;

        // ② Skip reparse points
        if (it->second.file_attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::unordered_map<DWORDLONG, int>::iterator pit = dir_idx.find(it->second.parent_ref);
        if (pit == dir_idx.end()) continue;
        int parent = pit->second;

        // Fix ⑧: dir_files counts recursively — add 1 to direct parent and all ancestors
        int p = parent;
        while (p >= 0 && p < nd) {
            dir_files[p]++;       // ⑧ recursive file count
            dir_sizes[p] += it->second.size;
            p = dir_parent[p];
        }
    }

    // Phase 4: Build result for directories under the scan root
    std::wstring norm_root = root;
    if (norm_root.size() >= 2 && norm_root[norm_root.size() - 1] == L'\\') {
        norm_root.resize(norm_root.size() - 1);
    }

    // Build path map for directories to find scan root
    std::unordered_map<DWORDLONG, std::wstring> path_cache;
    for (int i = 0; i < nd; i++) {
        if (path_cache.count(dir_refs[i])) continue;

        std::vector<DWORDLONG> chain;
        DWORDLONG cur = dir_refs[i];

        while (chain.size() < 200) {
            chain.push_back(cur);
            auto ei = find_entry(entries, cur);
            if (ei == entries.end()) break;
            if (ei->second.parent_ref == cur) break;
            std::unordered_map<DWORDLONG, int>::iterator pi = dir_idx.find(ei->second.parent_ref);
            if (pi == dir_idx.end()) break;
            if (path_cache.count(ei->second.parent_ref)) break;
            cur = ei->second.parent_ref;
        }

        std::wstring path;
        for (int j = (int)chain.size() - 1; j >= 0; j--) {
            auto ei = find_entry(entries, chain[j]);
            if (ei == entries.end()) break;
            if (path.empty()) {
                path = vol_root + ei->second.name;
            } else {
                path = path + L"\\" + ei->second.name;
            }
            path_cache[chain[j]] = path;
        }
    }

    // Find scan root directory index
    int root_idx = -1;
    for (int i = 0; i < nd; i++) {
        std::unordered_map<DWORDLONG, std::wstring>::iterator pi = path_cache.find(dir_refs[i]);
        if (pi != path_cache.end()) {
            std::wstring p = pi->second;
            if (!p.empty() && p.back() == L'\\') p.resize(p.size() - 1);
            if (_wcsicmp(p.c_str(), norm_root.c_str()) == 0) {
                root_idx = i;
                break;
            }
        }
    }

    if (root_idx < 0) return false;

    // Fix ⑦: Count actual subdirectories using children adjacency list
    for (int i = 0; i < nd; i++) {
        dir_subdirs[i] = children[i].size();
    }
    // BFS from root_idx, propagate in reverse BFS order (leaves first)
    {
        std::vector<int> bfs_order;
        std::vector<int> q;
        q.push_back(root_idx);
        size_t qi = 0;
        while (qi < q.size()) {
            int cur = q[qi++];
            bfs_order.push_back(cur);
            for (size_t ci = 0; ci < children[cur].size(); ci++) {
                q.push_back(children[cur][ci]);
            }
        }
        for (int i = (int)bfs_order.size() - 1; i >= 0; i--) {
            int idx = bfs_order[i];
            if (dir_parent[idx] >= 0) {
                dir_subdirs[dir_parent[idx]] += dir_subdirs[idx];
            }
        }
    }

    out.scan = ScanResult();
    out.folders.clear();

    // ② Collect immediate children, skipping reparse points and system dirs
    long long sum_size = 0;
    unsigned long long sum_files = 0;

    for (size_t ci = 0; ci < children[root_idx].size(); ci++) {
        int idx = children[root_idx][ci];
        auto eit = find_entry(entries, dir_refs[idx]);
        if (eit == entries.end()) continue;
        const std::wstring& name = eit->second.name;

        // ② Skip reparse points and system dirs
        if (mft_should_skip(name, eit->second.file_attr)) continue;

        DirEntry de;
        de.name = name;
        de.size.store(dir_sizes[idx], std::memory_order_relaxed);
        de.files.store(dir_files[idx], std::memory_order_relaxed);
        de.dirs.store(dir_subdirs[idx], std::memory_order_relaxed);
        out.folders.push_back(de);

        sum_files += dir_files[idx];
        long long abs_size = dir_sizes[idx];
        if (abs_size < 0) abs_size = -abs_size;
        sum_size += abs_size;
    }

    out.scan.stats.dirs = out.folders.size() + 1;
    out.scan.stats.files = sum_files;
    out.scan.stats.size = sum_size;

    // Fix ⑨: BFS using adjacency list (O(n) instead of O(n²))
    std::vector<bool> under_root(nd, false);
    under_root[root_idx] = true;
    {
        std::vector<int> queue;
        queue.push_back(root_idx);
        size_t qi = 0;
        while (qi < queue.size()) {
            int cur = queue[qi++];
            for (size_t ci = 0; ci < children[cur].size(); ci++) {
                int child = children[cur][ci];
                if (!under_root[child]) {
                    under_root[child] = true;
                    queue.push_back(child);
                }
            }
        }
    }

    // Aggregate file extensions and find top files
    std::unordered_map<std::string, long long> ext_sizes;
    std::unordered_map<std::string, unsigned long long> ext_counts;

    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->second.is_dir || it->second.size <= 0) continue;
        if (it->second.file_attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // ②

        std::unordered_map<DWORDLONG, int>::iterator pit = dir_idx.find(it->second.parent_ref);
        if (pit == dir_idx.end()) continue;
        if (!under_root[pit->second]) continue;

        std::string ext = mft_get_ext(it->second.name.c_str());
        if (!ext.empty()) {
            ext_sizes[ext] += it->second.size;
            ext_counts[ext]++;
        }

        // Top files with insertion sort
        if (top_n > 0) {
            std::wstring fpath;
            std::unordered_map<DWORDLONG, std::wstring>::iterator dpath = path_cache.find(it->second.parent_ref);
            if (dpath != path_cache.end()) {
                fpath = dpath->second + L"\\" + it->second.name;
            } else {
                fpath = it->second.name;
            }

            if ((int)out.scan.top_files.size() < top_n) {
                FileInfo fi;
                fi.path = fpath;
                fi.size = it->second.size;
                fi.extension = ext;
                out.scan.top_files.push_back(fi);
                if ((int)out.scan.top_files.size() == top_n) {
                    std::sort(out.scan.top_files.begin(), out.scan.top_files.end(),
                              [](const FileInfo& a, const FileInfo& b) { return a.size > b.size; });
                }
            } else if (it->second.size > out.scan.top_files.back().size) {
                out.scan.top_files.back().path = fpath;
                out.scan.top_files.back().size = it->second.size;
                out.scan.top_files.back().extension = ext;
                for (int j = (int)out.scan.top_files.size() - 1; j > 0; j--) {
                    if (out.scan.top_files[j].size > out.scan.top_files[j-1].size)
                        std::swap(out.scan.top_files[j], out.scan.top_files[j-1]);
                    else break;
                }
            }
        }
    }

    out.scan.ext_sizes = std::move(ext_sizes);
    out.scan.ext_counts = std::move(ext_counts);

    return true;
}

bool is_ntfs_volume(const std::wstring& drive_root) {
    std::wstring root = drive_root;
    if (root.size() >= 2 && root[1] == L':') {
        if (root.size() == 2) root += L"\\";
        if (root.size() > 3) root = root.substr(0, 3);
    }

    wchar_t fs_name[MAX_PATH + 1] = {};
    if (GetVolumeInformationW(root.c_str(), NULL, 0, NULL, NULL, NULL, fs_name, MAX_PATH + 1)) {
        return _wcsicmp(fs_name, L"NTFS") == 0;
    }
    return false;
}
