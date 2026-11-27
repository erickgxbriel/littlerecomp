#include "i18n.h"

#include <algorithm>
#include <atomic>
#include <cctype>

namespace i18n {

namespace {

std::atomic<int> g_setting{int(Lang::Auto)};
std::atomic<bool> g_pt_br{false};

struct Entry {
    const char* en;
    const char* pt;
};

constexpr Entry kTable[int(Str::Count)] = {
    // LangLabel
    {"Language", "Idioma"},
    {"Auto (System)", "Automático (Sistema)"},
    {"English", "English"},
    {"Português (BR)", "Português (BR)"},

    // Setup Wizard
    {"LittleRecomp — Setup Assistant", "LittleRecomp — Assistente de Configuração"},
    {"Native Xbox Static Recompilation Port for Android ARM64 (No game files included)",
     "Port Nativo de Recompilação Estática do Xbox para Android ARM64 (Nenhum arquivo do jogo incluso)"},
    {"1. Use your own Original Xbox copy", "1. Use sua própria cópia do Xbox Original"},
    {"Provide an Original Xbox .iso / .xiso image or extracted folder containing default.xbe.\n"
     "(PS2 and PC versions are NOT compatible.)",
     "Forneça uma imagem .iso / .xiso do Xbox Original ou uma pasta extraída contendo default.xbe.\n"
     "(Versões de PS2 e PC NÃO são compatíveis.)"},
    {"2. Pick your game directly on the phone", "2. Escolha o jogo direto no celular"},
    {"Tap \"Select ISO / XISO\" below — LittleRecomp reads directly without copying 3.2 GB!",
     "Toque em \"Selecionar ISO / XISO\" abaixo — o LittleRecomp lê direto sem precisar copiar 3.2 GB!"},
    {"3. Play & tune graphics anytime", "3. Jogue e ajuste os gráficos a qualquer hora"},
    {"Use a Bluetooth/USB gamepad or on-screen touch controls. Tap MENU in the top-right corner for options.",
     "Use um controle Bluetooth/USB ou os controles na tela. Toque em MENU no canto superior direito para opções."},
    {"Select ISO / XISO", "Selecionar ISO / XISO"},
    {"Select Game Folder", "Selecionar Pasta do Jogo"},
    {"Grant Folder Access", "Conceder Acesso a Pastas"},
    {"Retry Boot", "Tentar Novamente"},
    {"Options", "Opções"},
    {"Status:", "Status:"},
    {"Selected:", "Selecionado:"},
    {"Hardware:", "Hardware:"},
    {"Original PC port & Xbox runtime by efm-games/scarface-recomp (on XboxXBE-Recomp & XenonRecomp). Android port only.",
     "Port original de PC e runtime Xbox por efm-games/scarface-recomp (via XboxXBE-Recomp e XenonRecomp). Apenas port Android."},

    // Boot / BYOG status & errors
    {"Waiting for your Original Xbox game image (.iso / .xiso) or folder",
     "Aguardando sua imagem do jogo Xbox Original (.iso / .xiso) ou pasta"},
    {"Loading XBE/ISO...", "Carregando XBE/ISO..."},
    {"Booting guest image...", "Iniciando jogo..."},
    {"Running (Beta v0.3.0)", "Rodando (Beta v0.3.0)"},
    {"This file cannot be streamed (e.g. cloud Drive). Save the .iso/.xiso to local device storage first.",
     "Este arquivo não permite leitura direta (ex.: nuvem/Drive). Salve o .iso/.xiso na memória local do aparelho."},
    {"Not a valid Original Xbox .iso/.xiso image (PS2/PC ISOs are not supported).",
     "Não é uma imagem .iso/.xiso válida de Xbox Original (ISOs de PS2/PC não são suportadas)."},
    {"Folder access permission is required to read extracted folders. Tap 'Grant Folder Access' or pick an .iso directly.",
     "Permissão de acesso a pastas é necessária para pastas extraídas. Toque em 'Conceder Acesso a Pastas' ou escolha uma .iso direto."},
    {"default.xbe not found inside the selected ISO.",
     "default.xbe não encontrado dentro da ISO selecionada."},
    {"default.xbe not found inside the selected folder.",
     "default.xbe não encontrado na pasta selecionada."},
    {"Note: default.xbe SHA-1 differs from Redump USA/Europe dump — booting anyway.",
     "Aviso: SHA-1 de default.xbe difere do dump padrão USA/Europe — iniciando mesmo assim."},

    // HUD
    {"GPU", "GPU"},
    {"BAT", "BAT"},
    {"OK", "OK"},
    {"Warm", "Morno"},
    {"Hot", "Quente"},
    {"Critical", "Crítico"},
    {"%.1f fps", "%.1f fps"},

    // Options Tabs & Labels
    {"LittleRecomp Options", "Opções do LittleRecomp"},
    {"Graphics", "Gráficos"},
    {"Controls", "Controles"},
    {"Saves", "Saves"},
    {"Game & Language", "Jogo & Idioma"},
    {"Credits", "Créditos"},

    {"Resolution", "Resolução"},
    {"1.0x (480p)", "1.0x (480p)"},
    {"1.5x (720p)", "1.5x (720p)"},
    {"2.0x (960p)", "2.0x (960p)"},
    {"2.25x (1080p)", "2.25x (1080p)"},
    {"Upscaler", "Upscaler"},
    {"Aspect", "Proporção"},
    {"16:9", "16:9"},
    {"Fullscreen (Stretch)", "Esticar (Tela Cheia)"},
    {"True Ultrawide", "Ultrawide Real"},
    {"VSync", "VSync"},
    {"Anisotropic", "Anisotrópico"},
    {"Off", "Desl."},
    {"Experimental: 60 FPS Mode", "Experimental: Modo 60 FPS"},
    {"Native Aux Effects (1.0x)", "Efeitos Auxiliares (1.0x)"},
    {"Lower glow/blur GPU load", "Menor carga térmica de glow/blur"},
    {"Fast Aux Effects (0.5x Glow/Blur)", "Efeitos Auxiliares Rápidos (0.5x Glow/Blur)"},
    {"Cuts motion-blur/reflection GPU load by 75% in Miami", "Reduz 75% a carga de blur/reflexos ao dirigir em Miami"},
    {"Smart DRS (FPS + Thermal)", "DRS Inteligente (FPS + Térmico)"},
    {"Smoothly scales resolution to hold FPS & stay cool", "Ajusta escala suavemente para manter FPS e temperatura"},
    {"Pin Threads to Big Cores", "Fixar Threads (Núcleos Fortes)"},
    {"Low-End Phone Preset", "Preset: Celular Fraco"},
    {"Light Textures (-75% VRAM)", "Texturas Leves (-75% VRAM)"},
    {"Half-res 3D world mips; keeps 2D HUD sharp.", "Reduz mips 3D; mantém HUD/textos nítidos."},
    {"Vulkan Swapchain Pre-Rotation", "Pré-Rotação Vulkan (Swapchain)"},
    {"Performance HUD", "Desempenho (HUD)"},
    {"Off", "Desligado"},
    {"FPS Only", "Apenas FPS"},
    {"Basic", "Básico"},
    {"Advanced", "Avançado"},

    {"Virtual Touch Controls", "Controles Virtuais na Tela"},
    {"Auto-Hide Touch When Gamepad Connected", "Ocultar Touch com Controle Conectado"},
    {"Touch Controls Opacity", "Opacidade dos Controles Touch"},
    {"Touch Auto-Aim + Fire (On-Foot RT = LT+RT)", "Mira Automática + Tiro no Touch (RT a pé = LT+RT)"},
    {"Locks on while firing on foot without affecting driving.",
     "Trava a mira ao atirar a pé sem interferir ao dirigir veículos."},
    {"Controller Vibration (Rumble)", "Vibração do Controle (Rumble)"},
    {"GTA-Style Driving Controls (modern_pad)", "Direção Estilo GTA (modern_pad)"},
    {"Edit Touch Layout", "Editar Layout Touch"},
    {"Reset Layout", "Resetar Layout"},
    {"Drag buttons to move | Selected:", "Arraste para mover | Selecionado:"},
    {"Size", "Tam."},
    {"Save & Exit", "Salvar e Sair"},
    {"Physical Gamepad:", "Controle Físico:"},
    {"Connected", "Conectado"},
    {"Not detected", "Não detectado"},

    {"Interface Language", "Idioma da Interface"},
    {"Skip Intro Logos", "Pular Logos de Abertura"},
    {"Unlock All Pause Cheats", "Liberar Todos os Cheats no Pause"},
    {"Custom Path (ISO / Folder)", "Caminho Manual (ISO / Pasta)"},
    {"Apply Path & Retry", "Aplicar Caminho"},
    {"Save Settings", "Salvar Opções"},
    {"Resume Game", "Voltar ao Jogo"},

    // Saves Tab
    {"Export Backup (.zip)", "Exportar Backup (.zip)"},
    {"Import Backup (.zip)", "Importar Backup (.zip)"},
    {"Refresh", "Atualizar"},
    {"Exports/imports saves from hdd/E/UDATA/5655003f. Importing overwrites slots with the same ID.",
     "Exporta/importa saves de hdd/E/UDATA/5655003f. Importar sobrescreve slots com o mesmo ID."},
    {"No save data found yet in hdd/E/UDATA/5655003f.",
     "Nenhum save encontrado ainda em hdd/E/UDATA/5655003f."},
    {"Save Name", "Nome do Save"},
    {"Slot ID", "Slot"},
    {"Modified", "Data"},
    {"Size", "Tamanho"},
    {"Delete", "Excluir"},
    {"Confirm?", "Confirmar?"},

    // Achievements Tab & Toast
    {"Achievements", "Conquistas"},
    {"Unlocked", "Desbloqueadas"},
    {"All", "Todas"},
    {"Unlocked", "Obtidas"},
    {"Locked", "Bloqueadas"},
    {"Reset Progress", "Resetar Progresso"},
    {"Achievement", "Conquista"},
    {"Category", "Categoria"},
    {"Pts", "Pts"},
    {"Status", "Estado"},
    {"Locked", "Bloqueada"},
    {"Achievement Unlocked!", "Conquista Desbloqueada!"},

    // PT-BR Localization & Mods
    {"In-Game PT-BR Localization & Mods", "Tradução PT-BR no Jogo e Mods"},
    {"Game PT-BR Mode", "Modo PT-BR no Jogo"},
    {"Off (English)", "Desligado (Inglês)"},
    {"HD Subtitles", "Legendas HD"},
    {"Menus + HD Subtitles", "Menus + Legendas HD"},
    {"Subtitle Size", "Tamanho das Legendas"},
    {"Enable mods/ File Override Layer", "Ativar Camada de Mods (pasta mods/)"},
    {"Capture Unlocalized Text Keys (mods/ptbr/captured_keys.tsv)",
     "Capturar Chaves de Texto Faltantes (mods/ptbr/captured_keys.tsv)"},
    {"Import Mod / PT-BR Pack (.zip)", "Importar Mod / Tradução (.zip)"},
    {"Loaded PT-BR: %d built-in + %d external | mods/ hits: %llu | captured: %d",
     "PT-BR carregado: %d nativas + %d externas | hits mods/: %llu | capturadas: %d"},

    // Credits Tab
    {"LittleRecomp v0.4.0-beta — Native Android ARM64 Port",
     "LittleRecomp v0.4.0-beta — Port Nativo Android ARM64"},
    {"This project is an Android ARM64 + Vulkan 1.1 TBDR port of the Windows PC recompilation.\n\n"
     "FULL CREDITS TO THE ORIGINAL PC PORT AUTHOR:\n"
     "- efm-games/scarface-recomp: All static recompilation work, Xbox reverse-engineering, game hooks,\n"
     "  fixed-function/pixel-combiner translation concepts, save conversion, and PC native runtime.\n\n"
     "FOUNDATION & OPEN-SOURCE LIBRARIES:\n"
     "- Xbox XBE Recomp (TGP482) & XenonRecomp (hedge-dev)\n"
     "- SDL3, Google Oboe (AAudio), Dear ImGui, shaderc, Zydis, toml++\n\n"
     "LittleRecomp only ported the platform layer, Vulkan mobile TBDR renderer, Oboe audio,\n"
     "touch controls, ADPF tuning, and Android UI. No copyrighted game assets are included.",
     "Este projeto é um port para Android ARM64 + Vulkan 1.1 TBDR da recompilação original de PC.\n\n"
     "TODOS OS CRÉDITOS AO AUTOR DO PORT ORIGINAL PARA PC:\n"
     "- efm-games/scarface-recomp: Todo o trabalho de recompilação estática, engenharia reversa,\n"
     "  hooks do jogo, tradução NV2A, conversão de saves e runtime nativo base.\n\n"
     "BASE E BIBLIOTECAS OPEN-SOURCE:\n"
     "- Xbox XBE Recomp (TGP482) e XenonRecomp (hedge-dev)\n"
     "- SDL3, Google Oboe (AAudio), Dear ImGui, shaderc, Zydis, toml++\n\n"
     "Nosso trabalho no LittleRecomp foi exclusivamente portar a camada de plataforma, o renderer\n"
     "Vulkan mobile TBDR, áudio Oboe, controles touch, ADPF e UI Android. Nenhum asset do jogo está incluso."},
    {"Device Diagnostics:", "Diagnóstico do Aparelho:"},
};
static_assert(std::size(kTable) == size_t(Str::Count), "i18n table mismatch");

}  // namespace

void SetLanguage(Lang setting, const std::string& system_lang_code) {
    int s = std::clamp(int(setting), 0, 2);
    g_setting.store(s, std::memory_order_relaxed);
    if (s == int(Lang::PtBr)) {
        g_pt_br.store(true, std::memory_order_relaxed);
    } else if (s == int(Lang::En)) {
        g_pt_br.store(false, std::memory_order_relaxed);
    } else {
        std::string lc = system_lang_code;
        for (char& c : lc) c = static_cast<char>(std::tolower(static_cast<uint8_t>(c)));
        g_pt_br.store(lc.rfind("pt", 0) == 0, std::memory_order_relaxed);
    }
}

Lang GetLanguageSetting() {
    return static_cast<Lang>(g_setting.load(std::memory_order_relaxed));
}

bool IsPtBr() {
    return g_pt_br.load(std::memory_order_relaxed);
}

const char* T(Str id) {
    int idx = std::clamp(int(id), 0, int(Str::Count) - 1);
    const Entry& e = kTable[idx];
    return IsPtBr() ? e.pt : e.en;
}

}  // namespace i18n
