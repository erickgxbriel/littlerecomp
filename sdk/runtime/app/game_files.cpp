// The game in assets or the launcher's choice: its files, or a disc image (.iso) with this executable's xbe on it.
// The xbe must be the one this executable was recompiled from; another release's would run its own data under this
// code.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <memory>
#include <string_view>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

// the xbe this executable was recompiled from, or another release of it with its own config
bool Runs(const std::string& sha) {
    if (sha == kImageSha1) return true;
    for (auto& [xbe, release] : kGame.releases)
        if (std::string_view(xbe) == IMAGE_XBE && sha == release) return true;
    return false;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// a folder with default.xbe beside the game's data, or a disc image holding it; false with the reason
bool ReadGameAt(const fs::path& path, std::vector<uint8_t>& xbe, std::unique_ptr<kernel::Disc>& disc,
                std::string& error) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        if (!ReadHostFile(path / IMAGE_XBE, xbe)) {
            error = std::string(IMAGE_XBE) + " was not found in this folder";
            return false;
        }
        return true;
    }
    if (fs::is_regular_file(path, ec)) {
        auto image = std::make_unique<kernel::Disc>();
        if (!image->Open(path, error)) return false;
        if (!image->Load(IMAGE_XBE, xbe)) {
            error = std::string("the image has no ") + IMAGE_XBE;
            return false;
        }
        disc = std::move(image);
        return true;
    }
    error = "the path does not exist";
    return false;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

std::string Utf8Path(const fs::path& path) {
    std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path FromUtf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

GameProbe ProbeGame(const fs::path& path) {
    GameProbe probe;
    std::vector<uint8_t> xbe;
    std::unique_ptr<kernel::Disc> disc;  // dropped: probing mounts nothing
    if (!ReadGameAt(path, xbe, disc, probe.error)) return probe;
    probe.found = true;
    probe.sha = Sha1(xbe);
    probe.match = Runs(probe.sha);
    return probe;
}

bool LoadGameAt(const fs::path& path, GameFiles& game, std::string& error) {
    std::vector<uint8_t> xbe;
    std::unique_ptr<kernel::Disc> disc;
    if (!ReadGameAt(path, xbe, disc, error)) return false;
    std::error_code ec;
    game.dir = fs::is_directory(path, ec) ? path : path.parent_path();
    game.xbe = std::move(xbe);
    kernel::MountDisc(std::move(disc));
    LOGR("Game: using %s", Utf8Path(path).c_str());
    return true;
}

bool FindGame(GameFiles& game) {
    // the launcher's choice first, then assets: the extracted files before any image
    std::vector<fs::path> candidates;
    if (!Get().game.empty()) candidates.push_back(FromUtf8(Get().game));
    fs::path assets = FindAssets();
    if (!assets.empty()) {
        candidates.push_back(assets);
        std::vector<fs::path> isos;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
        std::sort(isos.begin(), isos.end());
        candidates.insert(candidates.end(), isos.begin(), isos.end());
    }

    // the first copy with this xbe wins; another release is kept to offer
    GameFiles other;
    std::wstring other_name;
    for (const fs::path& path : candidates) {
        GameProbe probe = ProbeGame(path);
        if (!probe.found) {
            LOGW("game: %s: %s", Utf8Path(path).c_str(), probe.error.c_str());
            continue;
        }
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, Utf8Path(path).c_str(), probe.sha.c_str(),
             probe.match ? "" : ", another release");
        std::string error;
        if (probe.match) {
            if (LoadGameAt(path, game, error)) return true;
            LOGW("game: %s: %s", Utf8Path(path).c_str(), error.c_str());
            continue;
        }
        if (other_name.empty() && LoadGameAt(path, other, error)) other_name = path.filename().wstring();
    }

    if (other_name.empty()) {
        MessageBoxW(nullptr, kGame.missing, kGame.short_title, MB_ICONERROR);
        return false;
    }
    std::wstring text = other_name +
                        L" is a different release of the game than this build was made from, and will most likely "
                        L"crash.\n\nStart it anyway?";
    if (MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return false;
    game = std::move(other);
    return true;
}

}  // namespace app
