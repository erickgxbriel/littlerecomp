// The save manager: the game's Xbox saves live in hdd\E\UDATA\<title id>\ beside the executable, written by the game
// itself. This lists them, imports original Xbox saves, converts PC/PS2 saves, and opens the folder in Explorer.
#include "app/app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/log.h"

namespace app::saves {

namespace {

namespace fs = std::filesystem;

const ImVec4 kAccent(0.569f, 0.620f, 0.510f, 1.0f);
const ImVec4 kDanger(0.85f, 0.42f, 0.38f, 1.0f);

struct Save {
    fs::path path;
    std::string name;
    std::string when;
    uint64_t bytes = 0;
};

struct List {
    std::vector<Save> saves;
    std::string error;
};

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size_t(size) : 0, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size > 0 ? size_t(size) : 0, L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) start++;
    return text.substr(start);
}

// SaveMeta.xbx and TitleMeta.xbx are UTF-16LE with a BOM, one Key=Value per line
std::string ReadMeta(const fs::path& file, const std::string& key) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return {};
    std::vector<char> bytes;
    char block[512];
    for (size_t n; (n = fread(block, 1, sizeof(block), f)) > 0;) bytes.insert(bytes.end(), block, block + n);
    fclose(f);
    std::string text;
    if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE) {
        std::wstring wide((bytes.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < wide.size(); i++)
            wide[i] = wchar_t(uint8_t(bytes[2 + i * 2]) | (uint8_t(bytes[3 + i * 2]) << 8));
        text = Narrow(wide);
    } else {
        text.assign(bytes.begin(), bytes.end());
    }
    // small files: one Key=Value per line, after the BOM
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(at, end - at));
        if (line.rfind(key + "=", 0) == 0) return Trim(line.substr(key.size() + 1));
        at = end + 1;
    }
    return {};
}

std::string TimeText(fs::file_time_type time) {
    using namespace std::chrono;
    auto system = system_clock::now() + duration_cast<system_clock::duration>(time - fs::file_time_type::clock::now());
    std::time_t t = system_clock::to_time_t(system);
    std::tm local{};
    localtime_s(&local, &t);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

std::string SizeText(uint64_t bytes) {
    char text[32];
    if (bytes >= 1024ull * 1024)
        std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::snprintf(text, sizeof(text), "%.0f KB", double(bytes) / 1024.0);
    else
        std::snprintf(text, sizeof(text), "%llu bytes", (unsigned long long)bytes);
    return text;
}

uint64_t FolderBytes(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error)) total += it->file_size(error);
    return total;
}

fs::path InstanceDir() {
    std::wstring name = Instance() > 1 ? L"hdd" + std::to_wstring(Instance()) : L"hdd";
    return ExeDir() / name;
}

bool IsSaveFolder(const fs::path& dir) {
    std::error_code error;
    if (!fs::is_directory(dir, error)) return false;
    return fs::is_regular_file(dir / "SaveMeta.xbx", error) || fs::is_regular_file(dir / "save.dat", error) ||
           fs::is_regular_file(dir / "SaveImage.xbx", error);
}

// a picked folder may be one save, a title folder, or a whole E:\UDATA dump: take every save folder under it
void FindSaves(const fs::path& dir, int depth, std::vector<fs::path>& out) {
    if (depth > 8) return;
    if (IsSaveFolder(dir)) {
        out.push_back(dir);
        return;
    }
    std::error_code error;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        std::string name = it->path().filename().string();
        if (name == "$RECYCLE.BIN" || name == "System Volume Information") continue;
        FindSaves(it->path(), depth + 1, out);
    }
}

std::string Stamp() {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char text[24];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

fs::path Root() {
    if (!kGame.title_id || !*kGame.title_id) return {};
    return InstanceDir() / "E" / "UDATA" / kGame.title_id;
}

List Enumerate() {
    List list;
    fs::path root = Root();
    if (root.empty()) return list;
    std::error_code error;
    fs::create_directories(root, error);
    if (error) {
        list.error = "Could not open " + Utf8Path(root) + ": " + error.message();
        return list;
    }
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_directory(entry_error)) continue;
        if (!IsSaveFolder(it->path())) continue;
        Save save;
        save.path = it->path();
        save.name = ReadMeta(save.path / "SaveMeta.xbx", "Name");
        if (save.name.empty()) save.name = save.path.filename().string();
        fs::path data = save.path / "save.dat";
        if (!fs::exists(data, entry_error)) data = save.path;
        save.when = TimeText(fs::last_write_time(data, entry_error));
        save.bytes = FolderBytes(save.path);
        list.saves.push_back(std::move(save));
    }
    std::sort(list.saves.begin(), list.saves.end(),
              [](const Save& a, const Save& b) { return a.when > b.when; });
    return list;
}

// A profile's folder id and the identity bytes the game expects in its save.dat.
struct SlotInfo {
    std::string folder;
    uint8_t identity[4]{};
};

// Declared here so the converter can use it; defined after the low-level helpers.
bool GetSlot(int slot, SlotInfo& out, std::string& error);

// PC and PS2 saves share the game's named-variable format with Xbox saves. The values are fixed-width strings
// in 32-byte slots; the byte after each name is the current value width. Xbox files split the slot table around
// their signature blocks, so the target offsets are not the same as the source offsets.
namespace convert {

const uint8_t kMagic[4] = {0xB0, 0x24, 0x1C, 0xD7};
const size_t kFirstCount = 68;
const size_t kSlotStride = 32;
const size_t kSlotValue = 31;
const size_t kPcSlots = 0x1776;
const size_t kPs2FirstSlots = 0x1776;
const size_t kPs2SecondSlots = 0x1FFA;
const size_t kXboxFirstSlots = 0x1776;
const size_t kXboxSecondSlots = 0x200A;
// PS2 value banks are 32-byte slots, but two variables are preceded by an extra four bytes. The first gap starts at
// variable 5 and consumes the space between the two banks; the second starts at variable 196. Both were confirmed by
// reading raw 169- and 332-record CodeBreaker saves against known PC/Xbox values, not by a guess.
const size_t kPs2FirstShiftStart = 5;
const size_t kPs2SecondShiftStart = 196;
const size_t kNoBase = size_t(-1);

// PC saves from different game revisions have 188 or 191 named variables. PS2 releases vary by region and game
// revision (169, 170 and 332 are known); all of them fit the same two value banks. The converter maps values by name.
const size_t kPcRecords = 188;
const size_t kPcRecordsExtended = 191;
const size_t kPs2Records = 170;
const size_t kPs2RecordsExtended = 332;
const size_t kPs2RecordsMin = 160;
const size_t kPs2RecordsMax = 400;

bool IsPcCount(size_t count) { return count == kPcRecords || count == kPcRecordsExtended; }
bool IsPs2Count(size_t count) { return count >= kPs2RecordsMin && count <= kPs2RecordsMax; }

// The four bytes at 0x0F identify a profile inside save.dat. The game compares them with the profile folder, so
// copying a template from another slot makes the converted save look damaged. The first three come from the original
// Xbox profiles; the fourth was confirmed from the save the game created in the empty fourth slot.
const size_t kSlotIdentity = 0x0F;
const uint8_t kFixedSlotIds[4][4] = {
    {0x0E, 0x14, 0x10, 0x38},  // profile 1, folder ...C0
    {0x13, 0x03, 0x2E, 0x10},  // profile 2, folder ...C1
    {0x13, 0x03, 0x38, 0x2B},  // profile 3, folder ...C2
    {0x15, 0x11, 0x1A, 0x30},  // profile 4, folder ...C3, from the game-created save
};
const char* kFixedSlotFolderIds[4] = {"267AFF5B38C0", "267AFF5B38C1", "267AFF5B38C2", "267AFF5B38C3"};

struct SignatureRange {
    size_t begin;
    size_t end;
    size_t signature;
};
const SignatureRange kSignatures[] = {
    {0x0000, 0x1FEC, 0x1FEC},
    {0x2000, 0x3FEC, 0x3FEC},
    {0x4000, 0x4601, 0x4601},
};

class Sha1 {
public:
    Sha1() { Reset(); }

    void Update(const uint8_t* data, size_t size) {
        bits_ += uint64_t(size) * 8;
        while (size) {
            size_t n = std::min(size, sizeof(block_) - used_);
            std::memcpy(block_ + used_, data, n);
            used_ += n;
            data += n;
            size -= n;
            if (used_ == sizeof(block_)) {
                Process();
                used_ = 0;
            }
        }
    }

    void Final(uint8_t out[20]) {
        uint64_t bits = bits_;
        uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (used_ != 56) Update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; i++) length[i] = uint8_t(bits >> ((7 - i) * 8));
        Update(length, sizeof(length));
        for (int i = 0; i < 5; i++) {
            out[i * 4 + 0] = uint8_t(h_[i] >> 24);
            out[i * 4 + 1] = uint8_t(h_[i] >> 16);
            out[i * 4 + 2] = uint8_t(h_[i] >> 8);
            out[i * 4 + 3] = uint8_t(h_[i]);
        }
    }

private:
    static uint32_t Rotate(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

    void Reset() {
        h_[0] = 0x67452301u;
        h_[1] = 0xEFCDAB89u;
        h_[2] = 0x98BADCFEu;
        h_[3] = 0x10325476u;
        h_[4] = 0xC3D2E1F0u;
        bits_ = 0;
        used_ = 0;
    }

    void Process() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(block_[i * 4]) << 24) | (uint32_t(block_[i * 4 + 1]) << 16) |
                   (uint32_t(block_[i * 4 + 2]) << 8) | uint32_t(block_[i * 4 + 3]);
        for (int i = 16; i < 80; i++)
            w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t next = Rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotate(b, 30);
            b = a;
            a = next;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint64_t bits_ = 0;
    uint8_t block_[64]{};
    size_t used_ = 0;
};

// The save signature key is HMAC-SHA1 with a sixteen-byte zero key.
void HmacSha1Zero(const uint8_t* data, size_t size, uint8_t out[20]) {
    uint8_t ipad[64], opad[64], inner[20];
    std::memset(ipad, 0x36, sizeof(ipad));
    std::memset(opad, 0x5C, sizeof(opad));
    Sha1 first;
    first.Update(ipad, sizeof(ipad));
    first.Update(data, size);
    first.Final(inner);
    Sha1 second;
    second.Update(opad, sizeof(opad));
    second.Update(inner, sizeof(inner));
    second.Final(out);
}

uint32_t Le32(const uint8_t* data, size_t at) {
    return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16) |
           (uint32_t(data[at + 3]) << 24);
}

uint32_t Le32(const std::vector<uint8_t>& data, size_t at) { return Le32(data.data(), at); }

void WriteLe32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    for (int i = 0; i < 4; i++) data[at + i] = uint8_t(value >> (i * 8));
}

size_t SharedBase(const std::vector<uint8_t>& data) {
    if (data.size() < 0x40) return kNoBase;
    if (Le32(data, 0) == data.size() && std::memcmp(data.data() + 4, kMagic, sizeof(kMagic)) == 0) return 4;
    if (std::memcmp(data.data(), kMagic, sizeof(kMagic)) == 0) return 0;
    return kNoBase;
}

struct Record {
    std::string name;
    uint8_t size = 0;
};

enum class Kind { Pc, Ps2, Xbox };

bool Parse(const std::vector<uint8_t>& data, size_t base, std::vector<Record>& records, std::string& error) {
    if (base == kNoBase || data.size() < base + 0x38) {
        error = "not a save file";
        return false;
    }
    uint32_t count = Le32(data, base + 0x34);
    if (!count || count > 1000 || data.size() < base + 0x38 + size_t(count) * 17) {
        error = "the save's variable table is damaged";
        return false;
    }
    records.clear();
    records.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        size_t at = base + 0x38 + size_t(i) * 17;
        size_t length = 0;
        while (length < 16 && data[at + length]) length++;
        if (!length || length == 16 || data[at + 16] > kSlotValue) {
            error = "the save's variable table is damaged";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(data.data() + at), length);
        for (char c : name)
            if (uint8_t(c) < 0x20 || uint8_t(c) > 0x7E) {
                error = "the save's variable table is damaged";
                return false;
            }
        records.push_back({std::move(name), data[at + 16]});
    }
    return true;
}

size_t SlotOffset(Kind kind, size_t index) {
    if (kind == Kind::Pc) return kPcSlots + index * kSlotStride;
    if (kind == Kind::Ps2) {
        if (index < kFirstCount) {
            size_t gap = index < kPs2FirstShiftStart ? 0 : 4;
            return kPs2FirstSlots + gap + index * kSlotStride;
        }
        size_t gap = index < kPs2SecondShiftStart ? 0 : 4;
        return kPs2SecondSlots + gap + (index - kFirstCount) * kSlotStride;
    }
    if (index < kFirstCount) return kXboxFirstSlots + index * kSlotStride;
    return kXboxSecondSlots + (index - kFirstCount) * kSlotStride;
}

bool ValueAt(const std::vector<uint8_t>& data, size_t base, Kind kind, size_t index, const Record& record,
             std::vector<uint8_t>& value, std::string& error) {
    size_t at = base + SlotOffset(kind, index);
    if (record.size > kSlotValue || at > data.size() || data.size() - at < record.size) {
        error = "a save value lies outside the file";
        return false;
    }
    value.assign(data.begin() + at, data.begin() + at + record.size);
    return true;
}

bool ReadBinary(const fs::path& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    out.resize(size_t(size));
    size_t read = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(read);
    return read == size_t(size);
}

bool WriteBinary(const fs::path& path, const std::vector<uint8_t>& data, std::error_code& error) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        error = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0) ok = false;
    if (!ok) error = std::make_error_code(std::errc::io_error);
    return ok;
}

std::vector<uint8_t> MetaBytes(int slot) {
    // The game checks this label against its fixed profile slots. A friendlier name such as "Converted Save" makes it
    // reject the profile as damaged, so generated saves use the same label as the original profile.
    std::string text = "Name=Profile  " + std::to_string(slot) + "\r\n";
    std::vector<uint8_t> out = {0xFF, 0xFE};
    for (char c : text) {
        out.push_back(uint8_t(c));
        out.push_back(0);
    }
    return out;
}

std::vector<uint8_t> ImageBytes() {
    const uint8_t header[] = {
        'X', 'P', 'R', '0', 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0x29, 0x0C, 0x61, 0x06, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    std::vector<uint8_t> out(4096, 0xAD);
    std::memcpy(out.data(), header, sizeof(header));
    return out;
}

void Resign(std::vector<uint8_t>& data) {
    for (const SignatureRange& range : kSignatures) {
        if (range.end > data.size() || range.signature + 20 > data.size()) continue;
        uint8_t digest[20];
        HmacSha1Zero(data.data() + range.begin, range.end - range.begin, digest);
        std::memcpy(data.data() + range.signature, digest, sizeof(digest));
    }
}

// Every Xbox profile has the same fixed size. When no game-created profile is available, start from the source's
// own header and variable table, move it behind the Xbox size word, and clear the signature areas. The converter
// then fills the Xbox value slots by name just as it does with a real template.
const size_t kXboxSaveSize = 0x4615;

bool BuildXboxTemplate(const std::vector<uint8_t>& source, const SlotInfo& info, std::vector<uint8_t>& out) {
    out.assign(kXboxSaveSize, 0);
    size_t copy = std::min(source.size(), out.size() - 4);
    if (copy) std::memcpy(out.data() + 4, source.data(), copy);
    WriteLe32(out, 0, uint32_t(out.size()));
    std::fill(out.begin() + 0x1FEC, out.begin() + 0x2000, 0);
    std::fill(out.begin() + 0x3FEC, out.begin() + 0x4000, 0);
    std::fill(out.begin() + 0x4601, out.end(), 0);
    if (out.size() >= kSlotIdentity + 4) std::memcpy(out.data() + kSlotIdentity, info.identity, sizeof(info.identity));
    return true;
}

// Writes one finished Xbox profile to its fixed folder id, backing up anything already there. Both the converter and
// the original-Xbox importer use this so a target slot always gets the right folder name, label and signature.
bool Install(const std::vector<uint8_t>& data, int slot, const fs::path& target, const fs::path& image_source,
             std::string& error) {
    fs::path root = target.parent_path();
    fs::path temp = root / (target.filename().string() + ".converting");
    fs::path backup = root / (target.filename().string() + ".backup-" + Stamp());
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    if (ec) {
        error = ec.message();
        fs::remove_all(temp, ec);
        return false;
    }
    if (!WriteBinary(temp / "save.dat", data, ec) ||
        !WriteBinary(temp / "SaveMeta.xbx", MetaBytes(slot), ec)) {
        error = ec.message();
        fs::remove_all(temp, ec);
        return false;
    }
    if (!image_source.empty() && fs::is_regular_file(image_source, ec)) {
        fs::copy_file(image_source, temp / "SaveImage.xbx", fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = ec.message();
            fs::remove_all(temp, ec);
            return false;
        }
    } else if (!WriteBinary(temp / "SaveImage.xbx", ImageBytes(), ec)) {
        error = ec.message();
        fs::remove_all(temp, ec);
        return false;
    }
    if (fs::exists(target, ec)) {
        fs::rename(target, backup, ec);
        if (ec) {
            error = ec.message();
            fs::remove_all(temp, ec);
            return false;
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        std::error_code restore_error;
        if (fs::exists(backup, restore_error)) fs::rename(backup, target, restore_error);
        error = ec.message();
        fs::remove_all(temp, restore_error);
        return false;
    }
    LOGI("Saves: wrote %s", Utf8Path(target).c_str());
    return true;
}

// An original Xbox save is already in the right file layout. Rebinding it means replacing the four bytes the game
// compares with the profile folder, then recomputing its signatures. The variable table is left untouched.
bool RebindXboxSave(const fs::path& source_dat, const fs::path& image_source, int slot, const fs::path& target,
                    std::string& error) {
    std::vector<uint8_t> data;
    if (!ReadBinary(source_dat, data)) {
        error = "a save file could not be read";
        return false;
    }
    size_t base = SharedBase(data);
    std::vector<Record> records;
    if (!Parse(data, base, records, error)) return false;
    if (base != 4 || data.size() < kSlotIdentity + 4) {
        error = "this is not an Xbox-format save";
        return false;
    }
    SlotInfo info;
    std::string slot_error;
    if (!GetSlot(slot, info, slot_error)) {
        error = slot_error;
        return false;
    }
    std::memcpy(data.data() + kSlotIdentity, info.identity, sizeof(info.identity));
    Resign(data);
    return Install(data, slot, target, image_source, error);
}

// A raw PC/PS2 save starts with the magic at offset zero and has one of the three known record counts.
bool LooksLikeRawData(const uint8_t* data, size_t size) {
    if (!data || size < 0x38 || std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return false;
    uint32_t count = Le32(data, 0x34);
    return IsPcCount(count) || IsPs2Count(count);
}

// Minimal zlib/deflate reader for CodeBreaker saves. It is deliberately bounded: a save is small, and a damaged
// archive must fail rather than allocate without limit.
struct ZlibError {};

struct Inflate {
    struct Huff {
        short count[16]{};
        short symbol[288]{};
    };

    const uint8_t* in;
    size_t len;
    size_t pos = 0;
    uint64_t bitbuf = 0;
    int bitcnt = 0;
    std::vector<uint8_t> out;
    size_t limit = 0;

    int Bits(int need) {
        while (bitcnt < need) {
            if (pos >= len) throw ZlibError{};
            bitbuf |= uint64_t(in[pos++]) << bitcnt;
            bitcnt += 8;
        }
        int value = int(bitbuf & ((1ull << need) - 1));
        bitbuf >>= need;
        bitcnt -= need;
        return value;
    }

    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= 15; length++) {
            code |= Bits(1);
            int count = h.count[length];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw ZlibError{};
    }

    static int Construct(Huff& h, const short* lengths, int n) {
        std::memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < n; i++) h.count[lengths[i]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int length = 1; length <= 15; length++) {
            left = (left << 1) - h.count[length];
            if (left < 0) return left;
        }
        short offset[16]{};
        for (int length = 1; length < 15; length++) offset[length + 1] = offset[length] + h.count[length];
        for (int i = 0; i < n; i++)
            if (lengths[i]) h.symbol[offset[lengths[i]]++] = short(i);
        return left;
    }

    void Put(uint8_t byte) {
        if (out.size() < limit) out.push_back(byte);
    }

    void Codes(const Huff& lengths, const Huff& distances) {
        constexpr short kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
                                           51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr short kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr short kDistanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                             6145, 8193, 12289, 16385, 24577};
        constexpr short kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(lengths);
            if (symbol < 256) {
                Put(uint8_t(symbol));
            } else if (symbol == 256) {
                return;
            } else if (symbol > 285) {
                throw ZlibError{};
            } else {
                symbol -= 257;
                int length = kLengthBase[symbol] + Bits(kLengthExtra[symbol]);
                symbol = Decode(distances);
                if (symbol >= 30) throw ZlibError{};
                size_t distance = size_t(kDistanceBase[symbol] + Bits(kDistanceExtra[symbol]));
                if (distance > out.size()) throw ZlibError{};
                while (length-- > 0) Put(out[out.size() - distance]);
            }
        }
    }

    void Stored() {
        bitbuf = 0;
        bitcnt = 0;
        if (pos + 4 > len) throw ZlibError{};
        unsigned length = in[pos] | (unsigned(in[pos + 1]) << 8);
        if ((in[pos + 2] | (unsigned(in[pos + 3]) << 8)) != (unsigned(~length) & 0xFFFF)) throw ZlibError{};
        pos += 4;
        if (pos + length > len) throw ZlibError{};
        for (unsigned i = 0; i < length; i++) Put(in[pos + i]);
        pos += length;
    }

    void Fixed() {
        static Huff lengths, distances;
        static bool ready = false;
        if (!ready) {
            short code[288];
            int i = 0;
            for (; i < 144; i++) code[i] = 8;
            for (; i < 256; i++) code[i] = 9;
            for (; i < 280; i++) code[i] = 7;
            for (; i < 288; i++) code[i] = 8;
            Construct(lengths, code, 288);
            for (i = 0; i < 30; i++) code[i] = 5;
            Construct(distances, code, 30);
            ready = true;
        }
        Codes(lengths, distances);
    }

    void Dynamic() {
        constexpr short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320]{};
        Huff length_codes, distance_codes;
        int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw ZlibError{};
        int index = 0;
        for (; index < ncode; index++) lengths[kOrder[index]] = short(Bits(3));
        for (; index < 19; index++) lengths[kOrder[index]] = 0;
        if (Construct(length_codes, lengths, 19) != 0) throw ZlibError{};
        index = 0;
        while (index < nlen + ndist) {
            int symbol = Decode(length_codes);
            short value = 0;
            int repeat = 1;
            if (symbol < 16) {
                value = short(symbol);
            } else if (symbol == 16) {
                if (!index) throw ZlibError{};
                value = lengths[index - 1];
                repeat = 3 + Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (index + repeat > nlen + ndist) throw ZlibError{};
            while (repeat-- > 0) lengths[index++] = value;
        }
        if (!lengths[256]) throw ZlibError{};
        int left = Construct(length_codes, lengths, nlen);
        if (left && (left < 0 || nlen != length_codes.count[0] + length_codes.count[1])) throw ZlibError{};
        left = Construct(distance_codes, lengths + nlen, ndist);
        if (left && (left < 0 || ndist != distance_codes.count[0] + distance_codes.count[1])) throw ZlibError{};
        Codes(length_codes, distance_codes);
    }

    bool Run(size_t expected, std::vector<uint8_t>& result, bool zlib_header = true) {
        try {
            if (!len) return false;
            if (zlib_header) {
                if (len < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
                    return false;
            }
            limit = expected ? expected : 64u * 1024u * 1024u;
            out.clear();
            pos = zlib_header ? 2 : 0;
            int last;
            do {
                last = Bits(1);
                int type = Bits(2);
                if (type == 0) Stored();
                else if (type == 1) Fixed();
                else if (type == 2) Dynamic();
                else throw ZlibError{};
            } while (!last && out.size() < limit);
            result = std::move(out);
            return true;
        } catch (ZlibError&) {
            return false;
        }
    }
};

// Max Drive's LZARI decoder. It is Okumura's arithmetic coder with a 4096-byte window; the window starts with spaces,
// and reading past the end yields zero bits, as in mymc's decoder. A save is small, so this stays deliberately bounded.
struct Lzari {
    enum { kWindow = 4096, kMatch = 60, kThreshold = 2, kSymbols = 256 - kThreshold + kMatch, kBits = 15 };
    static constexpr uint64_t kQ1 = 1ull << kBits, kQ2 = 2 * kQ1, kQ3 = 3 * kQ1, kQ4 = 4 * kQ1;
    static constexpr unsigned kMaxCum = unsigned(kQ1 - 1);

    const uint8_t* in = nullptr;
    size_t len = 0, pos = 0;
    unsigned mask = 0, buffer = 0;
    uint64_t low = 0, high = kQ4, value = 0;
    int symbol_to_char[kSymbols + 1]{};
    unsigned symbol_freq[kSymbols + 1]{}, symbol_cum[kSymbols + 1]{}, position_cum[kWindow + 1]{};

    int Bit() {
        if ((mask >>= 1) == 0) {
            buffer = pos < len ? in[pos] : 0;
            pos++;
            mask = 128;
        }
        return (buffer & mask) != 0;
    }

    void StartModel() {
        symbol_cum[kSymbols] = 0;
        for (int symbol = kSymbols; symbol >= 1; symbol--) {
            symbol_to_char[symbol] = symbol - 1;
            symbol_freq[symbol] = 1;
            symbol_cum[symbol - 1] = symbol_cum[symbol] + symbol_freq[symbol];
        }
        symbol_freq[0] = 0;
        position_cum[kWindow] = 0;
        for (int i = kWindow; i >= 1; i--) position_cum[i - 1] = position_cum[i] + 10000 / (i + 200);
    }

    void UpdateModel(int symbol) {
        if (symbol_cum[0] >= kMaxCum) {
            unsigned cumulative = 0;
            for (int i = kSymbols; i > 0; i--) {
                symbol_cum[i] = cumulative;
                cumulative += (symbol_freq[i] = (symbol_freq[i] + 1) >> 1);
            }
            symbol_cum[0] = cumulative;
        }
        int i = symbol;
        while (symbol_freq[i] == symbol_freq[i - 1]) i--;
        if (i < symbol) std::swap(symbol_to_char[i], symbol_to_char[symbol]);
        symbol_freq[i]++;
        while (--i >= 0) symbol_cum[i]++;
    }

    int SearchSymbol(unsigned x) const {
        int first = 1, last = kSymbols;
        while (first < last) {
            int middle = (first + last) / 2;
            if (symbol_cum[middle] > x) first = middle + 1;
            else last = middle;
        }
        return first;
    }

    int SearchPosition(unsigned x) const {
        int first = 1, last = kWindow;
        while (first < last) {
            int middle = (first + last) / 2;
            if (position_cum[middle] > x) first = middle + 1;
            else last = middle;
        }
        return first - 1;
    }

    void Normalize() {
        for (;;) {
            if (low >= kQ2) {
                value -= kQ2;
                low -= kQ2;
                high -= kQ2;
            } else if (low >= kQ1 && high <= kQ3) {
                value -= kQ1;
                low -= kQ1;
                high -= kQ1;
            } else if (high > kQ2) {
                return;
            }
            low += low;
            high += high;
            value = 2 * value + Bit();
        }
    }

    int DecodeChar() {
        uint64_t range = high - low;
        if (!range || !symbol_cum[0]) return -1;
        int symbol = SearchSymbol(unsigned(((value - low + 1) * symbol_cum[0] - 1) / range));
        high = low + range * symbol_cum[symbol - 1] / symbol_cum[0];
        low += range * symbol_cum[symbol] / symbol_cum[0];
        Normalize();
        int character = symbol_to_char[symbol];
        UpdateModel(symbol);
        return character;
    }

    int DecodePosition() {
        uint64_t range = high - low;
        if (!range || !position_cum[0]) return -1;
        int position = SearchPosition(unsigned(((value - low + 1) * position_cum[0] - 1) / range));
        high = low + range * position_cum[position] / position_cum[0];
        low += range * position_cum[position + 1] / position_cum[0];
        Normalize();
        return position;
    }

    bool Run(const uint8_t* data, size_t size, size_t text_size, std::vector<uint8_t>& out) {
        if (!data || text_size > 64u * 1024u * 1024u) return false;
        in = data;
        len = size;
        pos = 0;
        mask = 0;
        buffer = 0;
        low = 0;
        high = kQ4;
        value = 0;
        for (int i = 0; i < kBits + 2; i++) value = 2 * value + Bit();
        StartModel();
        uint8_t window[kWindow];
        std::memset(window, ' ', sizeof(window) - kMatch);
        std::memset(window + kWindow - kMatch, 0, kMatch);
        unsigned r = kWindow - kMatch;
        out.clear();
        out.reserve(text_size);
        while (out.size() < text_size) {
            if (pos > len + 64) return false;
            int character = DecodeChar();
            if (character < 0) return false;
            if (character < 256) {
                out.push_back(uint8_t(character));
                window[r++ & (kWindow - 1)] = uint8_t(character);
            } else {
                int position = DecodePosition();
                int count = character - 255 + kThreshold;
                if (position < 0 || count <= 0) return false;
                unsigned source = (r - unsigned(position) - 1) & (kWindow - 1);
                while (count-- > 0 && out.size() < text_size) {
                    uint8_t byte = window[source++ & (kWindow - 1)];
                    out.push_back(byte);
                    window[r++ & (kWindow - 1)] = byte;
                }
            }
        }
        return true;
    }
};

// CodeBreaker's fixed RC4 starting permutation, used by the format instead of a key.
const uint8_t kCbsRc4[256] = {
    0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c, 0x07, 0x4c, 0x9c, 0xb4,
    0x81, 0xb8, 0xef, 0x98, 0x59, 0xae, 0xf9, 0x26, 0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62,
    0x7c, 0x64, 0x46, 0xf4, 0x34, 0x1a, 0xf6, 0xe1, 0xba, 0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16,
    0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56, 0x0c, 0x91, 0x00, 0x0b,
    0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52, 0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0,
    0x65, 0x84, 0xb2, 0xd6, 0xdf, 0x15, 0x3c, 0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1,
    0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03, 0x92, 0x70, 0x05, 0x7d,
    0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d, 0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5,
    0x54, 0xcf, 0xe7, 0x41, 0xe4, 0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06,
    0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0, 0x11, 0xc2, 0xea, 0x74,
    0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38, 0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e,
    0x9b, 0x7f, 0xd8, 0x6e, 0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed,
    0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a, 0xbc, 0xa8, 0xf2, 0xee, 0xeb, 0xf5,
    0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d, 0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3,
    0x58, 0xdb, 0xc7, 0x4b, 0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b,
    0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8, 0x6b, 0x93, 0x32, 0x48, 0xb6, 0x30, 0x43, 0xa5,
};

// A CodeBreaker save is a container: CFU header, RC4-encrypted zlib data, then 64-byte file entries. Scarface's
// entry is copied out in memory and converted like any other raw PS2 save.
bool DecodeCbs(const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& error) {
    if (data.size() < 92 + 32 || std::memcmp(data.data(), "CFU\0", 4) != 0) {
        error = "not a CodeBreaker save";
        return false;
    }
    size_t header = Le32(data, 8), unpacked_size = Le32(data, 12), body_size = Le32(data, 16);
    if (header < 92 + 32 || header > data.size() || unpacked_size > 64u * 1024u * 1024u) {
        error = "the CodeBreaker header is damaged";
        return false;
    }
    size_t available = data.size() - header, length = 0;
    if (available >= body_size) length = body_size;
    else if (body_size >= header && available == body_size - header) length = available;
    if (!length) {
        error = "the CodeBreaker save is cut short";
        return false;
    }
    std::vector<uint8_t> body(data.begin() + header, data.begin() + header + length);
    uint8_t state[256];
    std::memcpy(state, kCbsRc4, sizeof(state));
    unsigned j = 0;
    for (size_t i = 0; i < body.size(); i++) {
        unsigned k = unsigned((i + 1) & 255);
        j = (j + state[k]) & 255;
        std::swap(state[k], state[j]);
        body[i] ^= uint8_t(state[(state[k] + state[j]) & 255]);
    }
    std::vector<uint8_t> unpacked;
    Inflate inflate{body.data(), body.size()};
    if (!inflate.Run(unpacked_size, unpacked)) {
        error = "the CodeBreaker save could not be unpacked";
        return false;
    }
    std::vector<uint8_t> best;
    for (size_t at = 0; at + 64 <= unpacked.size();) {
        size_t size = Le32(unpacked, at + 16);
        if (size > unpacked.size() - at - 64) {
            error = "the CodeBreaker save is damaged";
            return false;
        }
        const uint8_t* entry = unpacked.data() + at + 64;
        if (LooksLikeRawData(entry, size) && size > best.size()) best.assign(entry, entry + size);
        at += 64 + size;
    }
    if (best.empty()) {
        error = "the CodeBreaker save has no Scarface profile";
        return false;
    }
    out = std::move(best);
    return true;
}

void RememberRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& best) {
    if (LooksLikeRawData(data, size) && size > best.size()) best.assign(data, data + size);
}

bool DecodeMax(const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& error) {
    constexpr size_t kHeader = 0x5C;
    if (data.size() < kHeader || std::memcmp(data.data(), "Ps2PowerSave", 12) != 0) {
        error = "not a Max Drive save";
        return false;
    }
    size_t packed = Le32(data, 0x50), count = Le32(data, 0x54), unpacked = Le32(data, 0x58);
    if (unpacked > 64u * 1024u * 1024u || count > 4096) {
        error = "the Max Drive header is damaged";
        return false;
    }
    size_t available = data.size() - kHeader;
    if (packed != unpacked) {
        if (packed < 4 || packed - 4 > available) {
            error = "the Max Drive save is cut short";
            return false;
        }
        available = packed - 4;
    }
    std::vector<uint8_t> files;
    if (!Lzari().Run(data.data() + kHeader, available, unpacked, files)) {
        error = "the Max Drive save could not be unpacked";
        return false;
    }
    std::vector<uint8_t> best;
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        if (at + 36 > files.size()) break;
        size_t size = Le32(files, at);
        at += 36;
        if (size > files.size() - at) {
            error = "the Max Drive save is damaged";
            return false;
        }
        RememberRaw(files.data() + at, size, best);
        at += size;
        at = (at + 8 + 15) / 16 * 16 - 8;
    }
    if (best.empty()) {
        error = "the Max Drive save has no Scarface profile";
        return false;
    }
    out = std::move(best);
    return true;
}

bool DecodeSps(const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& error) {
    static const uint8_t kMagic[17] = {0x0D, 0, 0, 0, 'S', 'h', 'a', 'r', 'k', 'P', 'o', 'r', 't', 'S', 'a', 'v', 'e'};
    if (data.size() < 21 || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "not a SharkPort/X-Port save";
        return false;
    }
    size_t at = 17 + 4;  // version magic and save type
    auto skip_string = [&]() {
        if (at + 4 > data.size()) return false;
        size_t length = Le32(data, at);
        at += 4;
        if (length > data.size() - at) return false;
        at += length;
        return true;
    };
    if (!skip_string() || !skip_string() || !skip_string() || at + 4 > data.size()) {
        error = "the SharkPort header is damaged";
        return false;
    }
    at += 4;  // total size of the file entries
    if (at + 98 > data.size()) {
        error = "the SharkPort save is cut short";
        return false;
    }
    size_t header = data[at] | (unsigned(data[at + 1]) << 8);
    size_t count = Le32(data, at + 66);
    if (header < 98 || header > data.size() - at || count < 2 || count > 4096) {
        error = "the SharkPort header is damaged";
        return false;
    }
    at += header;
    std::vector<uint8_t> best;
    for (size_t i = 0; i < count - 2; i++) {
        if (at + 98 > data.size()) {
            error = "the SharkPort save is cut short";
            return false;
        }
        header = data[at] | (unsigned(data[at + 1]) << 8);
        size_t size = Le32(data, at + 66);
        if (header < 98 || header > data.size() - at) {
            error = "the SharkPort entry is damaged";
            return false;
        }
        at += header;
        if (size > data.size() - at) {
            error = "the SharkPort save is cut short";
            return false;
        }
        RememberRaw(data.data() + at, size, best);
        at += size;
    }
    if (best.empty()) {
        error = "the SharkPort save has no Scarface profile";
        return false;
    }
    out = std::move(best);
    return true;
}

bool DecodePsu(const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& error) {
    auto mode = [&](size_t at) { return unsigned(data[at]) | (unsigned(data[at + 1]) << 8); };
    auto is_dir = [&](size_t at) { return (mode(at) & 0x8030u) == 0x8020u; };
    auto is_file = [&](size_t at) { return (mode(at) & 0x8010u) == 0x8010u; };
    if (data.size() < 1536 || !is_dir(0) || !is_dir(512) || !is_dir(1024) || Le32(data, 4) < 2 ||
        std::memcmp(data.data() + 512 + 0x40, ".", 2) != 0 ||
        std::memcmp(data.data() + 1024 + 0x40, "..", 3) != 0) {
        error = "not an EMS/PSU save";
        return false;
    }
    size_t count = Le32(data, 4) - 2, at = 1536;
    if (count > 4096) {
        error = "the EMS/PSU header is damaged";
        return false;
    }
    std::vector<uint8_t> best;
    for (size_t i = 0; i < count; i++) {
        if (at + 512 > data.size() || !is_file(at)) {
            error = "the EMS/PSU save is damaged";
            return false;
        }
        size_t size = Le32(data, at + 4);
        at += 512;
        if (size > data.size() - at) {
            error = "the EMS/PSU save is cut short";
            return false;
        }
        RememberRaw(data.data() + at, size, best);
        at += (size + 1023) / 1024 * 1024;
    }
    if (best.empty()) {
        error = "the EMS/PSU save has no Scarface profile";
        return false;
    }
    out = std::move(best);
    return true;
}

bool DecodeZip(const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& error) {
    // Virtual memory card archives from GameFAQs hold a raw .psv image. Only stored and deflated entries are needed;
    // directory entries and extra fields are skipped, and the image itself is found by its Scarface magic.
    struct Entry {
        uint32_t compressed;
        uint32_t unpacked;
        unsigned method;
    };
    // A writer that streams to a pipe leaves zero sizes in the local header and puts the real sizes in a data
    // descriptor. The central directory always has them, so read it first and look each local header up by offset.
    std::map<size_t, Entry> central;
    size_t directory = size_t(-1);
    for (size_t p = data.size(); p >= 22; p--) {
        if (Le32(data, p - 1) == 0x06054B50u) {
            directory = Le32(data, p - 1 + 16);
            break;
        }
        if (data.size() - p > 65557) break;
    }
    if (directory != size_t(-1)) {
        size_t p = directory;
        while (p + 46 <= data.size() && Le32(data, p) == 0x02014B50u) {
            unsigned method = data[p + 10] | (unsigned(data[p + 11]) << 8);
            uint32_t compressed = Le32(data, p + 20), unpacked = Le32(data, p + 24);
            size_t name_size = data[p + 28] | (unsigned(data[p + 29]) << 8);
            size_t extra_size = data[p + 30] | (unsigned(data[p + 31]) << 8);
            size_t comment_size = data[p + 32] | (unsigned(data[p + 33]) << 8);
            size_t local = Le32(data, p + 42);
            if (p + 46 + name_size + extra_size + comment_size > data.size()) break;
            central[local] = {compressed, unpacked, method};
            p += 46 + name_size + extra_size + comment_size;
        }
    }
    std::vector<uint8_t> best;
    size_t at = 0;
    while (at + 30 <= data.size()) {
        if (Le32(data, at) != 0x04034B50u) break;  // stop at the central directory
        size_t local = at;
        unsigned flags = data[at + 6] | (unsigned(data[at + 7]) << 8);
        unsigned method = data[at + 8] | (unsigned(data[at + 9]) << 8);
        uint32_t compressed = Le32(data, at + 18), unpacked = Le32(data, at + 22);
        auto found = central.find(local);
        if (found != central.end()) {
            compressed = found->second.compressed;
            unpacked = found->second.unpacked;
            method = found->second.method;
        }
        size_t name_size = data[at + 26] | (unsigned(data[at + 27]) << 8);
        size_t extra_size = data[at + 28] | (unsigned(data[at + 29]) << 8);
        at += 30;
        if (name_size > data.size() - at || extra_size > data.size() - at - name_size) {
            error = "the ZIP archive is damaged";
            return false;
        }
        at += name_size + extra_size;
        if (compressed > data.size() - at) {
            error = "the ZIP archive is cut short";
            return false;
        }
        std::vector<uint8_t> file;
        if (method == 0) {
            file.assign(data.begin() + long(at), data.begin() + long(at + compressed));
        } else if (method == 8) {
            Inflate inflate{data.data() + at, compressed};
            if (!inflate.Run(unpacked, file, false)) {  // ZIP stores raw deflate, without the zlib header
                error = "the ZIP archive could not be unpacked";
                return false;
            }
        }
        at += compressed;
        if ((flags & 8) && at + 16 <= data.size() && Le32(data, at) == 0x08074B50u) at += 16;
        // GameFAQs' memory-card archives hold one .psv image. Its file table gives the exact offset and size of each
        // member, so the raw Scarface save can be lifted out without writing the image to disk first.
        static const uint8_t kVspMagic[5] = {0x00, 0x56, 0x53, 0x50, 0x00};
        if (file.size() >= 0xA0 && std::memcmp(file.data(), kVspMagic, sizeof(kVspMagic)) == 0 &&
            Le32(file, 0x3C) == 2) {
            uint32_t members = Le32(file, 0x64);
            if (members > 4096 || file.size() < 0xA0 + size_t(members) * 0x3C) {
                error = "the virtual memory card image is damaged";
                return false;
            }
            for (uint32_t i = 0; i < members; i++) {
                size_t entry = 0xA0 + size_t(i) * 0x3C;
                size_t size = Le32(file, entry + 0x10), pos = Le32(file, entry + 0x38);
                if (!size || pos > file.size() || size > file.size() - pos) {
                    error = "the virtual memory card image is damaged";
                    return false;
                }
                RememberRaw(file.data() + pos, size, best);
            }
        } else {
            static const uint8_t kMagic[4] = {0xB0, 0x24, 0x1C, 0xD7};
            for (size_t start = 0; start + 64 <= file.size(); start++) {
                if (std::memcmp(file.data() + start, kMagic, 4) != 0) continue;
                RememberRaw(file.data() + start, file.size() - start, best);
            }
        }
    }
    if (best.empty()) {
        error = "the ZIP archive has no Scarface profile";
        return false;
    }
    out = std::move(best);
    return true;
}

bool LoadSourceFile(const fs::path& path, std::vector<uint8_t>& data, std::string& label, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > 4u * 1024u * 1024u) return false;
    if (!ReadBinary(path, data)) return false;
    if (data.size() > 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0) {
        if (!DecodeZip(data, data, error)) return false;
        label = "PS2";
        return true;
    }
    static const uint8_t kSpsMagic[17] = {0x0D, 0, 0, 0, 'S', 'h', 'a', 'r', 'k', 'P', 'o', 'r', 't', 'S', 'a', 'v', 'e'};
    bool decoded = false;
    if (data.size() >= 4 && std::memcmp(data.data(), "CFU\0", 4) == 0) {
        decoded = DecodeCbs(data, data, error);  // replace the container with the save it holds
    } else if (data.size() >= 12 && std::memcmp(data.data(), "Ps2PowerSave", 12) == 0) {
        decoded = DecodeMax(data, data, error);
    } else if (data.size() >= sizeof(kSpsMagic) && std::memcmp(data.data(), kSpsMagic, sizeof(kSpsMagic)) == 0) {
        decoded = DecodeSps(data, data, error);
    } else if (data.size() >= 1536 && ((data[0] | (unsigned(data[1]) << 8)) & 0x8030u) == 0x8020u &&
               ((data[512] | (unsigned(data[513]) << 8)) & 0x8030u) == 0x8020u &&
               ((data[1024] | (unsigned(data[1025]) << 8)) & 0x8030u) == 0x8020u) {
        decoded = DecodePsu(data, data, error);
    }
    if (decoded) {
        label = "PS2";
        return true;
    }
    if (!error.empty()) return false;
    if (!LooksLikeRawData(data.data(), data.size())) return false;
    size_t count = Le32(data, 0x34);
    label = IsPcCount(count) ? "PC" : "PS2";
    return true;
}

bool LooksLikeSource(const fs::path& path) {
    std::vector<uint8_t> data;
    std::string label, error;
    return LoadSourceFile(path, data, label, error);
}

void FindSourceFiles(const fs::path& dir, int depth, std::vector<fs::path>& out) {
    if (depth > 6) return;
    std::error_code error;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error))
            FindSourceFiles(it->path(), depth + 1, out);
        else if (it->is_regular_file(error) && LooksLikeSource(it->path()))
            out.push_back(it->path());
    }
}

bool One(const std::vector<uint8_t>& source_data, const std::string& detected_label, const fs::path& template_save,
         int slot, const fs::path& target, std::string& label, std::string& error) {
    SlotInfo info;
    std::string slot_error;
    if (!GetSlot(slot, info, slot_error)) {
        error = slot_error;
        return false;
    }

    size_t source_base = SharedBase(source_data);
    std::vector<Record> source_records, template_records;
    if (!Parse(source_data, source_base, source_records, error)) return false;
    if (source_base != 0) {
        error = "choose a PC or PS2 save";
        return false;
    }
    Kind kind;
    if (detected_label == "PS2") {
        if (!IsPs2Count(source_records.size())) {
            error = "unsupported PS2 save format";
            return false;
        }
        kind = Kind::Ps2;
        label = "PS2";
    } else if (IsPcCount(source_records.size())) {
        kind = Kind::Pc;
        label = "PC";
    } else if (IsPs2Count(source_records.size())) {
        kind = Kind::Ps2;
        label = "PS2";
    } else {
        error = "unsupported save format";
        return false;
    }
    if (!detected_label.empty()) label = detected_label;

    // The game rewrites the named-variable table as it plays, so a template's record count varies. Any Xbox-format
    // profile is a valid template; when none exists, build one from the source's own table and the fixed save size.
    std::vector<uint8_t> template_data;
    size_t template_base = 4;
    if (!template_save.empty()) {
        if (!ReadBinary(template_save, template_data)) {
            error = "a save file could not be read";
            return false;
        }
        template_base = SharedBase(template_data);
        if (!Parse(template_data, template_base, template_records, error)) return false;
        if (template_base != 4 || template_records.empty()) {
            error = "the Xbox save template is damaged";
            return false;
        }
    } else {
        BuildXboxTemplate(source_data, info, template_data);
        // A PS2 save can carry hundreds of platform-specific variables. The Xbox value area is smaller, so use the
        // known PC/Xbox variable set as the synthesized template instead of letting late records land in a signature.
        size_t count = std::min(source_records.size(), kPcRecordsExtended);
        template_records.assign(source_records.begin(), source_records.begin() + count);
        WriteLe32(template_data, template_base + 0x34, uint32_t(count));
    }

    std::map<std::string, std::vector<uint8_t>> values;
    for (size_t i = 0; i < source_records.size(); i++) {
        std::vector<uint8_t> value;
        if (!ValueAt(source_data, source_base, kind, i, source_records[i], value, error)) return false;
        values[source_records[i].name] = std::move(value);
    }

    std::vector<uint8_t> out = template_data;
    for (size_t i = 0; i < template_records.size(); i++) {
        auto found = values.find(template_records[i].name);
        if (found == values.end()) continue;  // a value the Xbox profile has and the source does not: keep it
        size_t at = template_base + 0x38 + i * 17;
        size_t slot = template_base + SlotOffset(Kind::Xbox, i);
        size_t limit = std::min(found->second.size(), kSlotValue);
        if (slot < 0x1FEC && slot + limit > 0x1FEC) limit = 0x1FEC - slot;  // the last slot meets a signature
        if (slot >= 0x2000 && slot < 0x3FEC && slot + limit > 0x3FEC) limit = 0x3FEC - slot;
        if (slot >= 0x4000 && slot < 0x4601 && slot + limit > 0x4601) limit = 0x4601 - slot;
        if (at + 17 > out.size() || slot + limit > out.size()) {
            error = "the converted save would not fit";
            return false;
        }
        out[at + 16] = uint8_t(limit);
        if (limit) std::memcpy(out.data() + slot, found->second.data(), limit);
    }
    if (out.size() < kSlotIdentity + 4) {
        error = "the save file is too small";
        return false;
    }
    std::memcpy(out.data() + kSlotIdentity, info.identity, sizeof(info.identity));
    Resign(out);

    return Install(out, slot, target, {}, error);
}

}  // namespace convert

namespace {

// Xbox save folder ids are twelve hex digits. Backups keep that prefix but add a suffix, so they are not slots.
bool IsFolderId(const std::string& name) {
    if (name.size() != 12) return false;
    for (char c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

// Private launcher metadata: the fourth id and identity are created by the game, not chosen by this program. Keeping
// them lets that row import again after the player deletes the save. The game ignores files in the UDATA root.
const char* kSlot4MetaName = ".recomp-slot4";

std::string HexByte(uint8_t value) {
    char text[3];
    std::snprintf(text, sizeof(text), "%02X", value);
    return text;
}

bool ParseHex(const std::string& text, std::vector<uint8_t>& out) {
    if (text.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0;
        if (std::sscanf(text.substr(i, 2).c_str(), "%2x", &value) != 1) return false;
        out.push_back(uint8_t(value));
    }
    return true;
}

fs::path Slot4MetaPath() {
    fs::path root = Root();
    return root.empty() ? fs::path() : root / kSlot4MetaName;
}

bool LoadSlot4Meta(SlotInfo& out) {
    fs::path path = Slot4MetaPath();
    if (path.empty()) return false;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string folder, identity;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        std::string text = Trim(line);
        if (text.rfind("folder=", 0) == 0) folder = Trim(text.substr(7));
        else if (text.rfind("identity=", 0) == 0) identity = Trim(text.substr(9));
    }
    fclose(f);
    if (!IsFolderId(folder) || identity.size() != 8) return false;
    for (int i = 0; i < 3; i++)
        if (folder == convert::kFixedSlotFolderIds[i]) return false;
    std::vector<uint8_t> bytes;
    if (!ParseHex(identity, bytes) || bytes.size() != 4) return false;
    out.folder = folder;
    std::memcpy(out.identity, bytes.data(), sizeof(out.identity));
    return true;
}

bool SaveSlot4Meta(const SlotInfo& info) {
    fs::path path = Slot4MetaPath();
    if (path.empty() || !IsFolderId(info.folder)) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::string text = "folder=" + info.folder + "\nidentity=";
    for (uint8_t byte : info.identity) text += HexByte(byte);
    text += "\n";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

bool ReadIdentity(const fs::path& data_path, uint8_t out[4]) {
    std::vector<uint8_t> data;
    if (!convert::ReadBinary(data_path, data) || convert::SharedBase(data) != 4 ||
        data.size() < convert::kSlotIdentity + 4)
        return false;
    std::memcpy(out, data.data() + convert::kSlotIdentity, 4);
    return true;
}

}  // namespace

bool GetSlot(int slot, SlotInfo& out, std::string& error) {
    if (slot < 1 || slot > 4) {
        error = "choose Xbox profile 1, 2, 3 or 4";
        return false;
    }
    out.folder = convert::kFixedSlotFolderIds[slot - 1];
    std::memcpy(out.identity, convert::kFixedSlotIds[slot - 1], sizeof(out.identity));
    if (slot == 4) {
        // The game rewrites this identity each time it saves. Prefer the live value, then a remembered one, then the
        // observed default. Keep the private metadata current so the row still works after the save is deleted.
        uint8_t identity[4];
        SlotInfo remembered;
        if (ReadIdentity(Root() / out.folder / "save.dat", identity)) {
            std::memcpy(out.identity, identity, sizeof(out.identity));
        } else if (LoadSlot4Meta(remembered) && remembered.folder == out.folder) {
            std::memcpy(out.identity, remembered.identity, sizeof(out.identity));
        }
        if (!LoadSlot4Meta(remembered) || remembered.folder != out.folder ||
            std::memcmp(remembered.identity, out.identity, sizeof(out.identity)) != 0)
            SaveSlot4Meta(out);
    }
    return true;
}

bool Convert(const fs::path& source, int slot, std::string& status, bool& error) {
    error = false;
    fs::path root = Root();
    if (root.empty()) {
        status = "This game has no save title id set.";
        error = true;
        return false;
    }
    std::vector<fs::path> candidates;
    std::error_code ec;
    if (fs::is_regular_file(source, ec)) {
        if (convert::LooksLikeSource(source)) candidates.push_back(source);
    } else if (fs::is_directory(source, ec)) {
        convert::FindSourceFiles(source, 0, candidates);
    }
    if (candidates.empty()) {
        status = "No PC or PS2 save was found there. Choose the save file or the folder holding it.";
        error = true;
        return false;
    }
    std::vector<uint8_t> source_data;
    std::string source_label, source_error;
    if (!convert::LoadSourceFile(candidates.front(), source_data, source_label, source_error)) {
        status = "The save could not be read" + (source_error.empty() ? std::string(".") : ": " + source_error);
        error = true;
        return false;
    }
    // A real game save is a better template when one exists, but it is not required: One() builds a template from
    // the source when every profile is empty.
    fs::path template_save;
    List saves = Enumerate();
    if (!saves.saves.empty()) {
        template_save = saves.saves.front().path / "save.dat";
        if (!fs::is_regular_file(template_save, ec)) {
            status = "The Xbox save template could not be read.";
            error = true;
            return false;
        }
    }

    SlotInfo info;
    std::string slot_error;
    if (!GetSlot(slot, info, slot_error)) {
        status = slot_error;
        error = true;
        return false;
    }
    fs::path target = root / info.folder;
    if (fs::exists(target, ec)) {
        status = "Xbox profile " + std::to_string(slot) +
                 " is already in use. Delete that save in the save manager first.";
        error = true;
        return false;
    }

    std::string last_error, label;
    if (!convert::One(source_data, source_label, template_save, slot, target, label, last_error)) {
        status = "The save could not be converted" +
                 (last_error.empty() ? std::string(".") : ": " + last_error);
        error = true;
        return false;
    }
    status = "Imported one save (" + label + ") into profile " + std::to_string(slot) + ".";
    if (candidates.size() > 1)
        status += " " + std::to_string(candidates.size() - 1) + " other save files were ignored.";
    return true;
}

bool PickSource(bool files, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    // The launcher already owns COM on its window thread; S_FALSE means "already initialized here", so only S_OK
    // may be undone below. Undoing S_FALSE used to unbalance the launcher's COM lifetime and could make the picker fail.
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = init == S_OK;
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        error = "Windows shell initialization failed.";
        return false;
    }

    const GUID clsid = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
    const GUID iid = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
    IFileOpenDialog* dialog = nullptr;
    bool picked = false;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, reinterpret_cast<void**>(&dialog)))) {
        error = "The Windows file dialog could not be created.";
        if (uninit) CoUninitialize();
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT |
                       (files ? DWORD(FOS_FILEMUSTEXIST) : DWORD(FOS_PICKFOLDERS)));
    if (files) {
        // Many PC saves have no extension, so "All files" is the default; the shorter filter just helps.
        COMDLG_FILTERSPEC types[] = {{L"Save files (*.dat; *.cbs; *.max; *.sps; *.psu; *.zip)",
                                      L"*.dat;*.cbs;*.max;*.sps;*.psu;*.zip"},
                                     {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, types);
        dialog->SetFileTypeIndex(2);
        dialog->SetTitle(L"Choose a save file to import");
    } else {
        dialog->SetTitle(L"Choose the folder holding the save to import");
    }
    HWND owner = static_cast<HWND>(Window());
    if (!owner) owner = GetActiveWindow();
    HRESULT shown = dialog->Show(owner);
    if (shown == S_OK) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR wide = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide)) && wide) {
                out = Narrow(wide);
                picked = !out.empty();
                CoTaskMemFree(wide);
            }
            item->Release();
        }
    } else if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        char code[32];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(shown));
        error = std::string("The Windows file dialog failed (") + code + ").";
        LOGW("Saves: %s", error.c_str());
    }
    dialog->Release();
    if (uninit) CoUninitialize();
    return picked;
}

void OpenFolder(const fs::path& path, bool select) {
    std::wstring args;
    if (select) {
        args = L"explorer.exe /select,\"" + path.wstring() + L"\"";
    } else {
        args = L"explorer.exe \"" + path.wstring() + L"\"";
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, args.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

const char* kGameRunningSave = "Save management is unavailable while the game is running. Importing games is not allowed.";

// Imports one original Xbox save into the chosen profile. An imported save is rebound to that slot: its profile
// identity, label and signatures are rewritten. An occupied slot is rejected; the player has to delete that save in
// the manager first. A PC/PS2 file (including a CodeBreaker .cbs) is converted, while picking a member of an Xbox
// save folder uses that folder.
bool Import(fs::path source, int slot, std::string& status, bool& error) {
    error = false;
    SlotInfo info;
    std::string slot_error;
    if (!GetSlot(slot, info, slot_error)) {
        status = slot_error;
        error = true;
        return false;
    }
    fs::path root = Root();
    if (root.empty()) {
        status = "This game has no save title id set.";
        error = true;
        return false;
    }
    std::error_code ec;
    if (fs::is_regular_file(source, ec)) {
        std::string extension = source.extension().string();
        for (char& c : extension) c = char(std::tolower(uint8_t(c)));
        std::string name = source.filename().string();
        bool xbox_member = name == "save.dat" || name == "SaveMeta.xbx" || name == "SaveImage.xbx";
        if (xbox_member && IsSaveFolder(source.parent_path())) {
            source = source.parent_path();
        } else if (extension == ".cbs" || convert::LooksLikeSource(source)) {
            return Convert(source, slot, status, error);
        } else {
            status = "Choose a PC/PS2 save, a .cbs/.max/.sps/.xps/.psu/.zip file, or the folder holding an Xbox save.";
            error = true;
            return false;
        }
    }
    if (!fs::is_directory(source, ec)) {
        status = "That folder could not be read.";
        error = true;
        return false;
    }
    std::vector<fs::path> found;
    FindSaves(source, 0, found);
    if (found.empty()) return Convert(source, slot, status, error);
    if (found.size() > 1) {
        status = "Choose one save when importing into a specific Xbox profile.";
        error = true;
        return false;
    }
    fs::create_directories(root, ec);
    if (ec) {
        status = "Could not create " + Utf8Path(root) + ": " + ec.message();
        error = true;
        return false;
    }
    fs::path target = root / info.folder;
    if (fs::exists(target, ec)) {
        status = "Xbox profile " + std::to_string(slot) +
                 " is already in use. Delete that save in the save manager first.";
        error = true;
        return false;
    }
    // the title's own metadata, when the picked folder carries it and this install has none
    for (const wchar_t* meta : {L"TitleMeta.xbx", L"TitleImage.xbx"}) {
        if (fs::is_regular_file(root / meta, ec)) continue;
        std::vector<fs::path> candidates = {source / meta, source / "UDATA" / kGame.title_id / meta};
        if (!source.parent_path().empty()) candidates.push_back(source.parent_path() / meta);
        for (const fs::path& candidate : candidates) {
            if (!fs::is_regular_file(candidate, ec)) continue;
            fs::copy_file(candidate, root / meta, fs::copy_options::overwrite_existing, ec);
            break;
        }
    }

    const fs::path& dir = found.front();
    std::error_code same_error;
    if (fs::equivalent(dir, target, same_error)) {
        status = "That folder is already Xbox profile " + std::to_string(slot) + ".";
        return true;
    }
    fs::path data = dir / "save.dat";
    if (!fs::is_regular_file(data, ec)) {
        status = "The selected save folder has no save.dat.";
        error = true;
        return false;
    }
    std::string last_error;
    if (!convert::RebindXboxSave(data, dir / "SaveImage.xbx", slot, target, last_error)) {
        status = "The save could not be imported" + (last_error.empty() ? std::string(".") : ": " + last_error);
        error = true;
        return false;
    }
    LOGI("Saves: imported %s as %s", Utf8Path(dir).c_str(), Utf8Path(target).c_str());
    status = "Imported one save (Xbox) into profile " + std::to_string(slot) + ".";
    return true;
}

}  // namespace

bool CommandLine() {
    const wchar_t* line = GetCommandLineW();
    const wchar_t* at = std::wcsstr(line, L"--import-save");
    if (!at) return false;
    bool start = at == line || at[-1] == L' ' || at[-1] == L'\t' || at[-1] == L'"';
    if (!start || (at[13] != L' ' && at[13] != L'\t')) return false;
    at += 13;
    while (*at == L' ' || *at == L'\t') at++;
    std::wstring value;
    if (*at == L'"') {
        for (at++; *at && *at != L'"'; at++) value += *at;
    } else {
        for (; *at && *at != L' ' && *at != L'\t'; at++) value += *at;
    }
    if (value.empty()) return false;
    int slot = 0;
    const wchar_t* slot_at = std::wcsstr(at, L"--save-slot");
    if (slot_at) {
        slot_at += 11;
        while (*slot_at == L' ' || *slot_at == L'\t') slot_at++;
        if (*slot_at >= L'1' && *slot_at <= L'4') slot = int(*slot_at - L'0');
    }
    if (slot < 1 || slot > 4) {
        const wchar_t* message = L"Choose the target with --save-slot 1, 2, 3 or 4.";
        LOGW("Saves: %s", Narrow(message).c_str());
        MessageBoxW(nullptr, message, kGame.short_title, MB_OK | MB_ICONWARNING);
        return true;
    }
    std::string status;
    bool error = false;
    if (Import(fs::path(value), slot, status, error)) {
        LOGI("Saves: %s", status.c_str());
    } else {
        LOGW("Saves: %s", status.c_str());
        MessageBoxW(nullptr, (L"The save could not be imported or converted.\n\n" + Widen(status)).c_str(),
                    kGame.short_title, MB_OK | MB_ICONWARNING);
    }
    return true;
}

void Ui(bool game_running) {
    if (!kGame.title_id || !*kGame.title_id) {
        ImGui::TextDisabled("This game has no save folder set.");
        return;
    }
    static std::string status;
    static bool status_error = false;
    static fs::path pending_delete;
    static std::string pending_name;

    fs::path root = Root();
    std::error_code ec;
    fs::create_directories(root, ec);

    List list = Enumerate();
    if (!list.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", list.error.c_str());
        ImGui::PopStyleColor();
        return;
    }

    auto find_save = [&](const fs::path& folder) -> const Save* {
        for (const Save& save : list.saves)
            if (save.path.filename() == folder.filename()) return &save;
        return nullptr;
    };

    auto start_import = [&](int target_slot, bool files) {
        if (game_running) {
            status = kGameRunningSave;
            status_error = true;
            return;
        }
        std::string picked, pick_error;
        if (!PickSource(files, picked, pick_error)) {
            if (!pick_error.empty()) {
                status = pick_error;
                status_error = true;
            }
            return;
        }
        status.clear();
        Import(FromUtf8(picked), target_slot, status, status_error);
        if (status_error) LOGW("Saves: %s", status.c_str());
    };

    if (game_running) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", kGameRunningSave);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::TextWrapped("This table is the save manager. Any profile can be created by the game or imported here while "
                       "it is empty. Import From File opens the file picker; Import From Folder opens the folder "
                       "picker. Original Xbox saves and PC/PS2 saves, including CodeBreaker .cbs, Max Drive .max, "
                       "SharkPort .sps/.xps, EMS .psu and virtual-memory-card .zip files, are detected automatically. "
                       "Xbox saves are rebound and re-signed for the chosen profile; PC/PS2 saves are converted into "
                       "it. No prepared Xbox profile is needed. Source files are never changed. Changes show in the "
                       "load menu after a restart.");
    ImGui::Spacing();

    bool ask_delete = false;
    if (ImGui::BeginTable("##profiles", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 320.0f);
        ImGui::TableHeadersRow();

        for (int slot = 1; slot <= 4; slot++) {
            SlotInfo info;
            std::string slot_error;
            GetSlot(slot, info, slot_error);
            fs::path folder = root / info.folder;
            bool occupied = fs::exists(folder, ec);
            const Save* save = occupied ? find_save(folder) : nullptr;
            std::string detail;
            if (!occupied) {
                detail = "empty";
            } else if (save) {
                detail = "occupied - ";
                detail += save->name.empty() ? folder.filename().string() : save->name;
                detail += ", ";
                detail += save->when;
                detail += ", ";
                detail += SizeText(save->bytes);
            } else {
                detail = "occupied - no readable save data";
            }

            ImGui::TableNextRow();
            ImGui::PushID(slot);
            ImGui::TableNextColumn();
            ImGui::Text("Profile %d", slot);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", detail.c_str());
            ImGui::TableNextColumn();
            if (game_running) ImGui::BeginDisabled();
            if (!occupied) {
                if (ImGui::Button("Import From File", ImVec2(150.0f, 0.0f))) start_import(slot, true);
                ImGui::SameLine();
                if (ImGui::Button("Import From Folder", ImVec2(160.0f, 0.0f))) start_import(slot, false);
            } else {
                if (ImGui::Button("Open folder", ImVec2(150.0f, 0.0f))) OpenFolder(folder, true);
                ImGui::SameLine();
                if (ImGui::Button("Delete", ImVec2(160.0f, 0.0f))) {
                    pending_delete = folder;
                    pending_name = save && !save->name.empty() ? save->name : folder.filename().string();
                    ask_delete = true;
                }
            }
            if (game_running) ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Utf8Path(root).c_str());
    if (!status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, status_error ? kDanger : kAccent);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
    }

    // the popup lives in the tab's window, not the table's id stack
    if (!game_running && ask_delete) ImGui::OpenPopup("Delete this save?");
    if (ImGui::BeginPopupModal("Delete this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\" from this PC?", pending_name.c_str());
        ImGui::TextDisabled("The game must be restarted to forget it.");
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            std::error_code remove_error;
            fs::remove_all(pending_delete, remove_error);
            if (remove_error) {
                status = "Could not delete the save: " + remove_error.message();
                status_error = true;
                LOGW("Saves: %s", status.c_str());
            } else {
                status = "Deleted \"" + pending_name + "\".";
                status_error = false;
                LOGI("Saves: deleted %s", Utf8Path(pending_delete).c_str());
            }
            pending_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            pending_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace app::saves
