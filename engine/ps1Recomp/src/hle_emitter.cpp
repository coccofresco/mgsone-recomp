#include "ps1recomp/hle_emitter.h"

#include <fmt/format.h>

namespace ps1recomp {

std::string emitHleForwardDecl(const HleStub & /*stub*/) {
  // The preamble carries a single forward-decl for the runtime dispatcher
  // (registered names are looked up at runtime via psyq_registry).
  return "void psyq_dispatch(const char* name, recomp_context* ctx);\n";
}

std::string emitHleStub(const HleStub &stub) {
  return fmt::format(
      "// HLE stub for 0x{:08X} -> psyq_dispatch(\"{}\")\n"
      "void {}(uint8_t* rdram, recomp_context* ctx) {{\n"
      "    psyq_dispatch(\"{}\", ctx);\n"
      "}}\n",
      stub.address, stub.hleName, stub.funcName, stub.hleName);
}

} // namespace ps1recomp
