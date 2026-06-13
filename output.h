#ifndef SPACEMAP_OUTPUT_H
#define SPACEMAP_OUTPUT_H

#include "types.h"
#include "stats.h"
#include <iostream>
#include <string>
#include <vector>

bool enable_ansi_colors();
std::string color_for_size(long long bytes, bool ansi);
std::string render_bar(double fraction, int width, bool ansi, long long size_bytes);
std::string escape_json(const std::string& s);
std::string fmt_time(double seconds);

void print_help(std::ostream& out);

void print_text_report(
    std::ostream& out,
    const std::wstring& target,
    const std::vector<DirEntry>& folders,
    long long total_size,
    unsigned long long total_files,
    unsigned long long total_dirs,
    unsigned long long total_skipped,
    unsigned long long total_errors,
    const std::vector<FileTypeStats>& file_types,
    const std::vector<FileInfo>& top_files,
    double elapsed,
    bool ansi,
    bool verbose,
    bool show_all,
    const std::vector<std::wstring>& skipped_dirs,
    const std::vector<std::wstring>& error_dirs);

#endif
