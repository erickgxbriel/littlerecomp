#include "achievements.h"

#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "android_app.h"
#include "core/log.h"
#include "i18n.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace achievements {

namespace {

enum class Category : int {
    Empire = 0,
    Combat = 1,
    Economy = 2,
    Extras = 3,
};

struct DbSnapshot {
    bool valid = false;
    bool cheats_used = false;
    int32_t in_miami = 0;
    int32_t rep_level = 0;
    double turf_progress = 0.0;
    int32_t unlock_dealer = 0;
    int32_t unlock_supplier = 0;
    int32_t unlock_bank = 0;
    int32_t unlock_island = 0;
    int32_t num_mansion = 0;
    int32_t num_cars = 0;
    int32_t num_boats = 0;
    int32_t num_aircraft = 0;
    int32_t num_locations = 0;
    int32_t num_wardrobe = 0;
    int32_t num_hirelings = 0;
    int32_t num_femmes = 0;
    int32_t exotics = 0;
    int32_t kills = 0;
    int32_t cops_killed = 0;
    int32_t headshots = 0;
    int32_t groin_shots = 0;
    int32_t dealers_killed = 0;
    int32_t suppliers_killed = 0;
    int32_t big_suppliers_killed = 0;
    int32_t chainsaw_dismember = 0;
    int32_t chainsaw_heads = 0;
    int32_t in_rage = 0;
    double rage_time = 0.0;
    double highest_heat = 0.0;
    double clean_cash = 0.0;
    double dirty_cash = 0.0;
    double biggest_profit = 0.0;
    double total_winnings = 0.0;
    double total_balls = 0.0;
    int32_t ammo_used = 0;
    int32_t conversations = 0;
    double game_time = 0.0;
};

struct Def {
    const char* id;
    const char* title_en;
    const char* title_pt;
    const char* desc_en;
    const char* desc_pt;
    Category category;
    int points;
    bool (*check)(const DbSnapshot&);
};

const Def kDefs[] = {
    // Empire & Story (14 achievements, 185 pts) — aligned with RA #20684
    {"miami_arrival", "Back in Miami", "De Volta a Miami",
     "Survive the mansion ambush and reach Miami.", "Sobreviva ao ataque na mansão e chegue a Miami.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.in_miami >= 1; }},
    {"first_dealer", "Street Connections", "Contatos na Rua",
     "Unlock your first street dealer network.", "Desbloqueie seu primeiro traficante de rua.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.unlock_dealer >= 1; }},
    {"first_supplier", "Moving Weight", "Fornecedor de Peso",
     "Unlock a major cocaine supplier.", "Desbloqueie um grande fornecedor de mercadoria.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.unlock_supplier >= 1; }},
    {"bank_unlocked", "Clean Money", "Dinheiro Limpo",
     "Unlock the bank to launder dirty cash.", "Desbloqueie o banco para lavar dinheiro sujo.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.unlock_bank >= 1; }},
    {"island_cartel", "Island Smuggler", "Rota das Ilhas",
     "Unlock the island supplier operations.", "Desbloqueie os fornecedores das ilhas.",
     Category::Empire, 15, [](const DbSnapshot& s) { return s.unlock_island >= 1; }},
    {"front_first", "Hostile Takeover", "Primeira Frente",
     "Acquire your first business front or storehouse.", "Adquira sua primeira frente comercial ou depósito.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.num_locations >= 1; }},
    {"front_5", "Franchise Owner", "Dono da Rede",
     "Own 5 business fronts or locations across Miami.", "Possua 5 frentes comerciais ou propriedades em Miami.",
     Category::Empire, 15, [](const DbSnapshot& s) { return s.num_locations >= 5; }},
    {"rep_2", "Making a Name", "Subindo na Vida",
     "Reach Reputation Level 2.", "Alcance o Nível de Reputação 2.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.rep_level >= 2; }},
    {"rep_4", "Miami Enforcer", "Respeito nas Ruas",
     "Reach Reputation Level 4.", "Alcance o Nível de Reputação 4.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.rep_level >= 4; }},
    {"rep_6", "Cartel Kingpin", "Chefão de Miami",
     "Reach Reputation Level 6.", "Alcance o Nível de Reputação 6.",
     Category::Empire, 15, [](const DbSnapshot& s) { return s.rep_level >= 6; }},
    {"rep_8", "The World Is Yours", "O Mundo É Seu",
     "Reach maximum Reputation Level 8.", "Alcance o Nível máximo de Reputação 8.",
     Category::Empire, 25, [](const DbSnapshot& s) { return s.rep_level >= 8; }},
    {"turf_25", "Turf Claimed", "Dono do Pedaço",
     "Control at least 25% of Miami turf.", "Controle pelo menos 25% do território de Miami.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.turf_progress >= 25.0; }},
    {"turf_100", "King of Miami", "Rei de Miami",
     "Achieve 100% turf control across Miami.", "Alcance 100% de controle territorial em Miami.",
     Category::Empire, 25, [](const DbSnapshot& s) { return s.turf_progress >= 99.5; }},
    {"mansion_luxury", "Interior Decorator", "Mansão de Luxo",
     "Acquire 10 upgrades for Tony's mansion.", "Adquira 10 melhorias para a mansão do Tony.",
     Category::Empire, 10, [](const DbSnapshot& s) { return s.num_mansion >= 10; }},

    // Combat & Mayhem (14 achievements, 145 pts)
    {"kills_10", "Say Hello", "Diga Olá",
     "Eliminate 10 enemies.", "Elimine 10 inimigos.",
     Category::Combat, 5, [](const DbSnapshot& s) { return s.kills >= 10; }},
    {"kills_100", "Trigger Happy", "Chuva de Balas",
     "Eliminate 100 enemies.", "Elimine 100 inimigos.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.kills >= 100; }},
    {"kills_500", "One Man Army", "Exército de Um Homem Só",
     "Eliminate 500 enemies.", "Elimine 500 inimigos.",
     Category::Combat, 15, [](const DbSnapshot& s) { return s.kills >= 500; }},
    {"headshots_25", "Deadeye", "Na Mosca",
     "Score 25 headshots.", "Acerte 25 tiros na cabeça.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.headshots >= 25; }},
    {"headshots_100", "Marksman", "Atirador de Elite",
     "Score 100 headshots.", "Acerte 100 tiros na cabeça.",
     Category::Combat, 15, [](const DbSnapshot& s) { return s.headshots >= 100; }},
    {"groin_10", "Low Blow", "Golpe Baixo",
     "Land 10 groin shots on enemies.", "Acerte 10 tiros na região baixa dos inimigos.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.groin_shots >= 10; }},
    {"blind_rage", "Blind Rage", "Fúria Cega",
     "Trigger Blind Rage mode.", "Ative o modo Blind Rage (Fúria Cega).",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.in_rage >= 1 || s.rage_time >= 1.0; }},
    {"chainsaw_10", "Barton Lane", "Massacre da Motosserra",
     "Perform 10 chainsaw dismemberments.", "Realize 10 desmembramentos com a motosserra.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.chainsaw_dismember >= 10; }},
    {"chainsaw_heads_5", "Off With Their Heads", "Cabeças Rolando",
     "Decapitate 5 enemies using the chainsaw.", "Decapite 5 inimigos usando a motosserra.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.chainsaw_heads >= 5; }},
    {"dealers_10", "Bad for Business", "Sem Concorrência",
     "Take down 10 rival dealers.", "Elimine 10 traficantes rivais.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.dealers_killed >= 10; }},
    {"suppliers_5", "Supply Chain Cut", "Corte na Fonte",
     "Eliminate 5 cartel suppliers.", "Elimine 5 fornecedores do cartel.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.suppliers_killed >= 5; }},
    {"big_supplier_1", "Cartel Takedown", "Queda de Gigantes",
     "Eliminate a major cartel supplier.", "Elimine um grande fornecedor do cartel.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.big_suppliers_killed >= 1; }},
    {"cops_25", "Cop Killer", "Inimigo da Lei",
     "Eliminate 25 police officers in Miami.", "Elimine 25 policiais em Miami.",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.cops_killed >= 25; }},
    {"max_heat", "Public Enemy #1", "Calor Máximo",
     "Survive reaching maximum heat (100%).", "Alcance o nível máximo de procurado (100%).",
     Category::Combat, 10, [](const DbSnapshot& s) { return s.highest_heat >= 99.5; }},

    // Economy & Flash (11 achievements, 110 pts)
    {"dirty_100k", "Suitcase Full of Cash", "Mala Preta",
     "Hold $100,000 in dirty cash at once.", "Carregue $100.000 em dinheiro sujo de uma vez.",
     Category::Economy, 5, [](const DbSnapshot& s) { return s.dirty_cash >= 100000.0; }},
    {"clean_1m", "First Million", "Primeiro Milhão",
     "Accumulate $1,000,000 in clean bank cash.", "Acumule $1.000.000 em dinheiro limpo no banco.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.clean_cash >= 1000000.0; }},
    {"clean_10m", "Miami Tycoon", "Magnata de Miami",
     "Accumulate $10,000,000 in clean bank cash.", "Acumule $10.000.000 em dinheiro limpo no banco.",
     Category::Economy, 15, [](const DbSnapshot& s) { return s.clean_cash >= 10000000.0; }},
    {"profit_50k", "Big Score", "Negócio Fechado",
     "Make $50,000+ profit on a single deal.", "Lucre mais de $50.000 em uma única negociação.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.biggest_profit >= 50000.0; }},
    {"gambler_1", "High Roller", "Apostador Nato",
     "Win $10,000+ total in gambling.", "Ganhe mais de $10.000 no total em apostas.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.total_winnings >= 10000.0; }},
    {"exotics_5", "Connoisseur", "Colecionador Exótico",
     "Purchase 5 Exotic catalogue items.", "Compre 5 itens do catálogo de Exóticos.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.exotics >= 5; }},
    {"cars_5", "Showroom Fleet", "Garagem de Milionário",
     "Own 5 luxury vehicles.", "Possua 5 veículos de luxo.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.num_cars >= 5; }},
    {"boats_3", "Harbor Boss", "Frota Naval",
     "Own 3 boats in your fleet.", "Possua 3 barcos na sua frota.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.num_boats >= 3; }},
    {"aircraft_1", "Airborne Cartel", "Asas do Cartel",
     "Acquire an aircraft for your empire.", "Adquira uma aeronave para o seu império.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.num_aircraft >= 1; }},
    {"wardrobe_3", "Miami Fashion", "Estilo Tony Montana",
     "Unlock 3 outfits in Tony's wardrobe.", "Desbloqueie 3 trajes no guarda-roupa do Tony.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.num_wardrobe >= 3; }},
    {"balls_50k", "Balls of Steel", "Bolas de Aço",
     "Earn 50,000 total Balls.", "Acumule 50.000 pontos totais de Balls.",
     Category::Economy, 10, [](const DbSnapshot& s) { return s.total_balls >= 50000.0; }},

    // Extras & Hardcore (6 achievements, 60 pts)
    {"conversations_10", "Smooth Talker", "Bom de Papo",
     "Complete 10 conversations in Miami.", "Complete 10 conversas em Miami.",
     Category::Extras, 5, [](const DbSnapshot& s) { return s.conversations >= 10; }},
    {"hireling_1", "Personal Security", "Guarda-Costas",
     "Recruit a henchman or specialist.", "Recrute um capanga ou especialista.",
     Category::Extras, 10, [](const DbSnapshot& s) { return s.num_hirelings >= 1; }},
    {"femmes_1", "Ladies' Man", "Companhia de Luxo",
     "Acquire your first Femme companion.", "Conquiste sua primeira acompanhante (Femme).",
     Category::Extras, 5, [](const DbSnapshot& s) { return s.num_femmes >= 1; }},
    {"ammo_1000", "Lead Storm", "Chuva de Chumbo",
     "Fire 1,000 rounds of ammunition.", "Dispare 1.000 tiros com suas armas.",
     Category::Extras, 5, [](const DbSnapshot& s) { return s.ammo_used >= 1000; }},
    {"veteran_1h", "Scarface Veteran", "Veterano de Miami",
     "Play for 1 hour in your current profile.", "Jogue por 1 hora no seu perfil atual.",
     Category::Extras, 10, [](const DbSnapshot& s) { return s.game_time >= 3600.0; }},
    {"cartel_honor", "Cartel Honor [HC]", "Honra do Cartel [HC]",
     "Reach Reputation Level 4 with zero cheats active.", "Alcance Reputação 4 sem nenhum cheat ativo.",
     Category::Extras, 25, [](const DbSnapshot& s) { return s.rep_level >= 4 && !s.cheats_used; }},
};

constexpr size_t kAchCount = std::size(kDefs);

struct UnlockState {
    bool unlocked = false;
    bool hardcore = false;
    std::string when;
};

struct ToastItem {
    std::string title;
    std::string desc;
    int points = 0;
    bool hardcore = false;
};

std::mutex g_lock;
std::array<UnlockState, kAchCount> g_state{};
std::atomic<uint32_t> g_db_va{0};
uint32_t g_last_synced_db_va = 0;
int g_db_warm_polls = 0;
bool g_initial_sync_done = false;
int32_t g_prev_in_miami = 0;
std::deque<ToastItem> g_toast_queue;
bool g_toast_active = false;
ToastItem g_toast_cur{};
std::chrono::steady_clock::time_point g_toast_start{};

fs::path SavePath() { return app::ExeDir() / "achievements.toml"; }

std::string NowTimestamp() {
    std::time_t t = std::time(nullptr);
    struct tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min);
    return buf;
}

bool ReadDbNumber(const uint8_t* base, uint32_t db_va, uint32_t id, double& out) {
    if (!base || !db_va || id >= 0xC1u) return false;
    uint32_t slot = db_va + 4u + id * 8u;
    if (!mem::IsCommitted(slot) || !mem::IsCommitted(slot + 7u)) return false;
    uint8_t type = base[slot + 4u];
    uint32_t ptr = 0;
    std::memcpy(&ptr, base + slot, 4);
    if (!ptr || !mem::IsCommitted(ptr) || !mem::IsCommitted(ptr + 3u)) return false;
    uint32_t raw = 0;
    std::memcpy(&raw, base + ptr, 4);
    if (type == 0 || type == 2) {
        out = double(static_cast<int32_t>(raw));
        return true;
    }
    if (type == 1 || type == 3) {
        float f = 0.0f;
        std::memcpy(&f, &raw, 4);
        if (!std::isfinite(f)) return false;
        out = double(f);
        return true;
    }
    return false;
}

int32_t ReadDbInt(const uint8_t* base, uint32_t db_va, uint32_t id) {
    double v = 0.0;
    if (!ReadDbNumber(base, db_va, id, v)) return 0;
    return static_cast<int32_t>(std::llround(v));
}

double ReadDbFloat(const uint8_t* base, uint32_t db_va, uint32_t id) {
    double v = 0.0;
    return ReadDbNumber(base, db_va, id, v) ? v : 0.0;
}

DbSnapshot CaptureSnapshotLocked(uint32_t db_va) {
    DbSnapshot s{};
    uint8_t* base = mem::Base();
    if (!base || !db_va) return s;

    // Validate vtable/committed memory around DataBase slot 0x0D ($DB_CLEAN_CASH)
    double test_v = 0.0;
    if (!ReadDbNumber(base, db_va, 0x0Du, test_v) && !ReadDbNumber(base, db_va, 0x57u, test_v)) {
        return s;
    }

    s.valid = true;
    s.clean_cash = ReadDbFloat(base, db_va, 0x0Du);
    s.dirty_cash = ReadDbFloat(base, db_va, 0x0Eu);
    s.biggest_profit = ReadDbFloat(base, db_va, 0x2Fu);
    s.exotics = ReadDbInt(base, db_va, 0x31u);
    s.num_cars = ReadDbInt(base, db_va, 0x32u);
    s.num_boats = ReadDbInt(base, db_va, 0x33u);
    s.num_aircraft = ReadDbInt(base, db_va, 0x34u);
    s.num_locations = ReadDbInt(base, db_va, 0x35u);
    s.num_mansion = ReadDbInt(base, db_va, 0x36u);
    s.num_wardrobe = ReadDbInt(base, db_va, 0x37u);
    s.num_hirelings = ReadDbInt(base, db_va, 0x38u);
    s.num_femmes = ReadDbInt(base, db_va, 0x39u);
    s.highest_heat = ReadDbFloat(base, db_va, 0x3Du);
    s.cops_killed = ReadDbInt(base, db_va, 0x43u);
    s.dealers_killed = ReadDbInt(base, db_va, 0x45u);
    s.suppliers_killed = ReadDbInt(base, db_va, 0x46u);
    s.big_suppliers_killed = ReadDbInt(base, db_va, 0x47u);
    s.total_balls = ReadDbFloat(base, db_va, 0x4Cu);
    s.in_miami = ReadDbInt(base, db_va, 0x51u);
    s.total_winnings = ReadDbFloat(base, db_va, 0x52u);
    s.rep_level = ReadDbInt(base, db_va, 0x57u);
    s.turf_progress = ReadDbFloat(base, db_va, 0x60u);
    s.conversations = ReadDbInt(base, db_va, 0x61u);
    s.game_time = ReadDbFloat(base, db_va, 0x62u);
    s.rage_time = ReadDbFloat(base, db_va, 0x63u);
    s.ammo_used = ReadDbInt(base, db_va, 0x65u);
    s.kills = ReadDbInt(base, db_va, 0x66u);
    s.headshots = ReadDbInt(base, db_va, 0x67u);
    s.groin_shots = ReadDbInt(base, db_va, 0x68u) + ReadDbInt(base, db_va, 0x69u) + ReadDbInt(base, db_va, 0x6Au);
    s.chainsaw_heads = ReadDbInt(base, db_va, 0x6Fu);
    s.chainsaw_dismember = ReadDbInt(base, db_va, 0x6Eu) + s.chainsaw_heads +
                           ReadDbInt(base, db_va, 0x70u) + ReadDbInt(base, db_va, 0x71u);
    s.unlock_dealer = ReadDbInt(base, db_va, 0x73u);
    s.unlock_supplier = ReadDbInt(base, db_va, 0x74u);
    s.unlock_bank = ReadDbInt(base, db_va, 0x75u);
    s.unlock_island = ReadDbInt(base, db_va, 0x76u);
    s.in_rage = ReadDbInt(base, db_va, 0x79u);

    bool cheats = app::Get().all_cheats_unlocked;
    if (!cheats) {
        for (uint32_t cid = 0xA4u; cid <= 0xC0u; ++cid) {
            if (ReadDbInt(base, db_va, cid) != 0) {
                cheats = true;
                break;
            }
        }
    }
    s.cheats_used = cheats;
    return s;
}

void SaveLocked() {
    toml::table root;
    toml::table unlocked;
    for (size_t i = 0; i < kAchCount; ++i) {
        if (!g_state[i].unlocked) continue;
        toml::table entry;
        entry.insert("unlocked", true);
        entry.insert("hardcore", g_state[i].hardcore);
        entry.insert("when", g_state[i].when);
        unlocked.insert(kDefs[i].id, std::move(entry));
    }
    root.insert("achievements", std::move(unlocked));
    std::ofstream f(SavePath());
    if (f) f << root << "\n";
}

}  // namespace

void OnDataBaseConstructed(uint32_t db_va) {
    if (!db_va) return;
    g_db_va.store(db_va, std::memory_order_release);
    LOGR("achievements: DataBase hooked at 0x%08X", db_va);
}

void OnDataBaseDestroyed(uint32_t db_va) {
    uint32_t cur = g_db_va.load(std::memory_order_acquire);
    if (cur == db_va) {
        g_db_va.store(0, std::memory_order_release);
    }
}

void Load() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_state = {};
    toml::table root;
    try {
        root = toml::parse_file(SavePath().string());
    } catch (...) {
        return;
    }
    if (const toml::table* tbl = root["achievements"].as_table()) {
        for (size_t i = 0; i < kAchCount; ++i) {
            if (const toml::table* entry = (*tbl)[kDefs[i].id].as_table()) {
                g_state[i].unlocked = (*entry)["unlocked"].value_or(false);
                g_state[i].hardcore = (*entry)["hardcore"].value_or(false);
                g_state[i].when = (*entry)["when"].value_or(std::string{});
            }
        }
    }
}

void Save() {
    std::lock_guard<std::mutex> lk(g_lock);
    SaveLocked();
}

void ResetProgress() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_state = {};
    g_toast_queue.clear();
    g_toast_active = false;
    std::error_code ec;
    fs::remove(SavePath(), ec);
}

void Poll() {
    const uint32_t db_va = g_db_va.load(std::memory_order_acquire);
    if (!db_va) return;

    std::lock_guard<std::mutex> lk(g_lock);
    if (db_va != g_last_synced_db_va) {
        g_last_synced_db_va = db_va;
        g_db_warm_polls = 0;
        g_initial_sync_done = false;
        g_prev_in_miami = 0;
    }

    // Wait ~1.5s after DB creation so title screen or save load populates fields cleanly.
    if (++g_db_warm_polls < 3) return;

    DbSnapshot s = CaptureSnapshotLocked(db_va);
    if (!s.valid) return;

    const bool existing_progress =
        (s.rep_level >= 2 || s.kills >= 10 || s.clean_cash >= 50000.0 ||
         s.dirty_cash >= 50000.0 || s.total_balls >= 10000.0 || s.game_time >= 120.0);

    bool silent_pass = false;
    if (!g_initial_sync_done) {
        g_initial_sync_done = true;
        if (existing_progress) {
            silent_pass = true;
        }
    } else if (g_prev_in_miami == 0 && s.in_miami >= 1 && existing_progress) {
        // Save loaded from main menu: silently sync retroactive achievements without toast spam
        silent_pass = true;
    }
    g_prev_in_miami = s.in_miami;

    const bool pt = i18n::IsPtBr();
    bool any_new = false;
    const std::string stamp = NowTimestamp();

    for (size_t i = 0; i < kAchCount; ++i) {
        if (g_state[i].unlocked) continue;
        if (kDefs[i].check(s)) {
            g_state[i].unlocked = true;
            g_state[i].hardcore = !s.cheats_used;
            g_state[i].when = stamp;
            any_new = true;
            if (!silent_pass) {
                ToastItem item;
                item.title = pt ? kDefs[i].title_pt : kDefs[i].title_en;
                item.desc = pt ? kDefs[i].desc_pt : kDefs[i].desc_en;
                item.points = kDefs[i].points;
                item.hardcore = g_state[i].hardcore;
                g_toast_queue.push_back(std::move(item));
            }
            LOGR("achievements: unlocked '%s' (+%d pts%s%s)",
                 kDefs[i].id, kDefs[i].points,
                 g_state[i].hardcore ? " HC" : "",
                 silent_pass ? " retroactive" : "");
        }
    }

    if (any_new) {
        SaveLocked();
    }
}

void DrawToast(float view_w, float view_h) {
    std::lock_guard<std::mutex> lk(g_lock);
    const auto now = std::chrono::steady_clock::now();
    constexpr float kDurationSec = 4.2f;

    if (!g_toast_active) {
        if (g_toast_queue.empty()) return;
        g_toast_cur = std::move(g_toast_queue.front());
        g_toast_queue.pop_front();
        g_toast_active = true;
        g_toast_start = now;
    }

    float elapsed = std::chrono::duration<float>(now - g_toast_start).count();
    if (elapsed >= kDurationSec) {
        g_toast_active = false;
        return;
    }

    // Smooth slide-in (0.35s) and slide-out (0.45s)
    float slide = 1.0f;
    if (elapsed < 0.35f) {
        float t = elapsed / 0.35f;
        slide = 1.0f - (1.0f - t) * (1.0f - t);
    } else if (elapsed > kDurationSec - 0.45f) {
        float t = (kDurationSec - elapsed) / 0.45f;
        slide = std::clamp(t * t, 0.0f, 1.0f);
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float font_h = ImGui::GetFontSize();
    const float w = std::clamp(view_w * 0.34f, 380.0f, 580.0f);
    const float h = font_h * 3.1f + 22.0f;
    const float x0 = (view_w - w) * 0.5f;
    const float y_target = std::max(18.0f, view_h * 0.045f);
    const float y0 = -h + (h + y_target) * slide;
    const float x1 = x0 + w;
    const float y1 = y0 + h;

    const uint8_t alpha = uint8_t(std::clamp(slide, 0.0f, 1.0f) * 242.0f);
    const ImU32 bg = IM_COL32(16, 16, 22, alpha);
    const ImU32 border = g_toast_cur.hardcore ? IM_COL32(245, 199, 49, alpha) : IM_COL32(215, 48, 58, alpha);
    const ImU32 gold = IM_COL32(245, 199, 49, alpha);
    const ImU32 white = IM_COL32(248, 248, 250, alpha);
    const ImU32 sub_col = IM_COL32(200, 206, 218, alpha);

    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), bg, 10.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), border, 10.0f, 0, 2.2f);

    // Vector trophy icon on the left
    const float icx = x0 + h * 0.48f;
    const float icy = y0 + h * 0.50f;
    const float ir = h * 0.28f;
    dl->AddCircleFilled(ImVec2(icx, icy), ir * 1.25f, IM_COL32(245, 199, 49, uint8_t(alpha * 0.16f)), 24);
    dl->AddCircle(ImVec2(icx, icy), ir * 1.25f, gold, 24, 1.8f);
    // Cup body + base
    dl->AddQuadFilled(ImVec2(icx - ir * 0.55f, icy - ir * 0.55f),
                      ImVec2(icx + ir * 0.55f, icy - ir * 0.55f),
                      ImVec2(icx + ir * 0.32f, icy + ir * 0.15f),
                      ImVec2(icx - ir * 0.32f, icy + ir * 0.15f), gold);
    dl->AddRectFilled(ImVec2(icx - ir * 0.10f, icy + ir * 0.15f),
                      ImVec2(icx + ir * 0.10f, icy + ir * 0.45f), gold);
    dl->AddRectFilled(ImVec2(icx - ir * 0.38f, icy + ir * 0.45f),
                      ImVec2(icx + ir * 0.38f, icy + ir * 0.62f), gold, 2.0f);

    const float tx = x0 + h * 0.96f;
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr), "%s  +%d pts%s",
                  i18n::T(i18n::Str::AchToastHeader),
                  g_toast_cur.points,
                  g_toast_cur.hardcore ? " [HC]" : "");
    dl->AddText(ImVec2(tx, y0 + 8.0f), gold, hdr);
    dl->AddText(ImVec2(tx, y0 + 10.0f + font_h * 1.02f), white, g_toast_cur.title.c_str());
    dl->AddText(ImVec2(tx, y0 + 12.0f + font_h * 2.04f), sub_col, g_toast_cur.desc.c_str());
}

void DrawTab() {
    static int filter_mode = 0;  // 0 = All, 1 = Unlocked, 2 = Locked
    static bool confirm_reset = false;
    static auto confirm_tp = std::chrono::steady_clock::now();

    std::array<UnlockState, kAchCount> snap;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        snap = g_state;
    }

    int unlocked = 0;
    int pts = 0;
    int max_pts = 0;
    for (size_t i = 0; i < kAchCount; ++i) {
        max_pts += kDefs[i].points;
        if (snap[i].unlocked) {
            unlocked++;
            pts += kDefs[i].points;
        }
    }

    const bool pt = i18n::IsPtBr();
    float frac = max_pts > 0 ? float(pts) / float(max_pts) : 0.0f;

    ImGui::Text("%s: %d / %zu   |   %d / %d pts",
                i18n::T(i18n::Str::AchProgress), unlocked, kAchCount, pts, max_pts);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f));

    if (ImGui::RadioButton(i18n::T(i18n::Str::AchFilterAll), filter_mode == 0)) filter_mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton(i18n::T(i18n::Str::AchFilterUnlocked), filter_mode == 1)) filter_mode = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton(i18n::T(i18n::Str::AchFilterLocked), filter_mode == 2)) filter_mode = 2;
    ImGui::SameLine(0.0f, 20.0f);

    const auto now = std::chrono::steady_clock::now();
    if (confirm_reset && std::chrono::duration_cast<std::chrono::seconds>(now - confirm_tp).count() >= 5) {
        confirm_reset = false;
    }
    if (confirm_reset) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.14f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.22f, 0.22f, 1.0f));
    }
    if (ImGui::Button(confirm_reset ? i18n::T(i18n::Str::BtnConfirmDelete)
                                    : i18n::T(i18n::Str::AchReset))) {
        if (confirm_reset) {
            ResetProgress();
            confirm_reset = false;
        } else {
            confirm_reset = true;
            confirm_tp = now;
        }
    }
    if (confirm_reset) ImGui::PopStyleColor(2);

    ImGui::Separator();

    const char* cat_en[4] = {"Empire & Story", "Combat & Mayhem", "Economy & Flash", "Extras"};
    const char* cat_pt[4] = {"Império & História", "Combate & Caos", "Economia & Ostentação", "Extras"};

    if (ImGui::BeginTable("##AchTable", 4,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(i18n::T(i18n::Str::AchColTitle), ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn(i18n::T(i18n::Str::AchColCat), ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn(i18n::T(i18n::Str::AchColPts), ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableSetupColumn(i18n::T(i18n::Str::AchColStatus), ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < kAchCount; ++i) {
            const bool done = snap[i].unlocked;
            if (filter_mode == 1 && !done) continue;
            if (filter_mode == 2 && done) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const char* title = pt ? kDefs[i].title_pt : kDefs[i].title_en;
            const char* desc = pt ? kDefs[i].desc_pt : kDefs[i].desc_en;
            if (done) {
                ImGui::TextColored(ImVec4(0.98f, 0.82f, 0.25f, 1.0f), "%s", title);
                ImGui::TextDisabled("  %s", desc);
            } else {
                ImGui::TextDisabled("%s", title);
                ImGui::TextDisabled("  %s", desc);
            }

            ImGui::TableSetColumnIndex(1);
            int c = int(kDefs[i].category);
            ImGui::TextDisabled("%s", pt ? cat_pt[c] : cat_en[c]);

            ImGui::TableSetColumnIndex(2);
            if (done) {
                ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.62f, 1.0f), "%d%s",
                                   kDefs[i].points, snap[i].hardcore ? " [HC]" : "");
            } else {
                ImGui::TextDisabled("%d", kDefs[i].points);
            }

            ImGui::TableSetColumnIndex(3);
            if (done) {
                ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.62f, 1.0f), "%s",
                                   snap[i].when.empty() ? "OK" : snap[i].when.c_str());
            } else {
                ImGui::TextDisabled("%s", i18n::T(i18n::Str::AchLocked));
            }
        }
        ImGui::EndTable();
    }
}

int UnlockedCount() {
    std::lock_guard<std::mutex> lk(g_lock);
    int n = 0;
    for (const auto& s : g_state)
        if (s.unlocked) ++n;
    return n;
}

int TotalCount() { return int(kAchCount); }

int EarnedPoints() {
    std::lock_guard<std::mutex> lk(g_lock);
    int p = 0;
    for (size_t i = 0; i < kAchCount; ++i)
        if (g_state[i].unlocked) p += kDefs[i].points;
    return p;
}

int TotalPoints() {
    int p = 0;
    for (const auto& d : kDefs) p += d.points;
    return p;
}

}  // namespace achievements
