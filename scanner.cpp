#include "scanner.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <unordered_map>

std::atomic<bool> g_interrupted(false);

void setup_interrupt_handler() {
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

bool is_dot_dir(const wchar_t* name) {
    // Fast path: check first char
    if (name[0] != L'.') return false;
    if (name[1] == L'\0') return true;  // "."
    if (name[1] == L'.' && name[2] == L'\0') return true;  // ".."
    return false;
}

bool should_skip(const wchar_t* name) {
    // Fast path: check first char
    if (name[0] == L'.') return true;
    if (name[0] == L'$') {
        // Common system dirs starting with $
        return wcscmp(name, L"$RECYCLE.BIN") == 0 ||
               wcscmp(name, L"System Volume Information") == 0 ||
               wcscmp(name, L"$WinREAgent") == 0;
    }
    // Check other skip dirs
    return wcscmp(name, L"Recovery") == 0 || wcscmp(name, L"PerfLogs") == 0;
}

std::wstring join_path(const std::wstring& base, const wchar_t* name) {
    if (base.empty()) return name;
    size_t base_len = base.size();
    size_t name_len = wcslen(name);
    wchar_t last = base.back();

    if (last == L'\\' || last == L'/') {
        std::wstring result;
        result.reserve(base_len + name_len);
        result = base;
        result.append(name, name_len);
        return result;
    }

    std::wstring result;
    result.reserve(base_len + 1 + name_len);
    result = base;
    result += L'\\';
    result.append(name, name_len);
    return result;
}

std::wstring search_pattern(const std::wstring& dir) {
    // Inline optimization for "*" pattern
    std::wstring result;
    result.reserve(dir.size() + 2);
    result = dir;
    if (result.back() != L'\\' && result.back() != L'/') {
        result += L'\\';
    }
    result += L'*';
    return result;
}

std::wstring absolute_path(const wchar_t* input) {
    DWORD needed = GetFullPathNameW(input, 0, NULL, NULL);
    if (needed == 0) return input;
    std::wstring out(needed, L'\0');
    DWORD written = GetFullPathNameW(input, needed, &out[0], NULL);
    if (written == 0) return input;
    out.resize(written);
    return out;
}

std::wstring to_extended_path(const std::wstring& path) {
    if (path.rfind(LR"(\\?\)", 0) == 0) return path;
    if (path.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + path.substr(2);
    return LR"(\\?\)" + path;
}

std::string ws2s(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string fmt_size(long long b) {
    if (b < 0) return "0 B";
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = (double)b;
    int i = 0;
    while (val >= 1024.0 && i < 4) { val /= 1024.0; i++; }

    char buf[32];
    if (val >= 100.0) {
        snprintf(buf, sizeof(buf), "%.0f %s", val, units[i]);
    } else if (val >= 10.0) {
        snprintf(buf, sizeof(buf), "%.1f %s", val, units[i]);
    } else {
        snprintf(buf, sizeof(buf), "%.2f %s", val, units[i]);
    }
    return std::string(buf);
}

HANDLE find_first_fast(const std::wstring& pattern, WIN32_FIND_DATAW* fd) {
    // Try FindFirstFileExW first for better performance
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, fd,
                                FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    // Fallback to FindFirstFileW if Ex version fails
    if (h == INVALID_HANDLE_VALUE) {
        h = FindFirstFileW(pattern.c_str(), fd);
    }
    return h;
}

int worker_count_for(int total_count, int requested_workers) {
    if (requested_workers > 0) {
        return (std::min)((std::max)(1, requested_workers), total_count);
    }
    unsigned int hw = std::thread::hardware_concurrency();
    // I/O-bound: enough threads to saturate SSD IOPS without thrashing
    int by_cpu = hw == 0 ? 16 : (int)hw * 2;
    int workers = (std::min)(32, by_cpu);
    workers = (std::max)(1, workers);
    return (std::min)(workers, total_count);
}

// Extract extension from wide filename without converting entire string
static std::string get_extension(const wchar_t* name) {
    const wchar_t* dot = NULL;
    for (const wchar_t* p = name; *p; p++) {
        if (*p == L'.') dot = p;
    }
    if (!dot || dot == name) return "";

    // Fast path: all ASCII (covers 99.9%+ of real extensions)
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

ScanResult get_dir_size(const std::wstring& root, int top_n) {
    ScanResult result;
    // Use unordered_map for O(1) average insert
    std::unordered_map<std::string, long long> ext_sizes;
    std::unordered_map<std::string, unsigned long long> ext_counts;

    std::vector<std::wstring> stack;
    stack.reserve(256);
    stack.push_back(root);

    while (!stack.empty()) {
        if (g_interrupted.load()) break;
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        result.stats.dirs++;

        WIN32_FIND_DATAW fd;
        HANDLE h = find_first_fast(search_pattern(dir), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            result.stats.errors++;
            if (result.error_dirs.size() < ScanResult::MAX_LOGGED_DIRS)
                result.error_dirs.push_back(dir);
            continue;
        }

        unsigned int iter_count = 0;
        do {
            if ((iter_count++ & 0xFF) == 0 && g_interrupted.load()) break;
            if (is_dot_dir(fd.cFileName)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (should_skip(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    result.stats.skipped++;
                    if (result.skipped_dirs.size() < ScanResult::MAX_LOGGED_DIRS)
                        result.skipped_dirs.push_back(join_path(dir, fd.cFileName));
                    continue;
                }
                stack.push_back(join_path(dir, fd.cFileName));
            } else {
                result.stats.files++;
                long long fsize = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                result.stats.size += fsize;

                // Extract extension directly from wchar_t
                std::string ext = get_extension(fd.cFileName);
                if (!ext.empty()) {
                    ext_sizes[ext] += fsize;
                    ext_counts[ext]++;
                }

                // Track top files - only sort when needed
                if (top_n > 0) {
                    if ((int)result.top_files.size() < top_n) {
                        FileInfo fi;
                        fi.path = join_path(dir, fd.cFileName);
                        fi.size = fsize;
                        fi.extension = ext;
                        result.top_files.push_back(fi);
                        // Only sort when vector is full
                        if ((int)result.top_files.size() == top_n) {
                            std::sort(result.top_files.begin(), result.top_files.end(),
                                      [](const FileInfo& a, const FileInfo& b) { return a.size > b.size; });
                        }
                    } else if (fsize > result.top_files.back().size) {
                        result.top_files.back().path = join_path(dir, fd.cFileName);
                        result.top_files.back().size = fsize;
                        result.top_files.back().extension = ext;
                        // Bubble up to maintain sort order
                        for (int i = (int)result.top_files.size() - 1; i > 0; i--) {
                            if (result.top_files[i].size > result.top_files[i-1].size) {
                                std::swap(result.top_files[i], result.top_files[i-1]);
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
        } while (FindNextFileW(h, &fd));

        DWORD err = GetLastError();
        if (err != ERROR_NO_MORE_FILES) result.stats.errors++;
        FindClose(h);
    }

    result.ext_sizes = std::move(ext_sizes);
    result.ext_counts = std::move(ext_counts);

    return result;
}

void update_progress(ProgressState& prog, std::ostream& out, bool ansi) {
    int done = prog.done_count.load();
    long long bytes = prog.bytes_so_far.load();
    int total = prog.total;
    if (total == 0) return;

    int bar_w = 30;
    int filled = done * bar_w / total;

    std::ostringstream line;
    if (ansi) {
        line << "\x1b[2K\r  \x1b[36m[";
        for (int i = 0; i < filled; i++) line << "\xe2\x96\x88";
        for (int i = filled; i < bar_w; i++) line << "\xe2\x96\x91";
        line << "]\x1b[0m " << done << "/" << total
             << "  \x1b[1m" << fmt_size(bytes) << "\x1b[0m";
    } else {
        line << "\r  [";
        for (int i = 0; i < filled; i++) line << '#';
        for (int i = filled; i < bar_w; i++) line << '.';
        line << "] " << done << "/" << total << "  " << fmt_size(bytes);
    }
    out << line.str() << std::flush;
}
