#pragma once
#include <cstdint>
#include <string>

// Diagnostic: the state writes the title made, kept so a capture can show why a draw saw what it saw.
// The ring holds the writes of one captured frame, the capture dump writes it beside frame.txt.
namespace gpu {

enum : uint32_t {
    kWriteTextureStage = 0,  // index = stage * 32 + type
    kWriteTextureFactor = 1,
    kWriteTexture = 2,       // index = stage, from and to are resource headers
    kWritePixelShader = 3,
    kWriteDraw = 4,          // index = primitive, from = stage 0 packed, to = texture factor
    kWriteMirror = 5,        // the title wrote its deferred table itself, seen at a draw
};

// Records one write. pc is the title's return address, 0 when it is not known.
void RecordWrite(uint32_t kind, uint32_t index, uint32_t from, uint32_t to, uint32_t pc);
void ClearWrites();
// Dumps the ring to the file at path, with the legend of what the entries mean.
void DumpWrites(const std::wstring& path);

}  // namespace gpu
