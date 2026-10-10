#include "runtime/soak_probe.h"
#include "runtime/input/input.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <fmt/format.h>
#include <toml.hpp>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace ps1 {
namespace {
uint32_t ramHash(const uint8_t* ram) {
  uint32_t hash = 2166136261u;
  for (size_t i=0; i<2097152; ++i) hash=(hash^ram[i])*16777619u;
  return hash;
}
int64_t percentile(std::vector<uint64_t> values, size_t pct) {
  if (values.empty()) return -1;
  std::sort(values.begin(), values.end());
  return values[(values.size()-1)*pct/100];
}
}
bool SoakProbe::load(const std::string& manifest, std::string& error) {
  try {
    const auto data=toml::parse(manifest);
    const auto count=toml::find<int64_t>(data,"cycles");
    if (count<1 || count>1000) throw std::runtime_error("cycles outside 1..1000");
    cycles_=count;
    output_=toml::find<std::string>(data,"output");
    if (!output_.is_absolute() || !std::filesystem::is_directory(output_))
      throw std::runtime_error("output must be an existing absolute isolated directory");
    const auto rows=toml::find<std::vector<toml::value>>(data,"cases");
    if (rows.empty() || rows.size()>64) throw std::runtime_error("requires 1..64 cases");
    for (const auto& row:rows) {
      Case c;
      c.name=toml::find<std::string>(row,"name");
      if(c.name.empty() || c.name.size()>64 || c.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_")!=std::string::npos)
        throw std::runtime_error("invalid case name");
      for (const auto& old:cases_) if(old.name==c.name) throw std::runtime_error("duplicate case name");
      c.state=toml::find<std::string>(row,"state");
      if(!std::filesystem::path(c.state).is_absolute() || !std::filesystem::is_regular_file(c.state))
        throw std::runtime_error("state must name an existing absolute file");
      const auto frames=toml::find<int64_t>(row,"frames");
      if(frames<1 || frames>120000) throw std::runtime_error("frames outside 1..120000");
      c.frames=frames;
      c.endpoint=toml::find<std::string>(row,"endpoint");
      input::TasRoute validator;
      if(!validator.parse(c.endpoint,error)) throw std::runtime_error(error);
      if (row.contains("route")) {
        c.route=toml::find<std::string>(row,"route");
        if(!validator.parse(c.route,error)) throw std::runtime_error(error);
      }
      if(!c.inputs.parse(toml::find<std::string>(row,"inputs"),&error)) throw std::runtime_error(error);
      c.saved=(output_/(c.name+".ps1s")).string();
      if(std::filesystem::exists(c.saved)) throw std::runtime_error("refusing an existing soak save");
      cases_.push_back(std::move(c));
    }
    deadline_=Clock::now()+std::chrono::seconds(45);
    return true;
  } catch(const std::exception& e) { error=e.what(); cases_.clear(); return false; }
}
void SoakProbe::fail(const std::string& reason) {
  phase_=Phase::Failed;
  fmt::print(stderr,"[Soak] FAIL cycle={} case={}: {}\n",cycle_,cases_[index_].name,reason);
  std::fflush(stderr);
}
void SoakProbe::tick(SaveStateCoordinator& coordinator, uint64_t tick, const uint8_t* ram) {
  if(!enabled() || finished()) return;
  const auto now=Clock::now();
  if(now>deadline_) { fail("phase deadline exceeded"); return; }
  auto& c=cases_[index_];
  switch(phase_) {
  case Phase::Begin:
    coordinator.setHostPaused(true);
    if(coordinator.request(SaveStateCoordinator::Action::Load,c.state)) phase_=Phase::Loading;
    break;
  case Phase::Hold:
    if(!coordinator.hostPaused()) { fail("load failed to preserve host pause"); break; }
    if(now>=holdUntil_) {
      if(tick!=savedTick_ || ramHash(ram)!=savedHash_) { fail("RAM/clock advanced during acknowledged pause"); break; }
      startTick_=tick; runStart_=now; previousPresent_={}; workUs_.clear(); intervalUs_.clear();
      if (!c.route.empty()) { std::string error; route_=input::TasRoute{}; if(!route_.parse(c.route,error)) { fail(error); break; } }
      coordinator.setHostPaused(false); phase_=Phase::Run;
      deadline_=now+std::chrono::seconds(300);
    }
    break;
  case Phase::Run:
    if(tick<startTick_) { fail("unexpected timeline rewind"); break; }
    if(!c.route.empty() && route_.failed()) { fail(route_.error()); break; }
    if(c.route.empty() ? tick-startTick_>=c.frames : route_.done()) { coordinator.setHostPaused(true); phase_=Phase::Park; deadline_=now+std::chrono::seconds(45); }
    break;
  case Phase::Park:
    if(coordinator.hostPaused()) {
      input::TasRoute validator; std::string error;
      if(!validator.parse(c.endpoint,error)) { fail(error); break; }
      validator.sample(ram,2097152,tick);
      if(!validator.done() || validator.failed()) {
        for(const auto& event:validator.takeEvents()) fmt::print("[Soak endpoint] {}\n",event);
        fail("endpoint contract not satisfied"); break;
      }
      sample(ram,tick);
      if(finished()) break;
      savedTick_=tick; savedHash_=ramHash(ram);
      if(coordinator.request(SaveStateCoordinator::Action::Save,c.saved)) phase_=Phase::Saving;
    }
    break;
  case Phase::Restoring:
    if(!coordinator.pending()) coordinator.request(SaveStateCoordinator::Action::Load,c.saved);
    break;
  case Phase::Next:
    fmt::print("[Soak] PASS cycle={} case={} pause/save/restore verified\n",cycle_,c.name); std::fflush(stdout);
    if(++index_==cases_.size()) { index_=0; ++cycle_; }
    if(cycle_==cycles_) { phase_=Phase::Done; coordinator.setHostPaused(false); }
    else { phase_=Phase::Begin; deadline_=now+std::chrono::seconds(45); }
    break;
  default: break;
  }
}
void SoakProbe::completed(const SaveStateCoordinator::Request& req, bool ok, uint64_t tick, const uint8_t* ram) {
  if(!enabled() || finished()) return;
  if(!ok) { fail("save/load operation failed"); return; }
  if(phase_==Phase::Loading && req.action==SaveStateCoordinator::Action::Load) {
    savedTick_=tick; savedHash_=ramHash(ram); holdUntil_=Clock::now()+std::chrono::milliseconds(40); phase_=Phase::Hold;
  } else if(phase_==Phase::Saving && req.action==SaveStateCoordinator::Action::Save) phase_=Phase::Restoring;
  else if(phase_==Phase::Restoring && req.action==SaveStateCoordinator::Action::Load) {
    if(tick!=savedTick_ || ramHash(ram)!=savedHash_) fail("save/restore changed RAM or guest clock");
    else phase_=Phase::Next;
  } else fail("unexpected coordinator operation");
}
void SoakProbe::input(uint64_t tick, input::InputController& input, const uint8_t* ram) {
  if(phase_==Phase::Run && !cases_[index_].route.empty()) {
    const auto pads=route_.sample(ram,2097152,tick);
    for(unsigned port=0;port<2;++port) input.setScriptedButtons(pads[port],port);
    for(const auto& event:route_.takeEvents()) { fmt::print("[Soak route] {}\n",event); }
  }
  else if(phase_==Phase::Run && tick>startTick_) cases_[index_].inputs.apply(tick-startTick_,input);
  else for(unsigned port=0;port<2;++port) input.setScriptedButtons(0,port);
}
void SoakProbe::presented(bool vulkan, uint64_t workUs) {
  if(phase_!=Phase::Run) return;
  const auto now=Clock::now();
  if(intervalUs_.size()<120000 && previousPresent_!=Clock::time_point{})
    intervalUs_.push_back(std::chrono::duration_cast<std::chrono::microseconds>(now-previousPresent_).count());
  previousPresent_=now;
  if(vulkan && workUs_.size()<120000) workUs_.push_back(workUs);
}
void SoakProbe::sample(const uint8_t*, uint64_t tick) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS memory{};
  DWORD handles = 0;
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) ||
      !GetProcessHandleCount(GetCurrentProcess(), &handles)) {
    fail("could not read Windows process resource measurements"); return;
  }
  metrics::setState("soak.rss_bytes", memory.WorkingSetSize);
  metrics::setState("soak.open_handles", handles);
#else
  long pages=0,resident=0;
  const long pageSize=sysconf(_SC_PAGESIZE);
  if (!(std::ifstream("/proc/self/statm")>>pages>>resident) || resident<=0 || pageSize<=0) {
    fail("could not read resident memory measurement"); return;
  }
  int64_t fds=0;
  std::error_code error;
  std::filesystem::directory_iterator fd("/proc/self/fd",error),end;
  while(!error && fd!=end) { ++fds; fd.increment(error); }
  if(error || fds==0) { fail("could not read file descriptor measurement"); return; }
  metrics::setState("soak.rss_bytes",resident*pageSize);
  metrics::setState("soak.open_fds",fds);
#endif
  metrics::setState("soak.cycle",cycle_);
  metrics::setState("soak.case",index_);
  metrics::setState("soak.vblanks",tick-startTick_);
  metrics::setState("soak.run_us",std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-runStart_).count());
  metrics::setState("soak.present_cpu_p95_us",percentile(workUs_,95));
  metrics::setState("soak.host_interval_p95_us",percentile(intervalUs_,95));
  const auto path=output_/fmt::format("sample-{:04}-{}.json",cycle_,cases_[index_].name);
  if(!metrics::dumpJsonTo(path.string().c_str())) fail("could not publish sample");
}
}
