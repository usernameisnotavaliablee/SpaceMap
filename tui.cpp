#include "tui.h"
#include "scanner.h"
#include "output.h"
#include "stats.h"
#include "mft.h"
#include "tips.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

static HANDLE g_tui_buffer = INVALID_HANDLE_VALUE;
static HANDLE g_orig_buffer = INVALID_HANDLE_VALUE;
static HANDLE g_input_handle = INVALID_HANDLE_VALUE;

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static bool g_vt_supported = false;

// UTF-8 -> wstring（用于把 tips.cpp 的 UTF-8 贴士显示到宽字符 TUI）
static std::wstring tui_u8_to_w(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// 按显示宽度(CJK算2列)把宽字符串截断到 max_cols，超出以 … 结尾
static std::wstring tui_truncate_w(const std::wstring& s, int max_cols) {
    if (max_cols <= 0) return std::wstring();
    std::wstring out;
    int cols = 0;
    for (wchar_t ch : s) {
        int cw = (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3000 && ch <= 0x30FF) ||
                 (ch >= 0xFF00 && ch <= 0xFFEF) || (ch >= 0xAC00 && ch <= 0xD7AF) ? 2 : 1;
        if (cols + cw > max_cols) { out += L'…'; break; }
        out += ch;
        cols += cw;
    }
    return out;
}


static void tui_init() {
    g_orig_buffer = GetStdHandle(STD_OUTPUT_HANDLE);
    g_input_handle = GetStdHandle(STD_INPUT_HANDLE);
    g_tui_buffer = CreateConsoleScreenBuffer(
        GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    SetConsoleActiveScreenBuffer(g_tui_buffer);
    CONSOLE_CURSOR_INFO ci = {1, FALSE};
    SetConsoleCursorInfo(g_tui_buffer, &ci);
    // Detect VT (virtual terminal) support for 24-bit color and Unicode rendering
    DWORD mode;
    g_vt_supported = GetConsoleMode(g_tui_buffer, &mode) &&
                     SetConsoleMode(g_tui_buffer, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // Clear screen
    DWORD written;
    COORD origin = {0, 0};
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_tui_buffer, &info)) {
        FillConsoleOutputCharacterW(g_tui_buffer, L' ', info.dwSize.X * info.dwSize.Y, origin, &written);
        FillConsoleOutputAttribute(g_tui_buffer, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
                                   info.dwSize.X * info.dwSize.Y, origin, &written);
    }
}

static void tui_cleanup() {
    if (g_orig_buffer != INVALID_HANDLE_VALUE) {
        SetConsoleActiveScreenBuffer(g_orig_buffer);
    }
    if (g_tui_buffer != INVALID_HANDLE_VALUE) {
        CloseHandle(g_tui_buffer);
        g_tui_buffer = INVALID_HANDLE_VALUE;
    }
}

static void get_console_size(int& width, int& height) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_tui_buffer, &info)) {
        width = info.srWindow.Right - info.srWindow.Left + 1;
        height = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else {
        width = 80;
        height = 25;
    }
}

static void write_str(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
                       int x, int y, int w, const std::wstring& s, WORD attr) {
    for (int i = 0; i < (int)s.size() && x + i < w; i++) {
        int idx = y * w + x + i;
        if (idx >= 0 && idx < (int)chars.size()) {
            chars[idx] = s[i];
            attrs[idx] = attr;
        }
    }
}

static void fill_row(std::vector<wchar_t>& chars, std::vector<WORD>& attrs,
                      int y, int w, wchar_t ch, WORD attr) {
    for (int x = 0; x < w; x++) {
        int idx = y * w + x;
        chars[idx] = ch;
        attrs[idx] = attr;
    }
}

static std::wstring tui_get_drive_root(const std::wstring& path) {
    std::wstring p = path;
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') {
        p = p.substr(4);
    }
    if (p.size() >= 2 && p[1] == L':') {
        return p.substr(0, 2) + L"\\";
    }
    return L"";
}

static void populate_entries(TuiState& state, bool load_persistent_cache = false) {
    std::vector<DirEntry> new_entries;
    // Load persistent cache only when explicitly requested (initial entry)
    if (load_persistent_cache) {
        std::wstring cache_path = get_cache_path();
        if (is_cache_valid(cache_path, 86400)) {
            std::vector<DirEntry> cached_folders;
            std::vector<FileTypeStats> cached_types;
            std::vector<FileInfo> cached_top_files;
            long long cached_total_size = 0;
            unsigned long long cached_total_files = 0, cached_total_dirs = 0;
            unsigned long long cached_total_skipped = 0, cached_total_errors = 0;
            double cached_elapsed = 0;
            std::wstring cached_path;
            if (load_cache(cache_path, cached_folders, cached_total_size,
                           cached_total_files, cached_total_dirs,
                           cached_total_skipped, cached_total_errors,
                           cached_types, cached_top_files,
                           cached_elapsed, cached_path)) {
                // Only use cache if it matches the current path
                if (cached_path == state.current_path) {
                    std::lock_guard<std::mutex> lock(state.mtx);
                    for (size_t ci = 0; ci < cached_folders.size(); ci++) {
                        ScanResult sr;
                        sr.stats.size = cached_folders[ci].size.load(std::memory_order_relaxed);
                        sr.stats.files = cached_folders[ci].files.load(std::memory_order_relaxed);
                        sr.stats.dirs = cached_folders[ci].dirs.load(std::memory_order_relaxed);
                        std::wstring full_key = join_path(state.current_path, cached_folders[ci].name.c_str());
                        state.scan_cache[full_key] = sr;
                    }
                }
            }
        }
    }

    WIN32_FIND_DATAW fd;
    std::wstring ext_path = to_extended_path(state.current_path);
    std::wstring pattern = join_path(ext_path, L"*");
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(state.mtx);
        state.entries.clear();
        state.selected_index = 0;
        state.scroll_offset = 0;
        return;
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (is_dot_dir(fd.cFileName)) continue;
        if (should_skip(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;

        DirEntry entry;
        entry.name = fd.cFileName;
        entry.size.store(0, std::memory_order_relaxed);
        entry.files.store(0, std::memory_order_relaxed);
        entry.dirs.store(0, std::memory_order_relaxed);

        // Check cache (lock required — scan thread may write concurrently)
        std::wstring full_path = join_path(state.current_path, fd.cFileName);
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            std::unordered_map<std::wstring, ScanResult>::iterator it = state.scan_cache.find(full_path);
            if (it != state.scan_cache.end()) {
                entry.size.store(it->second.stats.size, std::memory_order_relaxed);
                entry.files.store(it->second.stats.files, std::memory_order_relaxed);
                entry.dirs.store(it->second.stats.dirs, std::memory_order_relaxed);
            }
        }

        new_entries.push_back(entry);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Sort by size descending
    std::sort(new_entries.begin(), new_entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.size.load(std::memory_order_relaxed) > b.size.load(std::memory_order_relaxed); });

    // Swap under lock
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        state.entries = std::move(new_entries);
        state.selected_index = 0;
        state.scroll_offset = 0;
    }
}

static void scan_directory_async(TuiState& state, const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        if (state.scan_cache.count(path)) return;
    }
    ScanResult result = get_dir_size(path, state.top_n);
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        state.scan_cache[path] = result;
    }
}

static void start_scan_all(TuiState& state) {
    // Signal previous scan thread to stop, then join
    if (state.scan_thread.joinable()) {
        state.scan_active.store(false);
        state.scan_thread.join();
    }

    // MFT fast path: any directory on NTFS volume (runs in background thread)
    std::wstring drive = tui_get_drive_root(state.current_path);
    bool use_mft = !drive.empty() && is_ntfs_volume(drive);

    // Check if all entries are already cached — skip scan if so
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        bool all_cached = true;
        for (size_t j = 0; j < state.entries.size(); j++) {
            std::wstring fp = join_path(state.current_path, state.entries[j].name.c_str());
            if (state.scan_cache.find(fp) == state.scan_cache.end()) {
                all_cached = false;
                break;
            }
        }
        if (all_cached && !state.entries.empty()) {
            for (size_t j = 0; j < state.entries.size(); j++) {
                std::wstring fp = join_path(state.current_path, state.entries[j].name.c_str());
                auto it = state.scan_cache.find(fp);
                state.entries[j].size.store(it->second.stats.size, std::memory_order_relaxed);
                state.entries[j].files.store(it->second.stats.files, std::memory_order_relaxed);
                state.entries[j].dirs.store(it->second.stats.dirs, std::memory_order_relaxed);
            }
            std::sort(state.entries.begin(), state.entries.end(),
                      [](const DirEntry& a, const DirEntry& b) {
                          return a.size.load(std::memory_order_relaxed) > b.size.load(std::memory_order_relaxed);
                      });
            state.scanning.store(false);
            return;
        }
    }

    // Background scan
    state.scanning.store(true);
    state.scan_active.store(true);
    // 记录扫描开始时刻 + 选定贴士起始下标，供渲染时轮换显示
    state.scan_start_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    state.tip_start = tips_random_start();
    std::wstring snap_path;
    std::vector<std::wstring> snap_names;
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        snap_path = state.current_path;
        snap_names.reserve(state.entries.size());
        for (size_t j = 0; j < state.entries.size(); j++) {
            snap_names.push_back(state.entries[j].name);
        }
    }
    int snap_top_n = state.top_n;

    state.scan_thread = std::thread([&state, snap_path, snap_names, use_mft, snap_top_n]() {
        // Try MFT first (fast path for NTFS)
        if (use_mft) {
            MftScanResult mft_out;
            if (get_dir_size_mft(snap_path, snap_top_n, mft_out)) {
                std::unordered_map<std::wstring, size_t> folder_map;
                for (size_t k = 0; k < mft_out.folders.size(); k++) {
                    folder_map[mft_out.folders[k].name] = k;
                }
                {
                    std::lock_guard<std::mutex> lock(state.mtx);
                    if (state.current_path == snap_path) {
                        for (size_t j = 0; j < state.entries.size(); j++) {
                            auto it = folder_map.find(state.entries[j].name);
                            if (it == folder_map.end()) continue;
                            size_t k = it->second;
                            state.entries[j].size.store(mft_out.folders[k].size.load(std::memory_order_relaxed), std::memory_order_relaxed);
                            state.entries[j].files.store(mft_out.folders[k].files.load(std::memory_order_relaxed), std::memory_order_relaxed);
                            state.entries[j].dirs.store(mft_out.folders[k].dirs.load(std::memory_order_relaxed), std::memory_order_relaxed);
                            std::wstring fp = join_path(snap_path, state.entries[j].name.c_str());
                            ScanResult cached;
                            cached.stats.size = mft_out.folders[k].size.load(std::memory_order_relaxed);
                            cached.stats.files = mft_out.folders[k].files.load(std::memory_order_relaxed);
                            cached.stats.dirs = mft_out.folders[k].dirs.load(std::memory_order_relaxed);
                            state.scan_cache[fp] = cached;
                        }
                        std::sort(state.entries.begin(), state.entries.end(),
                                  [](const DirEntry& a, const DirEntry& b) {
                                      return a.size.load(std::memory_order_relaxed) > b.size.load(std::memory_order_relaxed);
                                  });
                    }
                }
                state.scanning.store(false);
                return;
            }
            // MFT failed, fall through to per-directory scan
        }

        // Fallback: per-directory scan
        for (size_t j = 0; j < snap_names.size(); j++) {
            if (g_interrupted.load() || !state.scan_active.load()) break;
            std::wstring fp = join_path(snap_path, snap_names[j].c_str());
            scan_directory_async(state, fp);
            {
                std::lock_guard<std::mutex> lock(state.mtx);
                auto it = state.scan_cache.find(fp);
                if (it != state.scan_cache.end()) {
                    for (size_t k = 0; k < state.entries.size(); k++) {
                        if (state.entries[k].name == snap_names[j]) {
                            state.entries[k].size.store(it->second.stats.size, std::memory_order_relaxed);
                            state.entries[k].files.store(it->second.stats.files, std::memory_order_relaxed);
                            state.entries[k].dirs.store(it->second.stats.dirs, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
            }
        }
        // Sort entries by size after scan completes
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            std::sort(state.entries.begin(), state.entries.end(),
                      [](const DirEntry& a, const DirEntry& b) {
                          return a.size.load(std::memory_order_relaxed) > b.size.load(std::memory_order_relaxed);
                      });
        }
        state.scanning.store(false);
        state.scan_active.store(false);
    });
}
// ---- Legacy render: original WriteConsoleOutputW path ----
static void tui_render_legacy(TuiState& state, int w, int h,
    int selected_index, int& scroll_offset,
    const std::wstring& breadcrumb,
    bool show_top_files, bool is_scanning, bool is_top_loading,
    const std::vector<DirEntry>& entries_copy,
    const std::vector<FileInfo>& top_files_copy) {
    std::vector<wchar_t> chars(w * h, L' ');
    std::vector<WORD> attrs(w * h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    WORD dim = FOREGROUND_INTENSITY;
    WORD normal = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    WORD cyan = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    WORD selected_bg = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

    int row = 0;

    // Row 0: Breadcrumb
    std::wstring bc = breadcrumb;
    if (w > 4 && bc.size() > (size_t)w) {
        bc = L" ..." + bc.substr(bc.size() - w + 4);
    }
    fill_row(chars, attrs, row, w, L' ', cyan);
    write_str(chars, attrs, 0, row, w, bc, cyan);
    row++;

    // Row 1: Separator
    fill_row(chars, attrs, row, w, L'\u2500', dim);
    row++;

    if (show_top_files) {
        fill_row(chars, attrs, row, w, L' ', cyan);
        write_str(chars, attrs, 1, row, w, L"Size       Path", cyan);
        row++;

        fill_row(chars, attrs, row, w, L'\u2500', dim);
        row++;

        int visible_rows = h - row - 1;
        if (selected_index < scroll_offset) scroll_offset = selected_index;
        if (selected_index >= scroll_offset + visible_rows) scroll_offset = selected_index - visible_rows + 1;

        for (int i = 0; i < visible_rows; i++) {
            int idx = scroll_offset + i;
            if (idx >= (int)top_files_copy.size()) break;

            WORD row_attr = (idx == selected_index) ? selected_bg : normal;
            fill_row(chars, attrs, row, w, L' ', row_attr);

            const FileInfo& fi = top_files_copy[idx];
            std::string sz = fmt_size(fi.size);
            std::wstring wsz(sz.begin(), sz.end());
            write_str(chars, attrs, 1, row, w, wsz, row_attr);

            std::wstring rpath = fi.path;
            write_str(chars, attrs, 14, row, w, rpath, row_attr);
            row++;
        }
    } else {
        int visible_rows = h - row - 2;
        if (visible_rows < 1) visible_rows = 1;
        if (selected_index < scroll_offset) scroll_offset = selected_index;
        if (selected_index >= scroll_offset + visible_rows) scroll_offset = selected_index - visible_rows + 1;

        long long max_size = 1;
        for (size_t i = 0; i < entries_copy.size(); i++) {
            long long s = entries_copy[i].size.load(std::memory_order_relaxed);
            if (s > max_size) max_size = s;
        }

        for (int i = 0; i < visible_rows; i++) {
            int idx = scroll_offset + i;
            if (idx >= (int)entries_copy.size()) break;

            WORD row_attr = (idx == selected_index) ? selected_bg : normal;
            fill_row(chars, attrs, row, w, L' ', row_attr);

            const DirEntry& e = entries_copy[idx];
            long long e_size = e.size.load(std::memory_order_relaxed);

            // Name (CJK-aware truncation)
            std::wstring display_name = e.name;
            int dw = 0;
            for (size_t ci = 0; ci < display_name.size(); ci++) {
                wchar_t ch = display_name[ci];
                dw += (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3000 && ch <= 0x30FF) ||
                      (ch >= 0xFF00 && ch <= 0xFFEF) || (ch >= 0xAC00 && ch <= 0xD7AF) ? 2 : 1;
            }
            if (dw > 28) {
                int w2 = 0; size_t cut = 0;
                for (size_t ci = 0; ci < display_name.size(); ci++) {
                    wchar_t ch = display_name[ci];
                    w2 += (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3000 && ch <= 0x30FF) ||
                          (ch >= 0xFF00 && ch <= 0xFFEF) || (ch >= 0xAC00 && ch <= 0xD7AF) ? 2 : 1;
                    if (w2 > 25) { cut = ci; break; }
                    cut = ci + 1;
                }
                display_name = display_name.substr(0, cut) + L"...";
            }
            write_str(chars, attrs, 1, row, w, display_name, row_attr);

            // Size
            std::wstring size_str;
            if (e_size > 0) {
                std::string s = fmt_size(e_size);
                size_str.assign(s.begin(), s.end());
            } else if (is_scanning) {
                size_str = L"…";  // ellipsis — scanning in progress
            } else {
                size_str = L"0 B";
            }
            write_str(chars, attrs, 30, row, w, size_str, row_attr);

            // Bar
            if (e_size > 0 && max_size > 0) {
                int bar_start = 42;
                int bar_w = w - bar_start - 2;
                if (bar_w < 4) bar_w = 4;
                if (bar_start + bar_w > w) bar_w = w - bar_start;
                int filled = (int)((double)e_size / max_size * bar_w);
                if (filled > bar_w) filled = bar_w;
                if (filled < 0) filled = 0;

                WORD bar_color;
                long long mb = e_size / (1024 * 1024);
                if (mb <= 100) bar_color = FOREGROUND_GREEN;
                else if (mb <= 500) bar_color = FOREGROUND_GREEN | FOREGROUND_RED;
                else bar_color = FOREGROUND_RED;

                for (int j = 0; j < bar_w; j++) {
                    wchar_t ch = (j < filled) ? L'\u2588' : L' ';
                    WORD attr = (j < filled) ? (bar_color | FOREGROUND_INTENSITY) : FOREGROUND_INTENSITY;
                    int pos = row * w + bar_start + j;
                    if (pos >= 0 && pos < (int)chars.size()) {
                        chars[pos] = ch;
                        attrs[pos] = attr;
                    }
                }
            }

            row++;
        }
    }

    // Status line
    fill_row(chars, attrs, h - 1, w, L' ', cyan);
    if (is_top_loading) {
        write_str(chars, attrs, 1, h - 1, w, L"Loading large files list...", cyan);
    } else if (is_scanning) {
        write_str(chars, attrs, 1, h - 1, w, L"Scanning...", cyan);
    } else if (show_top_files) {
        write_str(chars, attrs, 1, h - 1, w, L"UP/DOWN navigate | Any key to go back", cyan);
    } else {
        write_str(chars, attrs, 1, h - 1, w, L"UP/DOWN | ENTER dir | BACK go back | T top | S sort | Q quit", cyan);
    }

    // Flush
    std::vector<CHAR_INFO> ci_buf(w * h);
    for (int i = 0; i < w * h; i++) {
        ci_buf[i].Char.UnicodeChar = chars[i];
        ci_buf[i].Attributes = attrs[i];
    }
    COORD buf_size = { (SHORT)w, (SHORT)h };
    COORD buf_origin = { 0, 0 };
    SMALL_RECT write_region = { 0, 0, (SHORT)(w - 1), (SHORT)(h - 1) };
    WriteConsoleOutputW(g_tui_buffer, ci_buf.data(), buf_size, buf_origin, &write_region);
}
// ---- VT render: 24-bit color with Unicode box drawing ----

// Pre-computed color escape sequences (avoid per-frame heap allocations)
static const std::wstring rst = L"\x1b[0m";
static const std::wstring VT_FG_BORDER = L"\x1b[38;2;80;80;80m";
static const std::wstring VT_FG_DIM = L"\x1b[38;2;150;150;150m";
static const std::wstring VT_FG_LABEL = L"\x1b[38;2;180;180;180m";
static const std::wstring VT_FG_HINT = L"\x1b[38;2;160;160;160m";
static const std::wstring VT_FG_STAT = L"\x1b[38;2;200;200;255m";
static const std::wstring VT_FG_SEP = L"\x1b[38;2;120;120;120m";
static const std::wstring VT_FG_CYAN = L"\x1b[38;2;0;200;200m";
static const std::wstring VT_FG_WHITE = L"\x1b[38;2;255;255;255m";
static const std::wstring VT_FG_TEXT = L"\x1b[38;2;220;220;220m";
static const std::wstring VT_FG_TEXTHI = L"\x1b[38;2;255;255;255m";
static const std::wstring VT_FG_SIZE = L"\x1b[38;2;150;150;150m";
static const std::wstring VT_FG_BARDIM = L"\x1b[38;2;60;60;60m";
static const std::wstring VT_FG_GREEN = L"\x1b[38;2;0;200;0m";
static const std::wstring VT_FG_YELLOW = L"\x1b[38;2;200;200;0m";
static const std::wstring VT_FG_YELLOWHI = L"\x1b[38;2;255;200;0m";
static const std::wstring VT_FG_TEXTDIM = L"\x1b[38;2;200;200;200m";
static const std::wstring VT_BG_SEL = L"\x1b[48;2;0;80;160m";

static void tui_render_vt(TuiState& state, int w, int h,
    int selected_index, int& scroll_offset,
    const std::wstring& breadcrumb,
    bool show_top_files, bool is_scanning, bool is_top_loading,
    const std::vector<DirEntry>& entries_copy,
    const std::vector<FileInfo>& top_files_copy) {

    // Per-frame line buffer for diff-based incremental refresh
    static std::vector<std::wstring> prev_lines;
    static int prev_w = 0, prev_h = 0;
    // Force full redraw if size changed
    if (w != prev_w || h != prev_h) {
        prev_lines.clear();
        prev_w = w;
        prev_h = h;
    }
    std::vector<std::wstring> cur_lines;
    cur_lines.reserve(h);

    // Use pre-computed constants for common colors; lambda only for dynamic RGB (gradient bars)
    auto fg = [](int r, int g, int b) -> std::wstring {
        return L"\x1b[38;2;" + std::to_wstring(r) + L";" + std::to_wstring(g) + L";" + std::to_wstring(b) + L"m";
    };

    auto cjk_w = [](const std::wstring& s) -> int {
        int d = 0;
        for (wchar_t ch : s)
            d += (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3000 && ch <= 0x30FF) ||
                 (ch >= 0xFF00 && ch <= 0xFFEF) || (ch >= 0xAC00 && ch <= 0xD7AF) ? 2 : 1;
        return d;
    };

    auto repeat_w = [](std::wstring& line, wchar_t ch, int n) {
        for (int i = 0; i < n; i++) line += ch;
    };

    // ---- Compute stats ----
    long long max_size = 1;
    long long total_size = 0;
    int scanned_count = 0;
    for (auto& e : entries_copy) {
        long long s = e.size.load(std::memory_order_relaxed);
        if (s > max_size) max_size = s;
        total_size += s;
        if (s > 0) scanned_count++;
    }

    std::string tss = fmt_size(total_size);
    std::wstring total_str(tss.begin(), tss.end());

    // Column layout
    int name_w = 28;
    int size_w = 10;
    int bar_start = 2 + name_w + 1 + size_w + 1;
    int bar_w = w - bar_start - 2;
    if (bar_w < 4) bar_w = 4;

    // Line 0: Top border with breadcrumb
    {
        std::wstring line;
        line += VT_FG_BORDER + L"\u250c\u2500" + VT_FG_CYAN;
        std::wstring bc = breadcrumb;
        int bc_max = w - 14 - (int)total_str.size();
        if (bc_max < 8) bc_max = 8;
        if ((int)bc.size() > bc_max) bc = L"..." + bc.substr(bc.size() - bc_max + 3);
        line += bc;
        int fill_w = w - 5 - cjk_w(bc) - (int)total_str.size();
        if (fill_w < 1) fill_w = 1;
        line += VT_FG_DIM;
        for (int i = 0; i < fill_w; i++) line += L'\u2500';
        line += L' ' + VT_FG_WHITE + total_str + L' ' + VT_FG_BORDER + L"\u2500\u2510";
        cur_lines.push_back(line);
    }

    // Line 1: Stats row
    {
        std::wstring line;
        line += VT_FG_BORDER + L"\u2502 " + VT_FG_STAT;
        line += std::to_wstring(entries_copy.size()) + L" dirs";
        line += VT_FG_SEP + L"  \u2502 " + VT_FG_STAT;
        line += total_str + L" total";
        if (is_scanning && (int)entries_copy.size() > 0) {
            line += VT_FG_SEP + L"  \u2502 " + VT_FG_STAT;
            int pct = scanned_count * 100 / (int)entries_copy.size();
            line += std::to_wstring(scanned_count) + L"/" + std::to_wstring(entries_copy.size()) +
                    L" (" + std::to_wstring(pct) + L"%)";
        }
        line += VT_FG_BORDER + L" \u2502";
        cur_lines.push_back(line);
    }

    if (show_top_files) {
        // Top files view in VT mode
        // Header separator
        {
            std::wstring line;
            line += L"\u251c" + VT_FG_DIM;
            for (int i = 1; i < w - 1; i++) line += L'\u2500';
            line += rst + VT_FG_BORDER + L"\u2524";
            cur_lines.push_back(line);
        }

        if (top_files_copy.empty()) {
            // Loading or empty state
            std::wstring line;
            line += VT_FG_BORDER + L"\u2502 " + VT_FG_TEXTDIM;
            line += is_top_loading ? L"Loading..." : L"No files found.";
            line += rst + VT_FG_BORDER + L" \u2502";
            cur_lines.push_back(line);
            // Fill remaining rows
            int visible_rows = h - 8;
            for (int i = 1; i < visible_rows; i++) {
                std::wstring fline;
                fline += VT_FG_BORDER + L"\u2502";
                repeat_w(fline, L' ', w - 2);
                fline += L"\u2502";
                cur_lines.push_back(fline);
            }
        } else {
        // Column labels
        {
            std::wstring line;
            line += L"\u2502 " + VT_FG_LABEL + L"Size";
            for (int i = 4; i < 12; i++) line += L' ';
            line += VT_FG_BORDER + L"\u2502 " + VT_FG_LABEL + L"Path";
            line += rst + VT_FG_BORDER + L" \u2502";
            cur_lines.push_back(line);
        }
        // Separator
        {
            std::wstring line;
            line += L"\u251c" + VT_FG_DIM;
            for (int i = 1; i < w - 1; i++) line += L'\u2500';
            line += rst + VT_FG_BORDER + L"\u2524";
            cur_lines.push_back(line);
        }

        int visible_rows = h - 8;
        if (visible_rows < 1) visible_rows = 1;
        if (selected_index < scroll_offset) scroll_offset = selected_index;
        if (selected_index >= scroll_offset + visible_rows) scroll_offset = selected_index - visible_rows + 1;

        for (int i = 0; i < visible_rows; i++) {
            std::wstring line;
            int idx = scroll_offset + i;
            if (idx >= (int)top_files_copy.size()) {
                line += VT_FG_BORDER + L"\u2502";
                repeat_w(line, L' ', w - 2);
                line += L"\u2502";
                cur_lines.push_back(line);
                continue;
            }

            const FileInfo& fi = top_files_copy[idx];
            bool is_sel = (idx == selected_index);

            if (is_sel) line += VT_BG_SEL;
            line += VT_FG_BORDER + L"\u2502 " + rst;

            // Size
            std::string ss = fmt_size(fi.size);
            std::wstring sz(ss.begin(), ss.end());
            line += (is_sel ? VT_FG_YELLOWHI : VT_FG_YELLOW) + sz;
            for (int p = (int)sz.size(); p < 10; p++) line += L' ';
            line += VT_FG_BORDER + L"\u2502 " + rst;

            // Path (truncate to fit)
            std::wstring fpath = fi.path;
            int max_path_w = w - 16;
            if (max_path_w < 10) max_path_w = 10;
            if ((int)fpath.size() > max_path_w) {
                fpath = L"..." + fpath.substr(fpath.size() - max_path_w + 3);
            }
            line += (is_sel ? VT_FG_WHITE : VT_FG_LABEL) + fpath;

            if (is_sel) line += rst;
            line += VT_FG_BORDER + L" \u2502";
            cur_lines.push_back(line);
        }
        } // end of else (non-empty top files)
    } else {
    // Line 2: Header separator
    {
        std::wstring line;
        line += L"\u251c" + VT_FG_DIM;
        for (int i = 1; i < 2 + name_w; i++) line += L'\u2500';
        line += L'\u252c';
        for (int i = 0; i < size_w; i++) line += L'\u2500';
        line += L'\u252c';
        for (int i = 0; i < bar_w + 1; i++) line += L'\u2500';
        line += rst + VT_FG_BORDER + L"\u2524";
        cur_lines.push_back(line);
    }

    // Line 3: Column labels
    {
        std::wstring line;
        line += L"\u2502 " + VT_FG_LABEL + L"Name";
        for (int i = 4; i < name_w; i++) line += L' ';
        line += VT_FG_BORDER + L"\u2502" + VT_FG_LABEL;
        int sz_pad = (size_w - 4) / 2;
        for (int i = 0; i < sz_pad; i++) line += L' ';
        line += L"Size";
        for (int i = sz_pad + 4; i < size_w; i++) line += L' ';
        line += VT_FG_BORDER + L"\u2502 " + VT_FG_LABEL + L"Distribution";
        line += rst + VT_FG_BORDER + L" \u2502";
        cur_lines.push_back(line);
    }

    // Line 4: Header bottom
    {
        std::wstring line;
        line += L"\u251c" + VT_FG_DIM;
        for (int i = 1; i < 2 + name_w; i++) line += L'\u2500';
        line += L'\u253c';
        for (int i = 0; i < size_w; i++) line += L'\u2500';
        line += L'\u253c';
        for (int i = 0; i < bar_w + 1; i++) line += L'\u2500';
        line += rst + VT_FG_BORDER + L"\u2524";
        cur_lines.push_back(line);
    }

    // Lines 5..h-4: Entries
    int visible_rows = h - 8;
    if (visible_rows < 1) visible_rows = 1;
    if (selected_index < scroll_offset) scroll_offset = selected_index;
    if (selected_index >= scroll_offset + visible_rows) scroll_offset = selected_index - visible_rows + 1;

    for (int i = 0; i < visible_rows; i++) {
        std::wstring line;
        int idx = scroll_offset + i;
        if (idx >= (int)entries_copy.size()) {
            line += VT_FG_BORDER + L"\u2502";
            repeat_w(line, L' ', w - 2);
            line += L"\u2502";
            cur_lines.push_back(line);
            continue;
        }

        const DirEntry& e = entries_copy[idx];
        long long e_size = e.size.load(std::memory_order_relaxed);
        bool is_sel = (idx == selected_index);

        if (is_sel) line += VT_BG_SEL;
        line += VT_FG_BORDER + L"\u2502 " + rst;

        // Name
        std::wstring dn = e.name;
        int dnw = cjk_w(dn);
        if (dnw > name_w) {
            int w2 = 0; size_t cut = 0;
            for (size_t ci = 0; ci < dn.size(); ci++) {
                wchar_t ch = dn[ci];
                w2 += (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3000 && ch <= 0x30FF) ||
                      (ch >= 0xFF00 && ch <= 0xFFEF) || (ch >= 0xAC00 && ch <= 0xD7AF) ? 2 : 1;
                if (w2 > name_w - 3) { cut = ci; break; }
                cut = ci + 1;
            }
            dn = dn.substr(0, cut) + L"...";
            dnw = name_w;
        }
        line += (is_sel ? VT_FG_WHITE : VT_FG_TEXT) + dn;
        for (int p = dnw; p < name_w; p++) line += L' ';
        line += VT_FG_BORDER + L"\u2502" + rst;

        // Size
        std::wstring sz;
        if (e_size > 0) {
            std::string ss = fmt_size(e_size);
            sz.assign(ss.begin(), ss.end());
        } else if (is_scanning) {
            sz = L"\u2026";  // ellipsis \u2014 scanning in progress
        } else {
            sz = L"0 B";
        }
        line += (is_sel ? VT_FG_WHITE : VT_FG_DIM);
        for (int p = (int)sz.size(); p < size_w; p++) line += L' ';
        line += sz;
        line += VT_FG_BORDER + L"\u2502 " + rst;

        // Gradient bar
        if (e_size > 0 && max_size > 0) {
            int filled = (int)((double)e_size / max_size * bar_w);
            if (filled > bar_w) filled = bar_w;
            if (filled < 0) filled = 0;

            long long mb = e_size / (1024 * 1024);
            int r, g, b;
            if (mb <= 100)      { r = 0;   g = 200; b = 0; }
            else if (mb <= 500) { double t = (double)(mb - 100) / 400.0; r = (int)(255*t); g = 200 + (int)(55*t); b = 0; }
            else if (mb <= 1024){ double t = (double)(mb - 500) / 524.0; r = 255; g = 255 - (int)(255*t); b = 0; }
            else                { r = 255; g = 0;   b = 0; }

            line += fg(r, g, b);
            for (int bi = 0; bi < filled; bi++) line += L'\u2588';
            line += VT_FG_BARDIM;
            for (int bi = filled; bi < bar_w; bi++) line += L'\u2591';
            line += rst;
        } else {
            repeat_w(line, L' ', bar_w);
        }

        if (is_sel) line += rst;
        line += VT_FG_BORDER + L" \u2502";
        cur_lines.push_back(line);
    }

    } // end of else (entries view)

    // Footer separator (shared)
    {
        std::wstring line;
        line += L"\u251c" + VT_FG_DIM;
        for (int i = 1; i < w - 1; i++) line += L'\u2500';
        line += rst + VT_FG_BORDER + L"\u2524";
        cur_lines.push_back(line);
    }

    // Key hints
    {
        std::wstring line;
        line += L"\u2502 " + VT_FG_HINT;
        std::wstring hint;
        if (show_top_files) {
            hint = L"UP/DOWN: Nav  |  Any key: Back  |  Q: Quit";
        } else if (is_top_loading) {
            hint = (w >= 84) ? L"Q: Quit  |  ENTER: Open  |  BKSP: Back  |  S: Sort  |  T: Top  |  *loading*"
                             : L"Q|ENTER|BKSP|S|T  *loading*";
        } else if (is_scanning) {
            hint = (w >= 84) ? L"Q: Quit  |  ENTER: Open  |  BKSP: Back  |  S: Sort  |  T: Top  |  *scanning*"
                             : L"Q|ENTER|BKSP|S|T  *scanning*";
        } else {
            hint = (w >= 74) ? L"Q: Quit  |  ENTER: Open  |  BKSP: Back  |  S: Sort  |  T: Top Files"
                             : L"Q|ENTER|BKSP|S|T";
        }
        int max_hint = w - 5;
        if ((int)hint.size() > max_hint && max_hint > 3)
            hint = hint.substr(0, max_hint - 3) + L"...";
        line += hint;
        line += rst + VT_FG_BORDER + L" \u2502";
        cur_lines.push_back(line);
    }

    // Progress bar
    {
        std::wstring line;
        line += L"\u2502 " + VT_FG_HINT;
        if (is_scanning && entries_copy.size() > 0) {
            int pct = scanned_count * 100 / (int)entries_copy.size();
            int prog_w = w - 32;
            if (prog_w > 24) prog_w = 24;
            if (prog_w < 4) prog_w = 4;
            int prog_filled = pct * prog_w / 100;

            if (w >= 45) {
                line += rst + VT_FG_WHITE + L"Scanning [";
                line += VT_FG_GREEN;
                for (int pi = 0; pi < prog_filled; pi++) line += L'\u2588';
                line += VT_FG_BARDIM;
                for (int pi = prog_filled; pi < prog_w; pi++) line += L'\u2591';
                line += rst + VT_FG_WHITE;
                wchar_t pbuf[64];
                swprintf(pbuf, 64, L"] %d%% (%d/%d)", pct, scanned_count, (int)entries_copy.size());
                line += pbuf;

                // \u8fdb\u5ea6\u540e\u9762\u63a5\u4e00\u6761\u968f\u65f6\u95f4\u8f6e\u6362\u7684\u5c0f\u8d34\u58eb\uff0c\u586b\u6ee1\u5230\u53f3\u8fb9\u6846
                long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                long long el = now_ms - state.scan_start_ms.load();
                if (el < 0) el = 0;
                std::wstring tip = tui_u8_to_w(tip_for_elapsed(state.tip_start, el, 2500));
                int used = cjk_w(L"Scanning [") + prog_w + cjk_w(pbuf);
                int room = w - 4 - used - 3; // \u9884\u7559 " | " \u5206\u9694
                if (room >= 8 && !tip.empty()) {
                    line += rst + VT_FG_DIM + L"  " + tui_truncate_w(tip, room);
                }
            } else {
                wchar_t pbuf[32];
                swprintf(pbuf, 32, L"%d%%", pct);
                line += rst + VT_FG_WHITE;
                line += pbuf;
            }
        }
        line += rst + VT_FG_BORDER + L" \u2502";
        cur_lines.push_back(line);
    }

    // Bottom border
    {
        std::wstring line;
        line += L"\u2514" + VT_FG_DIM;
        for (int i = 1; i < w - 1; i++) line += L'\u2500';
        line += rst + VT_FG_BORDER + L"\u2518";
        cur_lines.push_back(line);
    }

    // --- Diff-based incremental write ---
    bool first_frame = prev_lines.empty();
    int prev_sz = (int)prev_lines.size();
    int cur_sz = (int)cur_lines.size();

    std::wstring out;
    out.reserve(w * 30); // enough for a few changed lines

    // Hide cursor during update
    out += L"\x1b[?25l";

    for (int row = 0; row < cur_sz; row++) {
        bool changed = false;
        if (first_frame) {
            changed = true;
        } else if (row >= prev_sz) {
            changed = true;
        } else if (cur_lines[row] != prev_lines[row]) {
            changed = true;
        }

        if (changed) {
            // Move cursor to row, col 0; clear line first to avoid residual chars
            out += L"\x1b[" + std::to_wstring(row + 1) + L";1H\x1b[K";
            out += cur_lines[row];
            out += rst; // reset at end of every changed line
        }
    }

    // If screen shrunk, clear leftover old lines
    if (!first_frame && cur_sz < prev_sz) {
        for (int row = cur_sz; row < prev_sz; row++) {
            out += L"\x1b[" + std::to_wstring(row + 1) + L";1H";
            out += L"\x1b[K"; // clear line
        }
    }

    // Show cursor
    out += L"\x1b[?25h";

    DWORD written;
    WriteConsoleW(g_tui_buffer, out.data(), (DWORD)out.size(), &written, NULL);

    // Store current frame for next diff
    prev_lines = std::move(cur_lines);
}


// ---- Dispatch: picks VT or legacy renderer ----
static void tui_render(TuiState& state) {
    int w, h;
    int selected_index, scroll_offset;
    std::wstring breadcrumb;
    bool show_top_files;
    bool is_scanning;
    bool is_top_loading;
    std::vector<DirEntry> entries_copy;
    std::vector<FileInfo> top_files_copy;

    {
        std::lock_guard<std::mutex> lock(state.mtx);
        w = state.viewport_width;
        h = state.viewport_height;
        selected_index = state.selected_index;
        scroll_offset = state.scroll_offset;

        show_top_files = state.show_top_files;
        is_scanning = state.scanning.load();
        is_top_loading = state.top_files_loading.load();
        breadcrumb = L" ";
        for (size_t i = 0; i < state.breadcrumb.size(); i++) {
            if (i > 0) breadcrumb += L" > ";
            breadcrumb += state.breadcrumb[i];
        }
        entries_copy = state.entries;
        if (show_top_files) top_files_copy = state.top_files_list;
    }

    int scroll_before = scroll_offset;
    if (g_vt_supported) {
        tui_render_vt(state, w, h, selected_index, scroll_offset,
                      breadcrumb, show_top_files, is_scanning, is_top_loading,
                      entries_copy, top_files_copy);
    } else {
        tui_render_legacy(state, w, h, selected_index, scroll_offset,
                          breadcrumb, show_top_files, is_scanning, is_top_loading,
                          entries_copy, top_files_copy);
    }
    // Only write back scroll_offset if render adjusted it (avoid clobbering user input)
    if (scroll_offset != scroll_before) {
        std::lock_guard<std::mutex> lock(state.mtx);
        state.scroll_offset = scroll_offset;
    }
}
void tui_run(const std::wstring& initial_path, int top_n) {
    TuiState state;
    state.current_path = initial_path;
    state.top_n = top_n;
    state.scanning.store(false); // Start with scanning=false

    // Build initial breadcrumb (strip \\?\ prefix for display)
    std::wstring path = initial_path;
    // Split path into segments
    std::vector<std::wstring> segments;
    size_t start = 0;
    // Skip drive letter (e.g., "C:\")
    if (path.size() >= 2 && path[1] == L':') {
        segments.push_back(path.substr(0, 2));  // "C:"
        start = 2;
        if (start < path.size() && (path[start] == L'\\' || path[start] == L'/')) start++;
    }
    for (size_t i = start; i <= path.size(); i++) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') {
            if (i > start) {
                segments.push_back(path.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    state.breadcrumb = segments;
    // Build full path stack from segments
    {
        std::wstring full;
        for (size_t si = 0; si < segments.size(); si++) {
            if (si == 0) full = segments[si] + L"\\";
            else full += segments[si];
            if (si < segments.size() - 1) full += L"\\";
        }
        state.path_stack.clear();
        state.path_stack.push_back(to_extended_path(full));
    }

    tui_init();
    get_console_size(state.viewport_width, state.viewport_height);

    // Initial population
    populate_entries(state, true);

    start_scan_all(state);

    // Main loop
    while (state.running.load()) {
        if (g_interrupted.load()) break;
        tui_render(state);

        // Wait for input with timeout
        DWORD wait_result = WaitForSingleObject(g_input_handle, 100);
        if (wait_result == WAIT_OBJECT_0) {
            INPUT_RECORD records[16];
            DWORD count;
            if (ReadConsoleInputW(g_input_handle, records, 16, &count)) {
                for (DWORD i = 0; i < count; i++) {
                    if (records[i].EventType != KEY_EVENT) continue;
                    KEY_EVENT_RECORD& ker = records[i].Event.KeyEvent;
                    if (!ker.bKeyDown) continue;

                    switch (ker.wVirtualKeyCode) {
                        case VK_UP:
                            if (state.show_top_files) {
                                std::lock_guard<std::mutex> lock(state.mtx);
                                if (state.selected_index > 0) state.selected_index--;
                            } else if (state.selected_index > 0) {
                                state.selected_index--;
                            }
                            break;
                        case VK_DOWN:
                            if (state.show_top_files) {
                                std::lock_guard<std::mutex> lock(state.mtx);
                                if (state.selected_index < (int)state.top_files_list.size() - 1)
                                    state.selected_index++;
                            } else if (state.selected_index < (int)state.entries.size() - 1) {
                                state.selected_index++;
                            }
                            break;
                        case VK_RETURN: {
                            if (state.show_top_files) { state.show_top_files = false; break; }
                            if (state.selected_index >= (int)state.entries.size()) break;
                            state.top_files_active.store(false); // cancel top files scan
                            std::wstring full_path = join_path(state.current_path, state.entries[state.selected_index].name.c_str());
                            state.current_path = to_extended_path(full_path);
                            state.breadcrumb.push_back(state.entries[state.selected_index].name);
                            state.path_stack.push_back(state.current_path);
                            populate_entries(state);
                            start_scan_all(state);
                            break;
                        }
                        case VK_BACK: {
                            if (state.show_top_files) { state.show_top_files = false; break; }
                            state.top_files_active.store(false);
                            if (state.path_stack.size() > 1) {
                                state.path_stack.pop_back();
                                state.breadcrumb.pop_back();
                                state.current_path = state.path_stack.back();
                            } else {
                                // Navigate to parent directory even beyond initial path
                                std::wstring cur = state.current_path;
                                // Strip \\?\ prefix for parent computation
                                std::wstring stripped = cur;
                                bool had_prefix = false;
                                if (stripped.size() >= 4 && stripped[0] == L'\\' && stripped[1] == L'\\' &&
                                    stripped[2] == L'?' && stripped[3] == L'\\') {
                                    stripped = stripped.substr(4);
                                    had_prefix = true;
                                }
                                // Remove trailing backslash
                                while (stripped.size() > 3 && (stripped.back() == L'\\' || stripped.back() == L'/'))
                                    stripped.pop_back();
                                // Find last separator
                                size_t pos = stripped.find_last_of(L"\\/");
                                if (pos == std::wstring::npos || pos < 2) break; // already at root
                                // Don't go above drive root (e.g. "C:\")
                                std::wstring parent;
                                if (pos == 2 && stripped[1] == L':') {
                                    parent = stripped.substr(0, 3); // "C:\"
                                } else {
                                    parent = stripped.substr(0, pos);
                                }
                                if (had_prefix) parent = L"\\\\?\\" + parent;
                                state.current_path = parent;
                                // Rebuild breadcrumb from scratch
                                std::wstring display = parent;
                                if (display.size() >= 4 && display[0] == L'\\' && display[1] == L'\\' &&
                                    display[2] == L'?' && display[3] == L'\\')
                                    display = display.substr(4);
                                std::vector<std::wstring> segs;
                                size_t s = 0;
                                if (display.size() >= 2 && display[1] == L':') {
                                    segs.push_back(display.substr(0, 2));
                                    s = 2;
                                    if (s < display.size() && (display[s] == L'\\' || display[s] == L'/')) s++;
                                }
                                for (size_t i = s; i <= display.size(); i++) {
                                    if (i == display.size() || display[i] == L'\\' || display[i] == L'/') {
                                        if (i > s) segs.push_back(display.substr(s, i - s));
                                        s = i + 1;
                                    }
                                }
                                state.breadcrumb = segs;
                                state.path_stack.clear();
                                state.path_stack.push_back(state.current_path);
                            }
                            populate_entries(state);
                            start_scan_all(state);
                            break;
                        }
                        case 'S': {
                            std::lock_guard<std::mutex> lock(state.mtx);
                            state.sort_by_size = !state.sort_by_size;
                            if (state.sort_by_size) {
                                std::sort(state.entries.begin(), state.entries.end(),
                                          [](const DirEntry& a, const DirEntry& b) { return a.size.load(std::memory_order_relaxed) > b.size.load(std::memory_order_relaxed); });
                            } else {
                                std::sort(state.entries.begin(), state.entries.end(),
                                          [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
                            }
                            state.selected_index = 0;
                            state.scroll_offset = 0;
                            break;
                        }
                        case 'T': {
                            if (state.show_top_files) { state.show_top_files = false; break; }
                            if (state.top_files_loading.load()) break;
                            // Join previous thread if any
                            if (state.top_files_thread.joinable())
                                state.top_files_thread.join();
                            state.top_files_loading.store(true);
                            state.top_files_active.store(true);
                            state.selected_index = 0;
                            state.scroll_offset = 0;
                            state.top_files_thread = std::thread([&state]() {
                                // Collect files into local vector first
                                std::vector<FileInfo> local_list;
                                int top_n = state.top_n;
                                // Snapshot path — user may navigate while we scan
                                std::wstring snap_path;
                                {
                                    std::lock_guard<std::mutex> lock(state.mtx);
                                    snap_path = state.current_path;
                                }
                                std::wstring root = to_extended_path(snap_path);
                                std::vector<std::wstring> stk;
                                stk.push_back(root);
                                while (!stk.empty()) {
                                    if (g_interrupted.load() || !state.top_files_active.load()) break;
                                    std::wstring dir = std::move(stk.back());
                                    stk.pop_back();
                                    WIN32_FIND_DATAW fd;
                                    HANDLE h = find_first_fast(search_pattern(dir), &fd);
                                    if (h == INVALID_HANDLE_VALUE) continue;
                                    do {
                                        if (is_dot_dir(fd.cFileName)) continue;
                                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                                            if (should_skip(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                                            stk.push_back(join_path(dir, fd.cFileName));
                                        } else {
                                            long long fsize = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                                            if ((int)local_list.size() < top_n) {
                                                FileInfo fi;
                                                fi.path = join_path(dir, fd.cFileName);
                                                fi.size = fsize;
                                                local_list.push_back(fi);
                                                if ((int)local_list.size() == top_n) {
                                                    std::sort(local_list.begin(), local_list.end(),
                                                              [](const FileInfo& a, const FileInfo& b) { return a.size > b.size; });
                                                }
                                            } else if (fsize > local_list.back().size) {
                                                local_list.back().path = join_path(dir, fd.cFileName);
                                                local_list.back().size = fsize;
                                                for (int j = (int)local_list.size() - 1; j > 0; j--) {
                                                    if (local_list[j].size > local_list[j-1].size)
                                                        std::swap(local_list[j], local_list[j-1]);
                                                    else break;
                                                }
                                            }
                                        }
                                    } while (FindNextFileW(h, &fd));
                                    FindClose(h);
                                }
                                std::sort(local_list.begin(), local_list.end(),
                                          [](const FileInfo& a, const FileInfo& b) { return a.size > b.size; });
                                // Copy result under lock — only if user hasn't navigated away
                                {
                                    std::lock_guard<std::mutex> lock(state.mtx);
                                    if (state.current_path == snap_path) {
                                        state.top_files_list = std::move(local_list);
                                        state.show_top_files = true;
                                    }
                                    // else: user navigated, discard stale results
                                }
                                state.top_files_loading.store(false);
                            });
                            break;
                        }
                        case 'Q':
                        case VK_ESCAPE:
                            state.running.store(false);
                            break;
                        default:
                            // Any other key dismisses top files view
                            if (state.show_top_files) {
                                state.show_top_files = false;
                            }
                            break;
                    }
                }
            }
        }

        // Check for resize
        int new_w, new_h;
        get_console_size(new_w, new_h);
        if (new_w != state.viewport_width || new_h != state.viewport_height) {
            state.viewport_width = new_w;
            state.viewport_height = new_h;
            tui_render(state);  // immediate redraw on resize
        }
    }

    tui_cleanup();
}
