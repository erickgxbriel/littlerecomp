# LittleRecomp — *Scarface: The World Is Yours* (Port Android ARM64)

`v0.4.0-beta` | [Read in English](README.md) | [Créditos e Atribuições](CREDITS.md)

O **LittleRecomp** ([github.com/erickgxbriel/littlerecomp](https://github.com/erickgxbriel/littlerecomp)) é um port nativo não oficial para **Android (`arm64-v8a`) + Vulkan 1.1 TBDR** baseado no projeto **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)** (recompilação estática da versão de Xbox Original de *Scarface: The World Is Yours*).

> [!IMPORTANT]
> **Todos os créditos ao criador do port original para PC:**
> Todo o trabalho de recompilação estática, engenharia reversa do executável do Xbox, hooks específicos do jogo, conversão de saves e runtime base foi feito por **[efm-games/scarface-recomp](https://github.com/efm-games/scarface-recomp)** (usando [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) e [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)).
> Nosso trabalho no **LittleRecomp** foi exclusivamente portar o projeto para Android ARM64 (renderer Vulkan 1.1 TBDR mobile, áudio Oboe, controles touch/gamepad SDL3, gerenciamento térmico ADPF e interface mobile em PT-BR/EN).

> [!NOTE]
> **Modelo BYOG (Bring Your Own Game) — Nenhum arquivo do jogo incluso:**
> Este repositório público contém **apenas código-fonte**. Nenhum asset, textura, áudio, ISO ou `default.xbe` faz parte do repositório. Você precisa fornecer sua própria cópia legal da versão de **Xbox Original** do jogo.

---

## Requisitos (Celular)
- **Sistema:** Android 11 ou superior (API 30+), processador `arm64-v8a`.
- **GPU:** Suporte nativo a **Vulkan 1.1+** (validado no Samsung Galaxy A25 5G / Mali-G68 MP4).
- **Cópia do Jogo:** Sua própria imagem `.iso` ou `.xiso` do **Xbox Original** (ou pasta extraída com `default.xbe`). **Versões de PS2 e PC NÃO funcionam.**

---

## Tutorial de Instalação (100% no Celular)

1. Baixe e instale o APK do **LittleRecomp** no seu celular.
2. Salve sua imagem `.iso` ou `.xiso` do *Scarface* (**Xbox Original**) na memória interna ou cartão SD do aparelho *(não selecione arquivos direto do Google Drive sem baixar localmente antes)*.
3. Abra o **LittleRecomp**:
   - Na primeira execução, o **Assistente de Configuração** aparece automaticamente em **Português (BR)** ou **English**.
   - Toque em **Selecionar ISO / XISO** *(recomendado — lê o arquivo direto pelo seletor do Android sem precisar copiar os 3.2 GB e sem pedir permissões extras!)*.
   - Se preferir usar arquivos já extraídos, toque em **Selecionar Pasta do Jogo** e selecione a pasta que contém o `default.xbe`.
4. Jogue usando os **controles virtuais na tela** ou conectando um **controle físico Bluetooth/USB** (Xbox / PlayStation / Switch).
5. Toque em **MENU** no **canto superior direito** a qualquer momento para ajustar:
   - **Gráficos:** Escala interna (`0.6x / 0.75x / 1.0x / 1.5x / 2.0x / 2.25x`), proporção (**Ultrawide Real** padrão, Esticar/Tela Cheia, 16:9), upscaler FP16 (**Snapdragon GSR v1**, **FSR 1.0**, **Bilinear**), **Filtro Anisotrópico** (`2x–16x`), **VSync**, **Pré-Rotação Vulkan**, **Texturas Leves (-75% VRAM)**, **Preset: Celular Fraco**, opções de **Efeitos Auxiliares Rápidos (`0.5x`)** e **DRS Inteligente (FPS + Térmico)**, **Modo 60 FPS Experimental** e **HUD de Desempenho** (**Desligado**, **Apenas FPS**, **Básico** ou **Avançado**).
   - **Controles:** Controles touch vetoriais estilo arcade, **Editor de Layout Touch**, **Mira Automática + Tiro**, **Ocultar Touch com Controle Conectado**, **Vibração (Rumble)** e direção estilo GTA (`modern_pad`).
   - **Saves:** Gerenciador integrado de slots com **Exportar/Importar Backup (`.zip`)** via SAF (com recuperação automática de permissões Android 11+ FUSE).
   - **Conquistas:** 45 conquistas offline (500 pts, alinhadas ao RA #20684), leitura O(1) a 2 Hz (**0% de impacto no FPS**), selo **Hardcore `[HC]`**, sincronização retroativa ao carregar saves e notificações animadas.
   - **Jogo & Idioma:** Idioma da interface (`Automático / PT-BR / English`), **Tradução PT-BR Híbrida** (menus nativos `TextBible` + legendas HD em Roboto + packs `mods/ptbr/*.tsv`), **Camada de Mods (`mods/`)**, **Importar Mod / Tradução (`.zip`)**, Pular Logos de Abertura e Cheats.

---

## Recursos Técnicos do Port Android
- **Renderer Vulkan 1.1 / 1.2+ Otimizado para GPUs Mobile TBDR:** buffers depth/stencil `LAZILY_ALLOCATED` + `TRANSIENT_ATTACHMENT`, coalescência de Render Passes, **Ultrawide Real 20:9** sem distorção, caminho rápido **Vulkan 1.2/1.3 `VK_KHR_timeline_semaphore`** (com fallback 1.1), `MemSigFast()` 64-bit, `TexSlabAllocator` (slabs VRAM de 16 MB), cache persistente SPIR-V + `VkPipelineCache` e cache LRU de ISO (`512 KB`).
- **Compatibilidade Universal:** checagem dinâmica de `features.textureCompressionBC` (ativando decodificação ARM64 NEON BC1–BC3 apenas em GPUs sem suporte nativo), detecção dinâmica dos núcleos fortes via `cpuinfo_max_freq` e uso dinâmico de **Android ADPF** + sensores térmicos.
- **Áudio e Controles:** Google Oboe (AAudio) com suporte a Xbox ADPCM + SDL3 multitouch + editor de layout + Gamepad físico.

---

## Como Compilar do Código-Fonte (Linux x86_64 -> Android ARM64)

Este repositório **não** distribui a pasta `generated/`. Para compilar a partir da sua `default.xbe`:

```bash
cmake -S port/android -B out/android-arm64-phase1 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 \
  -DSCARFACE_BUILD_ANDROID_APP=ON \
  -DCMAKE_BUILD_TYPE=Release

# APK Debug:
bash port/android/build_apk.sh

# APK Release (debuggable=false, stripped, alinhado 16 KB):
BUILD_TYPE=release bash port/android/build_apk.sh
```

Consulte [CREDITS.md](CREDITS.md) para a lista completa de atribuições e bibliotecas utilizadas.
