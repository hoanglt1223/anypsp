// SPDX-License-Identifier: GPL-2.0-or-later

#include "disc.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <fstream>
#include <set>
#include <vector>

namespace anypsp::disc {
namespace fs = std::filesystem;
namespace {

constexpr std::uint64_t kSector = 2048u;
constexpr int kMaxDepth = 32;

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8u |
           static_cast<std::uint32_t>(p[2]) << 16u | static_cast<std::uint32_t>(p[3]) << 24u;
}

struct Entry {
    std::string name;
    std::uint32_t extent{};
    std::uint32_t size{};
    bool directory{};
};

class IsoReader {
public:
    explicit IsoReader(const fs::path &path) : path_(path), in_(path, std::ios::binary) {
        if (!in_) throw psprecomp::Error("Cannot open " + path.string());
        in_.seekg(0, std::ios::end);
        image_size_ = static_cast<std::uint64_t>(in_.tellg());
    }

    void read(std::uint64_t offset, void *out, std::size_t size) {
        if (offset + size > image_size_) throw psprecomp::Error(path_.string() + ": read past the end of the image");
        in_.seekg(static_cast<std::streamoff>(offset));
        in_.read(static_cast<char *>(out), static_cast<std::streamsize>(size));
        if (!in_) throw psprecomp::Error("Failed reading " + path_.string());
    }

    Entry root() {
        std::array<std::uint8_t, kSector> pvd{};
        read(16u * kSector, pvd.data(), pvd.size());
        if (pvd[0] != 1u || std::memcmp(&pvd[1], "CD001", 5) != 0)
            throw psprecomp::Error(path_.string() + " is not an ISO 9660 image (no primary volume descriptor)");
        return record(&pvd[156], 34u);
    }

    std::vector<Entry> list(const Entry &directory) {
        std::vector<std::uint8_t> data(directory.size);
        read(static_cast<std::uint64_t>(directory.extent) * kSector, data.data(), data.size());
        std::vector<Entry> entries;
        std::size_t at = 0;
        while (at < data.size()) {
            const std::uint8_t length = data[at];
            if (length == 0u) { // records never cross a sector: skip to the next one
                at = (at / kSector + 1u) * kSector;
                continue;
            }
            if (length < 34u || at + length > data.size()) throw psprecomp::Error(path_.string() + ": bad directory record");
            Entry e = record(&data[at], length);
            at += length;
            if (e.name.empty()) continue; // "." and ".."
            entries.push_back(std::move(e));
        }
        return entries;
    }

    [[nodiscard]] std::uint64_t image_size() const noexcept { return image_size_; }

private:
    Entry record(const std::uint8_t *r, std::size_t length) const {
        Entry e;
        e.extent = le32(r + 2);
        e.size = le32(r + 10);
        e.directory = (r[25] & 2u) != 0u;
        const std::size_t name_length = r[32];
        if (33u + name_length > length) throw psprecomp::Error(path_.string() + ": bad directory record name");
        std::string name(reinterpret_cast<const char *>(r + 33), name_length);
        if (name_length == 1u && (name[0] == '\0' || name[0] == '\1')) return e;
        if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
        if (!name.empty() && name.back() == '.') name.pop_back();
        // A crafted image must not write outside the destination.
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
            throw psprecomp::Error(path_.string() + ": unsafe file name in the image");
        e.name = std::move(name);
        return e;
    }

    fs::path path_;
    std::ifstream in_;
    std::uint64_t image_size_{};
};

std::uint64_t total_bytes(IsoReader &iso, const Entry &directory, int depth, std::set<std::uint32_t> &seen) {
    if (depth > kMaxDepth || !seen.insert(directory.extent).second) throw psprecomp::Error("ISO directory tree loops");
    std::uint64_t total = 0;
    for (const Entry &e : iso.list(directory))
        total += e.directory ? total_bytes(iso, e, depth + 1, seen) : e.size;
    return total;
}

void extract_dir(IsoReader &iso, const Entry &directory, const fs::path &out, int depth, std::uint64_t &done,
                 std::uint64_t total, const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
    fs::create_directories(out);
    std::vector<char> buffer(1u << 20u);
    for (const Entry &e : iso.list(directory)) {
        const fs::path target = out / e.name;
        if (e.directory) {
            extract_dir(iso, e, target, depth + 1, done, total, progress);
            continue;
        }
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) throw psprecomp::Error("Cannot write " + target.string());
        std::uint64_t offset = static_cast<std::uint64_t>(e.extent) * kSector;
        for (std::uint64_t left = e.size; left != 0u;) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
            iso.read(offset, buffer.data(), chunk);
            file.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!file) throw psprecomp::Error("Failed writing " + target.string());
            offset += chunk;
            left -= chunk;
            done += chunk;
            if (progress) progress(done, total);
        }
    }
}

std::string read_head(const fs::path &path, std::size_t size) {
    std::ifstream in(path, std::ios::binary);
    std::string head(size, '\0');
    in.read(head.data(), static_cast<std::streamsize>(size));
    head.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return head;
}

} // namespace

ExecutableKind classify_executable(const fs::path &path) {
    const std::string head = read_head(path, 4);
    if (head == std::string("\x7F" "ELF", 4)) return ExecutableKind::Elf;
    if (head == "~PSP") return ExecutableKind::Encrypted;
    if (head == std::string("\0PBP", 4)) return ExecutableKind::Pbp;
    return ExecutableKind::Other;
}

void extract_iso(const fs::path &iso_path, const fs::path &destination,
                 const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
    IsoReader iso(iso_path);
    const Entry root = iso.root();
    std::set<std::uint32_t> seen;
    const std::uint64_t total = total_bytes(iso, root, 0, seen);
    std::uint64_t done = 0;
    extract_dir(iso, root, destination, 0, done, total, progress);
}

fs::path cached_extraction(const fs::path &iso, const fs::path &cache_root) {
    std::error_code ec;
    const auto size = fs::file_size(iso, ec);
    if (ec) throw psprecomp::Error("Cannot read " + iso.string() + ": " + ec.message());
    // libc++ (Android) counts file times in __int128, which std::to_string does not take.
    const auto stamp = static_cast<long long>(fs::last_write_time(iso, ec).time_since_epoch().count());
    const std::string identity = fs::absolute(iso).string() + "\n" + std::to_string(size) + "\n" + std::to_string(stamp) + "\n";

    std::string folder = iso.stem().string();
    std::replace_if(folder.begin(), folder.end(), [](char c) { return std::string("<>:\"/\\|?*[]").find(c) != std::string::npos; }, '_');
    const fs::path destination = cache_root / (folder + "-" + std::to_string(size));
    const fs::path marker = destination / ".anypsp-extracted";
    if (std::ifstream in(marker, std::ios::binary); in) {
        const std::string recorded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (recorded == identity) return destination;
    }
    fs::remove_all(destination, ec);
    int last_percent = -1;
    extract_iso(iso, destination, [&](std::uint64_t done, std::uint64_t total) {
        const int percent = total == 0u ? 100 : static_cast<int>(done * 100u / total);
        if (percent / 10 != last_percent / 10) std::fprintf(stderr, "[anypsp] extracting %s: %d%%\n", iso.filename().string().c_str(), percent);
        last_percent = percent;
    });
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    out << identity;
    if (!out) throw psprecomp::Error("Cannot write " + marker.string());
    return destination;
}

BootTarget find_boot(const fs::path &disc_root) {
    const fs::path sysdir = disc_root / "PSP_GAME" / "SYSDIR";
    if (!fs::is_directory(sysdir)) throw psprecomp::Error(disc_root.string() + " has no PSP_GAME/SYSDIR: not a UMD game disc");
    std::string found;
    for (const char *name : {"BOOT.BIN", "EBOOT.BIN"}) {
        const fs::path candidate = sysdir / name;
        if (!fs::is_regular_file(candidate) || fs::file_size(candidate) == 0u) continue;
        const ExecutableKind kind = classify_executable(candidate);
        if (kind == ExecutableKind::Elf)
            return BootTarget{candidate, disc_root, std::string("disc0:/PSP_GAME/SYSDIR/") + name};
        found += std::string(found.empty() ? "" : ", ") + name + (kind == ExecutableKind::Encrypted ? " (encrypted)" : " (not an ELF)");
    }
    throw psprecomp::Error("No unencrypted executable in " + sysdir.string() + (found.empty() ? "" : ": found " + found) +
                           ". Supply a decrypted BOOT.BIN or EBOOT.BIN; AnyPSP does not decrypt.");
}

bool is_disc(const fs::path &path) {
    if (fs::is_directory(path)) return true;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".iso";
}

BootTarget resolve(const fs::path &path, const fs::path &cache_root) {
    if (fs::is_directory(path)) {
        // Accept the disc root or its PSP_GAME folder.
        if (!fs::is_directory(path / "PSP_GAME") && path.filename() == "PSP_GAME") return find_boot(path.parent_path());
        return find_boot(path);
    }
    if (!fs::is_regular_file(path)) throw psprecomp::Error("No such file or folder: " + path.string());
    return find_boot(cached_extraction(path, cache_root));
}

} // namespace anypsp::disc
