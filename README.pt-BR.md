# LittleRecomp — *Scarface: The World Is Yours* (Port Android ARM64)

`v0.1.0-beta` | [Read in English](README.md) | [Créditos e Atribuições](CREDITS.md)

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
   - **Gráficos:** Escala interna (`1.0x / 1.5x / 2.0x`), upscaler FP16 (**Snapdragon GSR v1**, **FSR 1.0**, **Bilinear**), **Efeitos Auxiliares Nativos (`1.0x`)**, **Resolução Dinâmica Térmica (DRS)**, **Modo 60 FPS Experimental** e **Contador de Desempenho (HUD)** (**Desligado**, **Compacto: FPS + Temp GPU** ou **Telemetria Completa**).
   - **Controles:** Ativar/ocultar controles virtuais, opacidade e direção estilo GTA (`modern_pad`).
   - **Jogo & Idioma:** Idioma (`Automático / Português (BR) / English`), Pular Logos de Abertura e Cheats.

---

## Recursos Técnicos do Port Android
- **Renderer Vulkan 1.1 Otimizado para GPUs Mobile TBDR:** buffers depth/stencil `LAZILY_ALLOCATED` + `TRANSIENT_ATTACHMENT`, coalescência de Render Passes, uploads de texturas em pré-pass (`upload_cmd`) sem quebrar o Render Pass ativo, render targets auxiliares em `1.0x` e cache persistente em disco de shaders SPIR-V + `VkPipelineCache`.
- **Compatibilidade Universal:** checagem dinâmica de `features.textureCompressionBC` (ativando decodificação ARM64 NEON BC1–BC3 apenas em GPUs sem suporte nativo), detecção dinâmica dos núcleos fortes via `cpuinfo_max_freq` e uso dinâmico de **Android ADPF** + sensores térmicos.
- **Áudio e Controles:** Google Oboe (AAudio) com suporte a Xbox ADPCM + SDL3 multitouch + Gamepad físico.

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
