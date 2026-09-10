// Opt-in, game-independent point-in-instruction snapshot for a donor PC hook.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace donor_snapshot {
inline void capture(uint32_t frame,uint32_t pc,const uint32_t* regs,
                    const uint8_t* ram,size_t ramSize,const uint8_t* scratch,size_t scratchSize) {
  static const char* prefix=std::getenv("PS1_DONOR_PC_SNAPSHOT_PREFIX");
  static bool done=false;
  if (!prefix || !*prefix || done) return;
  static const auto match=[] {
    std::vector<uint32_t> v;
    const char* p=std::getenv("PS1_DONOR_PC_SNAPSHOT_MATCH");
    while(p && *p) {
      char* end=nullptr; const auto n=std::strtoul(p,&end,0);
      if(end==p || n>UINT32_MAX) throw std::runtime_error("invalid snapshot match");
      v.push_back(uint32_t(n)); p=end;
      if(*p==',') ++p; else if(*p) throw std::runtime_error("invalid snapshot separator");
    }
    if(v.size()<2 || v.size()%2) throw std::runtime_error("snapshot requires frame,pc[,register,value...]");
    for(size_t i=2;i<v.size();i+=2) if(v[i]>=32) throw std::runtime_error("invalid snapshot register");
    return v;
  }();
  if(frame!=match[0] || pc!=match[1]) return;
  for(size_t i=2;i<match.size();i+=2) if(regs[match[i]]!=match[i+1]) return;
  auto write=[&](const char* suffix,const void* bytes,size_t size) {
    const std::string path=std::string(prefix)+suffix;
    FILE* out=std::fopen(path.c_str(),"wbx");
    if(!out) throw std::runtime_error("snapshot output exists or cannot be created");
    const bool ok=std::fwrite(bytes,1,size,out)==size;
    const bool closed=std::fclose(out)==0;
    if(!ok || !closed) throw std::runtime_error("snapshot write failed");
  };
  write(".ram",ram,ramSize); write(".scratch",scratch,scratchSize);
  std::string json="{\"schema\":\"donor-pc-memory/v1\",\"frame\":"+std::to_string(frame)+
      ",\"pc\":"+std::to_string(pc)+",\"registers\":[";
  for(unsigned i=0;i<32;++i) json+=(i?",":"")+std::to_string(regs[i]);
  json+="]}\n";
  write(".json",json.data(),json.size()); done=true;
}
}
