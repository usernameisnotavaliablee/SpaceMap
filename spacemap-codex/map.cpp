#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cctype>

#include "types.h"
#include "scanner.h"
#include "stats.h"
#include "output.h"
#include "tui.h"
#include "mft.h"

static size_t find_json_string_end(const std::string& content, size_t start) {
    bool escaped = false;
    for (size_t i = start; i < content.size(); i++) {
        char c = content[i];
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return i;
        }
    }
    return std::string::npos;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void append_utf8_codepoint(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string unescape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }

        char esc = s[++i];
        if (esc == '"' || esc == '\\' || esc == '/') out += esc;
        else if (esc == 'b') out += '\b';
        else if (esc == 'f') out += '\f';
        else if (esc == 'n') out += '\n';
        else if (esc == 'r') out += '\r';
        else if (esc == 't') out += '\t';
        else if (esc == 'u' && i + 4 < s.size()) {
            int h1 = hex_digit(s[i + 1]);
            int h2 = hex_digit(s[i + 2]);
            int h3 = hex_digit(s[i + 3]);
            int h4 = hex_digit(s[i + 4]);
            if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                unsigned int cp = (unsigned int)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                append_utf8_codepoint(out, cp);
                i += 4;
            }
        } else {
            out += esc;
        }
    }
    return out;
}

static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (wlen <= 0) return L"";
    std::vector<wchar_t> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wbuf.data(), wlen);
    return wbuf.data();
}

static void add_top_file(std::vector<FileInfo>& top_files, int top_n, const FileInfo& fi) {
    if (top_n <= 0) return;
    if ((int)top_files.size() < top_n) {
        top_files.push_back(fi);
        if ((int)top_files.size() == top_n) {
            std::sort(top_files.begin(), top_files.end(),
                      [](const FileInfo& a, const FileInfo& b) { return a.size > b.size; });
        }
    } else if (fi.size > top_files.back().size) {
        top_files.back() = fi;
        for (int i = (int)top_files.size() - 1; i > 0; i--) {
            if (top_files[i].size > top_files[i - 1].size) {
                std::swap(top_files[i], top_files[i - 1]);
            } else {
                break;
            }
        }
    }
}

static ScanResult enumerate_root_entries(const std::wstring& target_scan,
                                         std::vector<std::wstring>& dir_names,
                                         std::vector<std::wstring>& dir_paths,
                                         int top_n) {
    ScanResult root;
    root.stats.dirs = 1;

    WIN32_FIND_DATAW fd;
    HANDLE h = find_first_fast(search_pattern(target_scan), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        root.stats.errors++;
        root.error_dirs.push_back(target_scan);
        return root;
    }

    do {
        if (is_dot_dir(fd.cFileName)) continue;
        std::wstring child_path = join_path(target_scan, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (should_skip(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                root.stats.skipped++;
                if (root.skipped_dirs.size() < ScanResult::MAX_LOGGED_DIRS)
                    root.skipped_dirs.push_back(child_path);
                continue;
            }
            dir_names.push_back(fd.cFileName);
            dir_paths.push_back(child_path);
        } else {
            long long fsize = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            root.stats.files++;
            root.stats.size += fsize;

            std::string ext = get_file_extension(fd.cFileName);
            if (!ext.empty()) {
                root.ext_sizes[ext] += fsize;
                root.ext_counts[ext]++;
            }

            FileInfo fi;
            fi.path = child_path;
            fi.size = fsize;
            fi.extension = ext;
            add_top_file(root.top_files, top_n, fi);
        }
    } while (FindNextFileW(h, &fd));

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) root.stats.errors++;
    FindClose(h);
    return root;
}

// Cache management
std::wstring get_cache_path() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"spacemap_cache.json";
}

bool is_cache_valid(const std::wstring& cache_path, int max_age_seconds) {
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExW(cache_path.c_str(), GetFileExInfoStandard, &attrs)) {
        return false;
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long long now = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    long long cached = ((long long)attrs.ftLastWriteTime.dwHighDateTime << 32) | attrs.ftLastWriteTime.dwLowDateTime;
    // Convert to seconds (100ns intervals)
    long long age = (now - cached) / 10000000;
    return age <= max_age_seconds;
}

static void save_cache(const std::wstring& cache_path, const std::wstring& target,
                       const std::vector<DirEntry>& folders, long long total_size,
                       unsigned long long total_files, unsigned long long total_dirs,
                       unsigned long long total_skipped, unsigned long long total_errors,
                       const std::vector<FileTypeStats>& file_types,
                       const std::vector<FileInfo>& top_files, double elapsed) {
    std::string path_utf8 = ws2s(cache_path);
    std::ofstream f(path_utf8.c_str());
    if (!f.is_open()) return;
    f << "{\n";
    f << "  \"path\": \"" << escape_json(ws2s(target)) << "\",\n";
    f << "  \"scan_time_seconds\": " << elapsed << ",\n";
    f << "  \"total\": {\"size\": " << total_size << ", \"files\": " << total_files
      << ", \"dirs\": " << total_dirs << ", \"skipped\": " << total_skipped << ", \"errors\": " << total_errors << "},\n";
    f << "  \"folders\": [\n";
    for (size_t i = 0; i < folders.size(); i++) {
        long long sz = folders[i].size.load(std::memory_order_relaxed);
        f << "    {\"name\": \"" << escape_json(ws2s(folders[i].name)) << "\", \"size\": " << sz
          << ", \"files\": " << folders[i].files.load(std::memory_order_relaxed)
          << ", \"dirs\": " << folders[i].dirs.load(std::memory_order_relaxed) << "}";
        if (i + 1 < folders.size()) f << ",";
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"file_types\": [\n";
    for (size_t i = 0; i < file_types.size(); i++) {
        f << "    {\"category\": \"" << escape_json(file_types[i].category) << "\", \"size\": " << file_types[i].total_size
          << ", \"count\": " << file_types[i].count << "}";
        if (i + 1 < file_types.size()) f << ",";
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"top_files\": [\n";
    for (size_t i = 0; i < top_files.size(); i++) {
        f << "    {\"path\": \"" << escape_json(ws2s(top_files[i].path)) << "\", \"size\": " << top_files[i].size << "}";
        if (i + 1 < top_files.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
}

bool load_cache(const std::wstring& cache_path,
                       std::vector<DirEntry>& folders, long long& total_size,
                       unsigned long long& total_files, unsigned long long& total_dirs,
                       unsigned long long& total_skipped, unsigned long long& total_errors,
                       std::vector<FileTypeStats>& file_types, std::vector<FileInfo>& top_files,
                       double& elapsed, std::wstring& cached_path) {
    std::string path_utf8 = ws2s(cache_path);
    std::ifstream f(path_utf8.c_str());
    if (!f.is_open()) return false;

    // Read the entire file
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Extract path (UTF-8 to wstring)
    size_t pos = content.find("\"path\": \"");
    if (pos == std::string::npos) return false;
    pos += 9;
    size_t end = find_json_string_end(content, pos);
    if (end == std::string::npos) return false;
    std::string path_str = unescape_json_string(content.substr(pos, end - pos));
    cached_path = utf8_to_wstring(path_str);

    // Extract scan_time_seconds
    pos = content.find("\"scan_time_seconds\": ");
    if (pos == std::string::npos) return false;
    pos += 20;
    elapsed = std::stod(content.substr(pos));

    // Extract total size
    pos = content.find("\"total\": {\"size\": ");
    if (pos == std::string::npos) return false;
    pos += 18;
    total_size = std::stoll(content.substr(pos));

    // Extract total files
    pos = content.find("\"files\": ", pos);
    if (pos == std::string::npos) return false;
    pos += 9;
    total_files = std::stoull(content.substr(pos));

    // Extract total dirs
    pos = content.find("\"dirs\": ", pos);
    if (pos == std::string::npos) return false;
    pos += 8;
    total_dirs = std::stoull(content.substr(pos));

    // Extract total skipped
    pos = content.find("\"skipped\": ", pos);
    if (pos == std::string::npos) return false;
    pos += 11;
    total_skipped = std::stoull(content.substr(pos));

    // Extract total errors
    pos = content.find("\"errors\": ", pos);
    if (pos == std::string::npos) return false;
    pos += 10;
    total_errors = std::stoull(content.substr(pos));

    // Extract folders
    pos = content.find("\"folders\": [");
    if (pos == std::string::npos) return false;
    pos += 12;
    while (true) {
        size_t name_pos = content.find("\"name\": \"", pos);
        if (name_pos == std::string::npos || name_pos > content.find("]", pos)) break;
        name_pos += 9;
        size_t name_end = find_json_string_end(content, name_pos);
        if (name_end == std::string::npos) break;

        DirEntry entry;
        std::string name_utf8 = unescape_json_string(content.substr(name_pos, name_end - name_pos));
        entry.name = utf8_to_wstring(name_utf8);

        size_t size_pos = content.find("\"size\": ", name_end);
        if (size_pos == std::string::npos) break;
        size_pos += 8;
        entry.size.store(std::stoll(content.substr(size_pos)), std::memory_order_relaxed);

        size_t files_pos = content.find("\"files\": ", size_pos);
        if (files_pos == std::string::npos) break;
        files_pos += 9;
        entry.files.store(std::stoull(content.substr(files_pos)), std::memory_order_relaxed);

        size_t dirs_pos = content.find("\"dirs\": ", files_pos);
        if (dirs_pos == std::string::npos) break;
        dirs_pos += 8;
        entry.dirs.store(std::stoull(content.substr(dirs_pos)), std::memory_order_relaxed);

        folders.push_back(entry);
        pos = dirs_pos + 10;
    }

    // Extract file types
    pos = content.find("\"file_types\": [");
    if (pos == std::string::npos) return false;
    pos += 15;
    while (true) {
        size_t cat_pos = content.find("\"category\": \"", pos);
        if (cat_pos == std::string::npos || cat_pos > content.find("]", pos)) break;
        cat_pos += 13;
        size_t cat_end = find_json_string_end(content, cat_pos);
        if (cat_end == std::string::npos) break;

        FileTypeStats ft;
        ft.category = unescape_json_string(content.substr(cat_pos, cat_end - cat_pos));

        size_t size_pos = content.find("\"size\": ", cat_end);
        if (size_pos == std::string::npos) break;
        size_pos += 8;
        ft.total_size = std::stoll(content.substr(size_pos));

        size_t count_pos = content.find("\"count\": ", size_pos);
        if (count_pos == std::string::npos) break;
        count_pos += 9;
        ft.count = std::stoull(content.substr(count_pos));

        file_types.push_back(ft);
        pos = count_pos + 10;
    }

    // Extract top files
    pos = content.find("\"top_files\": [");
    if (pos == std::string::npos) return false;
    pos += 14;
    while (true) {
        size_t path_pos = content.find("\"path\": \"", pos);
        if (path_pos == std::string::npos || path_pos > content.find("]", pos)) break;
        path_pos += 9;
        size_t path_end = find_json_string_end(content, path_pos);
        if (path_end == std::string::npos) break;

        FileInfo fi;
        std::string fpath_utf8 = unescape_json_string(content.substr(path_pos, path_end - path_pos));
        fi.path = utf8_to_wstring(fpath_utf8);

        size_t size_pos = content.find("\"size\": ", path_end);
        if (size_pos == std::string::npos) break;
        size_pos += 8;
        fi.size = std::stoll(content.substr(size_pos));

        top_files.push_back(fi);
        pos = size_pos + 10;
    }

    return true;
}

static int parse_int_arg(int argc, wchar_t* argv[], int i) {
    if (i + 1 < argc) return _wtoi(argv[i + 1]);
    return 0;
}

static std::string parse_str_arg(int argc, wchar_t* argv[], int i) {
    if (i + 1 < argc) {
        std::wstring ws(argv[i + 1]);
        return ws2s(ws);
    }
    return "";
}

// Check if a path is a volume root (e.g., "C:\\" or "\\\\?\\C:\\")
static bool is_volume_root(const std::wstring& path) {
    std::wstring p = path;
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') {
        p = p.substr(4);
    }
    if (p.size() >= 2 && p[1] == L':') {
        if (p.size() == 2) return true;
        if (p.size() == 3 && (p[2] == L'\\' || p[2] == L'/')) return true;
    }
    return false;
}

// Extract drive root "C:\\" from a path
static std::wstring get_drive_root(const std::wstring& path) {
    std::wstring p = path;
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') {
        p = p.substr(4);
    }
    if (p.size() >= 2 && p[1] == L':') {
        return p.substr(0, 2) + L"\\";
    }
    return L"";
}

CliOptions parse_args(int argc, wchar_t* argv[]) {
    CliOptions opts;
    for (int i = 1; i < argc; i++) {
        std::wstring arg(argv[i]);
        if (arg == L"-h" || arg == L"--help") {
            print_help(std::cout);
            exit(0);
        } else if (arg == L"-w" || arg == L"--workers") {
            opts.workers = parse_int_arg(argc, argv, i);
            if (i + 1 < argc) i++;
        } else if (arg.find(L"--workers=") == 0) {
            opts.workers = _wtoi(arg.c_str() + 10);
        } else if (arg == L"-t" || arg == L"--top") {
            opts.top_n = parse_int_arg(argc, argv, i);
            if (opts.top_n <= 0) opts.top_n = 20;
            if (i + 1 < argc) i++;
        } else if (arg.find(L"--top=") == 0) {
            opts.top_n = _wtoi(arg.c_str() + 6);
            if (opts.top_n <= 0) opts.top_n = 20;
        } else if (arg == L"-s" || arg == L"--sort") {
            opts.sort_mode = parse_str_arg(argc, argv, i);
            if (!opts.sort_mode.empty() && i + 1 < argc) i++;
        } else if (arg.find(L"--sort=") == 0) {
            opts.sort_mode = ws2s(arg.substr(7));
        } else if (arg == L"-j" || arg == L"--json") {
            opts.json_output = true;
        } else if (arg == L"-i" || arg == L"--interactive") {
            opts.interactive = true;
        } else if (arg == L"--no-color") {
            opts.no_color = true;
        } else if (arg == L"-v" || arg == L"--verbose") {
            opts.verbose = true;
        } else if (arg == L"-a" || arg == L"--all") {
            opts.show_all = true;
        } else if (arg == L"-o" || arg == L"--output") {
            opts.output_file = parse_str_arg(argc, argv, i);
            if (!opts.output_file.empty() && i + 1 < argc) i++;
        } else if (arg.find(L"--output=") == 0) {
            opts.output_file = ws2s(arg.substr(9));
        } else if (arg[0] != L'-') {
            opts.target_path = absolute_path(argv[i]);
        }
    }

    if (opts.target_path.empty()) {
        DWORD needed = GetCurrentDirectoryW(0, NULL);
        std::wstring current(needed, L'\0');
        DWORD written = GetCurrentDirectoryW(needed, &current[0]);
        current.resize(written);
        opts.target_path = current;
    }

    return opts;
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    CliOptions opts = parse_args(argc, argv);
    bool quiet_status = opts.json_output;
    if (!quiet_status) {
        std::cout << "Loading..." << std::flush;
    }
    auto clear_status = [&]() {
        if (!quiet_status) std::cout << "\r" << std::string(12, ' ') << "\r";
    };

    if (opts.sort_mode != "name" && opts.sort_mode != "size") {
        if (!opts.sort_mode.empty())
            std::cerr << "warning: unknown sort mode '" << opts.sort_mode << "', using 'size'" << std::endl;
        opts.sort_mode = "size";
    }
    setup_interrupt_handler();

    std::wstring target_display = opts.target_path;
    std::wstring target_scan = to_extended_path(target_display);

    DWORD attrs = GetFileAttributesW(target_scan.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        clear_status();
        std::cerr << "error: not a valid directory: " << ws2s(target_display) << std::endl;
        return 1;
    }

    // Enable ANSI colors
    bool ansi = false;
    if (!opts.no_color) {
        ansi = enable_ansi_colors();
    }
    opts.ansi_available = ansi;

    // Interactive mode
    if (opts.interactive) {
        clear_status();
        tui_run(target_display, opts.top_n);
        return 0;
    }

    // Output data (filled by either MFT or regular scan)
    std::vector<DirEntry> folders;
    long long total_size = 0;
    unsigned long long total_files = 0;
    unsigned long long total_dirs = 0;
    unsigned long long total_skipped = 0;
    unsigned long long total_errors = 0;
    std::vector<FileTypeStats> file_types;
    std::vector<FileInfo> top_files;
    std::vector<std::wstring> all_skipped_dirs;
    std::vector<std::wstring> all_error_dirs;
    double elapsed = 0;
    bool mft_used = false;

    // Check cache for -a flag
    bool cache_used = false;
    if (opts.show_all) {
        std::wstring cache_path = get_cache_path();
        if (is_cache_valid(cache_path, 180)) { // 3 minutes
            std::wstring cached_path;
            if (load_cache(cache_path, folders, total_size, total_files, total_dirs,
                          total_skipped, total_errors, file_types, top_files, elapsed, cached_path)
                && cached_path == target_display) {
                cache_used = true;
            }
        }
    }

    if (!cache_used) {
    // MFT fast path: volume root on NTFS
    if (is_volume_root(target_scan)) {
        std::wstring drive = get_drive_root(target_scan);
        if (!drive.empty() && is_ntfs_volume(drive)) {
            clear_status();
            if (!quiet_status) {
                if (ansi) {
                    std::cout << "\n  \x1b[1m[MFT] Scanning volume\x1b[0m "
                              << ws2s(target_display) << " ...\n" << std::endl;
                } else {
                    std::cout << "\n  [MFT] Scanning volume " << ws2s(target_display) << " ...\n" << std::endl;
                }
            }

            auto t_start = std::chrono::steady_clock::now();
            MftScanResult mft_out;
            if (get_dir_size_mft(target_scan, opts.top_n, mft_out)) {
                auto t_end = std::chrono::steady_clock::now();
                elapsed = std::chrono::duration<double>(t_end - t_start).count();
                mft_used = true;

                total_size = mft_out.scan.stats.size;
                total_files = mft_out.scan.stats.files;
                total_dirs = mft_out.scan.stats.dirs;
                total_skipped = 0;
                total_errors = 0;

                folders = std::move(mft_out.folders);
                top_files = std::move(mft_out.scan.top_files);

                // Build file_types from ext_sizes/ext_counts
                std::vector<ScanResult> single_result;
                single_result.push_back(std::move(mft_out.scan));
                file_types = aggregate_file_types(single_result);
            }
        }
    }

    if (!mft_used) {
    // Regular scan path
    std::vector<std::wstring> dir_names;
    std::vector<std::wstring> dir_paths;

    auto t_start = std::chrono::steady_clock::now();
    ScanResult root_result = enumerate_root_entries(target_scan, dir_names, dir_paths, opts.top_n);

    int total_count = (int)dir_paths.size();

    // Setup progress
    ProgressState prog;
    prog.total = total_count;

    int workers = worker_count_for(total_count, opts.workers);

    clear_status();
    if (!quiet_status && total_count > 0) {
        if (ansi) {
            std::cout << "\n  \x1b[1mScanning " << total_count << " folders\x1b[0m in "
                      << ws2s(target_display) << " with " << workers << " workers ...\n" << std::endl;
        } else {
            std::cout << "\n  Scanning " << total_count << " folders in "
                      << ws2s(target_display) << " with " << workers << " workers ...\n" << std::endl;
        }
    }

    // Results storage
    std::vector<ScanResult> results(total_count);

    // Launch worker threads
    std::atomic<int> next_index(0);
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (int t = 0; t < workers; t++) {
        threads.emplace_back([&]() {
            for (;;) {
                int i = next_index.fetch_add(1);
                if (i >= total_count) break;

                ScanResult result;
                try {
                    result = get_dir_size(dir_paths[i], opts.top_n);
                } catch (...) {
                    result.stats.errors++;
                }
                long long sz = result.stats.size;
                results[i] = std::move(result);

                prog.done_count.fetch_add(1);
                prog.bytes_so_far.fetch_add(sz);
            }
        });
    }

    // Progress animation in main thread (skip in JSON mode for clean output)
    if (!quiet_status && total_count > 0) {
        while (prog.done_count.load() < total_count && !g_interrupted.load()) {
            {
                std::lock_guard<std::mutex> lock(prog.mtx);
                update_progress(prog, std::cout, ansi);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Final progress update
        {
            std::lock_guard<std::mutex> lock(prog.mtx);
            update_progress(prog, std::cout, ansi);
        }
    } else if (total_count > 0) {
        // JSON mode: just wait quietly
        while (prog.done_count.load() < total_count && !g_interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Wait for all threads
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i].join();
    }

    // Clear progress line and any leftover output
    if (!quiet_status && total_count > 0) {
        if (ansi) {
            std::cout << "\x1b[2K\r" << std::flush;
        } else {
            std::cout << "\r" << std::string(100, ' ') << "\r" << std::flush;
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    elapsed = std::chrono::duration<double>(t_end - t_start).count();

    // Build folder list
    for (int i = 0; i < total_count; i++) {
        DirEntry entry;
        entry.name = dir_names[i];
        entry.size.store(results[i].stats.size, std::memory_order_relaxed);
        entry.files.store(results[i].stats.files, std::memory_order_relaxed);
        entry.dirs.store(results[i].stats.dirs, std::memory_order_relaxed);
        folders.push_back(entry);
    }

    total_size = root_result.stats.size;
    total_files = root_result.stats.files;
    total_dirs = root_result.stats.dirs;
    total_skipped = root_result.stats.skipped;
    total_errors = root_result.stats.errors;

    all_skipped_dirs.insert(all_skipped_dirs.end(),
                            root_result.skipped_dirs.begin(), root_result.skipped_dirs.end());
    all_error_dirs.insert(all_error_dirs.end(),
                          root_result.error_dirs.begin(), root_result.error_dirs.end());

    // Aggregate stats
    for (int i = 0; i < total_count; i++) {
        total_size += results[i].stats.size;
        total_files += results[i].stats.files;
        total_dirs += results[i].stats.dirs;
        total_skipped += results[i].stats.skipped;
        total_errors += results[i].stats.errors;
    }

    // Aggregate skipped and error directories
    for (int i = 0; i < total_count; i++) {
        all_skipped_dirs.insert(all_skipped_dirs.end(),
                                results[i].skipped_dirs.begin(), results[i].skipped_dirs.end());
        all_error_dirs.insert(all_error_dirs.end(),
                              results[i].error_dirs.begin(), results[i].error_dirs.end());
    }

    // Aggregate file types and top files, including files directly under target.
    std::vector<ScanResult> aggregate_results;
    aggregate_results.reserve(results.size() + 1);
    aggregate_results.push_back(root_result);
    aggregate_results.insert(aggregate_results.end(), results.begin(), results.end());
    file_types = aggregate_file_types(aggregate_results);
    top_files = merge_top_files(aggregate_results, opts.top_n);
    } // end !mft_used
    } // end !cache_used

    // Sort folders
    if (opts.sort_mode == "name") {
        std::sort(folders.begin(), folders.end(),
                  [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    } else {
        std::sort(folders.begin(), folders.end(),
                  [](const DirEntry& a, const DirEntry& b) { return a.size.load(std::memory_order_relaxed) > b.size.load(std::memory_order_relaxed); });
    }

    // Save cache for future -a usage
    if (!cache_used) {
        std::wstring cache_path = get_cache_path();
        save_cache(cache_path, target_display, folders, total_size,
                   total_files, total_dirs, total_skipped, total_errors,
                   file_types, top_files, elapsed);
    }

    // Output
    std::ostream* out = &std::cout;
    std::ofstream file_stream;
    bool writing_file = false;
    if (!opts.output_file.empty()) {
        file_stream.open(opts.output_file, std::ios::binary);
        if (file_stream.is_open()) {
            out = &file_stream;
            writing_file = true;
        } else {
            std::cerr << "warning: cannot open output file: " << opts.output_file << std::endl;
        }
    }

    if (opts.json_output) {
        // UTF-8 BOM for Windows Notepad compatibility
        if (writing_file) {
            file_stream.write("\xEF\xBB\xBF", 3);
        }
        print_json_report(*out, target_display, folders, total_size,
                          total_files, total_dirs, total_skipped, total_errors,
                          file_types, top_files, elapsed);
    } else {
        print_text_report(*out, target_display, folders, total_size,
                          total_files, total_dirs, total_skipped, total_errors,
                          file_types, top_files, elapsed, ansi,
                          opts.verbose, opts.show_all, all_skipped_dirs, all_error_dirs);
    }

    return 0;
}
