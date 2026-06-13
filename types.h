#ifndef SPACEMAP_TYPES_H
#define SPACEMAP_TYPES_H

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <Windows.h>

struct CliOptions {
    int workers;
    int top_n;
    std::string sort_mode;
    bool interactive;
    bool no_color;
    bool verbose;
    bool show_all;
    bool ansi_available;
    std::string output_file;
    std::wstring target_path;

    CliOptions()
        : workers(0), top_n(0), sort_mode("size"),
          interactive(false), no_color(false), verbose(false), show_all(false), ansi_available(false) {}
};

struct ScanStats {
    long long size = 0;
    unsigned long long files = 0;
    unsigned long long dirs = 0;
    unsigned long long skipped = 0;
    unsigned long long errors = 0;
};

struct FileInfo {
    std::wstring path;
    long long size;
    std::string extension;
};

struct DirEntry {
    std::wstring name;
    std::atomic<long long> size;
    std::atomic<unsigned long long> files;
    std::atomic<unsigned long long> dirs;

    DirEntry() : size(0), files(0), dirs(0) {}
    DirEntry(const DirEntry& o) : name(o.name), size(o.size.load(std::memory_order_relaxed)),
                                   files(o.files.load(std::memory_order_relaxed)),
                                   dirs(o.dirs.load(std::memory_order_relaxed)) {}
    DirEntry& operator=(const DirEntry& o) {
        name = o.name;
        size.store(o.size.load(std::memory_order_relaxed), std::memory_order_relaxed);
        files.store(o.files.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dirs.store(o.dirs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
    DirEntry(DirEntry&& o) : name(std::move(o.name)), size(o.size.load(std::memory_order_relaxed)),
                              files(o.files.load(std::memory_order_relaxed)),
                              dirs(o.dirs.load(std::memory_order_relaxed)) {}
    DirEntry& operator=(DirEntry&& o) {
        name = std::move(o.name);
        size.store(o.size.load(std::memory_order_relaxed), std::memory_order_relaxed);
        files.store(o.files.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dirs.store(o.dirs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

struct ScanResult {
    ScanStats stats;
    std::vector<FileInfo> top_files;
    std::unordered_map<std::string, long long> ext_sizes;
    std::unordered_map<std::string, unsigned long long> ext_counts;
    std::vector<std::wstring> skipped_dirs;
    std::vector<std::wstring> error_dirs;

    static const size_t MAX_LOGGED_DIRS = 1000;
};

#endif
