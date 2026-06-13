#ifndef SPACEMAP_SCANNER_H
#define SPACEMAP_SCANNER_H

#include "stats.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

// Global interrupt flag (set by Ctrl+C handler)
extern std::atomic<bool> g_interrupted;
void setup_interrupt_handler();

// Helper functions
bool is_dot_dir(const wchar_t* name);
bool should_skip(const wchar_t* name);
std::wstring join_path(const std::wstring& base, const wchar_t* name);
std::wstring search_pattern(const std::wstring& dir);
std::wstring absolute_path(const wchar_t* input);
std::wstring to_extended_path(const std::wstring& path);
std::string ws2s(const std::wstring& ws);
std::string fmt_size(long long b);
HANDLE find_first_fast(const std::wstring& pattern, WIN32_FIND_DATAW* fd);
int worker_count_for(int total_count, int requested_workers);

// Scanning
ScanResult get_dir_size(const std::wstring& root, int top_n);

// Progress tracking (shared state)
struct ProgressState {
    std::atomic<int> done_count;
    std::atomic<long long> bytes_so_far;
    std::mutex mtx;
    int total;

    ProgressState() : done_count(0), bytes_so_far(0), total(0) {}
};

// 渲染进度条。tip_start>=0 且 ansi 为真时，在进度条下方显示一条随时间轮换的小贴士。
void update_progress(ProgressState& prog, std::ostream& out, bool ansi,
                     int tip_start = -1, long long elapsed_ms = 0);

// 不确定进度（如 MFT 扫描）时，渲染一行流动的扫描动画 + 下方滚动贴士（仅 ANSI）。
// label 为动画前缀文字（如 "[MFT] Scanning"）。
void update_indeterminate(std::ostream& out, const std::string& label,
                          int tip_start, long long elapsed_ms);

// Persistent cache helpers (shared between CLI and TUI)
std::wstring get_cache_path();
bool is_cache_valid(const std::wstring& cache_path, int max_age_seconds);
bool load_cache(const std::wstring& cache_path,
                std::vector<DirEntry>& folders, long long& total_size,
                unsigned long long& total_files, unsigned long long& total_dirs,
                unsigned long long& total_skipped, unsigned long long& total_errors,
                std::vector<FileTypeStats>& file_types, std::vector<FileInfo>& top_files,
                double& elapsed, std::wstring& cached_path);

inline BOOL WINAPI console_ctrl_handler(DWORD) { g_interrupted.store(true); return TRUE; }

#endif
