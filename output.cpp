#include "output.h"
#include "scanner.h"
#include <sstream>
#include <iomanip>
#include <fstream>

bool enable_ansi_colors() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return false;
    mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    if (!SetConsoleMode(hOut, mode)) return false;
    DWORD check = 0;
    GetConsoleMode(hOut, &check);
    return (check & 0x0004) != 0;
}

std::string color_for_size(long long bytes, bool ansi) {
    if (!ansi) return "";
    if (bytes == 0) return "\x1b[2m"; // dim for zero size

    // RGB gradient: green -> yellow -> red
    // 0-100MB: pure green (0, 200, 0)
    // 100MB-500MB: green to yellow (0,200,0) -> (255,255,0)
    // 500MB-1GB: yellow to red (255,255,0) -> (255,0,0)
    // >1GB: pure red (255, 0, 0)

    int r, g;
    long long mb = bytes / (1024 * 1024);

    if (mb <= 100) {
        r = 0;
        g = 200;
    } else if (mb <= 500) {
        // green to yellow: 0,200,0 -> 255,255,0
        double t = (double)(mb - 100) / 400.0;
        r = (int)(255 * t);
        g = 200 + (int)(55 * t);
    } else if (mb <= 1024) {
        // yellow to red: 255,255,0 -> 255,0,0
        double t = (double)(mb - 500) / 524.0;
        r = 255;
        g = 255 - (int)(255 * t);
    } else {
        r = 255;
        g = 0;
    }

    std::ostringstream ss;
    ss << "\x1b[38;2;" << r << ";" << g << ";0m";
    return ss.str();
}

std::string render_bar(double fraction, int width, bool ansi, long long size_bytes) {
    std::string result;
    int filled = (int)(fraction * width);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    // At least 1 block if size > 0
    if (filled == 0 && size_bytes > 0) filled = 1;

    if (ansi) {
        std::string clr = color_for_size(size_bytes, ansi);
        result += clr;
        for (int i = 0; i < filled; i++) result += "\xe2\x96\x88"; // U+2588 full block
        result += "\x1b[0m";
        for (int i = filled; i < width; i++) result += ' ';
    } else {
        // 无 ANSI 颜色时仍用 █ 实心块（控制台已设 UTF-8 代码页），不退化成 #
        for (int i = 0; i < filled; i++) result += "\xe2\x96\x88";
        for (int i = filled; i < width; i++) result += ' ';
    }
    return result;
}

std::string escape_json(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        }
        else out += (char)c;
    }
    return out;
}

std::string fmt_time(double seconds) {
    if (seconds < 60) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << seconds << "s";
        return ss.str();
    }
    int m = (int)(seconds / 60);
    int s = (int)seconds % 60;
    std::ostringstream ss;
    ss << m << "m" << s << "s";
    return ss.str();
}

void print_help(std::ostream& out) {
    out << "\n  SpaceMap - disk space analyzer\n"
        << "\n  Usage: map [OPTIONS] [PATH]\n"
        << "\n  Options:\n"
        << "    -w, --workers N      Worker thread count (default: auto)\n"
        << "    -t, --top N          Show top N largest files (default: 0, disabled)\n"
        << "    -s, --sort MODE      Sort by: size, name (default: size)\n"
        << "    -o, --output FILE    Write results to file\n"
        << "    -i, --interactive    Enter interactive TUI mode\n"
        << "    -a, --all            Show all folders (cached, 3min TTL)\n"
        << "    -v, --verbose        Show skipped/error directories\n"
        << "        --no-color       Disable ANSI colors\n"
        << "    -h, --help           Show this help\n"
        << std::endl;
}

static void print_separator(std::ostream& out, bool ansi, int width) {
    if (ansi) {
        out << "  \x1b[2m";
        for (int i = 0; i < width; i++) out << "\xe2\x94\x80"; // U+2500
        out << "\x1b[0m\n";
    } else {
        out << "  ";
        for (int i = 0; i < width; i++) out << '-';
        out << "\n";
    }
}

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
    const std::vector<std::wstring>& error_dirs) {

    // ⑤ Get terminal width for dynamic sizing
    int console_w = 120;
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (hOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hOut, &info)) {
            console_w = info.srWindow.Right - info.srWindow.Left + 1;
        }
    }
    int sep_width = console_w > 4 ? console_w - 4 : 76;
    long long threshold = 5LL * 1024 * 1024; // 5MB

    // Check if we should hide small folders (>=40% are >5MB)
    int big_count = 0;
    for (size_t i = 0; i < folders.size(); i++) {
        if (folders[i].size.load(std::memory_order_relaxed) >= threshold) big_count++;
    }
    bool hide_small = !show_all && folders.size() >= 3 &&
                      (double)big_count / folders.size() >= 0.4;

    // Split into big and small folders
    std::vector<DirEntry> display_folders;
    std::vector<DirEntry> small_folders;
    long long small_total = 0;
    unsigned long long small_files = 0;

    for (size_t i = 0; i < folders.size(); i++) {
        long long fsize = folders[i].size.load(std::memory_order_relaxed);
        // Always hide 0B folders (unless show_all); additionally hide <5MB if threshold met
        if (!show_all && (fsize == 0 || (hide_small && fsize < threshold))) {
            small_folders.push_back(folders[i]);
            small_total += fsize;
            small_files += folders[i].files.load(std::memory_order_relaxed);
        } else {
            display_folders.push_back(folders[i]);
        }
    }

    // Calculate max name width (from all visible folders)
    size_t name_w = 0;
    for (size_t i = 0; i < display_folders.size(); i++) {
        if (display_folders[i].name.size() > name_w) name_w = display_folders[i].name.size();
    }
    if (!hide_small) {
        for (size_t i = 0; i < small_folders.size(); i++) {
            if (small_folders[i].name.size() > name_w) name_w = small_folders[i].name.size();
        }
    }
    if (name_w < 20) name_w = 20;

    // ⑤ Dynamic bar width based on terminal and name width
    int bar_max = console_w > (int)name_w + 30 ? console_w - (int)name_w - 22 : 20;
    if (bar_max > 60) bar_max = 60;
    if (bar_max < 10) bar_max = 10;

    // Header
    out << "\n";
    if (ansi) {
        out << "  \x1b[1;36m" << ws2s(target) << "\x1b[0m\n";
    } else {
        out << "  " << ws2s(target) << "\n";
    }

    // Stats line
    out << "  " << folders.size() << " folders  |  "
        << "total " << fmt_size(total_size)
        << "  |  files " << total_files
        << "  |  dirs " << total_dirs << "\n";
    if (total_skipped || total_errors) {
        if (ansi) {
            out << "  \x1b[2mskipped " << total_skipped << " folders  |  errors "
                << total_errors << "\x1b[0m\n";
        } else {
            out << "  skipped " << total_skipped << " folders  |  errors "
                << total_errors << "\n";
        }
    }
    out << "\n";

    print_separator(out, ansi, sep_width);

    // Folder chart
    if (ansi) {
        out << "  \x1b[1m" << std::left << std::setw(name_w) << "Folder"
            << "  " << std::right << std::setw(10) << "Size"
            << "  Bar\x1b[0m\n";
    } else {
        out << "  " << std::left << std::setw(name_w) << "Folder"
            << "  " << std::right << std::setw(10) << "Size"
            << "  Bar\n";
    }

    long long max_size = display_folders.empty() ? 1 : display_folders[0].size.load(std::memory_order_relaxed);

    for (size_t i = 0; i < display_folders.size(); i++) {
        long long dsize = display_folders[i].size.load(std::memory_order_relaxed);
        double pct = total_size > 0 ? (double)dsize / total_size * 100.0 : 0.0;
        double frac = max_size > 0 ? (double)dsize / max_size : 0.0;
        std::string bar = render_bar(frac, bar_max, ansi, dsize);
        std::string clr = color_for_size(dsize, ansi);
        std::string rst = ansi ? "\x1b[0m" : "";

        std::string name_str = ws2s(display_folders[i].name);
        // ⑨ Truncate long names for text report
        if (name_str.size() > (size_t)(name_w + 10)) {
            name_str = name_str.substr(0, name_w + 7) + "...";
        }
        out << "  " << clr << std::left << std::setw(name_w) << name_str << rst
            << "  " << std::right << std::setw(10) << fmt_size(dsize)
            << "  " << bar
            << "  " << std::fixed << std::setprecision(1) << std::setw(5) << pct << "%"
            << "\n";
    }

    // Show folded small folders
    if (!small_folders.empty()) {
        if (ansi) {
            out << "  \x1b[2m" << small_folders.size() << " folders hidden ("
                << fmt_size(small_total) << ")\x1b[0m\n";
        } else {
            out << "  " << small_folders.size() << " folders hidden ("
                << fmt_size(small_total) << ")\n";
        }
    }

    // File types section
    if (!file_types.empty()) {
        out << "\n";
        print_separator(out, ansi, sep_width);
        if (ansi) {
            out << "  \x1b[1mFile Types\x1b[0m\n";
        } else {
            out << "  File Types\n";
        }

        long long max_cat_size = file_types.empty() ? 1 : file_types[0].total_size;

        for (size_t i = 0; i < file_types.size(); i++) {
            double pct = total_size > 0 ? (double)file_types[i].total_size / total_size * 100.0 : 0.0;
            double frac = max_cat_size > 0 ? (double)file_types[i].total_size / max_cat_size : 0.0;
            std::string bar = render_bar(frac, bar_max, ansi, file_types[i].total_size);

            out << "  " << std::left << std::setw(name_w) << file_types[i].category
                << "  " << std::right << std::setw(10) << fmt_size(file_types[i].total_size)
                << "  " << bar
                << "  " << std::fixed << std::setprecision(1) << std::setw(5) << pct << "%"
                << "\n";
        }
    }

    // Top files section
    if (!top_files.empty()) {
        out << "\n";
        print_separator(out, ansi, sep_width);
        if (ansi) {
            out << "  \x1b[1mTop " << top_files.size() << " Largest Files\x1b[0m\n";
        } else {
            out << "  Top " << top_files.size() << " Largest Files\n";
        }

        // ③ Convert target once outside the loop
        std::string target_str = ws2s(target);

        for (size_t i = 0; i < top_files.size(); i++) {
            std::string path_str = ws2s(top_files[i].path);
            // Strip \\?\ prefix
            if (path_str.find("\\\\?\\") == 0) {
                path_str = path_str.substr(4);
            }
            // Show relative path from target
            if (path_str.find(target_str) == 0) {
                path_str = path_str.substr(target_str.size());
                if (!path_str.empty() && (path_str[0] == '\\' || path_str[0] == '/')) {
                    path_str = path_str.substr(1);
                }
            }

            std::string clr = color_for_size(top_files[i].size, ansi);
            std::string rst = ansi ? "\x1b[0m" : "";

            out << "  " << std::right << std::setw(3) << (i + 1) << ".  "
                << clr << std::left << std::setw(50) << path_str << rst
                << "  " << std::right << std::setw(10) << fmt_size(top_files[i].size)
                << "\n";
        }
    }

    // Verbose: show skipped directories
    if (verbose && !skipped_dirs.empty()) {
        out << "\n";
        print_separator(out, ansi, sep_width);
        if (ansi) {
            out << "  \x1b[1mSkipped Directories (" << skipped_dirs.size() << ")\x1b[0m\n";
        } else {
            out << "  Skipped Directories (" << skipped_dirs.size() << ")\n";
        }
        for (size_t i = 0; i < skipped_dirs.size(); i++) {
            std::string dir_str = ws2s(skipped_dirs[i]);
            // Strip \\?\ prefix
            if (dir_str.find("\\\\?\\") == 0) {
                dir_str = dir_str.substr(4);
            }
            if (ansi) {
                out << "  \x1b[2m" << dir_str << "\x1b[0m\n";
            } else {
                out << "  " << dir_str << "\n";
            }
        }
    }

    // Verbose: show error directories
    if (verbose && !error_dirs.empty()) {
        out << "\n";
        print_separator(out, ansi, sep_width);
        if (ansi) {
            out << "  \x1b[1;31mError Directories (" << error_dirs.size() << ")\x1b[0m\n";
        } else {
            out << "  Error Directories (" << error_dirs.size() << ")\n";
        }
        for (size_t i = 0; i < error_dirs.size(); i++) {
            std::string dir_str = ws2s(error_dirs[i]);
            // Strip \\?\ prefix
            if (dir_str.find("\\\\?\\") == 0) {
                dir_str = dir_str.substr(4);
            }
            if (ansi) {
                out << "  \x1b[31m" << dir_str << "\x1b[0m\n";
            } else {
                out << "  " << dir_str << "\n";
            }
        }
    }

    // Footer
    out << "\n";
    print_separator(out, ansi, sep_width);
    if (ansi) {
        out << "  \x1b[2mDone in " << fmt_time(elapsed) << "\x1b[0m\n\n";
        out << "  \x1b[2m-h help  -i interactive  -t N top files  -a show all folders\x1b[0m\n\n";
    } else {
        out << "  Done in " << fmt_time(elapsed) << "\n\n";
        out << "  -h help  -i interactive  -t N top files  -a show all folders\n\n";
    }
}
