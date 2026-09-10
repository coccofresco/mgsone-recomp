// Tests for ps1Recomp -- HLE Stub Emitter
//
// Validates the stub C++ produced for `[[hle_functions]]` entries from
// ps1Analyzer Sessao 0.4 configs. The recompiler must, for every entry
// with `hle = true`, skip MIPS translation and emit a stub that delegates
// to `psyq_dispatch("<name>", ctx)` -- resolved at runtime by the PsyQ
// registry in ps1Runtime.

#include <algorithm>
#include <gtest/gtest.h>
#include <ps1recomp/hle_emitter.h>
#include <string>

using ps1recomp::HleStub;
using ps1recomp::emitHleForwardDecl;
using ps1recomp::emitHleStub;

// Forward declaration emission

TEST(HleEmitter, ForwardDeclIsRegistryDispatcher) {
  // The preamble carries one universal forward-decl for the runtime
  // dispatcher; per-name lookup happens in the registry.
  HleStub stub{0x801ABCDE, "func_801ABCDE", "libapi_VSync"};
  std::string decl = emitHleForwardDecl(stub);

  EXPECT_EQ(decl, "void psyq_dispatch(const char* name, recomp_context* ctx);\n");
}

TEST(HleEmitter, ForwardDeclIsStubIndependent) {
  // The forward-decl is emitted once regardless of how many HLE entries
  // exist -- different stubs must yield identical strings.
  HleStub a{0x80012340, "func_80012340", "libgpu_PutDispEnv"};
  HleStub b{0x80020000, "func_80020000", "libcd_CdInit"};

  EXPECT_EQ(emitHleForwardDecl(a), emitHleForwardDecl(b));
}

// Stub body emission

TEST(HleEmitter, StubBodyLibapiVSync) {
  HleStub stub{0x801ABCDE, "func_801ABCDE", "libapi_VSync"};
  std::string body = emitHleStub(stub);

  // The stub must:
  //   1. Carry a comment with the original PS1 address (for grep-ability)
  //   2. Have the C++ signature expected by the dispatch table
  //   3. Delegate to psyq_dispatch("<name>", ctx) -- *not* translate MIPS
  EXPECT_NE(body.find("0x801ABCDE"), std::string::npos);
  EXPECT_NE(body.find("void func_801ABCDE(uint8_t* rdram, recomp_context* ctx)"),
            std::string::npos);
  EXPECT_NE(body.find("psyq_dispatch(\"libapi_VSync\", ctx);"),
            std::string::npos);
}

TEST(HleEmitter, StubBodyDoesNotTranslateMips) {
  // Sanity: there should be no MIPS-shaped emission inside the stub
  // (no register references, no memory reads, no branches).
  HleStub stub{0x801ABCDE, "func_801ABCDE", "libapi_VSync"};
  std::string body = emitHleStub(stub);

  EXPECT_EQ(body.find("ctx->r["), std::string::npos);
  EXPECT_EQ(body.find("ctx->mem"), std::string::npos);
  EXPECT_EQ(body.find("goto L_"), std::string::npos);
}

TEST(HleEmitter, StubDoesNotCallLegacyHleSymbol) {
  // The 0.5 path called `hle_<name>(ctx)` directly via extern "C". The 0.6
  // path replaces that with the runtime dispatcher -- make sure the old form
  // is gone so missing impls don't fail at link time.
  HleStub stub{0x801ABCDE, "func_801ABCDE", "libapi_VSync"};
  std::string body = emitHleStub(stub);

  EXPECT_EQ(body.find("hle_libapi_VSync(ctx);"), std::string::npos);
  EXPECT_EQ(body.find("extern \"C\""), std::string::npos);
}

TEST(HleEmitter, StubFuncNameIsRespected) {
  // The recompiler may name the function from the analyzer's [[functions]]
  // entry (which is usually func_<addr>, but can be a symbol name like
  // "main"). The emitted body must use whatever funcName is provided.
  HleStub stub{0x80020000, "custom_name", "libgpu_DrawOTag"};
  std::string body = emitHleStub(stub);

  EXPECT_NE(body.find("void custom_name(uint8_t* rdram, recomp_context* ctx)"),
            std::string::npos);
  EXPECT_NE(body.find("psyq_dispatch(\"libgpu_DrawOTag\", ctx);"),
            std::string::npos);
  EXPECT_EQ(body.find("func_80020000"), std::string::npos);
}

TEST(HleEmitter, BodyIsValidCppFunctionShape) {
  // Smoke check: braces balance and the body ends with a newline so multiple
  // emissions concatenate cleanly into recompiled_out.cpp.
  HleStub stub{0x80030000, "func_80030000", "libcd_CdInit"};
  std::string body = emitHleStub(stub);

  size_t openBraces = std::count(body.begin(), body.end(), '{');
  size_t closeBraces = std::count(body.begin(), body.end(), '}');
  EXPECT_EQ(openBraces, closeBraces);
  ASSERT_FALSE(body.empty());
  EXPECT_EQ(body.back(), '\n');
}
