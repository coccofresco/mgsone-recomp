#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <ps1recomp/instruction_emitter.h>
#include <string>

using namespace ps1recomp;

TEST(FunctionResolver, ResolvesNamesCorrectly) {
  std::map<uint32_t, std::string> addr_to_name = {{0x80010000, "__start"},
                                                  {0x80010040, "GameLoop"}};

  InstructionEmitter emitter;
  emitter.setFuncResolver([&addr_to_name](uint32_t addr) -> std::string {
    if (addr_to_name.count(addr))
      return addr_to_name[addr];
    return "";
  });

  RecompFunction func;
  func.name = "TestDispatch";
  func.address = 0x80010A00;
  func.size = 12;
  uint32_t jal_inst = (3u << 26) | ((0x80010040 & 0x0FFFFFFF) >> 2);
  func.instructions = {jal_inst};

  std::string resulting_code = emitter.emitFunction(func);
  EXPECT_NE(resulting_code.find("GameLoop"),
            std::string::npos); // Should resolve it
}
