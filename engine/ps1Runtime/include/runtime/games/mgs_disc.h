#pragma once
#include <cstdint>
#include <istream>
struct recomp_context;
namespace ps1::cdrom { class VirtualFs; class CdromController; }
namespace ps1::games::mgs {
// These bindings are for fingerprinted USA v1.0 change.bin only.
inline constexpr uint32_t UsaFsDiskNum = 0x800AF368;
void configureDiscChange(cdrom::VirtualFs *fs, cdrom::CdromController *drive);
bool restoreLegacyDisc(std::istream &state, uint32_t version,
                       cdrom::CdromController &drive);
}
bool mgsTryDiscChange(recomp_context *ctx, uint32_t entry);
