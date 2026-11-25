// Hooks for default.xbe. A strong definition replaces the recompiled function at that address.
#include "app/app.h"

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

#include "common/kbm.h"
#include "cpu/x86_ops.h"
#include "core/log.h"
#include "game/hud_prompts.h"
#include "kernel/memory.h"

// The startup state machine sets up a playlist of the five publisher logos through sub_00028940 and only advances to
// the menu after the manager finishes it. The manager finishes in phase 5, which restores the script update mask the
// setup saved and lets the boot state machine resume; suppressing the update instead strands the boot, and emptying
// the playlist walks past the end of the table. Skipping is therefore a forced jump to phase 5 on the first update,
// so the original phase 5 block performs the real completion handshake.
constexpr uint32_t kLogoTable = 0x44E968;

X86_EXTERN_FUNC(__imp__sub_00028940);
X86_EXTERN_FUNC(__imp__sub_00028B10);
X86_EXTERN_FUNC(__imp__sub_0024DF70);
X86_EXTERN_FUNC(__imp__sub_0024D900);
X86_EXTERN_FUNC(__imp__sub_0024D9B0);
X86_EXTERN_FUNC(__imp__sub_00194A30);
X86_EXTERN_FUNC(__imp__sub_002492A0);
X86_EXTERN_FUNC(__imp__sub_00249300);
X86_EXTERN_FUNC(__imp__sub_00240570);
X86_EXTERN_FUNC(__imp__sub_00099BC0);
X86_EXTERN_FUNC(sub_0006FB50);  // guest script evaluator, called only for queued diagnostic commands

namespace hud_prompts {

std::atomic<bool> g_frozen{false};
std::atomic<bool> g_key_labels{true};

bool UseKeyLabels() {
    if (g_frozen.load(std::memory_order_relaxed)) return g_key_labels.load(std::memory_order_relaxed);
    return kbm::enabled && kbm::hud_prompts && kbm::LastDevice() != kbm::Device::Pad;
}

bool Frozen() { return g_frozen.load(std::memory_order_relaxed); }

void SetMusicLibraryPage(bool active, bool key_labels) {
    if (active) {
        g_key_labels.store(key_labels, std::memory_order_relaxed);
        g_frozen.store(true, std::memory_order_relaxed);
    } else {
        g_frozen.store(false, std::memory_order_relaxed);
    }
}

}  // namespace hud_prompts

namespace {

// Set once the playlist of the current boot has been advanced, so the phase is not forced again after it resets.
bool g_logo_skipped = false;

bool IsLogoManager(uint8_t* base, uint32_t mgr) {
    return mgr != 0 && X86_MEM32(mgr + 0x14) == kLogoTable;
}

// Calls a player object's destructor (vtable + 0x04) exactly like the playlist completion does.
void ReleaseObject(X86Context& ctx, uint8_t* base, uint32_t obj) {
    if (!obj) return;
    uint32_t vt = X86_MEM32(obj);
    if (!vt) return;
    uint32_t fn = X86_MEM32(vt + 4);
    if (!fn) return;
    X86_PUSH32(0);  // the guest `ret` pops this and returns to the caller of x86_icall
    ctx.ecx = obj;
    x86_icall(ctx, base, fn);
}

// The HUD expands [TOKEN] strings through sub_0024D900. The dispatcher takes a narrow token but returns a
// UTF-16LE guest string. The caller passes that result to wcslen and copies it into the wide output, so our
// labels must be UTF-16LE as well; a bracketed label survives the recursive expander as plain text.
constexpr uint32_t kPromptPage = 0x1000;
constexpr uint32_t kPromptContexts = 3;

static_assert(sizeof(char16_t) == 2, "guest strings are UTF-16LE");

// The binding editor can rebuild labels on the window thread while the game thread expands text.
std::mutex g_label_mutex;

struct KeyPrompt {
    const char* token;
    // Indexed by kbm::PromptContext: on foot, vehicle, menu. Rebuilt from the bindings when they change.
    std::u16string labels[kPromptContexts];
    uint32_t va[kPromptContexts] = {};
};

// Token meanings come from the game's own controller-options text in cement.rcf; keys follow docs/controls.md.
// A pad token can mean different things in a menu, on foot, and in a vehicle, so each has its own label.
KeyPrompt kKeyPrompts[] = {
    {"BUTTON_GRAPHIC_L", {u"[WASD]", u"[A/D]", u"[WASD]"}},                  // left stick: move / steer
    {"BUTTON_GRAPHIC_R", {u"[Mouse]", u"[Mouse]", u"[Mouse]"}},              // right stick: look
    {"BUTTON_GRAPHIC_X", {u"[Shift]", u"[W]", u"[Enter]"}},                  // A: sprint / accelerate / accept
    {"BUTTON_GRAPHIC_CIRCLE", {u"[T]", u"[Space]", u"[Esc]"}},               // B: taunt / handbrake / back
    {"BUTTON_GRAPHIC_TRIANGLE", {u"[E]", u"[E]", u"[E]"}},                   // Y: interact / enter or exit
    {"BUTTON_GRAPHIC_SQUARE", {u"[R]", u"[S]", u"[R]"}},                     // X: reload / brake
    {"BUTTON_GRAPHIC_R1", {u"[Space]", u"[LMB]", u"[Space]"}},               // RT: fire
    {"BUTTON_GRAPHIC_R2", {u"[F]", u"[Z]", u"[F]"}},                         // Black: grapple / look back
    {"BUTTON_GRAPHIC_L1", {u"[Caps]", u"[Caps]", u"[Caps]"}},                // LT: lock / strafe
    {"BUTTON_GRAPHIC_L2", {u"[Tab]", u"[Tab]", u"[Tab]"}},                   // White: SAT phone
    {"BUTTON_GRAPHIC_L3", {u"[Ctrl]", u"[Ctrl]", u"[Ctrl]"}},                // L3: crouch
    {"BUTTON_GRAPHIC_R3", {u"[C]", u"[C]", u"[C]"}},                         // R3: camera
    {"BUTTON_GRAPHIC_DPAD_DOWN", {u"[3]", u"[3]", u"[3]"}},                  // equip / conceal
    {"BUTTON_GRAPHIC_DPAD_UP", {u"[Up]", u"[Up]", u"[Up]"}},
    {"BUTTON_GRAPHIC_DPAD_RIGHT", {u"[Right]", u"[Right]", u"[Right]"}},
    {"BUTTON_GRAPHIC_DPAD_VERTICAL", {u"[Up/Down]", u"[Up/Down]", u"[Up/Down]"}},
    {"BUTTON_GRAPHIC_DPAD_HORIZONTAL", {u"[1/2]", u"[1/2]", u"[1/2]"}},      // cycle weapons
    {"BUTTON_GRAPHIC_DECLINE", {u"[Esc]", u"[Esc]", u"[Esc]"}},              // generic back/cancel glyph
    {"BUTTON_GRAPHIC_START", {u"[Esc]", u"[Esc]", u"[Esc]"}},                // pad Start: pause / back
    {"BUTTON_TEXT_X", {u"[Enter]", u"[Enter]", u"[Enter]"}},                 // license screen's textual X
    {"BUTTON_TEXT_L2", {u"[Tab]", u"[Tab]", u"[Tab]"}},
    {"BUTTON_TEXT_L1", {u"[Caps]", u"[Caps]", u"[Caps]"}},
    {"BUTTON_TEXT_R1", {u"[Space]", u"[Space]", u"[Space]"}},
    {"BUTTON_TEXT_R2", {u"[F]", u"[F]", u"[F]"}},
    {"BUTTON_TEXT_CIRCLE", {u"[T]", u"[T]", u"[T]"}},
    {"BUTTON_TEXT_SQUARE", {u"[R]", u"[R]", u"[R]"}},
    {"BUTTON_TEXT_LEFT_ANALOG", {u"[WASD]", u"[WASD]", u"[WASD]"}},
    {"BUTTON_TEXT_RIGHT_ANALOG", {u"[Mouse]", u"[Mouse]", u"[Mouse]"}},
    {"BUTTON_TEXT_L3_PRESS", {u"[Ctrl]", u"[Ctrl]", u"[Ctrl]"}},
    {"BUTTON_TEXT_RIGHT", {u"[Right]", u"[Right]", u"[Right]"}},
    {"BUTTON_GRAPHIC_SELECT", {u"[Tab]", u"[Tab]", u"[Tab]"}},              // Back: SAT phone
    // These tokens pair a pad glyph with a bare action word in a page, so hiding the artwork would leave just the
    // word. Prepend the PC key the same way the embedded [BUTTON_GRAPHIC_*] dialog tokens are rendered.
    {"XSTART", {u"[Enter] Start", u"[Enter] Start", u"[Enter] Start"}},       // mission-start screen
    {"XSELECT", {u"[Enter] Select", u"[Enter] Select", u"[Enter] Select"}},   // pause menus
    {"XRESTART", {u"[Enter] Restart", u"[Enter] Restart", u"[Enter] Restart"}},
    {"XCONFIRM", {u"[Enter] Confirm", u"[Enter] Confirm", u"[Enter] Confirm"}},
    {"XPROCEED", {u"[Enter] Proceed", u"[Enter] Proceed", u"[Enter] Proceed"}},
    {"TBACK", {u"[E] Back", u"[E] Back", u"[E] Back"}},                       // triangle prompts
    {"TCANCEL", {u"[E] Cancel", u"[E] Cancel", u"[E] Cancel"}},
    {"TEXIT", {u"[E] Exit", u"[E] Exit", u"[E] Exit"}},
    // The look tutorial's controller page is Xbox-only; the PC combat tutorial has no equivalent screen. Keep the
    // original wording and supply the PC menu keys the user chose for the two prompts whose glyphs are hidden.
    {"LOOK_TUTORIAL_START", {u"[Enter] Options and Profiles", u"[Enter] Options and Profiles", u"[Enter] Options and Profiles"}},
    {"LOOK_TUTORIAL_EXIT", {u"[Esc] Exit Tutorial", u"[Esc] Exit Tutorial", u"[Esc] Exit Tutorial"}},
    // Some scripts use the bare face-letter shorthand instead of BUTTON_GRAPHIC_*.
    {"A", {u"[Shift]", u"[W]", u"[Enter]"}},
    {"B", {u"[T]", u"[Space]", u"[Esc]"}},
    {"X", {u"[R]", u"[S]", u"[R]"}},
    {"Y", {u"[E]", u"[E]", u"[E]"}},
};

// Line-level rules override the context mapping when the same pad button has a different PC meaning. Every label
// here is the one the official PC manual uses, and all three context slots are the same because the rule already
// decided what the button means on this line.
KeyPrompt kSprintPrompt = {"BUTTON_GRAPHIC_X", {u"[Shift]", u"[Shift]", u"[Shift]"}};
KeyPrompt kAcceptPrompt = {"BUTTON_GRAPHIC_X", {u"[Enter]", u"[Enter]", u"[Enter]"}};
KeyPrompt kCancelXPrompt = {"BUTTON_GRAPHIC_X", {u"[Esc]", u"[Esc]", u"[Esc]"}};
KeyPrompt kRagePrompt = {"BUTTON_GRAPHIC_CIRCLE", {u"[V]", u"[V]", u"[V]"}};
KeyPrompt kTauntPrompt = {"BUTTON_GRAPHIC_CIRCLE", {u"[T]", u"[T]", u"[T]"}};
KeyPrompt kClosePrompt = {"BUTTON_GRAPHIC_CIRCLE", {u"[Esc]", u"[Esc]", u"[Esc]"}};
KeyPrompt kCancelPrompt = {"BUTTON_GRAPHIC_CIRCLE", {u"[Esc]", u"[Esc]", u"[Esc]"}};
KeyPrompt kPickupPrompt = {"BUTTON_GRAPHIC_TRIANGLE", {u"[E]", u"[E]", u"[E]"}};
// The look tutorial is the one screen where the pad's Start opens Options and Profiles and B exits. The line-level
// rules below pick these even when the game reuses the generic Start/Circle glyph tokens on that page.
KeyPrompt kLookTutorialStartPrompt = {"BUTTON_GRAPHIC_START",
                                      {u"[Enter] Options and Profiles", u"[Enter] Options and Profiles",
                                       u"[Enter] Options and Profiles"}};
KeyPrompt kLookTutorialExitPrompt = {"BUTTON_GRAPHIC_CIRCLE",
                                     {u"[Esc] Exit Tutorial", u"[Esc] Exit Tutorial", u"[Esc] Exit Tutorial"}};

namespace {

std::u16string Wide(const std::string& narrow) {
    std::u16string out;
    out.reserve(narrow.size());
    for (char ch : narrow) out.push_back(char16_t(uint8_t(ch)));
    return out;
}

std::string KeyCombo(std::initializer_list<kbm::Binding> bindings) {
    std::string parts;
    for (kbm::Binding binding : bindings) {
        const char* name = kbm::KeyCodeName(kbm::BindingKeyCode(binding));
        if (!name || !std::strcmp(name, "Unbound")) continue;
        if (!parts.empty()) parts += '/';
        parts += name;
    }
    if (parts.empty()) parts = "Unbound";
    return "[" + parts + "]";
}

std::string Key(kbm::Binding binding) { return KeyCombo({binding}); }

// PC prompts are experimental: they name the key now bound to the pad control the game is asking for. This keeps a
// rebind visible in the HUD instead of showing the old PC-manual key.
std::string PromptText(const char* token) {
    auto is = [token](const char* name) { return std::strcmp(token, name) == 0; };
    if (is("BUTTON_GRAPHIC_L") || is("BUTTON_TEXT_LEFT_ANALOG"))
        return KeyCombo({kbm::Binding::MoveUp, kbm::Binding::MoveLeft, kbm::Binding::MoveDown,
                         kbm::Binding::MoveRight});
    if (is("BUTTON_GRAPHIC_R") || is("BUTTON_TEXT_RIGHT_ANALOG")) return "[Mouse]";
    if (is("BUTTON_GRAPHIC_X") || is("BUTTON_TEXT_X") || is("A")) return Key(kbm::Binding::A);
    if (is("BUTTON_GRAPHIC_CIRCLE") || is("B") || is("BUTTON_GRAPHIC_DECLINE") || is("BUTTON_TEXT_CIRCLE"))
        return Key(kbm::Binding::B);
    if (is("BUTTON_GRAPHIC_TRIANGLE") || is("Y")) return Key(kbm::Binding::Y);
    if (is("BUTTON_GRAPHIC_SQUARE") || is("X") || is("BUTTON_TEXT_SQUARE")) return Key(kbm::Binding::X);
    if (is("BUTTON_GRAPHIC_R1") || is("BUTTON_TEXT_R1")) return Key(kbm::Binding::RT);
    if (is("BUTTON_GRAPHIC_R2") || is("BUTTON_TEXT_R2")) return Key(kbm::Binding::RB);
    if (is("BUTTON_GRAPHIC_L1") || is("BUTTON_TEXT_L1")) return Key(kbm::Binding::LT);
    if (is("BUTTON_GRAPHIC_L2") || is("BUTTON_TEXT_L2")) return Key(kbm::Binding::LB);
    if (is("BUTTON_GRAPHIC_L3") || is("BUTTON_TEXT_L3_PRESS")) return Key(kbm::Binding::L3);
    if (is("BUTTON_GRAPHIC_R3")) return Key(kbm::Binding::R3);
    if (is("BUTTON_GRAPHIC_DPAD_UP")) return Key(kbm::Binding::DpadUp);
    if (is("BUTTON_GRAPHIC_DPAD_DOWN")) return Key(kbm::Binding::DpadDown);
    if (is("BUTTON_GRAPHIC_DPAD_LEFT")) return Key(kbm::Binding::DpadLeft);
    if (is("BUTTON_GRAPHIC_DPAD_RIGHT") || is("BUTTON_TEXT_RIGHT")) return Key(kbm::Binding::DpadRight);
    if (is("BUTTON_GRAPHIC_DPAD_VERTICAL")) return KeyCombo({kbm::Binding::DpadUp, kbm::Binding::DpadDown});
    if (is("BUTTON_GRAPHIC_DPAD_HORIZONTAL")) return KeyCombo({kbm::Binding::DpadLeft, kbm::Binding::DpadRight});
    if (is("BUTTON_GRAPHIC_SELECT")) return Key(kbm::Binding::Back);
    if (is("BUTTON_GRAPHIC_START")) return Key(kbm::Binding::Start);
    if (is("XSTART")) return Key(kbm::Binding::A) + " Start";
    if (is("XSELECT")) return Key(kbm::Binding::A) + " Select";
    if (is("XRESTART")) return Key(kbm::Binding::A) + " Restart";
    if (is("XCONFIRM")) return Key(kbm::Binding::A) + " Confirm";
    if (is("XPROCEED")) return Key(kbm::Binding::A) + " Proceed";
    if (is("TBACK")) return Key(kbm::Binding::Y) + " Back";
    if (is("TCANCEL")) return Key(kbm::Binding::Y) + " Cancel";
    if (is("TEXIT")) return Key(kbm::Binding::Y) + " Exit";
    if (is("LOOK_TUTORIAL_START")) return Key(kbm::Binding::Start) + " Options and Profiles";
    if (is("LOOK_TUTORIAL_EXIT")) return Key(kbm::Binding::B) + " Exit Tutorial";
    return {};
}

void SetLabels(KeyPrompt& prompt, const std::string& text) {
    std::u16string wide = Wide(text);
    for (uint32_t context = 0; context < kPromptContexts; ++context) {
        prompt.labels[context] = wide;
        prompt.va[context] = 0;
    }
}

}  // namespace

// Defined just below with the prompt page; declared here so the label refresh can reset the cached page.
extern uint32_t g_prompt_base;
extern uint32_t g_prompt_cursor;

void RefreshKeyLabelsImpl() {
    std::lock_guard<std::mutex> lock(g_label_mutex);
    for (KeyPrompt& prompt : kKeyPrompts) {
        if (std::string text = PromptText(prompt.token); !text.empty())
            SetLabels(prompt, text);
        else
            for (uint32_t context = 0; context < kPromptContexts; ++context) prompt.va[context] = 0;
    }
    SetLabels(kSprintPrompt, Key(kbm::Binding::A));
    SetLabels(kAcceptPrompt, Key(kbm::Binding::A));
    SetLabels(kCancelXPrompt, Key(kbm::Binding::B));
    SetLabels(kRagePrompt, Key(kbm::Binding::B));
    SetLabels(kTauntPrompt, Key(kbm::Binding::B));
    SetLabels(kClosePrompt, Key(kbm::Binding::B));
    SetLabels(kCancelPrompt, Key(kbm::Binding::B));
    SetLabels(kPickupPrompt, Key(kbm::Binding::Y));
    SetLabels(kLookTutorialStartPrompt, Key(kbm::Binding::Start) + " Options and Profiles");
    SetLabels(kLookTutorialExitPrompt, Key(kbm::Binding::B) + " Exit Tutorial");
    g_prompt_cursor = g_prompt_base;  // rewrite the same guest page on the next expansion
}

// The expander sets this before it calls the token dispatcher and restores it afterwards.
thread_local uint32_t g_source_line = 0;

// Nested tokens are extracted from a wide string by sub_0024D9B0, which calls the token dispatcher directly. Keep a
// few narrow copies of that wide string in guest memory so the line rules can see the sentence around each token.
constexpr uint32_t kSourcePage = 0x2000;
constexpr uint32_t kSourceSlot = 1024;
uint32_t g_source_base = 0;
thread_local uint32_t g_source_depth = 0;

uint32_t g_prompt_base = 0;
uint32_t g_prompt_cursor = 0;

// One page of guest memory holds the labels: the expander calls wcslen/sub_0024D9B0 on the returned pointer,
// so it has to be a guest address and stay valid for the rest of the run.
uint32_t PromptPage() {
    if (g_prompt_base) return g_prompt_base;
    uint32_t va = 0, size = kPromptPage;
    if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
    g_prompt_base = va;
    g_prompt_cursor = va;
    return va;
}

bool EnsureLabel(uint8_t* mem_base, KeyPrompt& p, uint32_t context) {
    std::lock_guard<std::mutex> lock(g_label_mutex);
    const std::u16string& label = p.labels[context];
    if (label.empty()) return false;
    uint32_t& va = p.va[context];
    if (va) return true;
    if (!PromptPage()) return false;
    uint32_t bytes = uint32_t((label.size() + 1) * sizeof(char16_t));
    uint32_t span = (bytes + 3) & ~3u;
    if (g_prompt_cursor + span > g_prompt_base + kPromptPage) return false;
    std::memcpy(mem_base + g_prompt_cursor, label.c_str(), bytes);
    va = g_prompt_cursor;
    g_prompt_cursor += span;
    return true;
}

bool TokenEquals(uint8_t* mem_base, uint32_t token, const char* expected) {
    for (uint32_t i = 0; i <= 64; ++i) {
        char want = expected[i];
        uint8_t got = mem_base[token + i];
        if (want == '\0') return got == '\0';
        if (got != uint8_t(want)) return false;
    }
    return false;
}

uint32_t SourcePage() {
    if (g_source_base) return g_source_base;
    uint32_t va = 0, size = kSourcePage;
    if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
    g_source_base = va;
    return va;
}

// sub_0024D9B0 extracts bracketed tokens from a UTF-16LE string. Copy that string to a narrow guest buffer so the
// token dispatcher can classify the token from the words around it, including nested tokens in localized messages.
uint32_t CopyWideSource(uint8_t* mem_base, uint32_t wide) {
    if (!mem_base || !wide || !mem::IsCommitted(wide)) return 0;
    uint32_t base = SourcePage();
    if (!base) return 0;
    uint32_t slot = g_source_depth & 7;
    uint8_t* dst = mem_base + base + slot * kSourceSlot;
    uint32_t copied = 0;
    for (; copied < kSourceSlot - 1; ++copied) {
        uint16_t ch = *reinterpret_cast<const uint16_t*>(mem_base + wide + copied * 2);
        if (!ch) break;
        dst[copied] = ch < 128 ? uint8_t(ch) : '?';
    }
    dst[copied] = 0;  // terminate immediately: stale bytes can otherwise change the line rule on the next expansion
    return base + slot * kSourceSlot;
}

// Opt-in diagnostic commands need a guest address for the script text and a guest thread to run on. Reuse a small
// scratch page and the text-expansion hook below; the page is only written by the hook that is about to consume it.
constexpr uint32_t kDiagnosticCommandPage = 0x1000;
uint32_t g_diagnostic_command_base = 0;

uint32_t DiagnosticCommandPage() {
    if (g_diagnostic_command_base) return g_diagnostic_command_base;
    uint32_t va = 0, size = kDiagnosticCommandPage;
    if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
    g_diagnostic_command_base = va;
    return va;
}

// sub_0006FB50 takes (script_string, object) on the guest stack and returns a script object in EAX. Passing 0 as the
// object is what the debug menu's "Save Game To Profile N" entries do. Save every register and flag afterwards: this
// call is an injected side effect, not part of the guest function we interrupted.
void RunDiagnosticScriptCommand(X86Context& ctx, uint8_t* base) {
    if (!kbm::DiagnosticCommandsEnabled()) return;
    char command[256];
    if (!kbm::TakeDiagnosticCommand(command)) return;
    if (!base) {
        LOGE("diagnostic: no guest memory for script command '%s'", command);
        return;
    }
    uint32_t page = DiagnosticCommandPage();
    size_t length = std::strlen(command) + 1;  // TakeDiagnosticCommand guarantees the buffer is terminated
    if (!page || length > kDiagnosticCommandPage) {
        LOGE("diagnostic: no scratch page for script command '%s'", command);
        return;
    }
    std::memcpy(base + page, command, length);

    X86Context saved = ctx;
    X86_PUSH32(0);             // object argument; debug save entries use 0
    X86_PUSH32(page);          // narrow script string
    X86_PUSH32(0x6FB50u);      // return address consumed by the guest ret
    sub_0006FB50(ctx, base);
    ctx = saved;
    LOGI("diagnostic: executed script command '%s'", command);
}

// The second expansion pass parses our own bracketed labels ("[Enter] ...", "[Esc] ...") as tokens again. Never feed
// those back into the line rules: a tutorial line containing "options and profiles" would otherwise relabel the Esc
// token as the Start prompt, and vice versa.
bool IsInjectedLabelToken(uint8_t* mem_base, uint32_t token) {
    static const char* const labels[] = {"Enter", "Esc", "Shift", "Mouse", "Space", "WASD", "A/D", "T", "V", "E"};
    for (const char* label : labels)
        if (TokenEquals(mem_base, token, label)) return true;
    return false;
}

// The expander receives the complete narrow source line. A bounded scan is enough to classify the few lines where a
// pad token needs a label that context alone cannot choose.
bool SourceContains(uint8_t* mem_base, uint32_t src, const char* needle, bool insensitive = false) {
    if (!mem_base || !src || !needle || !needle[0]) return false;
    auto eq = [insensitive](uint8_t a, char b) {
        if (!insensitive) return a == uint8_t(b);
        uint8_t x = a >= 'a' && a <= 'z' ? a - 32 : a;
        uint8_t y = b >= 'a' && b <= 'z' ? b - 32 : b;
        return x == y;
    };
    for (uint32_t i = 0; i < 1024; ++i) {
        uint8_t c = mem_base[src + i];
        if (!c) return false;
        if (!eq(c, needle[0])) continue;
        uint32_t j = 1;
        while (needle[j] && eq(mem_base[src + i + j], needle[j])) ++j;
        if (!needle[j]) return true;
    }
    return false;
}

// Choose a label from the actual words on the line. The ActionMap stack is not reliable here: the title screen had no
// menu map yet, and a tutorial notice can leave a dialog map on top, so context alone produced [Shift] on "start" and
// [Enter] on "sprint". The line is authoritative for these well-known strings.
KeyPrompt* OverrideForToken(uint8_t* mem_base, uint32_t src, uint32_t token) {
    if (!mem_base || !token || IsInjectedLabelToken(mem_base, token)) return nullptr;
    auto has = [&](const char* text) { return SourceContains(mem_base, src, text, true); };

    // The tutorial can hand the generic Start/B glyph tokens to the dispatcher without a source line. Its ActionMap
    // is the only surviving clue there, so use it instead of the generic "[Esc]" Start label.
    if (!src && kbm::LookTutorialActive()) {
        if (TokenEquals(mem_base, token, "BUTTON_GRAPHIC_START") || TokenEquals(mem_base, token, "LOOK_TUTORIAL_START"))
            return &kLookTutorialStartPrompt;
        if (TokenEquals(mem_base, token, "BUTTON_GRAPHIC_CIRCLE") || TokenEquals(mem_base, token, "LOOK_TUTORIAL_EXIT"))
            return &kLookTutorialExitPrompt;
    }
    if (!src) return nullptr;  // without a line, the remaining rules have no words to classify

    // Look tutorial captions use the generic Start/B glyphs. Match the sentence first so the top and bottom prompts
    // cannot both collapse to the generic "[Esc] Exit Tutorial" label.
    if (has("[look_tutorial_start]") || has("options and profiles")) return &kLookTutorialStartPrompt;
    if (has("[look_tutorial_exit]") || has("exit tutorial")) return &kLookTutorialExitPrompt;

    bool x = TokenEquals(mem_base, token, "BUTTON_GRAPHIC_X") || TokenEquals(mem_base, token, "A");
    bool circle = TokenEquals(mem_base, token, "BUTTON_GRAPHIC_CIRCLE") || TokenEquals(mem_base, token, "B");
    bool triangle = TokenEquals(mem_base, token, "BUTTON_GRAPHIC_TRIANGLE") || TokenEquals(mem_base, token, "Y");

    // The title screen calls X "start" in its localized string; the line itself still says start.
    if (x && has("[look_tutorial_x]")) return &kAcceptPrompt;

    // Check sprint before start: "start sprinting" must stay L-Shift, not Enter.
    if (x && (has("sprint") || has("run while"))) return &kSprintPrompt;
    if (x && (has("start") || has("accept") || has("continue") || has("select") || has("[yes]") || has("[load]") ||
              has("cover"))) {
        return &kAcceptPrompt;
    }
    // The game's [CLOSE] action is pad B, which is Esc on the PC bindings even when the notice also has an A caption.
    if (x && has("[close]")) return &kClosePrompt;
    if (x && (has("[cancel]") || has("[back]"))) return &kCancelXPrompt;

    if (circle && (has("rage") || has("[hold]"))) return &kRagePrompt;
    if (circle && (has("taunt") || has("provoke"))) return &kTauntPrompt;
    if (circle && has("[close]")) return &kClosePrompt;
    if (circle && (has("[cancel]") || has("[back]") || has("[create]") || has("[decline]") || has("[no]"))) {
        return &kCancelPrompt;
    }

    // Every triangle/Y demand uses the same PC key. There is no context where the original glyph should survive with
    // key prompts enabled, so this is deliberately global rather than tied to a few pickup/interact wordings.
    if (triangle) return &kPickupPrompt;
    return nullptr;
}

// Some strings are sent straight to the dispatcher without an expander source line. Those bare tokens still have a
// well-known PC meaning, so classify them by token alone instead of falling back to the context-only pad glyph.
KeyPrompt* OverrideForBareToken(uint8_t* mem_base, uint32_t token) {
    if (!mem_base || !token) return nullptr;
    if (TokenEquals(mem_base, token, "START_MISSION") || TokenEquals(mem_base, token, "ACCEPT") ||
        TokenEquals(mem_base, token, "YES") || TokenEquals(mem_base, token, "LOAD")) {
        return &kAcceptPrompt;
    }
    // Triangle/Y is consistently interact or pick up, so it is safe without the surrounding line.
    if (TokenEquals(mem_base, token, "BUTTON_GRAPHIC_TRIANGLE") || TokenEquals(mem_base, token, "Y")) {
        return &kPickupPrompt;
    }
    if (TokenEquals(mem_base, token, "CLOSE")) return &kClosePrompt;
    if (TokenEquals(mem_base, token, "CANCEL") || TokenEquals(mem_base, token, "BACK") ||
        TokenEquals(mem_base, token, "DECLINE") || TokenEquals(mem_base, token, "NO") ||
        TokenEquals(mem_base, token, "CREATE")) {
        return &kCancelPrompt;
    }
    return nullptr;
}

// Key labels are the default while keyboard/mouse is enabled; a real pad takes them back as soon as it sends input.
// A last-device value of None happens before the first input poll, and should not leave pad glyphs on screen.
bool UseKeyPrompts() {
    return hud_prompts::UseKeyLabels();
}

// Some in-game prompts are not written with a button token. The game hides the triangle artwork but leaves the
// localized words untouched, so the bracketed key has to be prepended to the expanded wide string instead. The
// text-object constructor below copies that string immediately, which makes a small rotating scratch area safe.
constexpr uint32_t kKeyPrefixPage = 0x1000;
constexpr uint32_t kKeyPrefixUnits = 256;
constexpr uint32_t kKeyPrefixSlots = 8;
uint32_t g_key_prefix_base = 0;
thread_local uint32_t g_key_prefix_cursor = 0;

uint32_t KeyPrefixSlot() {
    if (!g_key_prefix_base) {
        uint32_t va = 0, size = kKeyPrefixPage;
        if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
        g_key_prefix_base = va;
    }
    uint32_t slot = g_key_prefix_cursor++ & (kKeyPrefixSlots - 1);
    return g_key_prefix_base + slot * kKeyPrefixUnits * sizeof(char16_t);
}

uint16_t& WideAt(uint8_t* mem_base, uint32_t wide, uint32_t index) {
    return *reinterpret_cast<uint16_t*>(mem_base + wide + index * sizeof(char16_t));
}

bool WideUnitReadable(uint32_t wide, uint32_t index) {
    uint32_t va = wide + index * sizeof(char16_t);
    return mem::IsCommitted(va) && mem::IsCommitted(va + 1);
}

char UpperAscii(char c) {
    return c >= 'a' && c <= 'z' ? char(c - 32) : c;
}

bool WideCharEquals(uint16_t ch, char ascii, bool ignoreCase) {
    if (ch > 127) return false;
    char c = static_cast<char>(ch);
    return ignoreCase ? UpperAscii(c) == UpperAscii(ascii) : c == ascii;
}

bool WideStartsWith(uint8_t* mem_base, uint32_t wide, const char* ascii, bool ignoreCase = true) {
    if (!mem_base || !wide || !ascii) return false;
    for (uint32_t i = 0; ascii[i]; ++i) {
        if (!WideUnitReadable(wide, i) || !WideCharEquals(WideAt(mem_base, wide, i), ascii[i], ignoreCase))
            return false;
    }
    return true;
}

bool AsciiStartsWith(uint8_t* mem_base, uint32_t src, const char* ascii);

// These are the localized static captions the game binds to the triangle/Y action in its scripts, plus the ones seen
// in the HUD captures. Their wording is left untouched; only the bracketed key is added while key prompts are active.
// The music-player captions use their own keys because Y denotes a different pad action there.
constexpr const char* kInteractLabels[] = {
    "Weapons Locker", "Enter Vehicle", "Enter Garage", "Enter Boathouse", "Boathouse", "BOAT",
    "Dismiss Henchman", "Open Door", "Plant Bomb", "Collect Ammo", "Collect Shell", "Buy Weapons", "Buy drugs",
    "Launder money", "Refill Health", "Get information", "Take a Piss", "Distribute", "Equip", "Sell",
    "Pickup ", "Pick up ", "Pick Up ", "PICK UP ", "Pickup", "Pick up", "Pick Up", "PICK UP",
};

// Y is the global interact key. The tape player uses B for Play and A for Shuffle, which is Enter in the PC menus.
// Enter is also a normal game label, so the injected Shuffle prefix is recognized only on Shuffle captions.
constexpr char16_t kInteractPrefix[] = u"[E] ";
constexpr char16_t kPlayPrefix[] = u"[T] ";
constexpr char16_t kShufflePrefix[] = u"[Enter] ";
const char16_t* const kGenericInjectedPrefixes[] = {kInteractPrefix, kPlayPrefix};

template <typename Starts>
bool IsKnownInteractLabel(Starts starts) {
    for (const char* label : kInteractLabels)
        if (starts(label)) return true;
    return false;
}

// A guest string can point into a buffer that was freed after the page was built. Check every unit's page before
// reading it: a single start-of-string check is not enough when the terminator sits near an uncommitted page.
uint32_t WideLength(uint8_t* mem_base, uint32_t wide, uint32_t limit) {
    if (!mem_base || !wide || !mem::IsCommitted(wide)) return 0;
    for (uint32_t i = 0; i < limit; ++i) {
        if (!WideUnitReadable(wide, i)) return 0;
        if (!WideAt(mem_base, wide, i)) return i;
    }
    return 0;
}

// The game's constructor calls an unbounded wcslen on the pointer it receives. Never hand it one of our scratch
// strings unless the page is still committed and the string is terminated well inside the bounded scan.
bool SafeWideString(uint8_t* mem_base, uint32_t wide, uint32_t limit = 512) {
    return WideLength(mem_base, wide, limit) != 0;
}

bool WideEqualsAscii(uint8_t* mem_base, uint32_t wide, const char* ascii) {
    if (!mem_base || !wide || !ascii) return false;
    for (uint32_t i = 0; ascii[i]; ++i) {
        if (!WideUnitReadable(wide, i) || !WideCharEquals(WideAt(mem_base, wide, i), ascii[i], true)) return false;
    }
    uint32_t end = static_cast<uint32_t>(std::strlen(ascii));
    return WideUnitReadable(wide, end) && WideAt(mem_base, wide, end) == 0;
}

constexpr uint32_t kNoWideMatch = 0xFFFFFFFFu;

uint32_t FindWideAscii(uint8_t* mem_base, uint32_t wide, uint32_t capacity, const char* needle, bool ignoreCase) {
    if (!mem_base || !wide || !needle || !needle[0] || !capacity) return kNoWideMatch;
    uint32_t n = static_cast<uint32_t>(std::strlen(needle));
    if (n > capacity) return kNoWideMatch;
    for (uint32_t i = 0; i + n <= capacity; ++i) {
        if (!WideUnitReadable(wide, i)) return kNoWideMatch;
        uint32_t j = 0;
        while (j < n && WideUnitReadable(wide, i + j) &&
               WideCharEquals(WideAt(mem_base, wide, i + j), needle[j], ignoreCase))
            ++j;
        if (j == n) return i;
        if (WideAt(mem_base, wide, i) == 0) return kNoWideMatch;
    }
    return kNoWideMatch;
}

bool WideContainsAscii(uint8_t* mem_base, uint32_t wide, uint32_t capacity, const char* needle) {
    return FindWideAscii(mem_base, wide, capacity, needle, false) != kNoWideMatch;
}

bool WideStartsWithWide(uint8_t* mem_base, uint32_t wide, const char16_t* prefix) {
    if (!mem_base || !wide || !prefix) return false;
    for (uint32_t i = 0; prefix[i]; ++i) {
        if (!WideUnitReadable(wide, i) || WideAt(mem_base, wide, i) != prefix[i]) return false;
    }
    return true;
}

uint32_t PrefixUnits(const char16_t* prefix) {
    uint32_t n = 0;
    if (prefix)
        while (prefix[n]) ++n;
    return n;
}

bool IsKnownInteractLabelWide(uint8_t* mem_base, uint32_t wide);
bool IsKnownInteractLabelAscii(uint8_t* mem_base, uint32_t src);

// The tape player writes its pad-button glyph as U+00B9 before captions such as PLAY and STOP. Key prompts replace
// that glyph with the PC key; the pad path puts it back so the original button is the only one shown.
constexpr uint16_t kPlayGlyph = 0x00B9;   // B / Play-Stop button
constexpr uint16_t kShuffleGlyph = 0x00BD;  // the tape player's shuffle button

bool IsGlyphMusicWord(uint8_t* mem_base, uint32_t wide, const char* word) {
    if (!mem_base || !wide || !word || WideAt(mem_base, wide, 0) != kPlayGlyph) return false;
    for (uint32_t i = 0; word[i]; ++i)
        if (!WideCharEquals(WideAt(mem_base, wide, i + 1), word[i], true)) return false;
    return WideAt(mem_base, wide, std::strlen(word) + 1) == 0;
}

bool IsGlyphPlayPrompt(uint8_t* mem_base, uint32_t wide) {
    return IsGlyphMusicWord(mem_base, wide, "PLAY") || IsGlyphMusicWord(mem_base, wide, "STOP") ||
           IsGlyphMusicWord(mem_base, wide, "ADD");
}

bool IsGlyphShufflePrompt(uint8_t* mem_base, uint32_t wide) {
    return mem_base && wide && WideAt(mem_base, wide, 0) == kShuffleGlyph &&
           WideContainsAscii(mem_base, wide + sizeof(char16_t), 254, "SHUFFLE");
}

bool IsGlyphMusicPrompt(uint8_t* mem_base, uint32_t wide) {
    return IsGlyphPlayPrompt(mem_base, wide) || IsGlyphShufflePrompt(mem_base, wide);
}

bool IsPlayPromptWide(uint8_t* mem_base, uint32_t wide) {
    return WideEqualsAscii(mem_base, wide, "PLAY") || WideEqualsAscii(mem_base, wide, "STOP") ||
           IsGlyphPlayPrompt(mem_base, wide);
}

bool IsShufflePromptWide(uint8_t* mem_base, uint32_t wide) {
    // The page sets this caption in more than one way (TURN SHUFFLE ON/OFF, a placeholder control name, and a
    // glyph-prefixed variant). Matching SHUFFLE anywhere in a short prompt covers all of them without depending on
    // which form the current script chose.
    return WideLength(mem_base, wide, 256) <= 64 && WideContainsAscii(mem_base, wide, 256, "SHUFFLE");
}

constexpr uint32_t kNoAsciiMatch = 0xFFFFFFFFu;

uint32_t FindAsciiText(uint8_t* mem_base, uint32_t src, const char* text) {
    if (!mem_base || !src || !text || !text[0] || !mem::IsCommitted(src)) return kNoAsciiMatch;
    for (uint32_t i = 0; i < 128; ++i) {
        if (!mem_base[src + i]) break;
        uint32_t j = 0;
        while (text[j] && UpperAscii(static_cast<char>(mem_base[src + i + j])) == UpperAscii(text[j])) ++j;
        if (!text[j]) return i;
    }
    return kNoAsciiMatch;
}

// The narrow source can already carry an injected prefix, so match the Shuffle word anywhere in the short string.
bool AsciiContainsText(uint8_t* mem_base, uint32_t src, const char* text) {
    return FindAsciiText(mem_base, src, text) != kNoAsciiMatch;
}

bool IsShufflePromptAscii(uint8_t* mem_base, uint32_t src) {
    return AsciiContainsText(mem_base, src, "SHUFFLE");
}

// The key-labeled caption drops "TURN" so "[Enter] SHUFFLE ON/OFF" does not run into the other HUD tip. The original
// wording is kept in the plain variant, so only the keyboard label is shortened here.
void RemoveTurnFromShuffleAscii(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src || !mem::IsCommitted(src)) return;
    uint32_t at = FindAsciiText(mem_base, src, "TURN SHUFFLE");
    if (at == kNoAsciiMatch) return;
    uint32_t length = 0;
    while (length < 255 && mem_base[src + length]) ++length;
    if (at + 5 > length) return;
    for (uint32_t i = at + 5; i <= length; ++i) mem_base[src + i - 5] = mem_base[src + i];
}

bool IsMusicPromptWide(uint8_t* mem_base, uint32_t wide) {
    return IsPlayPromptWide(mem_base, wide) || IsShufflePromptWide(mem_base, wide);
}

bool IsMusicPromptAscii(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src || !mem::IsCommitted(src)) return false;
    auto equals = [&](const char* text) {
        for (uint32_t i = 0; text[i]; ++i)
            if (UpperAscii(static_cast<char>(mem_base[src + i])) != UpperAscii(text[i])) return false;
        return mem_base[src + std::strlen(text)] == 0;
    };
    return equals("PLAY") || equals("STOP") || IsShufflePromptAscii(mem_base, src) ||
           AsciiStartsWith(mem_base, src, "TAPE_PLAYER_ADD");
}

const char16_t* PromptPrefixForWide(uint8_t* mem_base, uint32_t wide) {
    if (IsShufflePromptWide(mem_base, wide)) return kShufflePrefix;
    if (IsPlayPromptWide(mem_base, wide)) return kPlayPrefix;
    return IsKnownInteractLabelWide(mem_base, wide) ? kInteractPrefix : nullptr;
}

const char16_t* PromptPrefixForAscii(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src) return nullptr;
    if (IsShufflePromptAscii(mem_base, src)) return kShufflePrefix;
    if (IsMusicPromptAscii(mem_base, src)) return kPlayPrefix;
    return IsKnownInteractLabelAscii(mem_base, src) ? kInteractPrefix : nullptr;
}

// Returns the length of a key label this feature injected, so the pad path can remove it and so a second expansion
// never prefixes the same caption twice. Other bracketed text in the game is left alone.
bool PrefixMatchesWide(uint8_t* mem_base, uint32_t wide, const char16_t* prefix, uint32_t units) {
    for (uint32_t i = 0; i < units; ++i) {
        if (!WideUnitReadable(wide, i) || WideAt(mem_base, wide, i) != prefix[i]) return false;
    }
    return true;
}

uint32_t InjectedPrefixLengthWide(uint8_t* mem_base, uint32_t wide) {
    if (!mem_base || !wide || !mem::IsCommitted(wide)) return 0;
    for (const char16_t* prefix : kGenericInjectedPrefixes) {
        uint32_t units = PrefixUnits(prefix);
        if (PrefixMatchesWide(mem_base, wide, prefix, units)) return units;
    }
    uint32_t shuffle_units = PrefixUnits(kShufflePrefix);
    if (IsShufflePromptWide(mem_base, wide) && PrefixMatchesWide(mem_base, wide, kShufflePrefix, shuffle_units))
        return shuffle_units;
    return 0;
}

uint32_t InjectedPrefixLengthAscii(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src || !mem::IsCommitted(src)) return 0;
    for (const char16_t* prefix : kGenericInjectedPrefixes) {
        uint32_t units = 0;
        while (prefix[units]) {
            if (mem_base[src + units] != static_cast<uint8_t>(prefix[units])) break;
            ++units;
        }
        if (prefix[units] == 0) return units;
    }
    uint32_t shuffle_units = 0;
    while (kShufflePrefix[shuffle_units]) {
        if (mem_base[src + shuffle_units] != static_cast<uint8_t>(kShufflePrefix[shuffle_units])) break;
        ++shuffle_units;
    }
    if (kShufflePrefix[shuffle_units] == 0 && IsShufflePromptAscii(mem_base, src)) return shuffle_units;
    return 0;
}

// A line can already contain a label because the token dispatcher replaced the glyph inline. Do not add a second one
// at the front of the sentence; only plain captions whose glyph artwork was hidden need the prefix.
bool HasInjectedLabelAnywhereWide(uint8_t* mem_base, uint32_t wide, uint32_t capacity) {
    return WideContainsAscii(mem_base, wide, capacity, "[E] ") ||
           WideContainsAscii(mem_base, wide, capacity, "[T] ") ||
           (IsShufflePromptWide(mem_base, wide) && WideContainsAscii(mem_base, wide, capacity, "[Enter] "));
}

bool IsKnownInteractLabelWide(uint8_t* mem_base, uint32_t wide) {
    if (!mem_base || !wide || !mem::IsCommitted(wide)) return false;
    return IsKnownInteractLabel([&](const char* label) { return WideStartsWith(mem_base, wide, label); });
}

bool NeedsInteractPrefix(uint8_t* mem_base, uint32_t wide) {
    if (!mem_base || !wide || !mem::IsCommitted(wide)) return false;
    return InjectedPrefixLengthWide(mem_base, wide) == 0 && PromptPrefixForWide(mem_base, wide) != nullptr;
}

uint32_t PrefixedPromptString(uint8_t* mem_base, uint32_t wide, const char16_t* prefix) {
    uint32_t length = WideLength(mem_base, wide, kKeyPrefixUnits - 16);
    if (!length) return 0;
    uint32_t dst = KeyPrefixSlot();
    if (!dst) return 0;
    char16_t* out = reinterpret_cast<char16_t*>(mem_base + dst);
    uint32_t n = 0;
    while (prefix[n]) {
        out[n] = prefix[n];
        ++n;
    }
    for (uint32_t i = 0; n + i < kKeyPrefixUnits - 1 && i <= length; ++i) {
        uint16_t ch = WideAt(mem_base, wide, i);
        out[n + i] = static_cast<char16_t>(ch);
        if (!ch) break;
    }
    out[kKeyPrefixUnits - 1] = 0;
    return dst;
}

// Narrow prompt strings use their own scratch page. The wide page above stores two bytes per unit; this one stores
// one byte per character and is only used before the guest converts the string to UTF-16.
constexpr uint32_t kAsciiPrefixPage = 0x1000;
constexpr uint32_t kAsciiPrefixUnits = 256;
constexpr uint32_t kAsciiPrefixSlots = 8;
uint32_t g_ascii_prefix_base = 0;
thread_local uint32_t g_ascii_prefix_cursor = 0;

uint32_t AsciiPrefixSlot() {
    if (!g_ascii_prefix_base) {
        uint32_t va = 0, size = kAsciiPrefixPage;
        if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
        g_ascii_prefix_base = va;
    }
    uint32_t slot = g_ascii_prefix_cursor++ & (kAsciiPrefixSlots - 1);
    return g_ascii_prefix_base + slot * kAsciiPrefixUnits;
}

uint32_t PrefixedAsciiString(uint8_t* mem_base, uint32_t src, const char16_t* prefix) {
    uint32_t dst = AsciiPrefixSlot();
    if (!dst) return 0;
    uint8_t* out = mem_base + dst;
    uint32_t n = 0;
    while (prefix[n]) {
        out[n] = static_cast<uint8_t>(prefix[n]);
        ++n;
    }
    for (uint32_t i = 0; n + i < kAsciiPrefixUnits - 1; ++i) {
        uint8_t ch = mem_base[src + i];
        out[n + i] = ch;
        if (!ch) break;
    }
    out[kAsciiPrefixUnits - 1] = 0;
    if (prefix == kShufflePrefix) RemoveTurnFromShuffleAscii(mem_base, dst);
    return dst;
}

// Separate scratch space for the pad path: never write into a cached HUD string, which could still be reused later.
// Wide strings need two bytes per unit, so each slot is sized for the full 256-unit wide buffer.
constexpr uint32_t kStripPage = 0x1000;
constexpr uint32_t kStripUnits = 256;
constexpr uint32_t kStripSlots = 8;
constexpr uint32_t kStripSlotBytes = kStripUnits * sizeof(char16_t);
uint32_t g_strip_base = 0;
thread_local uint32_t g_strip_cursor = 0;

uint32_t StripSlot() {
    if (!g_strip_base) {
        uint32_t va = 0, size = kStripPage;
        if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
        g_strip_base = va;
    }
    uint32_t slot = g_strip_cursor++ & (kStripSlots - 1);
    return g_strip_base + slot * kStripSlotBytes;
}

uint32_t UnprefixedPromptString(uint8_t* mem_base, uint32_t wide, uint32_t units) {
    uint32_t dst = StripSlot();
    if (!dst) return 0;
    char16_t* out = reinterpret_cast<char16_t*>(mem_base + dst);
    for (uint32_t i = 0; i < kStripUnits - 1; ++i) {
        uint16_t ch = WideAt(mem_base, wide, i + units);
        out[i] = static_cast<char16_t>(ch);
        if (!ch) break;
    }
    out[kStripUnits - 1] = 0;
    return dst;
}

uint32_t UnprefixedAsciiString(uint8_t* mem_base, uint32_t src, uint32_t units) {
    uint32_t dst = StripSlot();
    if (!dst) return 0;
    uint8_t* out = mem_base + dst;
    for (uint32_t i = 0; i < kStripUnits - 1; ++i) {
        uint8_t ch = mem_base[src + i + units];
        out[i] = ch;
        if (!ch) break;
    }
    out[kStripUnits - 1] = 0;
    return dst;
}

// Text objects copy the expanded string into their own buffer when the page is built. A device switch therefore has
// to rewrite those buffers; otherwise a label inserted while the keyboard was active survives a pad switch. Both
// variants live in guest scratch memory so the rewriter can copy them from any guest thread.
constexpr uint32_t kTextVariantUnits = 128;
constexpr uint32_t kTextVariantSlots = 256;
constexpr uint32_t kTextVariantSlotBytes = kTextVariantUnits * sizeof(char16_t);
uint32_t g_text_variant_base = 0;
thread_local uint32_t g_text_variant_cursor = 0;
std::mutex g_text_lock;

uint32_t TextVariantSlot() {
    if (!g_text_variant_base) {
        uint32_t va = 0, size = kTextVariantSlots * kTextVariantSlotBytes;
        if (mem::AllocateVirtual(va, size, mem::kMemCommit | mem::kMemReserve, 4) != 0 || va == 0) return 0;
        g_text_variant_base = va;
    }
    uint32_t slot = g_text_variant_cursor++ & (kTextVariantSlots - 1);
    return g_text_variant_base + slot * kTextVariantSlotBytes;
}

uint32_t CopyWideVariant(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src) return 0;
    uint32_t length = WideLength(mem_base, src, kTextVariantUnits);
    if (!length) return 0;
    uint32_t dst = TextVariantSlot();
    if (!dst) return 0;
    for (uint32_t i = 0; i <= length; ++i) WideAt(mem_base, dst, i) = WideAt(mem_base, src, i);
    return dst;
}

uint32_t CopyAsciiWideVariant(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src) return 0;
    uint32_t length = 0;
    for (; length < kTextVariantUnits; ++length) {
        if (!mem::IsCommitted(src + length)) return 0;
        if (!mem_base[src + length]) break;
    }
    if (length == kTextVariantUnits) return 0;
    uint32_t dst = TextVariantSlot();
    if (!dst) return 0;
    for (uint32_t i = 0; i <= length; ++i) {
        uint8_t ch = mem_base[src + i];
        WideAt(mem_base, dst, i) = ch < 128 ? ch : '?';
    }
    return dst;
}

// The key-labeled caption drops "TURN" so "[Enter] SHUFFLE ON/OFF" fits beside the other HUD tip. The plain variant
// keeps the original glyph and wording, so a pad still sees the game's own caption.
void RemoveTurnFromShuffleWide(uint8_t* mem_base, uint32_t wide, uint32_t capacity) {
    if (!IsShufflePromptWide(mem_base, wide)) return;
    uint32_t at = FindWideAscii(mem_base, wide, capacity, "TURN SHUFFLE", true);
    if (at == kNoWideMatch) return;
    uint32_t length = WideLength(mem_base, wide, capacity);
    if (!length || at + 5 > length) return;
    for (uint32_t i = at + 5; i <= length; ++i) WideAt(mem_base, wide, i - 5) = WideAt(mem_base, wide, i);
}

void RestoreTurnForShuffleWide(uint8_t* mem_base, uint32_t wide, uint32_t capacity) {
    if (!IsShufflePromptWide(mem_base, wide)) return;
    uint32_t at = FindWideAscii(mem_base, wide, capacity, "SHUFFLE", true);
    if (at == kNoWideMatch) return;
    constexpr char16_t kTurn[] = u"TURN ";
    bool has_turn = at >= 5;
    if (has_turn) {
        for (uint32_t i = 0; i < 5; ++i) {
            if (WideAt(mem_base, wide, at - 5 + i) != kTurn[i]) {
                has_turn = false;
                break;
            }
        }
    }
    if (has_turn) return;
    uint32_t length = WideLength(mem_base, wide, capacity);
    if (!length || length + 5 >= capacity) return;
    for (uint32_t i = length + 1; i-- > at;) WideAt(mem_base, wide, i + 5) = WideAt(mem_base, wide, i);
    for (uint32_t i = 0; i < 5; ++i) WideAt(mem_base, wide, at + i) = kTurn[i];
}

bool ShuffleOffStateWide(uint8_t* mem_base, uint32_t wide) {
    return FindWideAscii(mem_base, wide, 256, "OFF", true) != kNoWideMatch;
}

uint32_t BuildLabeledWideVariant(uint8_t* mem_base, uint32_t plain, const char16_t* prefix, bool replace_glyph) {
    uint32_t units = PrefixUnits(prefix);
    if (!mem_base || !plain || !units) return 0;
    uint32_t length = WideLength(mem_base, plain, kTextVariantUnits);
    if (!length) return 0;
    uint32_t dst = TextVariantSlot();
    if (!dst) return 0;
    uint32_t n = 0;
    for (; n < units; ++n) WideAt(mem_base, dst, n) = prefix[n];
    uint32_t start = replace_glyph ? 1 : 0;
    for (uint32_t i = 0; i + start <= length && n < kTextVariantUnits - 1; ++i) {
        uint16_t ch = WideAt(mem_base, plain, i + start);
        WideAt(mem_base, dst, n++) = ch;
        if (!ch) break;
    }
    WideAt(mem_base, dst, kTextVariantUnits - 1) = 0;
    if (prefix == kShufflePrefix) RemoveTurnFromShuffleWide(mem_base, dst, kTextVariantUnits);
    return dst;
}

uint32_t UnprefixedWideVariant(uint8_t* mem_base, uint32_t labeled, uint32_t units) {
    if (!mem_base || !labeled || !units) return 0;
    uint32_t length = WideLength(mem_base, labeled, kTextVariantUnits);
    if (!length) return 0;
    uint32_t dst = TextVariantSlot();
    if (!dst) return 0;
    for (uint32_t i = 0; i < kTextVariantUnits && i + units <= length; ++i) {
        WideAt(mem_base, dst, i) = WideAt(mem_base, labeled, i + units);
        if (!WideAt(mem_base, dst, i)) break;
    }
    return dst;
}

// sub_0024D900's guest result is the original glyph string. Keep it beside the label we return so the text-object
// hook can restore it verbatim when a pad takes over.
struct LabelPlain {
    uint32_t label;
    uint32_t plain;
    char key[64];  // the label text, so a copy in the caller's buffer is still recognized
};
LabelPlain g_label_plain[256] = {};
uint32_t g_label_plain_cursor = 0;

void RememberLabelPlain(uint32_t label, uint32_t plain) {
    uint8_t* mem_base = mem::Base();
    if (!label || !plain || !mem_base) return;
    LabelPlain entry = {};
    entry.label = label;
    entry.plain = plain;
    uint32_t length = WideLength(mem_base, label, 64);
    for (uint32_t i = 0; i < length && i + 1 < sizeof(entry.key); ++i) {
        uint16_t ch = WideAt(mem_base, label, i);
        entry.key[i] = ch < 128 ? static_cast<char>(ch) : '?';
    }
    std::lock_guard<std::mutex> lock(g_text_lock);
    uint32_t slot = g_label_plain_cursor++ % 256;
    g_label_plain[slot] = entry;
}

bool WideMatchesAscii(uint8_t* mem_base, uint32_t wide, uint32_t offset, const char* ascii) {
    if (!mem_base || !wide || !ascii) return false;
    for (uint32_t i = 0; ascii[i]; ++i) {
        uint32_t va = wide + (offset + i) * sizeof(char16_t);
        if (!mem::IsCommitted(va) || !mem::IsCommitted(va + 1)) return false;
        if (!WideCharEquals(WideAt(mem_base, wide, offset + i), ascii[i], true)) return false;
    }
    return true;
}

bool WideRangesEqual(uint8_t* mem_base, uint32_t a, uint32_t aOffset, uint32_t b, uint32_t bOffset, uint32_t units) {
    for (uint32_t i = 0; i < units; ++i) {
        uint32_t va = a + (aOffset + i) * sizeof(char16_t);
        uint32_t vb = b + (bOffset + i) * sizeof(char16_t);
        if (!mem::IsCommitted(va) || !mem::IsCommitted(va + 1) || !mem::IsCommitted(vb) || !mem::IsCommitted(vb + 1))
            return false;
        if (WideAt(mem_base, a, aOffset + i) != WideAt(mem_base, b, bOffset + i)) return false;
    }
    return true;
}

// Rebuild the original caption from a label the dispatcher produced. The label may be the whole string ("[E] Exit")
// or just a key in front of localized words ("[Enter] Start"); the plain token result has the same layout.
uint32_t PlainVariantForIncoming(uint8_t* mem_base, uint32_t incoming) {
    uint8_t* base = mem::Base();
    if (!base || !incoming || !WideLength(base, incoming, kTextVariantUnits)) return 0;
    std::lock_guard<std::mutex> lock(g_text_lock);
    for (const LabelPlain& entry : g_label_plain) {
        if (!entry.plain || !WideLength(base, entry.plain, kTextVariantUnits)) continue;
        if (entry.label == incoming) return CopyWideVariant(base, entry.plain);
        if (!entry.key[0] || !WideMatchesAscii(base, incoming, 0, entry.key)) continue;

        // A shortened shuffle label cannot be rebuilt by copying the words after the prefix: "TURN" is deliberately
        // missing there. Return the remembered original caption instead, matching ON/OFF so the state stays correct.
        if (IsShufflePromptWide(base, incoming) && IsShufflePromptWide(base, entry.plain) &&
            ShuffleOffStateWide(base, incoming) == ShuffleOffStateWide(base, entry.plain)) {
            return CopyWideVariant(base, entry.plain);
        }

        uint32_t label_units = static_cast<uint32_t>(std::strlen(entry.key));
        uint32_t incoming_length = WideLength(base, incoming, kTextVariantUnits);
        uint32_t plain_length = WideLength(base, entry.plain, kTextVariantUnits);
        uint32_t rest = incoming_length > label_units ? incoming_length - label_units : 0;
        uint32_t glyph = plain_length && WideAt(base, entry.plain, 0) > 127 ? 1 : 0;
        uint32_t plain_words = plain_length > glyph ? plain_length - glyph : 0;

        // A full caption label ("[E] Exit") has no words left after it; a key-only label ("[E]") leaves the localized
        // wording in the caller's buffer and it has to be appended to the token's glyph.
        if (rest == 0 || (rest == plain_words &&
                          WideRangesEqual(base, incoming, label_units, entry.plain, glyph, rest))) {
            return CopyWideVariant(base, entry.plain);
        }

        uint32_t dst = TextVariantSlot();
        if (!dst) return 0;
        uint32_t n = 0;
        if (glyph) WideAt(base, dst, n++) = WideAt(base, entry.plain, 0);
        for (uint32_t i = 0; i < rest && n < kTextVariantUnits - 1; ++i)
            WideAt(base, dst, n++) = WideAt(base, incoming, label_units + i);
        WideAt(base, dst, n) = 0;
        return dst;
    }
    return 0;
}

struct ManagedText {
    uint32_t object;
    uint32_t buffer;
    uint32_t plain;
    uint32_t labeled;
};
ManagedText g_managed_text[256] = {};
uint32_t g_managed_cursor = 0;

void RegisterManagedText(uint32_t object, uint32_t buffer, uint32_t plain, uint32_t labeled) {
    if (!object || !buffer || !plain || !labeled) return;
    std::lock_guard<std::mutex> lock(g_text_lock);
    for (ManagedText& entry : g_managed_text) {
        if (entry.object == object && entry.buffer == buffer) {
            entry.plain = plain;
            entry.labeled = labeled;
            return;
        }
    }
    uint32_t slot = g_managed_cursor++ % 256;
    g_managed_text[slot] = {object, buffer, plain, labeled};
}

}  // namespace

void hud_prompts::RefreshKeyLabels() { RefreshKeyLabelsImpl(); }

void ApplyHudDeviceText(bool key_prompts) {
    uint8_t* mem_base = mem::Base();
    if (!mem_base) return;
    std::lock_guard<std::mutex> lock(g_text_lock);
    for (ManagedText& entry : g_managed_text) {
        if (!entry.buffer) continue;
        if (!mem::IsCommitted(entry.object) || !mem::IsCommitted(entry.buffer) ||
            *reinterpret_cast<uint32_t*>(mem_base + entry.object) != 0x3D4F7Cu) {
            entry = {};
            continue;
        }
        uint32_t src = key_prompts ? entry.labeled : entry.plain;
        uint32_t length = WideLength(mem_base, src, kTextVariantUnits);
        if (!length) {
            entry = {};
            continue;
        }
        for (uint32_t i = 0; i <= length; ++i) WideAt(mem_base, entry.buffer, i) = WideAt(mem_base, src, i);
    }
}

namespace {

// The expander's caller-owned buffer is reused for each string on a page. Keep the source caption beside the one we
// rewrote so the constructor immediately after the expander can store both variants.
thread_local uint32_t g_expanded_wide = 0;
thread_local uint32_t g_expanded_plain = 0;

// The complete source line can name a triangle token even when the caption itself is plain localized text. Prefixing
// the expanded output at this point reaches every prompt, including the ones that do not go through a text-object
// constructor with a recognizable caption.
bool NeedsExpandedPrefix(uint8_t* mem_base, uint32_t src, uint32_t wide, uint32_t capacity) {
    if (!mem_base || !src || !wide || capacity < 8) return false;
    uint32_t length = WideLength(mem_base, wide, capacity);
    if (!length) return false;
    if (InjectedPrefixLengthWide(mem_base, wide) || HasInjectedLabelAnywhereWide(mem_base, wide, capacity)) return false;
    const char16_t* prefix = PromptPrefixForWide(mem_base, wide);
    bool triangle_token = SourceContains(mem_base, src, "BUTTON_GRAPHIC_TRIANGLE", true);
    if (!prefix && !triangle_token) return false;
    if (!prefix) prefix = kInteractPrefix;
    return length + PrefixUnits(prefix) + 1 <= capacity;
}

void PrefixExpandedString(uint8_t* mem_base, uint32_t wide, uint32_t capacity, const char16_t* prefix) {
    uint32_t length = WideLength(mem_base, wide, capacity);
    uint32_t units = PrefixUnits(prefix);
    if (!length || length + units + 1 > capacity) return;
    for (uint32_t i = length + 1; i-- > 0;) WideAt(mem_base, wide, i + units) = WideAt(mem_base, wide, i);
    for (uint32_t i = 0; i < units; ++i) WideAt(mem_base, wide, i) = prefix[i];
}

// The tape player's glyph is a single UTF-16 unit before the caption. Replace it with the key label rather than
// stacking a label in front of the pad button, so keyboard mode shows only the PC key.
void ReplaceMusicGlyph(uint8_t* mem_base, uint32_t wide, uint32_t capacity, const char16_t* prefix) {
    if (!IsGlyphMusicPrompt(mem_base, wide) || !prefix || capacity < 8) return;
    uint32_t units = PrefixUnits(prefix);
    uint32_t rest = WideLength(mem_base, wide + sizeof(char16_t), capacity - 2);
    if (!rest || capacity < units + rest + 1) return;
    for (uint32_t i = rest + 1; i-- > 0;)
        WideAt(mem_base, wide, units + i) = WideAt(mem_base, wide, 1 + i);
    for (uint32_t i = 0; i < units; ++i) WideAt(mem_base, wide, i) = prefix[i];
    if (IsShufflePromptWide(mem_base, wide)) RemoveTurnFromShuffleWide(mem_base, wide, capacity);
}

// Inverse of the above for a cached keyboard string: strip the label first, then put the original button glyph back.
void RestoreMusicGlyph(uint8_t* mem_base, uint32_t wide, uint32_t capacity) {
    if (capacity < 8 || WideAt(mem_base, wide, 0) == kPlayGlyph || WideAt(mem_base, wide, 0) == kShuffleGlyph) return;
    bool shuffle = IsShufflePromptWide(mem_base, wide);
    if (!shuffle && !IsPlayPromptWide(mem_base, wide)) return;
    uint32_t length = WideLength(mem_base, wide, capacity);
    if (!length || length + 2 > capacity) return;
    for (uint32_t i = length + 1; i-- > 0;) WideAt(mem_base, wide, i + 1) = WideAt(mem_base, wide, i);
    WideAt(mem_base, wide, 0) = shuffle ? kShuffleGlyph : kPlayGlyph;
}

void UnprefixExpandedString(uint8_t* mem_base, uint32_t wide, uint32_t capacity) {
    uint32_t units = InjectedPrefixLengthWide(mem_base, wide);
    if (!units || !capacity) return;
    uint32_t length = WideLength(mem_base, wide, capacity);
    if (length + 1 <= units) return;
    // Any prefix we injected is removed, even when the caption is no longer in the known list. That is what keeps a
    // gamepad switch from leaving a keyboard label behind on a cached prompt.
    for (uint32_t i = 0; i + units <= length; ++i)
        WideAt(mem_base, wide, i) = WideAt(mem_base, wide, i + units);
}

bool AsciiStartsWith(uint8_t* mem_base, uint32_t src, const char* ascii) {
    if (!mem_base || !src || !ascii) return false;
    for (uint32_t i = 0; ascii[i]; ++i) {
        if (UpperAscii(static_cast<char>(mem_base[src + i])) != UpperAscii(ascii[i])) return false;
    }
    return true;
}

// The stream reader fills the HUD page name into a stack buffer before the caller NUL-terminates it, so compare the
// exact byte count instead of using the NUL-terminated helpers.
bool AsciiRangeContains(uint8_t* mem_base, uint32_t src, uint32_t count, const char* text) {
    if (!mem_base || !src || !text || !count || !mem::IsCommitted(src)) return false;
    uint32_t length = 0;
    while (text[length]) ++length;
    if (length > count) return false;
    for (uint32_t i = 0; i + length <= count; ++i) {
        uint32_t j = 0;
        while (j < length && UpperAscii(static_cast<char>(mem_base[src + i + j])) == UpperAscii(text[j])) ++j;
        if (j == length) return true;
    }
    return false;
}

// Both HUD page loaders read a length byte and then that many name bytes. These are the return addresses of the name
// reads, which distinguishes them from the many primitive reads that use the same stream helper.
bool IsHudPageNameRead(uint32_t return_address) {
    return return_address == 0x24C26Fu || return_address == 0x24C9D1u;
}

bool IsKnownInteractLabelAscii(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src || !mem::IsCommitted(src)) return false;
    return IsKnownInteractLabel([&](const char* label) { return AsciiStartsWith(mem_base, src, label); });
}

bool NeedsAsciiInteractPrefix(uint8_t* mem_base, uint32_t src) {
    if (!mem_base || !src || !mem::IsCommitted(src)) return false;
    return InjectedPrefixLengthAscii(mem_base, src) == 0 && PromptPrefixForAscii(mem_base, src) != nullptr;
}

}  // namespace

// Localized strings are expanded in two passes. The first pass resolves a string ID such as [LOOK_TUTORIAL_X] to a
// UTF-16 sentence; this helper extracts each nested [TOKEN] from that sentence and calls the dispatcher directly.
// Keeping the sentence available here is what lets sprint/start/pickup lines choose the right key label.
X86_FUNC(sub_0024D9B0) {
    RunDiagnosticScriptCommand(ctx, base);

    uint8_t* mem_base = mem::Base();
    uint32_t previous_line = g_source_line;
    uint32_t previous_depth = g_source_depth;
    if (UseKeyPrompts() && mem_base) {
        uint32_t wide = X86_MEM32(ctx.esp + 4);
        uint32_t narrow = CopyWideSource(mem_base, wide);
        if (narrow) g_source_line = narrow;
        g_source_depth = (g_source_depth + 1) & 7;
    }
    __imp__sub_0024D9B0(ctx, base);
    g_source_line = previous_line;
    g_source_depth = previous_depth;
}

// The playlist setup starts a new boot; allow the next playlist to be advanced again.
X86_FUNC(sub_00028940) {
    g_logo_skipped = false;
    __imp__sub_00028940(ctx, base);
}

X86_FUNC(sub_00028B10) {
    uint32_t mgr = ctx.ecx;
    if (app::Get().skip_intro && !g_logo_skipped && IsLogoManager(base, mgr)) {
        g_logo_skipped = true;
        uint32_t phase = X86_MEM32(mgr + 0x78);
        LOGI("skip_intro: playlist forced to completion from phase %u index %u", phase, X86_MEM32(mgr + 0x1C));
        if (phase >= 1 && phase <= 4) {
            // A movie is already playing: release the same objects the normal completion releases.
            ReleaseObject(ctx, base, X86_MEM32(mgr + 0x74));
            ReleaseObject(ctx, base, X86_MEM32(mgr + 0x10));
            ReleaseObject(ctx, base, X86_MEM32(mgr + 0x70));
            X86_MEM32(mgr + 0x74) = 0;
            X86_MEM32(mgr + 0x10) = 0;
            X86_MEM32(mgr + 0x70) = 0;
            X86_MEM8(mgr + 0x68) = 0;
        }
        ctx.ecx = mgr;
        X86_MEM32(mgr + 0x78) = 5;
    }
    __imp__sub_00028B10(ctx, base);
}

// sub_001CC7C0 converts the result of "PauseIsRewardLocked( %i );" to a bool here. Forcing just that one result to
// false unlocks every pause-menu reward without inventing a script string or touching the shared evaluator's return
// value. Selecting a code still runs its original effect.
constexpr uint32_t kPauseIsRewardLockedBoolReturn = 0x1CC812u;

X86_FUNC(sub_00099BC0) {
    uint32_t return_address = X86_MEM32(ctx.esp);
    __imp__sub_00099BC0(ctx, base);
    if (app::Get().all_cheats_unlocked && return_address == kPauseIsRewardLockedBoolReturn) ctx.eax = 0;
}

// The prompt sprite manager selects one controller artwork at a time. Indices 2, 3 and 4 are the R, R1 and L1
// shoulder-button drawings; 0 and 1 are the left/right curved navigation arrows and must stay visible. Let the
// original select, cache and show the sprite so every hide/show handshake still runs, then hide only the shoulder
// drawing when a key label is doing its job. The manager's timer skips a sprite whose active index is -1, so
// clear that as well or it would show the artwork again on the next update. None of this runs for a real pad,
// which keeps the original controller artwork intact.
X86_FUNC(sub_00194A30) {
    uint32_t manager = ctx.ecx;
    uint32_t index = X86_MEM32(ctx.esp + 4);  // first of two stack arguments; the guest returns with ret 0x08
    uint8_t* mem_base = mem::Base();

    __imp__sub_00194A30(ctx, base);

    if (!mem_base || !UseKeyPrompts() || index < 2 || index > 4) return;

    uint32_t sprite = X86_MEM32(manager + 0x68);
    if (!sprite) return;
    uint32_t vtable = X86_MEM32(sprite);
    uint32_t hide = vtable ? X86_MEM32(vtable + 0x44) : 0;
    if (hide) {
        uint32_t saved_eax = ctx.eax;
        X86_PUSH32(0);          // 0 = hide
        X86_PUSH32(0x194A30u);  // guest return address; +0x44 is ret 0x04
        ctx.ecx = sprite;
        x86_icall(ctx, base, hide);
        ctx.eax = saved_eax;
    }
    X86_MEM32(manager + 0x7C) = 0xFFFFFFFFu;
    X86_MEM32(manager + 0x78) = 0;
    X86_MEM32(manager + 0x74) = 0;
    ctx.ecx = manager;
}

// Replace pad button tokens with keyboard/mouse key labels only while the keyboard or mouse is the active device.
// The HUD re-expands text on each draw, so switching to a controller restores the original glyphs immediately.
X86_FUNC(sub_0024D900) {
    uint32_t token = X86_MEM32(ctx.esp + 4);
    uint8_t* mem_base = mem::Base();

    // Resolve the original caption first. It is the glyph string we need to put back when a pad takes over; our label
    // then replaces it only while key prompts are active.
    __imp__sub_0024D900(ctx, base);
    uint32_t plain = ctx.eax;

    if (UseKeyPrompts() && token && mem_base && plain) {
        KeyPrompt* forced = OverrideForToken(mem_base, g_source_line, token);
        if (!forced && !g_source_line) forced = OverrideForBareToken(mem_base, token);
        if (forced && EnsureLabel(mem_base, *forced, 0)) {
            RememberLabelPlain(forced->va[0], plain);
            ctx.eax = forced->va[0];
            return;  // the original call already consumed the guest return address
        }
        uint32_t context = static_cast<uint32_t>(kbm::PromptContextNow());
        for (KeyPrompt& p : kKeyPrompts) {
            if (TokenEquals(mem_base, token, p.token) && EnsureLabel(mem_base, p, context)) {
                RememberLabelPlain(p.va[context], plain);
                ctx.eax = p.va[context];
                return;
            }
        }
    }
}

// Remember the line being expanded so the token dispatcher can choose a label from the words around the token.
// The expanded wide string is then rewritten in the caller's buffer before any text object copies it: some prompts
// never reach the constructors below, and prefixing only there left triangle captions without a key label.
// On the pad path the same pass removes any injected label so a device switch restores only the original glyph.
X86_FUNC(sub_0024DF70) {
    uint8_t* mem_base = mem::Base();
    uint32_t src = mem_base ? X86_MEM32(ctx.esp + 4) : 0;
    uint32_t wide = mem_base ? X86_MEM32(ctx.esp + 8) : 0;
    uint32_t capacity = mem_base ? X86_MEM32(ctx.esp + 12) : 0;
    bool key_prompts = UseKeyPrompts();
    uint32_t previous = g_source_line;
    g_source_line = key_prompts ? src : 0;
    __imp__sub_0024DF70(ctx, base);
    g_source_line = previous;

    g_expanded_wide = 0;
    g_expanded_plain = 0;
    if (!mem_base || !wide || !capacity) return;
    if (key_prompts) {
        if (NeedsExpandedPrefix(mem_base, src, wide, capacity)) {
            g_expanded_plain = CopyWideVariant(mem_base, wide);
            g_expanded_wide = wide;
            const char16_t* prefix = PromptPrefixForWide(mem_base, wide);
            if (!prefix) prefix = kInteractPrefix;
            if (IsGlyphMusicPrompt(mem_base, wide)) ReplaceMusicGlyph(mem_base, wide, capacity, prefix);
            else PrefixExpandedString(mem_base, wide, capacity, prefix);
        }
    } else {
        uint32_t units = InjectedPrefixLengthWide(mem_base, wide);
        UnprefixExpandedString(mem_base, wide, capacity);
        if (units && (IsPlayPromptWide(mem_base, wide) || IsShufflePromptWide(mem_base, wide)))
            RestoreMusicGlyph(mem_base, wide, capacity);
        if (IsShufflePromptWide(mem_base, wide)) RestoreTurnForShuffleWide(mem_base, wide, capacity);
    }
}

// The expanded HUD string is handed to this text-object constructor, which copies it into the object's own buffer.
// Replacing the incoming pointer here keeps the displayed wording intact and adds the key label using scratch memory;
// the expander's caller-owned buffer is never modified. A pad gets the original string, or a scratch copy with a stale
// label removed, so the original triangle artwork is the only button shown after a device switch.
X86_FUNC(sub_002492A0) {
    uint8_t* mem_base = mem::Base();
    uint32_t wide = mem_base ? X86_MEM32(ctx.esp + 4) : 0;
    bool key_prompts = UseKeyPrompts();
    uint32_t plain = 0;
    uint32_t labeled = 0;

    if (mem_base && wide) {
        plain = PlainVariantForIncoming(mem_base, wide);
        if (plain) {
            labeled = wide;
        } else if (wide == g_expanded_wide && g_expanded_plain) {
            plain = g_expanded_plain;
            labeled = wide;
            g_expanded_wide = g_expanded_plain = 0;
        } else {
            uint32_t units = InjectedPrefixLengthWide(mem_base, wide);
            if (units) {
                labeled = wide;
                plain = UnprefixedWideVariant(mem_base, wide, units);
                if (IsShufflePromptWide(mem_base, plain))
                    RestoreTurnForShuffleWide(mem_base, plain, kTextVariantUnits);
            } else if (const char16_t* prefix = PromptPrefixForWide(mem_base, wide)) {
                plain = CopyWideVariant(mem_base, wide);
                labeled = BuildLabeledWideVariant(mem_base, wide, prefix, IsGlyphMusicPrompt(mem_base, wide));
            }
        }
        // Never let a stale or unterminated scratch string reach the guest's unbounded wcslen. A rejected pair simply
        // leaves the original pointer in place, which is always the caller-owned buffer from the expander.
        if (plain && !SafeWideString(mem_base, plain)) plain = 0;
        if (labeled && !SafeWideString(mem_base, labeled)) labeled = 0;
        if (!plain || !labeled) {
            g_expanded_wide = g_expanded_plain = 0;
        } else if (key_prompts && IsShufflePromptWide(mem_base, labeled)) {
            uint32_t shortened = CopyWideVariant(mem_base, labeled);
            if (shortened) {
                RemoveTurnFromShuffleWide(mem_base, shortened, kTextVariantUnits);
                labeled = shortened;
            }
        }
        // Always give the guest the longer labeled string. Its buffer must be large enough for the key prompt even if
        // this object is being built while a pad is active; the pad variant is written in afterwards.
        if (plain && labeled) X86_MEM32(ctx.esp + 4) = labeled;
    }

    __imp__sub_002492A0(ctx, base);

    if (mem_base && plain && labeled) {
        uint32_t object = ctx.eax;
        if (mem::IsCommitted(object) && X86_MEM32(object) == 0x3D4F7Cu) {
            uint32_t buffer = X86_MEM32(object + 4);
            if (buffer && mem::IsCommitted(buffer)) {
                RegisterManagedText(object, buffer, plain, labeled);
                if (!key_prompts) {
                    uint32_t length = WideLength(mem_base, plain, kTextVariantUnits);
                    for (uint32_t i = 0; i <= length; ++i) WideAt(mem_base, buffer, i) = WideAt(mem_base, plain, i);
                }
            }
        }
    }
}

// Narrow prompt strings take this constructor instead of the wide one. The prefix has to be added before the
// narrow-to-wide copy so the rendered words keep their original spelling; the pad path removes it again.
X86_FUNC(sub_00249300) {
    uint8_t* mem_base = mem::Base();
    uint32_t src = mem_base ? X86_MEM32(ctx.esp + 4) : 0;
    bool key_prompts = UseKeyPrompts();
    uint32_t plain = 0;
    uint32_t labeled = 0;

    if (mem_base && src) {
        uint32_t units = InjectedPrefixLengthAscii(mem_base, src);
        if (units) {
            labeled = CopyAsciiWideVariant(mem_base, src);
            plain = UnprefixedWideVariant(mem_base, labeled, units);
            if (IsShufflePromptWide(mem_base, plain)) RestoreTurnForShuffleWide(mem_base, plain, kTextVariantUnits);
            if (IsShufflePromptWide(mem_base, labeled)) RemoveTurnFromShuffleWide(mem_base, labeled, kTextVariantUnits);
        } else if (const char16_t* prefix = PromptPrefixForAscii(mem_base, src)) {
            plain = CopyAsciiWideVariant(mem_base, src);
            labeled = BuildLabeledWideVariant(mem_base, plain, prefix, false);
            uint32_t ascii_labeled = PrefixedAsciiString(mem_base, src, prefix);
            if (ascii_labeled) X86_MEM32(ctx.esp + 4) = ascii_labeled;
        }
    }

    __imp__sub_00249300(ctx, base);

    if (mem_base && plain && labeled) {
        uint32_t object = ctx.eax;
        if (mem::IsCommitted(object) && X86_MEM32(object) == 0x3D4F7Cu) {
            uint32_t buffer = X86_MEM32(object + 4);
            if (buffer && mem::IsCommitted(buffer)) {
                RegisterManagedText(object, buffer, plain, labeled);
                if (!key_prompts) {
                    uint32_t length = WideLength(mem_base, plain, kTextVariantUnits);
                    for (uint32_t i = 0; i <= length; ++i) WideAt(mem_base, buffer, i) = WideAt(mem_base, plain, i);
                }
            }
        }
    }
}

// The music library keeps the prompt style it had on entry. Its captions are cached in text objects and toggling
// Shuffle rebuilds them, so following device changes while the page is open produced mixed labels and stale glyphs.
// Snapshotting the page name as the HUD loader reads it happens before those text objects are constructed.
X86_FUNC(sub_00240570) {
    uint8_t* mem_base = mem::Base();
    uint32_t return_address = mem_base ? X86_MEM32(ctx.esp) : 0;
    bool page_name = mem_base && IsHudPageNameRead(return_address);
    uint32_t destination = page_name ? X86_MEM32(ctx.esp + 4) : 0;
    uint32_t count = page_name ? X86_MEM32(ctx.esp + 8) : 0;

    __imp__sub_00240570(ctx, base);

    if (!page_name || !destination || !count || count > 128 || !mem::IsCommitted(destination)) return;
    bool music = AsciiRangeContains(mem_base, destination, count, "TapePlayer") ||
                 AsciiRangeContains(mem_base, destination, count, "Tape_Player") ||
                 AsciiRangeContains(mem_base, destination, count, "Tape Player");
    bool key_labels = kbm::enabled && kbm::hud_prompts && kbm::LastDevice() != kbm::Device::Pad;
    if (music != hud_prompts::Frozen())
        LOGI("hud: music library %s with %s prompts", music ? "entered" : "left",
             key_labels ? "keyboard" : "pad");
    hud_prompts::SetMusicLibraryPage(music, key_labels);
}
