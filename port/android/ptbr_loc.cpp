#include "ptbr_loc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "android_app.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace ptbr_loc {

namespace {

struct BuiltinPair {
    const char* key;
    const char* pt;
};

// Curated built-in PT-BR dictionary covering menus, prompts, HUD stats, objectives, and key dialogue cues.
// External *.tsv packs placed in <ExeDir>/mods/ptbr/ override and extend these entries cleanly.
constexpr BuiltinPair kBuiltinPairs[] = {
    // Title, Main Menu & Profiles
    {"Press [A] to start", "Pressione [A] para iniciar"},
    {"Press A to start", "Pressione A para iniciar"},
    {"NEW GAME", "NOVO JOGO"},
    {"New Game", "Novo Jogo"},
    {"LOAD GAME", "CARREGAR JOGO"},
    {"Load Game", "Carregar Jogo"},
    {"CONTINUE", "CONTINUAR"},
    {"Continue", "Continuar"},
    {"OPTIONS", "OPÇÕES"},
    {"Options", "Opções"},
    {"PROFILES", "PERFIS"},
    {"Profiles", "Perfis"},
    {"Options and Profiles", "Opções e Perfis"},
    {"Exit Tutorial", "Sair do Tutorial"},
    {"QUIT GAME", "SAIR DO JOGO"},
    {"Quit Game", "Sair do Jogo"},
    {"BACK", "VOLTAR"},
    {"Back", "Voltar"},
    {"SELECT", "SELECIONAR"},
    {"Select", "Selecionar"},
    {"CANCEL", "CANCELAR"},
    {"Cancel", "Cancelar"},
    {"CLOSE", "FECHAR"},
    {"Close", "Fechar"},
    {"CONFIRM", "CONFIRMAR"},
    {"Confirm", "Confirmar"},
    {"PROCEED", "PROSSEGUIR"},
    {"Proceed", "Prosseguir"},
    {"YES", "SIM"},
    {"Yes", "Sim"},
    {"NO", "NÃO"},
    {"No", "Não"},
    {"EXIT", "SAIR"},
    {"Exit", "Sair"},
    {"CREATE", "CRIAR"},
    {"Create", "Criar"},
    {"DELETE", "EXCLUIR"},
    {"Delete", "Excluir"},
    {"SAVE", "SALVAR"},
    {"Save", "Salvar"},
    {"LOAD", "CARREGAR"},
    {"Load", "Carregar"},
    {"LOADING", "CARREGANDO"},
    {"Loading...", "Carregando..."},
    {"No Save Games present.", "Nenhum jogo salvo encontrado."},
    {"Profile loads successfully", "Perfil carregado com sucesso"},

    // Pause Menu & Empire Tabs
    {"RESUME", "CONTINUAR"},
    {"Resume", "Continuar"},
    {"RESTART", "REINICIAR"},
    {"Restart", "Reiniciar"},
    {"RESTART MISSION", "REINICIAR MISSÃO"},
    {"Restart Mission", "Reiniciar Missão"},
    {"EMPIRE", "IMPÉRIO"},
    {"Empire", "Império"},
    {"MAP", "MAPA"},
    {"Map", "Mapa"},
    {"STATS", "ESTATÍSTICAS"},
    {"Stats", "Estatísticas"},
    {"CHEATS", "TRAPAÇAS"},
    {"Cheats", "Trapaças"},
    {"TAPE PLAYER", "TOCA-FITAS"},
    {"Tape Player", "Toca-Fitas"},
    {"AUDIO", "ÁUDIO"},
    {"Audio", "Áudio"},
    {"DISPLAY", "VÍDEO"},
    {"Display", "Vídeo"},
    {"CONTROLS", "CONTROLES"},
    {"Controls", "Controles"},
    {"SUBTITLES", "LEGENDAS"},
    {"Subtitles", "Legendas"},
    {"ON", "LIGADO"},
    {"OFF", "DESLIGADO"},
    {"Game Options", "Opções do Jogo"},
    {"Sound Options", "Opções de Áudio"},
    {"Control Options", "Opções de Controle"},
    {"Display Options", "Opções de Vídeo"},
    {"Save Game", "Salvar Jogo"},
    {"Quit to Main Menu", "Sair para o Menu Principal"},

    // HUD & Empire Terms
    {"REPUTATION", "REPUTAÇÃO"},
    {"Reputation", "Reputação"},
    {"DIRTY CASH", "DINHEIRO SUJO"},
    {"Dirty Cash", "Dinheiro Sujo"},
    {"CLEAN CASH", "DINHEIRO LIMPO"},
    {"Clean Cash", "Dinheiro Limpo"},
    {"TURF", "TERRITÓRIO"},
    {"Turf", "Território"},
    {"COP HEAT", "CALOR POLICIAL"},
    {"Cop Heat", "Calor Policial"},
    {"GANG HEAT", "CALOR DAS GANGUES"},
    {"Gang Heat", "Calor das Gangues"},
    {"BLIND RAGE", "FÚRIA CEGA"},
    {"Blind Rage", "Fúria Cega"},
    {"MISSION COMPLETE", "MISSÃO CONCLUÍDA"},
    {"Mission Complete", "Missão Concluída"},
    {"MISSION FAILED", "MISSÃO FRACASSADA"},
    {"Mission Failed", "Missão Fracassada"},
    {"YOU ARE DEAD", "VOCÊ MORREU"},
    {"BUSTED", "PRESO"},
    {"EXOTICS", "EXÓTICOS"},
    {"Exotics", "Exóticos"},
    {"DISTRIBUTION", "DISTRIBUIÇÃO"},
    {"Distribution", "Distribuição"},
    {"FRONTS", "FACHADAS"},
    {"Fronts", "Fachadas"},
    {"HENCHMEN", "CAPANGAS"},
    {"Henchmen", "Capangas"},
    {"VEHICLES", "VEÍCULOS"},
    {"Vehicles", "Veículos"},
    {"BOATS", "BARCOS"},
    {"Boats", "Barcos"},
    {"WEAPONS", "ARMAS"},
    {"Weapons", "Armas"},
    {"SAT PHONE", "TELEFONE VIA SATÉLITE"},
    {"Sat Phone", "Telefone Satélite"},

    // Interaction Prompts
    {"Weapons Locker", "Armário de Armas"},
    {"Enter Vehicle", "Entrar no Veículo"},
    {"Enter Garage", "Entrar na Garagem"},
    {"Enter Boathouse", "Entrar no Ancoradouro"},
    {"Dismiss Henchman", "Dispensar Capanga"},
    {"Open Door", "Abrir Porta"},
    {"Plant Bomb", "Plantar Bomba"},
    {"Collect Ammo", "Pegar Munição"},
    {"Buy Weapons", "Comprar Armas"},
    {"Buy drugs", "Comprar drogas"},
    {"Launder money", "Lavar dinheiro"},
    {"Refill Health", "Recuperar Vida"},
    {"Get information", "Obter informação"},
    {"Distribute", "Distribuir"},
    {"Equip", "Equipar"},
    {"Sell", "Vender"},

    // Opening / Mansion / Early Miami Mission Cues
    {"I told you to tell them you were in a sanitarium.",
     "Eu mandei você dizer que estava em um sanatório."},
    {"You're still alive, Tony.",
     "Você ainda está vivo, Tony."},
    {"Who put this thing together? Me, that's who!",
     "Quem montou tudo isso? Fui eu, porra!"},
    {"Escape the mansion!", "Fuja da mansão!"},
    {"Escape Tony's mansion!", "Fuja da mansão do Tony!"},
    {"Kill Sosa's hitmen!", "Elimine os assassinos do Sosa!"},
    {"Get to the garage.", "Vá até a garagem."},
    {"Get to the car!", "Vá até o carro!"},
    {"Clear the courtyard of Sosa's men.", "Limpe o pátio dos homens do Sosa."},
    {"Take out the remaining assassins.", "Acabe com os assassinos restantes."},
    {"Use Blind Rage to regain health.", "Use a Fúria Cega para recuperar vida."},
    {"Taunt enemies to fill your Balls meter.", "Provoque os inimigos para encher a barra de Balls."},
    {"Press R3 to enter first-person aim.", "Pressione R3 para entrar na mira em primeira pessoa."},
};

constexpr uint32_t kGuestLocPageSize = 0x10000;  // 64 KB guest scratch page for PT-BR UTF-16LE strings

std::mutex g_loc_lock;
std::unordered_map<std::string, std::string> g_exact_map;
std::unordered_map<std::string, std::string> g_lower_map;
std::unordered_map<std::string, uint32_t> g_guest_va_cache;
std::unordered_set<std::string> g_captured_seen;

int g_builtin_count = 0;
int g_external_count = 0;
int g_captured_count = 0;

uint32_t g_loc_base = 0;
uint32_t g_loc_cursor = 0;

std::string g_active_sub_text;
std::chrono::steady_clock::time_point g_active_sub_tp{};
bool g_active_sub_visible = false;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<uint8_t>(c)));
    return s;
}

std::string WideToUtf8(const uint8_t* base, uint32_t wide_va, size_t max_chars = 512) {
    if (!base || !wide_va || !mem::IsCommitted(wide_va)) return {};
    std::string out;
    out.reserve(64);
    for (size_t i = 0; i < max_chars; ++i) {
        uint32_t va = wide_va + uint32_t(i * 2u);
        if (!mem::IsCommitted(va) || !mem::IsCommitted(va + 1u)) break;
        uint16_t ch = 0;
        std::memcpy(&ch, base + va, 2);
        if (ch == 0) break;
        if (ch < 0x80u) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (ch >> 6)));
            out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xE0u | (ch >> 12)));
            out.push_back(static_cast<char>(0x80u | ((ch >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
        }
    }
    return out;
}

std::string ReadNarrowSafe(const uint8_t* base, uint32_t va, size_t max_len = 64) {
    if (!base || !va || !mem::IsCommitted(va)) return {};
    std::string out;
    for (size_t i = 0; i < max_len; ++i) {
        if (!mem::IsCommitted(va + uint32_t(i))) break;
        char c = static_cast<char>(base[va + i]);
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

std::vector<uint16_t> Utf8ToGuestUtf16(const std::string& utf8) {
    std::vector<uint16_t> out;
    out.reserve(utf8.size() + 1);
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if (b0 < 0x80u) {
            cp = b0;
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
            i += 2;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            cp = ((b0 & 0x0Fu) << 12) | ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu);
            i += 3;
        } else {
            ++i;
            continue;
        }
        // Normalize glyphs absent from Scarface's NTSC-U Xbox bitmap font atlas (Ç/Ã/Õ),
        // while keeping full UTF-8 accents intact in the Android HD Roboto subtitle overlay.
        if (cp == 0x2014u || cp == 0x2013u) cp = '-';
        else if (cp == 0x2018u || cp == 0x2019u) cp = '\'';
        else if (cp == 0x201Cu || cp == 0x201Du) cp = '"';
        else if (cp == 0x00C7u) cp = 'C';  // Ç
        else if (cp == 0x00E7u) cp = 'c';  // ç
        else if (cp == 0x00C3u) cp = 'A';  // Ã
        else if (cp == 0x00E3u) cp = 'a';  // ã
        else if (cp == 0x00D5u) cp = 'O';  // Õ
        else if (cp == 0x00F5u) cp = 'o';  // õ
        if (cp <= 0xFFFFu) out.push_back(static_cast<uint16_t>(cp));
    }
    out.push_back(0);
    return out;
}

uint32_t AllocGuestWideStringLocked(const std::string& utf8) {
    if (auto it = g_guest_va_cache.find(utf8); it != g_guest_va_cache.end()) {
        return it->second;
    }
    if (!g_loc_base) {
        uint32_t va = 0, sz = kGuestLocPageSize;
        if (mem::AllocateVirtual(va, sz, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) {
            return 0;
        }
        g_loc_base = va;
        g_loc_cursor = va;
    }
    std::vector<uint16_t> w = Utf8ToGuestUtf16(utf8);
    uint32_t bytes = uint32_t(w.size() * sizeof(uint16_t));
    uint32_t aligned = (bytes + 3u) & ~3u;
    if (g_loc_cursor + aligned > g_loc_base + kGuestLocPageSize) {
        return 0;  // Pool full; keep graceful fallback
    }
    uint8_t* base = mem::Base();
    if (!base) return 0;
    uint32_t dst = g_loc_cursor;
    std::memcpy(base + dst, w.data(), bytes);
    g_loc_cursor += aligned;
    g_guest_va_cache[utf8] = dst;
    return dst;
}

void InsertPairLocked(const std::string& key, const std::string& val) {
    if (key.empty() || val.empty()) return;
    g_exact_map[key] = val;
    g_lower_map[LowerAscii(Trim(key))] = val;
}

bool LookupTranslationLocked(const std::string& key, const std::string& orig_utf8, std::string& out_pt) {
    if (!key.empty()) {
        if (auto it = g_exact_map.find(key); it != g_exact_map.end()) {
            out_pt = it->second;
            return true;
        }
        if (auto it = g_lower_map.find(LowerAscii(key)); it != g_lower_map.end()) {
            out_pt = it->second;
            return true;
        }
    }
    if (!orig_utf8.empty()) {
        if (auto it = g_exact_map.find(orig_utf8); it != g_exact_map.end()) {
            out_pt = it->second;
            return true;
        }
        std::string low = LowerAscii(Trim(orig_utf8));
        if (auto it = g_lower_map.find(low); it != g_lower_map.end()) {
            out_pt = it->second;
            return true;
        }
    }
    return false;
}

void MaybeCaptureKeyLocked(const std::string& key, const std::string& orig_utf8) {
    if (!app::AndroidSettings().capture_subtitles) return;
    const std::string& raw = !orig_utf8.empty() ? orig_utf8 : key;
    if (raw.size() < 2) return;
    if (!g_captured_seen.insert(raw).second) return;

    fs::path dir = app::ExeDir() / "mods" / "ptbr";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / "captured_keys.tsv", std::ios::app);
    if (out) {
        std::string clean = raw;
        std::replace(clean.begin(), clean.end(), '\t', ' ');
        std::replace(clean.begin(), clean.end(), '\r', ' ');
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        out << clean << "\t\n";
        g_captured_count++;
    }
}

void LoadExternalTsvLocked() {
    g_external_count = 0;
    fs::path ptbr_dir = app::ExeDir() / "mods" / "ptbr";
    std::error_code ec;
    if (!fs::is_directory(ptbr_dir, ec)) return;

    for (const auto& entry : fs::directory_iterator(ptbr_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (LowerAscii(entry.path().extension().string()) != ".tsv") continue;
        if (LowerAscii(entry.path().filename().string()) == "captured_keys.tsv") continue;

        std::ifstream f(entry.path());
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string k = Trim(line.substr(0, tab));
            std::string v = Trim(line.substr(tab + 1));
            if (!k.empty() && !v.empty()) {
                InsertPairLocked(k, v);
                g_external_count++;
            }
        }
    }
}

}  // namespace

void Load() {
    std::lock_guard<std::mutex> lk(g_loc_lock);
    g_exact_map.clear();
    g_lower_map.clear();
    g_builtin_count = 0;
    for (const BuiltinPair& p : kBuiltinPairs) {
        InsertPairLocked(p.key, p.pt);
        g_builtin_count++;
    }
    LoadExternalTsvLocked();
    LOGR("ptbr_loc: loaded %d built-in + %d external PT-BR entries", g_builtin_count, g_external_count);
}

void ReloadExternal() {
    Load();
}

uint32_t TranslateTextBible(uint8_t* mem_base, uint32_t token_va, uint32_t orig_wide_va) {
    if (app::AndroidSettings().ptbr_mode != 2) return 0;
    if (!mem_base || !token_va || !orig_wide_va) return 0;

    std::string key = ReadNarrowSafe(mem_base, token_va, 64);
    std::string orig = WideToUtf8(mem_base, orig_wide_va, 256);

    std::lock_guard<std::mutex> lk(g_loc_lock);
    std::string translated;
    if (LookupTranslationLocked(key, orig, translated)) {
        return AllocGuestWideStringLocked(translated);
    }
    MaybeCaptureKeyLocked(key, orig);
    return 0;
}

void OnSubtitleActivated(uint32_t wide_va) {
    const int mode = app::AndroidSettings().ptbr_mode;
    if (mode == 0 || !wide_va) return;
    uint8_t* base = mem::Base();
    std::string orig = WideToUtf8(base, wide_va, 512);
    if (orig.empty()) return;

    std::lock_guard<std::mutex> lk(g_loc_lock);
    std::string translated;
    if (LookupTranslationLocked("", orig, translated)) {
        g_active_sub_text = std::move(translated);
    } else {
        MaybeCaptureKeyLocked("", orig);
        g_active_sub_text = std::move(orig);
    }
    g_active_sub_tp = std::chrono::steady_clock::now();
    g_active_sub_visible = true;
}

void OnSubtitleDeactivated() {
    std::lock_guard<std::mutex> lk(g_loc_lock);
    g_active_sub_visible = false;
    g_active_sub_text.clear();
}

void DrawSubtitleOverlay(float view_w, float view_h) {
    const auto& ex = app::AndroidSettings();
    if (ex.ptbr_mode == 0 || app::OverlayOpen() || !app::GetStatus().game_booted) return;

    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_loc_lock);
        if (!g_active_sub_visible || g_active_sub_text.empty()) return;
        float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_active_sub_tp).count();
        if (age > 7.0f) {
            g_active_sub_visible = false;
            g_active_sub_text.clear();
            return;
        }
        text = g_active_sub_text;
    }

    const float sub_scale = std::clamp(ex.subtitle_scale, 0.8f, 1.6f);
    ImGui::SetNextWindowPos(ImVec2(view_w * 0.5f, view_h * 0.855f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 0.0f),
                                        ImVec2(std::clamp(view_w * 0.58f, 460.0f, 1180.0f), view_h * 0.25f));
    ImGui::SetNextWindowBgAlpha(0.76f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 9.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##PtBrSubtitles", nullptr, flags)) {
        ImGui::SetWindowFontScale(sub_scale);
        ImGui::TextWrapped("%s", text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

int BuiltinCount() {
    std::lock_guard<std::mutex> lk(g_loc_lock);
    return g_builtin_count;
}

int ExternalCount() {
    std::lock_guard<std::mutex> lk(g_loc_lock);
    return g_external_count;
}

int CapturedCount() {
    std::lock_guard<std::mutex> lk(g_loc_lock);
    return g_captured_count;
}

}  // namespace ptbr_loc
