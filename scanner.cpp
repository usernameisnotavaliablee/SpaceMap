#include "scanner.h"
#include "tips.h"
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
    // 点开头目录（.cache/.git/.gradle ...）在 Windows 上是普通目录，需正常扫描。
    // "." 与 ".." 已在每个调用点由 is_dot_dir() 过滤，此处无需再判断。
    if (name[0] == L'$') {
        return wcscmp(name, L"$RECYCLE.BIN") == 0 ||
               wcscmp(name, L"$WinREAgent") == 0;
    }
    return wcscmp(name, L"System Volume Information") == 0 ||
           wcscmp(name, L"Recovery") == 0 ||
           wcscmp(name, L"PerfLogs") == 0;
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

// 估算一个 Unicode 码点的终端显示宽度（CJK/全角/emoji 记 2 列，其余记 1 列）
static int cp_width(unsigned int cp) {
    if (cp == 0) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK 部首、标点
        (cp >= 0x3041 && cp <= 0x33FF) ||   // 假名、CJK 符号
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK 扩展 A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 统一表意
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul 音节
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容
        (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角 ASCII
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) || // emoji
        (cp >= 0x20000 && cp <= 0x3FFFD)) { // CJK 扩展 B+
        return 2;
    }
    return 1;
}

// 把 UTF-8 字符串按显示宽度截断到 max_cols 列，超出则以 … 结尾
static std::string truncate_to_width(const std::string& s, int max_cols) {
    if (max_cols <= 0) return std::string();
    std::string out;
    int cols = 0;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len; unsigned int cp;
        if (c < 0x80)      { len = 1; cp = c; }
        else if (c < 0xE0) { len = 2; cp = c & 0x1F; }
        else if (c < 0xF0) { len = 3; cp = c & 0x0F; }
        else               { len = 4; cp = c & 0x07; }
        if (i + (size_t)len > n) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        int w = cp_width(cp);
        if (cols + w > max_cols) {
            out += "\xe2\x80\xa6"; // …
            break;
        }
        out.append(s, i, len);
        cols += w;
        i += (size_t)len;
    }
    return out;
}

// 查询当前控制台宽度（失败时返回 80）
static int console_width() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hOut, &info)) {
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        if (w > 0) return w;
    }
    return 80;
}

void update_progress(ProgressState& prog, std::ostream& out, bool ansi,
                     int tip_start, long long elapsed_ms) {
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

        // 进度条下方滚动显示小贴士（仅 ANSI 模式，需要光标移动）
        if (tip_start >= 0) {
            const char* tip = tip_for_elapsed(tip_start, elapsed_ms, 2500);
            int max_cols = console_width() - 4;
            std::string shown = truncate_to_width(tip, max_cols);
            // 换行写贴士行，清掉残留，再把光标移回进度条行行首
            line << "\n\x1b[2K  \x1b[2m" << shown << "\x1b[0m\x1b[1A\r";
        }
    } else {
        line << "\r  [";
        // 无 ANSI 时仍用 █/░ 块字符（控制台已设 UTF-8 代码页），不退化成 #/.
        for (int i = 0; i < filled; i++) line << "\xe2\x96\x88";
        for (int i = filled; i < bar_w; i++) line << "\xe2\x96\x91";
        line << "] " << done << "/" << total << "  " << fmt_size(bytes);
    }
    out << line.str() << std::flush;
}

void update_indeterminate(std::ostream& out, const std::string& label,
                          int tip_start, long long elapsed_ms) {
    const int bar_w = 30;
    // 一段宽度为 6 的亮块在 30 格内来回流动
    const int block = 6;
    int span = bar_w - block;            // 0..span
    long long step = elapsed_ms / 120;   // 每 120ms 移一格
    int period = span * 2;
    int phase = period > 0 ? (int)(step % period) : 0;
    int pos = phase <= span ? phase : period - phase;  // 三角波，来回弹

    std::ostringstream line;
    line << "\x1b[2K\r  \x1b[1m" << label << "\x1b[0m \x1b[36m[";
    for (int i = 0; i < bar_w; i++) {
        if (i >= pos && i < pos + block) line << "\xe2\x96\x88"; // █
        else line << "\xe2\x96\x91";                             // ░
    }
    line << "]\x1b[0m";

    const char* tip = tip_for_elapsed(tip_start, elapsed_ms, 2500);
    int max_cols = console_width() - 4;
    std::string shown = truncate_to_width(tip, max_cols);
    line << "\n\x1b[2K  \x1b[2m" << shown << "\x1b[0m\x1b[1A\r";

    out << line.str() << std::flush;
}
