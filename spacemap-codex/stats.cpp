#include "stats.h"
#include <algorithm>

std::string categorize_extension(const std::string& ext) {
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" ||
        ext == ".wmv" || ext == ".flv" || ext == ".webm" || ext == ".m4v" ||
        ext == ".mpg" || ext == ".mpeg")
        return "Video";
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
        ext == ".bmp" || ext == ".svg" || ext == ".webp" || ext == ".ico" ||
        ext == ".tiff" || ext == ".heic" || ext == ".heif" || ext == ".avif")
        return "Image";
    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".aac" ||
        ext == ".ogg" || ext == ".wma" || ext == ".m4a")
        return "Audio";
    if (ext == ".pdf" || ext == ".doc" || ext == ".docx" || ext == ".xls" ||
        ext == ".xlsx" || ext == ".ppt" || ext == ".pptx" || ext == ".txt" ||
        ext == ".rtf" || ext == ".odt" || ext == ".csv" || ext == ".log" ||
        ext == ".ini" || ext == ".cfg" || ext == ".env" || ext == ".conf" ||
        ext == ".properties" || ext == ".rst")
        return "Document";
    if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" ||
        ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".iso" || ext == ".cab" ||
        ext == ".img" || ext == ".vmdk" || ext == ".vhd" || ext == ".vhdx" ||
        ext == ".lz" || ext == ".lz4" || ext == ".zst")
        return "Archive";
    if (ext == ".cpp" || ext == ".h" || ext == ".c" || ext == ".py" ||
        ext == ".js" || ext == ".ts" || ext == ".java" || ext == ".cs" ||
        ext == ".go" || ext == ".rs" || ext == ".html" || ext == ".css" ||
        ext == ".json" || ext == ".xml" || ext == ".yaml" || ext == ".yml" || ext == ".toml" ||
        ext == ".gradle" || ext == ".lock" ||
        ext == ".md" || ext == ".sh" || ext == ".rb" || ext == ".php" ||
        ext == ".swift" || ext == ".kt" || ext == ".dart" || ext == ".vue" ||
        ext == ".tsx" || ext == ".jsx" || ext == ".lua" || ext == ".r" ||
        ext == ".pl" || ext == ".scala" || ext == ".groovy" ||
        ext == ".sln" || ext == ".csproj" || ext == ".vcxproj" || ext == ".cmake" ||
        ext == ".wasm" || ext == ".sql")
        return "Code";
    if (ext == ".exe" || ext == ".dll" || ext == ".sys" || ext == ".msi" ||
        ext == ".bat" || ext == ".cmd")
        return "Executable";
    if (ext == ".db" || ext == ".sqlite" || ext == ".sqlite3" ||
        ext == ".mdb" || ext == ".accdb")
        return "Database";
    if (ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2" || ext == ".eot")
        return "Font";
    if (ext == ".psd" || ext == ".ai" || ext == ".sketch" || ext == ".fig" ||
        ext == ".xcf" || ext == ".indd")
        return "Design";
    return "Other";
}

std::vector<FileTypeStats> aggregate_file_types(
    const std::vector<ScanResult>& results) {

    std::map<std::string, long long> cat_sizes;
    std::map<std::string, unsigned long long> cat_counts;

    for (size_t i = 0; i < results.size(); i++) {
        for (auto it = results[i].ext_sizes.begin(); it != results[i].ext_sizes.end(); ++it) {
            std::string cat = categorize_extension(it->first);
            cat_sizes[cat] += it->second;
        }
        for (auto it2 = results[i].ext_counts.begin(); it2 != results[i].ext_counts.end(); ++it2) {
            std::string cat = categorize_extension(it2->first);
            cat_counts[cat] += it2->second;
        }
    }

    std::vector<FileTypeStats> types;
    std::map<std::string, long long>::const_iterator it;
    for (it = cat_sizes.begin(); it != cat_sizes.end(); ++it) {
        FileTypeStats ft;
        ft.category = it->first;
        ft.total_size = it->second;
        ft.count = cat_counts[it->first];
        types.push_back(ft);
    }

    std::sort(types.begin(), types.end(),
              [](const FileTypeStats& a, const FileTypeStats& b) { return a.total_size > b.total_size; });

    return types;
}

std::vector<FileInfo> merge_top_files(
    const std::vector<ScanResult>& results, int top_n) {

    std::vector<FileInfo> all;
    for (size_t i = 0; i < results.size(); i++) {
        for (size_t j = 0; j < results[i].top_files.size(); j++) {
            all.push_back(results[i].top_files[j]);
        }
    }

    std::sort(all.begin(), all.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.size > b.size; });

    if ((int)all.size() > top_n) {
        all.resize(top_n);
    }
    return all;
}
