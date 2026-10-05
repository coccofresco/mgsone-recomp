#include "runtime/input/tas_route.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ps1::input {
namespace {
// Retail GCL variable operand, e.g. gcl_1402002d = global flag bit 2 at
// offset 0x2d; gcl_11800002 = linked signed short (difficulty).
// The bases are verified in USA GCL_GetVar (800227B8); types and bounds
// follow libgcl/variable.c, libgcl.h and include/linkvar.h in mgs_reversing.
struct GclVariable { uint32_t address, type, flag; };
std::optional<GclVariable> gclVariable(const std::string& field) {
  if (field.size()!=12 || field.compare(0,4,"gcl_")!=0) return {};
  uint32_t raw=0;
  for (size_t i=4;i<12;++i) {
    const char c=field[i];
    if (!(c>='0' && c<='9') && !(c>='a' && c<='f')) return {};
    raw=(raw<<4) | uint32_t(c<='9' ? c-'0' : c-'a'+10);
  }
  const uint32_t type=(raw>>24)&15, scope=(raw>>20)&15;
  const uint32_t offset=raw&65535, flag=(raw>>16)&15;
  if ((raw>>28)!=1 || (scope!=0 && scope!=8) ||
      (type!=1 && type!=2 && type!=3 && type!=4 && type!=6 && type!=8) ||
      (type==4 ? flag>7 : flag!=0)) return {};
  const uint32_t width=(type==1 || type==6 || type==8)?2:1;
  if (offset+width>(scope==8?192u:2048u)) return {};
  return GclVariable{(scope==8?0xB7510u:0xB6440u)+offset,type,flag};
}
struct WatcherVariable { uint32_t name, offset; };
std::optional<std::string> actorVariable(const std::string& field) {
  if (!field.starts_with("actor_") || !field.ends_with("_count") ||
      field.size()<13 || field.size()>60) return {};
  const auto name=field.substr(6,field.size()-12);
  for (const char c : name)
    if (!(c>='a' && c<='z') && !(c>='0' && c<='9') && c!='_') return {};
  return name+".c";
}
std::optional<WatcherVariable> watcherVariable(const std::string& field) {
  const bool camera=field.rfind("camera_",0)==0;
  const bool elevator=field.rfind("evpanel_",0)==0;
  const bool zako=field.rfind("zako_",0)==0;
  const bool revolver=field.rfind("revolver_",0)==0;
  const size_t prefix=revolver?9:zako?5:camera?7:8;
  if (field.size()<prefix+6 || (!camera && !elevator && !zako && !revolver && field.compare(0,8,"watcher_")) || field[prefix+4]!='_') return {};
  uint32_t name=0;
  for (size_t i=prefix;i<prefix+4;++i) {
    const char c=field[i];
    if (!(c>='0' && c<='9') && !(c>='a' && c<='f')) return {};
    name=(name<<4) | (c<='9' ? c-'0' : c-'a'+10);
  }
  if (revolver) {
    // onoda/s04b/revolver.c: CONTROL follows the actor's pad pointer.
    static const std::unordered_map<std::string,uint32_t> offsets{
      {"x",0x24},{"y",0x26},{"z",0x28},{"facing",0x2e},
      {"life",0x854},{"phase",0x8fc},{"timer",0x900}};
    const auto offset=offsets.find(field.substr(prefix+5));
    if (offset==offsets.end()) return {};
    return WatcherVariable{name|0x80000u,offset->second};
  }
  if (zako) {
    static const std::unordered_map<std::string,uint32_t> offsets{
      {"x",0x20},{"y",0x22},{"z",0x24},{"facing",0x2a},{"life",0x8fc}};
    const auto offset=offsets.find(field.substr(prefix+5));
    if (offset==offsets.end()) return {};
    return WatcherVariable{name|0x40000u,offset->second};
  }
  // game/evpanel.c: the arrival delay depends on GV_Time, not elapsed input frames.
  if (elevator) {
    static const std::unordered_map<std::string,uint32_t> offsets{
      {"phase",0x2e},{"selection",0x32},{"timer",0x36},
      {"door_phase",0x42},{"occupants",0x44}};
    const auto offset=offsets.find(field.substr(prefix+5));
    if (offset==offsets.end()) return {};
    return WatcherVariable{name|0x20000u,offset->second};
  }
  // enemy/enemy.h and game/g_struct.h; verified against USA live actors.
  static const std::unordered_map<std::string,uint32_t> offsets{
    {"x",0x20},{"y",0x22},{"z",0x24},{"facing",0x2a},{"pitch",0x28},
    {"route",0xb7d},{"target_x",0xbe0},{"target_z",0xbe4},
    {"vision_facing",0xb8c},{"vision_angle",0xb8e},{"vision_range",0xb90},{"visible",0xb92},
    {"phase",0x1e8},{"timer",0x1ec}};
  const auto offset=offsets.find(field.substr(prefix+5));
  if (offset==offsets.end() || (camera && offset->second>0x2a && offset->second!=0x1e8 && offset->second!=0x1ec) ||
      (!camera && (offset->second==0x1e8 || offset->second==0x1ec))) return {};
  return WatcherVariable{name | (camera?0x10000u:0u),offset->second};
}
const std::unordered_set<std::string> fields{
    "stage", "game_status", "load_request", "game_over_timer", "player_status", "pause_level", "alert_mode", "alert_level",
    "control", "difficulty", "disc", "life", "max_life", "weapon", "item",
    "stance", "story", "x", "y", "z", "yaw", "turn_yaw", "pad_origin", "socom_ammo",
    "guards_alive", "guards_in_room", "ambush_phase", "ambush_kills", "title_state", "title_selection", "title_level"};
struct Observation {
  const uint8_t* ram;
  std::string stage;
  std::unordered_map<std::string, int64_t> values;
  std::optional<uint32_t> title;
  bool scannedTitle = false, scannedGuards = false;
  bool scannedWatchers = false;
  bool scannedActors = false, validActors = false;
  std::unordered_map<std::string,int64_t> actors;
  std::unordered_map<uint32_t,std::optional<size_t>> watchers;
  uint32_t word(size_t p) const {
    return uint32_t(ram[p]) | uint32_t(ram[p+1]) << 8 |
           uint32_t(ram[p+2]) << 16 | uint32_t(ram[p+3]) << 24;
  }
  int16_t half(size_t p) const { return int16_t(uint16_t(ram[p]) | uint16_t(ram[p+1]) << 8); }
  Observation(const uint8_t* bytes) : ram(bytes) {
    for (size_t i = 0; i < 8 && ram[0xB7500+i]; ++i) stage += char(ram[0xB7500+i]);
    for (auto [name, address] : std::initializer_list<std::pair<const char*, size_t>>{
        {"game_status",0xADB3C}, {"load_request",0xADB40}, {"player_status",0xAE1C8},
        {"pause_level",0xAE0A0}, {"control",0xAE16C}, {"alert_mode",0xAE178}, {"alert_level",0xAE190}}) values[name] = word(address);
    for (auto [name, offset] : std::initializer_list<std::pair<const char*, size_t>>{
        {"difficulty",1}, {"disc",4}, {"life",11}, {"max_life",12},
        {"weapon",14}, {"item",15}, {"stance",16}, {"socom_ammo",17}})
      values[name] = half(0xB7510 + offset*2);
    values["story"] = half(0xB644A);
    // USA wire.c sets this to -2 on the president's death, with Snake alive.
    values["game_over_timer"] = int32_t(word(0xADB44));
    values["x"] = half(0xAE188); values["y"] = half(0xAE18A); values["z"] = half(0xAE18C);
    values["pad_origin"] = word(0xADAA8) & 4095;
    // USA s03d equivalents of Integral C2F8 and ZAKOCOM_MGR.field_60.
    // The commander's phases distinguish grenade volleys from guard waves.
    if (stage=="s03d") {
      if (word(0xDFB70)<=15) values["ambush_phase"]=word(0xDFB70);
      if (word(0xDFBE8)<=18) values["ambush_kills"]=word(0xDFBE8);
    }
    const auto control=word(0xAE16C)&0x1fffffff;
    if (control && control<2097152-80) {
      values["yaw"]=uint16_t(half(control+10))&4095;
      values["turn_yaw"]=uint16_t(half(control+0x4e))&4095;
    }
  }
  std::optional<int64_t> get(const std::string& field) {
    if (const auto name=actorVariable(field)) {
      if (!scannedActors) {
        scannedActors=true;validActors=true;
        std::unordered_set<uint32_t> visited;
        for (unsigned level=0;level<9 && validActors;++level) {
          const uint32_t head=0x800af390+level*68,tail=head+32;
          uint32_t previous=head,actor=word((head+4)&0x1fffffff);
          while (actor!=tail) {
            if (actor<0x80010000 || actor>0x801fffe0 || visited.size()>=1024 ||
                !visited.insert(actor).second || word(actor&0x1fffffff)!=previous) {
              validActors=false;break;
            }
            const auto at=actor&0x1fffffff,nameAt=word(at+20)&0x1fffffff;
            if (nameAt && (word(at+8) || word(at+12))) {
              if (nameAt<0x10000 || nameAt>=2097152) {validActors=false;break;}
              const char* begin=reinterpret_cast<const char*>(ram+nameAt);
              const auto end=static_cast<const char*>(std::memchr(begin,0,std::min(64u,2097152-nameAt)));
              if (!end) {validActors=false;break;}
              ++actors[std::string(begin,end)];
            }
            previous=actor;actor=word(at+4);
          }
          if (word(tail&0x1fffffff)!=previous) validActors=false;
        }
      }
      if (!validActors) return {};
      const auto found=actors.find(*name);
      return found==actors.end() ? 0 : found->second;
    }
    if (const auto variable=watcherVariable(field)) {
      if (!scannedWatchers) {
        scannedWatchers=true;
        for (size_t base=0;base+0xc50<2097152;base+=4) {
          const auto act=word(base+8), name=word(base+20)&0x1fffffff;
          if (act<0x80010000 || act>=0x80200000 || name>2097152-11) continue;
          const bool camera=std::memcmp(ram+name,"camera.c",9)==0;
          const bool elevator=std::memcmp(ram+name,"evpanel.c",10)==0;
          const bool zako=stage=="s03d" && std::memcmp(ram+name,"zako.c",7)==0;
          const bool revolver=stage=="s04b" && std::memcmp(ram+name,"revolver.c",11)==0;
          if (!camera && !elevator && !zako && !revolver && std::memcmp(ram+name,"watcher.c",10)) continue;
          const auto prev=word(base)&0x1fffffff, next=word(base+4)&0x1fffffff;
          if (!prev || prev>=2097152-8 || !next || next>=2097152-8 ||
              word(prev+4)!=(base|0x80000000) || word(next)!=(base|0x80000000)) continue;
          const uint32_t id=uint16_t(half(base+(revolver?0x54:elevator?0x2c:0x50))) |
                            (revolver?0x80000u:zako?0x40000u:elevator?0x20000u:camera?0x10000u:0u);
          if (watchers.count(id)) watchers[id]={};
          else watchers[id]=base;
        }
      }
      const auto found=watchers.find(variable->name);
      if (found==watchers.end() || !found->second) return {};
      const auto address=*found->second+variable->offset;
      if (variable->name&0x80000u)
        return variable->offset>=0x854 ? int32_t(word(address)) : half(address);
      if (variable->offset==0x8fc) {
        const auto target=word(address)&0x1fffffff;
        if (!target || target>2097152-40) return {};
        return half(target+38);
      }
      if (variable->offset==0x1e8 || variable->offset==0x1ec) return int32_t(word(address));
      return variable->offset==0xb7d ? int8_t(ram[address]) : half(address);
    }
    if (const auto variable=gclVariable(field)) {
      const auto p=variable->address;
      switch (variable->type) {
        case 1: return half(p);
        case 6: case 8: return uint16_t(half(p));
        case 4: return (ram[p]>>variable->flag)&1;
        default: return ram[p];
      }
    }
    if (field.rfind("title_", 0) == 0 && !scannedTitle) {
      scannedTitle = true;
      unsigned matches = 0;
      if (stage == "title") for (size_t base=0; base+0x24BC<=2097152; base+=4) {
        if (word(base+8) != 0x800D26E0) continue;
        uint32_t name = word(base+20) & 0x1fffffff;
        if (name <= 2097152-7 && std::memcmp(ram+name,"open.c",7)==0) { ++matches; title=base; }
      }
      if (matches == 1) {
        values["title_state"] = int32_t(word(*title+0xA48));
        values["title_selection"] = int32_t(word(*title+0xA4C));
        values["title_level"] = int32_t(word(*title+0x24B8));
      }
    }
    if (field.rfind("guards_", 0)==0 && !scannedGuards) {
      scannedGuards = true;
      if (stage == "s03d") {
        int alive=0, inside=0;
        for (size_t base=0; base+0x900<=2097152; base+=4) {
          uint32_t act=word(base+8);
          if (act<0x80010000 || act>=0x80200000) continue;
          uint32_t name=word(base+20)&0x1fffffff;
          if (name>2097152-7 || std::memcmp(ram+name,"zako.c",7)) continue;
          uint32_t target=word(base+0x8fc)&0x1fffffff;
          if (!target || target>=2097152-40 || half(target+38)<=0) continue;
          ++alive;
          const auto x=half(base+32), z=half(base+36);
          if (x>=4500 && x<=11000 && z>=-2000 && z<=3000) ++inside;
        }
        values["guards_alive"]=alive; values["guards_in_room"]=inside;
      }
    }
    const auto found=values.find(field);
    if (found == values.end()) return {};
    return found->second;
  }
};
}

bool TasRoute::load(const std::filesystem::path& path, std::string& error) {
  std::ifstream file(path);
  if (!file) { error="cannot open route"; return false; }
  std::ostringstream text; text << file.rdbuf();
  return parse(text.str(), error);
}

bool TasRoute::parse(std::string_view text, std::string& error) {
  *this = TasRoute{};
  std::istringstream stream{std::string(text)};
  std::string line;
  if (!std::getline(stream,line) || line!="MGSROUTE 1") { error="invalid route header"; return false; }
  bool inStep=false;
  try {
    while (std::getline(stream,line)) {
      if (line.empty() || line[0]=='#') continue;
      std::istringstream row(line);
      std::string command, extra; row >> command;
      if (command=="STEP") {
        if (inStep) throw std::runtime_error("nested route step");
        Step step;
        if (!(row>>step.name>>step.budget>>step.attempts) || !step.budget || step.budget>10000000 || !step.attempts || step.attempts>1000000)
          throw std::runtime_error("invalid step budget/attempts");
        steps_.push_back(step); inStep=true;
      } else if (command=="END") {
        if (!inStep || steps_.back().until.empty()) throw std::runtime_error("step requires a goal");
        if (steps_.back().move && (!steps_.back().runs.empty() || steps_.back().gameClock)) throw std::runtime_error("MOVE cannot mix with INPUT/CLOCK");
        inStep=false;
      } else if (!inStep) throw std::runtime_error("command outside step");
      else if (command=="STABLE") {
        if (!(row>>steps_.back().stableFrames) || !steps_.back().stableFrames ||
            steps_.back().stableFrames>3600)
          throw std::runtime_error("invalid stable goal duration");
      }
      else if (command=="CLOCK") {
        std::string domain;
        if (!(row>>domain) || domain!="GAME" || steps_.back().gameClock)
          throw std::runtime_error("invalid/duplicate route clock");
        steps_.back().gameClock=true;
      } else if (command=="MOVE") {
        auto& step=steps_.back();
        if (step.move || !(row>>step.targetX>>step.targetZ) ||
            step.targetX < -32768 || step.targetX > 32767 ||
            step.targetZ < -32768 || step.targetZ > 32767)
          throw std::runtime_error("invalid movement target");
        step.move=true;
      } else if (command=="INPUT") {
        Run run; uint32_t a,b;
        if (!(row>>run.frames>>a>>b) || !run.frames || run.frames>10000000 || a>65535 || b>65535)
          throw std::runtime_error("invalid input run");
        run.pads={uint16_t(a),uint16_t(b)}; steps_.back().runs.push_back(run);
      } else {
        Predicate p;
        uint64_t mask;
        if (!(row>>p.field>>p.op>>p.value>>mask) || mask>0xffffffff ||
            (!fields.count(p.field) && !gclVariable(p.field) && !watcherVariable(p.field) && !actorVariable(p.field)) ||
            (p.op!="eq" && p.op!="ne" && p.op!="min" && p.op!="max"))
          throw std::runtime_error("invalid route predicate");
        p.mask=uint32_t(mask);
        if (p.field=="stage") {
          if ((p.op!="eq" && p.op!="ne") || p.value.size()>8) throw std::runtime_error("invalid stage predicate");
        } else {
          size_t end=0; std::stoll(p.value,&end);
          if (end!=p.value.size()) throw std::runtime_error("invalid numeric predicate");
        }
        auto& step=steps_.back();
        if (command=="WHEN") step.when.push_back(p);
        else if (command=="UNTIL") step.until.push_back(p);
        else if (command=="GUARD") step.guard.push_back(p);
        else if (command=="ABORT") step.abort.push_back(p);
        else throw std::runtime_error("unknown route command");
      }
      if (row>>extra) throw std::runtime_error("trailing route fields");
    }
    if (inStep || steps_.empty()) throw std::runtime_error("unfinished or empty route");
  } catch (const std::exception& e) { error=e.what(); steps_.clear(); return false; }
  error.clear(); return true;
}

std::array<uint16_t, 2> TasRoute::sample(const uint8_t* ram, size_t size, uint64_t vblank) {
  if (done() || failed()) return {};
  if (!ram || size!=2097152) { error_="route requires complete USA RAM"; return {}; }
  Observation state(ram);
  const auto matches=[&](const std::vector<Predicate>& conditions) {
    for (const auto& p : conditions) {
      if (p.field=="stage") {
        if ((p.op=="eq") != (state.stage==p.value)) return false;
        continue;
      }
      auto read=state.get(p.field);
      if (!read) return false;
      const int64_t value=p.mask==0xffffffff ? *read : (uint32_t(*read)&p.mask);
      const int64_t expected=std::stoll(p.value);
      if ((p.op=="eq" && value!=expected) || (p.op=="ne" && value==expected) ||
          (p.op=="min" && value<expected) || (p.op=="max" && value>expected)) return false;
    }
    return true;
  };
  const auto& step=steps_[index_];
  auto event=[&](const std::string& what) {
    std::ostringstream line;
    line << what << ' ' << step.name << " vblank=" << vblank
         << " elapsed=" << elapsed_ << " attempts=" << used_ << " stage=" << state.stage;
    std::set<std::string> observed{"story", "difficulty", "life", "x", "y", "z", "stance"};
    for (const auto* conditions : {&step.when, &step.until, &step.guard, &step.abort})
      for (const auto& p : *conditions) if (p.field!="stage") observed.insert(p.field);
    for (const auto& field : observed) {
      const auto value=state.get(field);
      line << ' ' << field << '=' << (value ? std::to_string(*value) : "missing");
    }
    events_.push_back(line.str());
  };
  if (!matches(step.guard) || (!step.abort.empty() && matches(step.abort))) {
    error_="guard/abort condition: "+step.name; event("failed"); return {};
  }
  if (matches(step.until)) {
    if (++goalStreak_ < step.stableFrames) {
      if (elapsed_++>=step.budget) { error_="condition timeout: "+step.name; event("failed"); }
      return {};
    }
    event("satisfied"); ++index_; elapsed_=used_=remaining_=run_=0; playing_=false;
    goalStreak_=0;
    return {}; // release all keys at every goal before arming the next step
  }
  goalStreak_=0;
  if (elapsed_++>=step.budget) { error_="condition timeout: "+step.name; event("failed"); return {}; }
  if (step.move) {
    if (!matches(step.when)) return {};
    // Only controller input: coordinates never change the guest state directly.
    // libgv/pad.c adds PadOrg to direction (Down=0, Right=1024, Up=2048).
    // Mandatory Codec can interrupt a traversal; advance it through normal keys.
    if (*state.get("pause_level")==1)
      return {uint16_t(elapsed_%30<6 ? 0x4008 : 0),0};
    if (!*state.get("control") || *state.get("pause_level") ||
        (*state.get("game_status") & 0xdc080000) || (*state.get("player_status") & 0x20000000)) return {};
    if (!playing_) { playing_=true; ++used_; event("move"); }
    const auto dx=step.targetX-*state.get("x"), dz=step.targetZ-*state.get("z");
    if (!dx && !dz) return {};
    constexpr double tau=6.2831853071795864769;
    const double worldDirection=std::atan2(double(dx),double(dz))*4096/tau;
    if (*state.get("player_status") & 2) {
      // PLAYER_INTRUDE: sna_init.c sub_8005684C/5688C/56928 use relative
      // controls in ducts. Left increases CONTROL.turn.vy, Right decreases it;
      // Up crawls along the current orientation. Never write CONTROL itself.
      const auto control=uint32_t(*state.get("control")) & 0x1fffffff;
      if (!control || control>size-16) return {};
      const int yaw=state.half(control+10) & 4095;
      const int delta=(int(std::lround(worldDirection))-yaw+6144)%4096-2048;
      uint16_t pad=std::abs(delta)<=128 ? 0x10 : 0;
      if (std::abs(delta)>32) pad|=delta>0 ? 0x80 : 0x20;
      return {pad,0};
    }
    const double direction=worldDirection-*state.get("pad_origin");
    const unsigned sector=unsigned(int(std::lround(direction/512)) & 7);
    constexpr uint16_t pads[]{0x40,0x60,0x20,0x30,0x10,0x90,0x80,0xc0};
    return {pads[sector],0};
  }
  const uint32_t gameTick=state.word(0xADA48); // USA GV_Time; gvd.c Act, gp+76
  if (playing_ && step.gameClock && gameTick!=lastGameTick_) {
    if (uint32_t(gameTick-lastGameTick_)!=1) {
      error_="missed/reset game input tick: "+step.name; event("failed"); return {};
    }
    lastGameTick_=gameTick;
    if (--remaining_==0) {
      if (++run_==step.runs.size()) playing_=false;
      else remaining_=step.runs[run_].frames;
    }
  }
  if (!playing_ && used_<step.attempts && !step.runs.empty() && matches(step.when)) {
    playing_=true; ++used_; run_=0; remaining_=step.runs[0].frames; event("action");
    lastGameTick_=gameTick;
  }
  if (!playing_) return {};
  auto pads=step.runs[run_].pads;
  if (!step.gameClock && --remaining_==0) {
    if (++run_==step.runs.size()) playing_=false;
    else remaining_=step.runs[run_].frames;
  }
  return pads;
}
std::vector<std::string> TasRoute::takeEvents() {
  auto result=std::move(events_); events_.clear(); return result;
}
}
