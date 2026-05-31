#ifndef SPACEMAP_TUI_H
#define SPACEMAP_TUI_H

#include "types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include <Windows.h>

// Forward declaration — defined in scanner.h/scanner.cpp
extern std::atomic<bool> g_interrupted;

struct TuiState {
    std::wstring current_path;
    std::vector<DirEntry> entries;
    int selected_index;
    int scroll_offset;
    int viewport_height;
    int viewport_width;
    std::vector<std::wstring> breadcrumb;
    std::atomic<bool> running;
    std::unordered_map<std::wstring, ScanResult> scan_cache;
    std::mutex mtx;
    int top_n;
    std::vector<std::wstring> path_stack;
    bool sort_by_size;
    std::atomic<bool> show_top_files;
    std::vector<FileInfo> top_files_list;
    std::atomic<bool> scanning;
    std::atomic<bool> scan_active;   // separate signal for scan thread lifecycle
    std::atomic<bool> top_files_active; // signal for top files thread
    std::thread scan_thread;
    std::atomic<bool> top_files_loading;
    std::thread top_files_thread;

    TuiState() : selected_index(0), scroll_offset(0),
                 viewport_height(25), viewport_width(80),
                 running(true), top_n(20),
                 sort_by_size(true), show_top_files(false), scanning(false),
                 scan_active(false), top_files_active(false), top_files_loading(false) {}

    ~TuiState() {
        running = false;
        scan_active = false;
        top_files_active = false;
        g_interrupted.store(true);
        if (scan_thread.joinable()) scan_thread.join();
        if (top_files_thread.joinable()) top_files_thread.join();
    }
};

void tui_run(const std::wstring& initial_path, int top_n);

#endif
