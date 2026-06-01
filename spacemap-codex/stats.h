#ifndef SPACEMAP_STATS_H
#define SPACEMAP_STATS_H

#include "types.h"
#include <string>
#include <map>
#include <vector>

std::string categorize_extension(const std::string& ext);

struct FileTypeStats {
    std::string category;
    long long total_size;
    unsigned long long count;
};

std::vector<FileTypeStats> aggregate_file_types(
    const std::vector<ScanResult>& results);

std::vector<FileInfo> merge_top_files(
    const std::vector<ScanResult>& results, int top_n);

#endif
