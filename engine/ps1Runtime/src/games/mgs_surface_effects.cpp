#include "runtime/games/mgs_surface_effects.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace ps1::games::mgs {
namespace r = gpu::replay;
namespace {
using SurfaceKind = r::MgsSurfaceKind;
struct AnimationSignature {
  const char *source, *symbol;
  uint16_t texture;
  std::array<uint16_t, 3> atlas;
  SurfaceKind kind;
  std::span<const uint8_t> bytes;
};
#include "mgs_animation_signatures.inc"

struct Ram {
  std::span<const uint8_t> bytes;
  bool valid(uint32_t p, uint64_t n) const {
    p &= 0x1fffffffu;
    return p >= 0x10000 && p < bytes.size() && n <= bytes.size() - p;
  }
  uint16_t half(uint32_t p) const {
    p &= 0x1fffffffu;
    return uint16_t(bytes[p]) | uint16_t(bytes[p + 1]) << 8;
  }
  uint32_t word(uint32_t p) const { return half(p) | uint32_t(half(p+2)) << 16; }
  bool equal(uint32_t p, std::span<const uint8_t> v) const {
    return valid(p, v.size()) && !std::memcmp(bytes.data() + (p & 0x1fffffffu), v.data(), v.size());
  }
  std::string_view name(uint32_t p) const {
    if (!valid(p, 1)) return {};
    const char *s = reinterpret_cast<const char *>(bytes.data() + (p & 0x1fffffffu));
    const size_t max = std::min<size_t>(64, bytes.size() - (p & 0x1fffffffu));
    const auto *end = static_cast<const char *>(std::memchr(s, 0, max));
    return end ? std::string_view(s, end - s) : std::string_view{};
  }
};
struct BoundSurface {
  r::MgsSurfaceEffect effect;
  uint32_t color, page, clut;
  uint32_t opcode;
  // Original DG positions transformed before integer projection, for TILEs
  // and line primitives which do not have a KMD/quad geometry adapter.
  std::array<std::array<double,3>,4> view{};
  float projection=0;
  bool sourceShape=false;
  bool authoredUv=false;
  std::optional<r::OriginalMeshLighting> lighting;
  std::array<float,2> screenShift{}, tileExtent{};
};
std::unordered_map<uint32_t, BoundSurface> surfaces;
std::unordered_map<uint32_t, r::MgsSurfaceEffect> objects;
struct InterfaceShape {uint32_t actor;uint8_t opcode;float xOffset;};
std::unordered_map<uint32_t,InterfaceShape> interfaceShapes;
std::array<float,4> fog{};
float paletteMode=0;
uint32_t photoActor=0;
bool photoArmed=false;
uint64_t photoSerial=0,pendingPhoto=0;

void addObjectEffect(std::unordered_map<uint32_t,r::MgsSurfaceEffect> &target,
                     uint32_t object,const r::MgsSurfaceEffect &effect) {
  auto [it,inserted]=target.emplace(object&0x1fffffffu,effect);
  if(inserted) return;
  auto &current=it->second;
  // A reflection can legitimately contain an optically camouflaged model.
  // Keep the material and attach the reflection's world-space clipping plane.
  if(current.kind==SurfaceKind::Mirror && effect.kind==SurfaceKind::Camo) {
    const auto plane=current.parameters[1];current=effect;
    current.parameters[1]=plane;current.parameters[2][2]=1;
  } else if(current.kind==SurfaceKind::Camo && effect.kind==SurfaceKind::Mirror) {
    current.parameters[1]=effect.parameters[1];current.parameters[2][2]=1;
  } else current.kind=SurfaceKind::None;
}

std::array<float,4> fogRange(const Ram &m,uint32_t act) {
  if(!m.valid(act,96)) return {};
  // Identify the USA scale updater by its retail instructions, following
  // its call relocation instead of borrowing Integral overlay addresses.
  for(unsigned offset=0;offset<88;offset+=4) {
    const uint32_t call=m.word(act+offset);
    if((call>>26)!=3 || m.word(act+offset+4)!=0x00a42021) continue;
    const uint32_t target=(call&0x3ffffffu)*4u|0x80000000u;
    if(!m.valid(target,132) || m.word(target)!=0x28820100 ||
       m.word(target+4)!=0x14400002 || m.word(target+8)!=0 ||
       m.word(target+12)!=0x240400ff ||
       (m.word(target+24)>>16)!=0x3c01 ||
       (m.word(target+28)>>16)!=0xac24 ||
       (m.word(target+92)>>16)!=0x3c01 ||
       (m.word(target+96)>>16)!=0xac22 ||
       m.word(target+124)!=0x03e00008) continue;
    const auto address=[&](unsigned o) {
      return (m.word(target+o)<<16)+int16_t(m.word(target+o+4));
    };
    const uint32_t low=address(24),high=address(92);
    if(!m.valid(low,4) || !m.valid(high,4)) continue;
    const uint32_t start=m.word(low),end=m.word(high);
    if(start<end && end<=255) return {float(start*256),float(end*256),1,0};
  }
  return {};
}

std::optional<SurfaceKind> animationKind(const Ram &m, uint32_t actor, uint32_t unit) {
  const uint32_t texture = m.word(actor + 32);
  if (!m.valid(texture, 12)) return {};
  const uint16_t id = m.half(texture);
  const std::array<uint16_t, 3> atlas{m.half(actor + 58), m.half(actor + 60), m.half(actor + 62)};
  const uint32_t start = m.word(unit + 20), cursor = m.word(unit + 24);
  // SnowStorm constructs this program at runtime. Only the two-byte loop
  // duration is variable; require the complete remaining program, atlas and
  // one of its three authored texture identities (kojo/sstorm.c).
  constexpr std::array<uint8_t,21> storm{
      0,16,1,0,5,1,254,12,0,0,1,255,8,0,0,0,2,0,1,13,15};
  const auto hash=[](std::string_view name) {
    uint16_t h=0;for(const unsigned char c:name) h=uint16_t((h<<5)|(h>>11))+c;
    return h;
  };
  if(atlas==std::array<uint16_t,3>{1,1,1} &&
     (id==hash("snow_ex1") || id==hash("snow_ex2") || id==hash("snow_ex3")) &&
     start>=5 && cursor>=start && cursor<start+16 && m.valid(start-5,21)) {
    bool match=true;
    for(unsigned i=0;i<storm.size();++i)
      if(i!=8 && i!=9) match&=m.bytes[((start-5)&0x1fffffffu)+i]==storm[i];
    if(match) return SurfaceKind::SnowStorm;
  }
  std::optional<SurfaceKind> found;
  for (const auto &s : animationSignatures) {
    if (s.texture != id || s.atlas != atlas) continue;
    for (unsigned i = 0; i < s.bytes[2]; ++i) {
      const uint32_t offset = uint32_t(s.bytes[3 + i*2]) * 256 + s.bytes[4 + i*2];
      if (start < offset || cursor < start || cursor >= start - offset + s.bytes.size() ||
          !m.equal(start - offset, s.bytes)) continue;
      if (found && *found != s.kind) return {}; // shared bytecode with ambiguous semantics
      found = s.kind;
    }
  }
  return found;
}

// Register only allocations with the complete DG_PRIM layout and original
// packet/position provenance. Both buffers are snapshots, never RAM pointers.
void primitive(const Ram &m, uint32_t actor, uint32_t p, SurfaceKind kind,
               float time, std::unordered_map<uint32_t, BoundSurface> &out,
               int only = -1, float progress = 0) {
  if (!m.valid(p, 84)) return;
  const uint32_t type = m.word(p + 36), count = m.half(p + 42);
  const unsigned base=type&31;
  const bool flat = base==18 || base==21, gouraud=base==20 || base==22;
  const bool tile=base==9 && kind==SurfaceKind::Snow;
  const bool freeLine=base==23 && kind==SurfaceKind::Spark;
  const bool line=(base==0 || base==3 || base==21 || base==22 || freeLine);
  const auto callback=m.word(p+80);
  const bool billboard=base==18 && (type&0x2000) && m.half(p+72)==count &&
      m.valid(callback,16) && m.word(callback)==0x00a04021 &&
      m.word(callback+4)==0x24c6ffff && m.word(callback+8)==0x8c8b0038 &&
      m.word(callback+12)==0x3c0d800b;
  const bool ripple=kind==SurfaceKind::Ripple && base==18 && count==8;
  const uint32_t stride = flat ? 40 : gouraud ? 52 : base==3 || freeLine ? 20 : 16;
  const uint32_t step = flat ? 8 : gouraud ? 12 : base==3 ? 8 : tile ? 0 : 4;
  const uint32_t verts=tile ? 1 : line ? 2 : 4;
  if ((!flat && !gouraud && !tile && !line) || (type & 0x100) || !count || count > 1024 ||
      m.half(p + 48) != stride || m.half(p + 50) != verts ||
      m.half(p + 52) != (freeLine ? 12 : 8) || m.half(p + 54) != step ||
      !m.valid(m.word(p + 56), uint64_t(count) * ((type & 0x400) || billboard ? 1 : verts) * 8)) return;
  if((type&0x2000) && !billboard) return;
  const auto scene=latestSceneSnapshot();
  if((tile || line || ripple || billboard) && (!scene || scene->projection<=0)) return;
  if(kind==SurfaceKind::Ripple && !ripple) return;
  for (unsigned b = 0; b < 2; ++b) {
    const uint32_t packs = m.word(p + 64 + b*4);
    if (!m.valid(packs, uint64_t(count) * stride)) continue;
    for (uint32_t i = 0; i < count; ++i) {
      if (only >= 0 && i != unsigned(only)) continue;
      const uint32_t packet = packs + i*stride;
      const uint32_t command=packet+(freeLine ? 8 : 4);
      const uint32_t color = m.word(command), opcode = color >> 24;
      if(freeLine && (m.word(packet+4)&0xff000000u)!=0xe1000000u) continue;
      if ((opcode & 0xfc) != (flat ? 0x2c : gouraud ? 0x3c : tile ? 0x60 : base==3 ? 0x50 : 0x40)) continue;
      r::MgsSurfaceEffect e;
      e.kind = kind;
      e.actor = actor & 0x1fffffffu;
      e.parameters[0] = {float(kind), time, progress, float(i)};
      // DG_PRIM.raise is the authored ordering-depth offset for all owned
      // particles (e.g. blast=2000, bubbles=300/3000), not a world transform.
      // Retain this metadata separately from physical particle dimensions.
      e.parameters[2][0] = float(int16_t(m.half(p + 46)));
      if (kind == SurfaceKind::Explosion && (type & 0x600) == 0x400) {
        const auto rect = m.word(p + 60);
        if (m.valid(rect, 8)) {
          const int width = int16_t(m.half(rect + 4));
          const int height = int16_t(m.half(rect + 6));
          // anime.c expands this centered rectangle from its original
          // script. Its world-space extent, never the ordering-table raise,
          // defines the presentation volume around the live emitter.
          if (width > 0 && height > 0 &&
              int16_t(m.half(rect)) == width / 2 &&
              int16_t(m.half(rect + 2)) == height / 2) {
            e.parameters[2][1] = width * 0.5f;
            e.parameters[2][2] = height * 0.5f;
          }
        }
      }
      // Camera-space glints are optical overlays, not particles in the
      // scene: their bookkeeping Z must not intersect world geometry.
      if (kind == SurfaceKind::Glow && (type & 0x200)) e.parameters[2][2] = 1.f;
      // Bubbles intentionally move forward in the ordering table (3000 in
      // bubble_p, 300 in bub_d_sn). Soft intersections must respect that
      // authored layering, without moving their source geometry or simulation.
      if (kind == SurfaceKind::Bubble) {
        const auto name=m.name(m.word(actor+20));
        // These original sprites contain clusters, not a single large bubble.
        // Match the decompiled actor's allocation/texture sequence, already
        // authenticated above; no inference from a recycled texture page.
        if(name=="bubble_p.c" && count==3) e.parameters[2][1]=float(i+1);
        else if(name=="bub_d_sn.c" && count==16) {
          if(p==m.word(actor+44)) e.parameters[2][1]=2;
          else if(p==m.word(actor+48)) e.parameters[2][1]=3;
        } else if(name=="splash2.c") e.parameters[2][1]=3;
      }
      const uint32_t key = command & 0x1fffffffu;
      BoundSurface bound{e, color & 0xffffffu,
                         flat || gouraud ? m.half(packet + 14 + step) : 0u,
                         flat || gouraud ? m.half(packet + 14) : 0u, opcode};
      if(kind==SurfaceKind::Cloth && m.valid(actor,0xa7c)) {
        const uint32_t light=m.word(actor+0xa74), colors=m.word(actor+0xa78);
        if(m.valid(light,32) && m.valid(colors,32) && scene) {
          r::OriginalMeshLighting lighting;
          lighting.ambient={scene->ambient[0],scene->ambient[1],scene->ambient[2],1};
          for(unsigned row=0;row<3;++row) for(unsigned a=0;a<3;++a) {
            lighting.directions[row][a]=int16_t(m.half(light+(row*3+a)*2))/4096.f;
            lighting.colors[row][a]=int16_t(m.half(colors+(row*3+a)*2))/16.f;
          }
          // Recompute continuous corner normals from adjacent deformed faces.
          // Area weighting avoids the GTE-normal/vertex-RGB quantization.
          const uint32_t positions=m.word(p+56);
          for(unsigned k=0;k<4;++k) {
            const uint32_t target=positions+(i*4+k)*8;
            std::array<double,3> normal{};
            for(unsigned face=0;face<count;++face) {
              bool shared=false;
              for(unsigned c=0;c<4;++c)
                shared|=m.half(target)==m.half(positions+(face*4+c)*8) &&
                        m.half(target+2)==m.half(positions+(face*4+c)*8+2) &&
                        m.half(target+4)==m.half(positions+(face*4+c)*8+4);
              if(!shared) continue;
              std::array<double,3> a{},b{};
              for(unsigned axis=0;axis<3;++axis) {
                const uint32_t v=positions+face*32+axis*2;
                a[axis]=int16_t(m.half(v+8))-int16_t(m.half(v));
                b[axis]=int16_t(m.half(v+16))-int16_t(m.half(v));
              }
              for(unsigned axis=0;axis<3;++axis)
                normal[axis]+=a[(axis+1)%3]*b[(axis+2)%3]-a[(axis+2)%3]*b[(axis+1)%3];
            }
            const double length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
            for(unsigned axis=0;axis<3;++axis) lighting.normals[k][axis]=float(normal[axis]/std::max(length,1e-9));
          }
          bound.lighting=std::move(lighting);
        }
      }
      if(tile || line || billboard) {
        std::array<std::array<double,3>,2> points{};
        for(unsigned k=0;k<(line ? 2u : 1u);++k) {
          const uint32_t v=m.word(p+56)+(i*(billboard ? 1 : verts)+k)*8;
          std::array<double,3> world{};
          const uint32_t root=m.word(p+32),matrix=m.valid(root,32) ? root : p;
          for(unsigned row=0;row<3;++row) {
            world[row]=int32_t(m.word(matrix+20+row*4));
            for(unsigned a=0;a<3;++a)
              world[row]+=int16_t(m.half(matrix+(row*3+a)*2))/4096.0*int16_t(m.half(v+a*2));
          }
          if(m.word(p+36)&0x200) {
            for(unsigned a=0;a<3;++a) points[k][a]=int16_t(m.half(v+a*2));
          } else {
            for(unsigned row=0;row<3;++row) {
              points[k][row]=scene->view[12+row];
              for(unsigned a=0;a<3;++a) points[k][row]+=scene->view[a*4+row]*world[a];
            }
            points[k][1]*=58.0/64.0;
          }
        }
        if(points[0][2]<1 || (line && points[1][2]<1)) continue;
        bound.projection=float(scene->projection);
        bound.sourceShape=true;
        bound.authoredUv=billboard;
        const double width=billboard ? std::abs(int16_t(m.half(m.word(p+56)+i*8+6))) : tile ? 20.0 : 4.0;
        double nx=1,ny=0;
        if(line) {
          const double dx=points[1][0]/points[1][2]-points[0][0]/points[0][2];
          const double dy=points[1][1]/points[1][2]-points[0][1]/points[0][2];
          const double length=std::hypot(dx,dy);
          if(length>1e-10){nx=-dy/length;ny=dx/length;}
        }
        for(unsigned k=0;k<4;++k) {
          auto &v=bound.view[k];v=points[line ? k/2 : 0];
          const double halfWidth=std::max(width,v[2]/scene->projection*(tile ? 0.6 : 0.7));
          const double side=k&1 ? 1 : -1;
          v[0]+=nx*halfWidth*side;
          v[1]+=ny*halfWidth*side;
          if(tile || billboard) v[1]+=(k>>1 ? 1 : -1)*halfWidth;
          if(scene->widescreen && !(m.word(p+36)&0x200)) v[0]*=0.75;
        }
      }
      if(ripple) {
        const auto positions=m.word(p+56);
        double radius=0;
        for(unsigned k=0;k<32;++k)
          radius=std::max(radius,std::hypot(double(int16_t(m.half(positions+k*8))),
                                          double(int16_t(m.half(positions+k*8+4)))));
        if(radius<1) continue;
        bound.projection=float(scene->projection);bound.sourceShape=true;
        bool visible=true;
        for(unsigned k=0;k<4;++k) {
          std::array<double,3> world{};
          const std::array<double,3> local{(k&1 ? 1 : -1)*radius,0,(k>>1 ? 1 : -1)*radius};
          for(unsigned row=0;row<3;++row) {
            world[row]=int32_t(m.word(p+20+row*4));
            for(unsigned a=0;a<3;++a) world[row]+=int16_t(m.half(p+(row*3+a)*2))/4096.0*local[a];
          }
          for(unsigned row=0;row<3;++row) {
            bound.view[k][row]=scene->view[12+row];
            for(unsigned a=0;a<3;++a) bound.view[k][row]+=scene->view[a*4+row]*world[a];
          }
          bound.view[k][1]*=58.0/64.0;
          if(scene->widescreen) bound.view[k][0]*=0.75;
          visible&=bound.view[k][2]>=1;
        }
        if(!visible) continue;
      }
      if (auto [it, inserted] = out.emplace(key, bound); !inserted && it->second.effect.actor != e.actor)
        it->second.effect.kind = SurfaceKind::None; // conflicting ownership
    }
  }
}
} // namespace

void captureSurfaceEffects(std::span<const uint8_t> bytes) {
  surfaces.clear();
  objects.clear();
  interfaceShapes.clear();
  fog={};
  paletteMode=0;
  if (!active() || bytes.size()<0xada4c) {
    photoActor=0;photoArmed=false;pendingPhoto=0;return;
  }
  const Ram m{bytes};
  if (!m.valid(0x800ada48, 4)) return;
  // GV_Time is saved/restored by the game. No second random generator,
  // simulation, particle count or lifetime is introduced on the host.
  const float time = float(m.word(0x800ada48) & 0xffffu) / 30.f;
  std::optional<std::array<uint32_t,3>> photo;
  for (uint32_t level = 0; level < 9; ++level) {
    const uint32_t head = 0x800af390 + level*68, tail = head+32;
    if (!m.valid(head, 68)) continue;
    uint32_t previous = head, actor = m.word(head+4);
    std::unordered_map<uint32_t, BoundSurface> pending;
    std::unordered_map<uint32_t, r::MgsSurfaceEffect> pendingObjects;
    std::unordered_map<uint32_t,InterfaceShape> pendingInterface;
    std::array<float,4> pendingFog{};
    float pendingPalette=0;
    std::optional<std::array<uint32_t,3>> candidatePhoto;
    for (unsigned guard = 0; actor != tail && actor && guard < 1024; ++guard) {
      if (!m.valid(actor, 32) || m.word(actor) != previous) break;
      const auto name = m.name(m.word(actor+20));
      // emitter.c intentionally has no Act; Die + live list establish life.
      if (!m.word(actor+8) && !m.word(actor+12)) break;
      if(name=="sight.c" && m.valid(actor,92)) {
        const auto info=m.word(actor+52),offset=m.word(actor+76);
        if(m.valid(info,24)) {
          const uint32_t size=m.half(info),count=m.bytes[(info&0x1fffffffu)+3];
          const auto entries=m.word(info+4),source=m.word(info+8);
          if(size>=12 && m.valid(source,size) && m.valid(entries,count*4)) {
            const float xOffset=offset && m.valid(offset,4) ? int16_t(m.half(offset)) : 0.f;
            for(unsigned b=0;b<2;++b) {
              const auto buffer=m.word(actor+56+b*4);
              if(!m.valid(buffer,size)) continue;
              for(unsigned i=0;i<count;++i) {
                const auto at=m.half(entries+i*4+2);
                if(at>size-8 || m.word(source+at)==255) continue; // text pseudo-primitive
                const unsigned words=m.word(source+at);
                if(words<2 || words>16 || at+(words+1)*4>size) continue;
                const uint8_t op=m.word(buffer+at+4)>>24;
                if((op&0xfc)!=(m.word(source+at+4)>>24 & 0xfc)) continue;
                pendingInterface.emplace((buffer+at+4)&0x1fffffffu,InterfaceShape{actor,op,xOffset});
              }
            }
          }
        }
      }
      const auto interfaceArray=[&](uint32_t p,unsigned count,unsigned stride) {
        if(!m.valid(p,uint64_t(count)*stride)) return;
        for(unsigned i=0;i<count;++i) {
          const auto q=p+i*stride;const uint8_t op=m.word(q+4)>>24;
          if((op&0xe0)==0x40 || (op&0xfc)==0x68 || (op&0xfc)==0x38)
            pendingInterface.emplace((q+4)&0x1fffffffu,InterfaceShape{actor,op,0});
        }
      };
      if(name=="scope.c" && m.valid(actor,0x98)) for(unsigned b=0;b<2;++b) {
        interfaceArray(m.word(actor+0x74+b*4),8,16);
        interfaceArray(m.word(actor+0x7c+b*4),2,28);
        interfaceArray(m.word(actor+0x88+b*4),16,24);
        interfaceArray(m.word(actor+0x90+b*4),1,24);
      }
      if(name=="gglsight.c" && m.valid(actor,0x370)) for(unsigned b=0;b<2;++b) {
        interfaceArray(actor+0x40+b*24*12,24,12);
        interfaceArray(actor+0x280+b*3*16,3,16);
      }
      if(name=="stnsight.c" && m.valid(actor,88)) for(unsigned b=0;b<2;++b) {
        interfaceArray(m.word(actor+40+b*4),14,28);
        interfaceArray(m.word(actor+48+b*4),14,28);
        interfaceArray(m.word(actor+56+b*4),2,28);
        interfaceArray(m.word(actor+64+b*4),3,28);
        interfaceArray(m.word(actor+72+b*4),7,12);
        interfaceArray(m.word(actor+80+b*4),32,36);
      }
      if(name=="jpegcam.c" && m.valid(actor,0x9c) &&
         m.valid(m.word(actor+32),124) && m.valid(m.word(actor+36),4))
        candidatePhoto=std::array<uint32_t,3>{actor,m.word(actor+0x64),m.word(actor+0x70)};
      if(name=="fog.c" && m.valid(actor,68) && m.word(actor+64)==1 &&
         (m.word(actor+32)&m.word(0x800ae0e0))) pendingFog=fogRange(m,m.word(actor+8));
      if(name=="sepia.c" && m.valid(actor,36)) {
        const uint32_t p=m.word(actor+32),act=m.word(actor+8);
        if(!p && m.valid(act,8) && m.word(act)==0x03e00008 && m.word(act+4)==0)
          pendingPalette=2; // monotone palette, empty Act
        else if(m.valid(p,48) && m.word(p+20)>>24==0x62 &&
                m.word(p+24)==0xfff1ff80 && m.word(p+28)==0x001e0100)
          pendingPalette=1; // half-luminance palette plus original local RGB tile
      }
      if(name=="menuman.c" && m.valid(actor,0x220) &&
         (m.word(0x800adb3c)&0x800001u) &&
         m.bytes[(actor&0x1fffffffu)+0x1d8] &&
         m.bytes[(actor&0x1fffffffu)+0x1d9]==1) {
        const auto arena=m.word(actor+32);
        if(m.valid(arena,20)) {
          const uint32_t next=m.word(arena),end=m.word(arena+8);
          for(unsigned b=0;b<2;++b) {
            const auto begin=m.word(arena+12+b*4);
            if(next<begin || next>end || next-begin>65536 || !m.valid(begin,next-begin)) continue;
            for(auto q=begin;q+16<=next;q+=4) {
              if(m.word(q)>>24!=3 || m.word(q+4)!=0x6248a000 ||
                 m.word(q+8)!=0xffe6ffde || m.word(q+12)!=0x00340045) continue;
              r::MgsSurfaceEffect e;e.kind=SurfaceKind::RadarJamming;e.actor=actor&0x1fffffffu;
              e.parameters[0]={float(e.kind),time,0,0};
              pending.emplace((q+4)&0x1fffffffu,BoundSurface{e,0x48a000,0,0,0x62});
            }
          }
        }
      }
      if(name=="menuman.c" && m.valid(actor,36)) {
        // radiofacedraw owns two 52x89 video planes and a native rolling
        // scan beam. Authenticate their complete FT4/UV/CLUT layout inside
        // the live menu arena; atlas coordinates alone are not an identity.
        const auto arena=m.word(actor+32);
        if(m.valid(arena,20)) {
          const uint32_t next=m.word(arena),end=m.word(arena+8);
          for(unsigned b=0;b<2;++b) {
            const auto begin=m.word(arena+12+b*4);
            if(next<begin || next>end || next-begin>65536 || !m.valid(begin,next-begin)) continue;
            for(auto q=begin;q+40<=next;q+=4) {
              if(m.word(q)>>24!=9 || m.word(q+4)>>24!=0x2e) continue;
              const uint32_t clut=m.half(q+14),page=m.half(q+22);
              const uint32_t portrait=(clut>>6)&1u;
              if((clut&63u)!=48u || (clut>>6)<256u || (clut>>6)>259u ||
                 page!=((clut>>6)>=258u ? 0xbeu : 0xbfu)) continue;
              const uint32_t uv=portrait*64u+(80u<<8);
              if(m.half(q+12)!=uv || m.half(q+20)!=uv+52 ||
                 m.half(q+28)!=uv+(89u<<8) || m.half(q+36)!=uv+52+(89u<<8)) continue;
              const int x=int16_t(m.half(q+8)),y=int16_t(m.half(q+10));
              const int w=int16_t(m.half(q+16))-x,h=int16_t(m.half(q+26))-y;
              if(w<=0 || w>52 || h<=0 || h>89 || y<30 || y+h>119 ||
                 (portrait==0 ? x<31 || x+w>83 : x<237 || x+w>289) ||
                 int16_t(m.half(q+18))!=y || int16_t(m.half(q+24))!=x ||
                 m.word(q+32)!=(uint32_t(uint16_t(x+w))|(uint32_t(uint16_t(y+h))<<16))) continue;
              r::MgsSurfaceEffect e;e.kind=SurfaceKind::CodecVideo;e.actor=actor&0x1fffffffu;
              e.parameters[0]={float(e.kind),time,0,0};
              pending.emplace((q+4)&0x1fffffffu,BoundSurface{e,m.word(q+4)&0xffffffu,page,clut,0x2e});
            }
          }
        }
      }
      if(name=="mosaic.c" && m.valid(actor,108) && m.word(actor+96)==1) {
        // USA has a real censorship renderer, unlike Integral's empty stub.
        // Its retail allocation contains two FT4 arrays and two black TILEs.
        const uint32_t p=m.word(actor+72),nx=m.word(actor+80),ny=m.word(actor+84);
        if(nx>0 && nx<=32 && ny>0 && ny<=32 && m.valid(p,64) &&
           m.word(actor+88)>0 && m.word(actor+92)>0) {
          r::MgsSurfaceEffect e;e.kind=SurfaceKind::Mosaic;e.actor=actor&0x1fffffffu;
          e.parameters[0]={float(e.kind),time,float(int16_t(m.half(actor+104))),float(int16_t(m.half(actor+106)))};
          e.parameters[2]={float(m.word(actor+88)),float(m.word(actor+92)),0,float(e.actor)};
          for(unsigned b=0;b<2;++b) {
            const uint32_t packs=m.word(p+56+b*4),tile=p+24+b*16;
            if(!m.valid(packs,uint64_t(nx)*ny*40) || m.word(tile+4)!=0x60000000) continue;
            bool valid=true;
            for(unsigned i=0;i<nx*ny;++i)
              valid&=(m.word(packs+i*40+4)&0xfcffffffu)==0x2c808080;
            if(!valid) continue;
            pending.emplace((tile+4)&0x1fffffffu,BoundSurface{e,0,0,0,0x60});
            for(unsigned i=0;i<nx*ny;++i) {
              const auto q=packs+i*40;
              pending.emplace((q+4)&0x1fffffffu,BoundSurface{e,0x808080,m.half(q+22),m.half(q+14),m.word(q+4)>>24});
            }
          }
        }
      }
      if(name=="psyco.c" && m.valid(actor,0xa10)) {
        // Retail s07b 0x800d42b0 builds these two HIDEO backplates.
        // Only a submitted, owned black plate activates signal loss.
        for(unsigned b=0;b<2;++b) {
          const auto q=actor+0x9d4+b*24;
          if(m.word(q+4)!=0x28000000 || m.word(q+8)!=0 || m.word(q+12)!=320 ||
             m.word(q+16)!=(224u<<16) || m.word(q+20)!=((224u<<16)|320)) continue;
          r::MgsSurfaceEffect e;e.kind=SurfaceKind::SignalLoss;e.actor=actor&0x1fffffffu;
          e.parameters[0]={float(e.kind),time,0,0};
          pending.emplace((q+4)&0x1fffffffu,BoundSurface{e,0,0,0,0x28});
          for(unsigned offset:{0x934u,0x984u}) {
            const auto logo=actor+offset+b*40;
            if(m.word(logo+4)==0x2c808080)
              pendingInterface.emplace((logo+4)&0x1fffffffu,InterfaceShape{actor,0x2c,320});
          }
        }
      }
      if((name=="sphere.c" || name=="sphere2.c") && m.valid(actor,108)) {
        const uint32_t p=m.word(actor+32),img=m.word(actor+36);
        const auto scene=latestSceneSnapshot();
        if(scene && m.valid(img,20) && m.valid(p,84) && m.word(p+36)==0x812 &&
           m.half(p+48)==40 && m.half(p+42)>0 && m.half(p+42)<=1024) {
          const unsigned width=m.half(img),height=m.half(img+2),tw=m.half(img+4),th=m.half(img+6);
          const uint32_t ids=m.word(img+8),attrs=m.word(img+12),map=m.word(img+16);
          if(width && height && tw && th && tw<=256 && th<=256 && width%tw==0 && height%th==0 &&
             m.valid(ids,2) && m.half(ids)>0 && m.half(ids)<=16 &&
             m.valid(ids,2*(m.half(ids)+1)) && m.valid(attrs,4) && m.valid(map,(width/tw)*(height/th))) {
            double yaw=std::atan2(scene->view[8],scene->view[10]);
            if(yaw<0) yaw+=2*3.141592653589793;
            const double x=yaw*width/(2*3.141592653589793);
            const double y=-scene->view[9]*285.0/std::sqrt(std::max(1e-7,1.0-double(scene->view[9])*scene->view[9]));
            for(unsigned b=0;b<2;++b) {
              const auto packs=m.word(p+64+b*4);
              if(!m.valid(packs,m.half(p+42)*40u)) continue;
              for(unsigned i=0;i<m.half(p+42);++i) {
                const auto q=packs+i*40;
                if(m.word(q+4)>>24!=0x2c) continue;
                const unsigned dx=uint16_t(m.half(q+16)-m.half(q+8));
                const unsigned dy=uint16_t(m.half(q+26)-m.half(q+10));
                if(dx!=tw || dy!=th) continue;
                bool texture=false;
                for(unsigned t=0;t<m.half(ids);++t) {
                  const auto tex=m.word(actor+40+t*4);
                  texture|=m.valid(tex,12) && m.half(tex)==m.half(ids+2+t*2) &&
                           m.half(tex+4)==m.half(q+22) && m.half(tex+6)==m.half(q+14);
                }
                if(!texture) continue;
                r::MgsSurfaceEffect e;e.kind=SurfaceKind::Panorama;e.actor=actor&0x1fffffffu;
                e.parameters[0]={float(e.kind),time,0,0};
                BoundSurface bound{e,m.word(q+4)&0xffffffu,m.half(q+22),m.half(q+14),0x2c};
                bound.screenShift={float(x-std::floor(x)),float(std::trunc(y)-y)};
                bound.tileExtent={float(tw),float(th)};
                pending.emplace((q+4)&0x1fffffffu,bound);
              }
            }
          }
        }
      }
      if(name=="mirror.c" && m.valid(actor,2592) && m.word(actor+48)<3 &&
         m.word(actor+56)<=8 && m.word(actor+732)<=8) {
        const auto scene=latestSceneSnapshot();
        if(scene) {
          const unsigned axis=m.word(actor+48);
          const float plane=int16_t(m.half(actor+40+axis*2));
          float eye=0;
          for(unsigned row=0;row<3;++row) eye-=scene->view[axis*4+row]*scene->view[12+row];
          const float sign=eye>=plane ? 1.f : -1.f;
          r::MgsSurfaceEffect e;e.kind=SurfaceKind::Mirror;e.actor=actor&0x1fffffffu;
          e.parameters[0]={float(e.kind),time,0,0};
          e.parameters[1][axis]=sign;e.parameters[1][3]=plane*sign;
          e.parameters[2][3]=float(e.actor);
          const auto add=[&](uint32_t object) {
            if(m.valid(object,72) && m.valid(m.word(object+36),32))
              addObjectEffect(pendingObjects,object,e);
          };
          for(unsigned i=0;i<m.word(actor+56);++i) add(m.word(actor+60+i*84+8));
          for(unsigned i=0;i<m.word(actor+732);++i) add(actor+740+i*232);
        }
      }
      struct ObjectAdapter { std::string_view name; uint16_t offset; SurfaceKind kind; };
      static constexpr ObjectAdapter objectAdapters[] = {
        {"shadow.c",40,SurfaceKind::Shadow},
        {"bunsin2.c",32,SurfaceKind::Afterimage},
        {"mgrexlzr.c",56,SurfaceKind::Energy},
        {"mg_laser.c",0x1a4,SurfaceKind::Energy},
        {"lit_mdl.c",36,SurfaceKind::VolumeLight},
        {"optxtscn.c",68,SurfaceKind::Monitor}, {"optxtscn.c",72,SurfaceKind::Monitor},
        {"p_sphere.c",32,SurfaceKind::Energy}, {"p_sphere.c",0x104,SurfaceKind::Energy}
      };
      for(const auto &a:objectAdapters) if(name==a.name && m.valid(actor+a.offset,4)) {
        const uint32_t object=m.word(actor+a.offset);
        if(!m.valid(object,72) || !m.valid(m.word(object+36),32)) continue;
        r::MgsSurfaceEffect effect;effect.kind=a.kind;effect.actor=actor&0x1fffffffu;
        effect.parameters[0]={float(a.kind),time,0,0};
        effect.parameters[2][3]=float(effect.actor);
        addObjectEffect(pendingObjects,object,effect);
      }
      if ((name=="envmap3.c" || name=="sub_efct.c" || name=="kogaku2.c") &&
          m.valid(actor,72)) {
        const auto parent=m.word(actor+32);
        if (m.valid(parent,4) && m.valid(m.word(parent),72)) {
          const uint32_t object=m.word(parent);
          r::MgsSurfaceEffect effect;
          effect.actor=actor&0x1fffffffu;
          effect.parameters[0][1]=time;
          effect.parameters[2][3]=float(effect.actor);
          if(name=="kogaku2.c" && !(m.word(0x800adb3c)&8u)) {
            effect.kind=SurfaceKind::Camo;
            effect.parameters[0][2]=float(int32_t(m.word(actor+44)));
          } else if(name=="envmap3.c") {
            const auto tex=m.word(actor+40);
            if(m.valid(tex,12)) {
              effect.kind=SurfaceKind::Environment;
              const auto *t=bytes.data()+(tex&0x1fffffffu);
              effect.parameters[1]={float(t[8]),float(t[9]),float(t[8]+t[10]),float(t[9]+t[11])};
              effect.parameters[2][0]=float(m.half(tex));
            }
          } else if(name=="sub_efct.c") {
            const uint32_t rotation=m.word(actor+36);
            if(m.valid(rotation,8)) {
              effect.kind=SurfaceKind::Submarine;
              const float angle=float(int16_t(m.half(rotation+4)))*6.28318530718f/4096.f;
              effect.parameters[2][0]=std::cos(angle);
              effect.parameters[2][1]=std::sin(angle);
              effect.parameters[0][3]=1.f;
            }
          }
          if(effect.kind!=SurfaceKind::None) {
            addObjectEffect(pendingObjects,object,effect);
          }
        }
      }
      if(name=="snow.c" && m.valid(actor,72) && m.word(actor+64)<=32 &&
         m.valid(actor+72,uint64_t(m.word(actor+64))*292)) {
        for(unsigned i=0;i<m.word(actor+64);++i)
          primitive(m,actor,m.word(actor+72+i*292+32),SurfaceKind::Snow,time,pending);
      }
      if (name == "anime.c" && m.valid(actor, 76)) {
        const uint32_t count = m.half(actor+66), p = m.word(actor+36);
        if (count && count <= 1024 && m.valid(actor+76, uint64_t(count)*52) &&
            m.valid(p, 84) && m.half(p+42) == count &&
            m.word(p+56) == m.word(actor+72) && m.word(p+60) == actor+40) {
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t unit = actor+76+i*52;
            const auto kind = animationKind(m, actor, unit);
            if (!kind) { metrics::count("mgs.effects.animation.unknown"); continue; }
            const float frame = float(m.word(unit+4)) / std::max(1.f, float(m.half(actor+62)));
            primitive(m, actor, p, *kind, time, pending, int(i), frame);
            metrics::count("mgs.effects.animation.identified");
          }
        }
      } else {
        struct Adapter { std::string_view name; uint16_t offset; SurfaceKind kind; };
        // Offsets from the pinned actor Work definitions; packet, allocation
        // and position checks below are mandatory for each concrete instance.
        static constexpr Adapter adapters[] = {
          {"bub_d_sn.c",40,SurfaceKind::Bubble}, {"bub_d_sn.c",44,SurfaceKind::Bubble},
          {"bub_d_sn.c",48,SurfaceKind::Bubble}, {"spark.c",36,SurfaceKind::Spark},
          {"bubble_p.c",40,SurfaceKind::Bubble}, {"bubble_t.c",36,SurfaceKind::Bubble},
          {"bubble_s.c",40,SurfaceKind::Bubble}, {"bubble_s.c",44,SurfaceKind::Bubble},
          {"bubble_s.c",48,SurfaceKind::Bubble}, {"bubble_s.c",52,SurfaceKind::Bubble},
          {"plasma.c",48,SurfaceKind::Electricity}, {"blood_bl.c",32,SurfaceKind::Liquid},
          {"blooddrp.c",36,SurfaceKind::Blood}, {"splash.c",36,SurfaceKind::Droplet},
          {"splash2.c",36,SurfaceKind::Bubble}, {"splash3.c",36,SurfaceKind::Bubble},
          {"gsplash.c",36,SurfaceKind::Glass}, {"emitter.c",32,SurfaceKind::Glow},
          {"lamp.c",32,SurfaceKind::Glow}, {"asiato.c",32,SurfaceKind::Footprint}, {"asiato2.c",32,SurfaceKind::Footprint},
          {"wsurface.c",40,SurfaceKind::Water}, {"cape.c",32,SurfaceKind::Cloth},
          {"windcrcl.c",36,SurfaceKind::Wind},
          {"ripple.c",36,SurfaceKind::Ripple},
          {"plasma_h.c",40,SurfaceKind::Electricity}, {"plasma_l.c",40,SurfaceKind::Electricity},
          {"blood.c",36,SurfaceKind::Blood}, {"d_bloodr.c",32,SurfaceKind::Blood},
          {"blood_cl.c",32,SurfaceKind::Liquid}, {"d_bloods.c",32,SurfaceKind::Liquid},
          {"pfall.c",36,SurfaceKind::Debris}, {"glass.c",64,SurfaceKind::Glass},
          {"glight.c",32,SurfaceKind::Muzzle}, {"famaslit.c",36,SurfaceKind::Muzzle},
          {"eyeflash.c",40,SurfaceKind::Glow}, {"pato_lmp.c",0x1008,SurfaceKind::Glow},
          {"searchli.c",0x254,SurfaceKind::Beam}, {"katana.c",36,SurfaceKind::Beam},
          {"demokage.c",36,SurfaceKind::Shadow},
          {"l_sight.c",32,SurfaceKind::Beam}, {"ir_cens.c",40,SurfaceKind::Beam},
          {"revbullt.c",164,SurfaceKind::Beam}
          ,{"spark2.c",36,SurfaceKind::Spark}, {"spark2l.c",36,SurfaceKind::Spark},
          {"spark2_m.c",36,SurfaceKind::Spark},
          {"mg_smke1.c",36,SurfaceKind::Smoke}, {"mg_smke2.c",36,SurfaceKind::Smoke},
          {"mg_laser.c",0x134,SurfaceKind::Glow}, {"mg_laser.c",0x138,SurfaceKind::Glow},
          {"jbullet.c",140,SurfaceKind::Beam}, {"jblood.c",36,SurfaceKind::Blood},
          {"jspark.c",36,SurfaceKind::Spark}, {"gsplash3.c",36,SurfaceKind::Glass},
          {"jdrumsmk.c",36,SurfaceKind::Smoke}, {"jdrumfir.c",36,SurfaceKind::Explosion},
          {"jlamp.c",56,SurfaceKind::Glow}, {"jlamp2.c",40,SurfaceKind::Glow}
          ,{"amissile.c",0x134,SurfaceKind::Muzzle}, {"rmissile.c",0x2d8,SurfaceKind::Muzzle},
          {"bullet.c",140,SurfaceKind::Beam}, {"hind_bul.c",0xf8,SurfaceKind::Beam},
          {"hd_bul2.c",0xf8,SurfaceKind::Beam}, {"socom.c",0x58,SurfaceKind::Beam},
          {"camera.c",0x194,SurfaceKind::Glow}, {"camera2.c",0x1b4,SurfaceKind::Glow},
          {"guncame.c",0x328,SurfaceKind::Glow}
          ,{"wolf_eye.c",164,SurfaceKind::Glow}, {"wolf_eye.c",168,SurfaceKind::Glow},
          {"blink_tx.c",32,SurfaceKind::Authored}, {"valcan.c",0x6f4,SurfaceKind::Shadow},
          {"blastoff.c",36,SurfaceKind::Explosion}, {"m1e1shel.c",36,SurfaceKind::Debris}
          ,{"panel.c",36,SurfaceKind::Monitor},
          {"evpanel.c",32,SurfaceKind::Monitor}, {"evpanel.c",36,SurfaceKind::Monitor},
          {"mg_room.c",32,SurfaceKind::Monitor}, {"mg_room.c",92,SurfaceKind::Monitor},
          {"sub_room.c",32,SurfaceKind::Monitor}, {"sub_room.c",100,SurfaceKind::Monitor},
          {"sub_room.c",1128,SurfaceKind::Monitor}, {"sub_room.c",1412,SurfaceKind::Glow}
        };
        for (const auto &adapter : adapters)
          if (name == adapter.name && m.valid(actor, adapter.offset+4u))
            primitive(m, actor, m.word(actor+adapter.offset), adapter.kind, time, pending);
        if (name == "stngrnd.c" && m.valid(actor, 0xf8) &&
            m.word(actor+0xe8) >= 1 && m.word(actor+0xe8) <= 15 &&
            m.word(actor+0xf4) == 1) {
          // USA live allocation agrees with stngrnd.c GetResources: eight
          // camera-space refrection6 planes with owned centers and RECTs.
          // Authenticate the complete array before adapting any member.
          constexpr std::array<unsigned,8> sizes{8,34,6,25,12,70,11,50};
          bool layout = true;
          for (unsigned i=0; i<sizes.size() && layout; ++i) {
            const auto p=m.word(actor+0x60+i*4), rect=actor+0x20+i*8;
            layout=m.valid(p,84) && m.word(p+36)==0x612 && m.half(p+42)==1 &&
                   m.word(p+56)==actor+0xa0+i*8 && m.word(p+60)==rect &&
                   m.half(rect)==sizes[i]/2 && m.half(rect+2)==sizes[i]/2 &&
                   m.half(rect+4)==sizes[i] && m.half(rect+6)==sizes[i];
          }
          if (layout) for (unsigned i=0; i<sizes.size(); ++i)
            primitive(m,actor,m.word(actor+0x60+i*4),SurfaceKind::Glow,time,pending);
        }
        if(name=="p_lamp.c" || name=="inverlt2.c") {
          const unsigned first=name=="p_lamp.c" ? 32 : 36;
          const unsigned count=name=="p_lamp.c" ? 32 : 8;
          if(m.valid(actor+first,count*4)) for(unsigned i=0;i<count;++i)
            primitive(m,actor,m.word(actor+first+i*4),SurfaceKind::Glow,time,pending);
        }
      }
      previous = actor;
      actor = m.word(actor+4);
    }
    if (actor == tail && m.word(tail) == previous && m.word(tail+4) == 0) {
      interfaceShapes.insert(pendingInterface.begin(),pendingInterface.end());
      if(candidatePhoto) photo=candidatePhoto;
      if(pendingFog[2]) fog=pendingFog;
      if(pendingPalette) paletteMode=pendingPalette;
      for (auto &[key, value] : pending) {
        auto [it, inserted] = surfaces.emplace(key, value);
        if (!inserted) it->second.effect.kind = SurfaceKind::None;
      }
      for(auto &[key,value]:pendingObjects) {
        addObjectEffect(objects,key,value);
      }
    }
  }
  if(!photo) {photoActor=0;photoArmed=false;}
  else {
    const auto [actor,state,mode]=*photo;
    if(actor!=photoActor) {photoActor=actor;photoArmed=false;}
    if(mode==0 || (mode==1 && state<4)) photoArmed=true;
    if(mode==1 && state==4 && photoArmed && (m.word(0x800adb3c)&0x400)) {
      pendingPhoto=++photoSerial;photoArmed=false;
      metrics::count("mgs.effects.photo.shutter");
    }
  }
}
uint64_t consumePhotoCaptureEvent() {
  const auto event=pendingPhoto;pendingPhoto=0;return event;
}
void bindInterfaceGeometry(r::DrawPacket &packet,uint32_t commandAddress) {
  if(!active() || packet.route!=r::PacketRoute::Screen2D || commandAddress==UINT32_MAX) return;
  const auto found=interfaceShapes.find(commandAddress&0x1fffffffu);
  if(found==interfaceShapes.end() || found->second.opcode!=packet.opcode) return;
  if(packet.surfaceEffect.kind==SurfaceKind::None) {
    packet.surfaceEffect.kind=SurfaceKind::Authored;
    packet.surfaceEffect.actor=found->second.actor&0x1fffffffu;
    packet.surfaceEffect.parameters[0][0]=float(SurfaceKind::Authored);
  }
  if(widescreen16x9()) {
    const double pivot=packet.state.drawOffset.x+found->second.xOffset;
    bool black=!packet.textured;
    for(unsigned k=0;k<packet.vertexCount;++k) {
      const auto &c=packet.vertices[k].color;black&=!(c.r|c.g|c.b);
    }
    for(unsigned k=0;k<packet.vertexCount;++k) {
      auto &v=packet.vertices[k];
      // Extend the exterior of opaque sight masks to the viewport edge.
      // Their aperture and the reticle keep the authored 4:3 proportions.
      if(black && !packet.state.blend.enabled &&
         (v.x<=packet.state.drawArea.left || v.x>=packet.state.drawArea.right+1)) continue;
      v.x=pivot+(v.x-pivot)*0.75;
    }
  }
  metrics::count("mgs.presentation.source_sgt_draws");
}
bool hasSourceSurfaceGeometry(uint32_t commandAddress) {
  const auto found=surfaces.find(commandAddress&0x1fffffffu);
  return active() && commandAddress!=UINT32_MAX && found!=surfaces.end() &&
         found->second.sourceShape && found->second.effect.kind!=SurfaceKind::None;
}

void bindSurfaceEffect(r::DrawPacket &packet, uint32_t commandAddress) {
  if(active() && packet.route==r::PacketRoute::Projected3D &&
     (packet.originalMesh || packet.validMask)) packet.surfaceEffect.parameters[3]=fog;
  if(active()) packet.surfaceEffect.parameters[3][3]=paletteMode;
  if (!active() || commandAddress == UINT32_MAX || packet.vertexCount != 4 ||
      packet.state.mask.checkMaskBit) return;
  const auto found = surfaces.find(commandAddress & 0x1fffffffu);
  if (found == surfaces.end()) return;
  const auto &s = found->second;
  const auto &c = packet.vertices[0].color;
  if (s.effect.kind == SurfaceKind::None || s.opcode != packet.opcode ||
      (packet.textured && (s.page != (packet.state.texturePage.raw & 0x1ffu) || s.clut != packet.state.clut.raw)) ||
      s.color != (uint32_t(c.r) | uint32_t(c.g)<<8 | uint32_t(c.b)<<16)) return;
  auto e = s.effect;
  e.parameters[3]=packet.surfaceEffect.parameters[3];
  if(e.kind==SurfaceKind::Panorama) {
    // SORTONLY is camera-oriented affine background geometry, even when
    // generic GP0 classification had conservatively called it unproven 3D.
    packet.route=r::PacketRoute::Screen2D;packet.validMask=0;
    packet.semanticReason=r::PacketSemanticReason::DisplayAnchoredComposite;
    packet.fallbackReason=r::PacketFallbackReason::None;
    packet.state.depth.testEnabled=packet.state.depth.writeEnabled=false;
    packet.state.depth.invalidateOnAccepted=true;packet.state.depth.compare=r::DepthCompare::Always;
    const double u=packet.vertices[0].u,v=packet.vertices[0].v;
    for(unsigned k=0;k<4;++k) {
      auto &corner=packet.vertices[k];
      corner.x+=s.screenShift[0];corner.y+=s.screenShift[1];
      corner.u=u+(k&1)*s.tileExtent[0];corner.v=v+(k>>1)*s.tileExtent[1];
      corner.w=r::CRASH_REPLAY_FALLBACK_W;corner.rawDepth=corner.nearDepth=0;
      corner.preciseXY=corner.preciseX=corner.preciseY=corner.preciseUV=false;
      corner.rawDepthValid=corner.inventedW=corner.nearSaturated=false;
      corner.reconciledX=corner.reconciledY=corner.homogeneousQ=false;
      corner.provenance={};
    }
    packet.state.textureUvDomain.minU=u;packet.state.textureUvDomain.minV=v;
    packet.state.textureUvDomain.maxU=u+s.tileExtent[0]-1;
    packet.state.textureUvDomain.maxV=v+s.tileExtent[1]-1;
    packet.state.textureUvDomain.enabled=true;
  }
  if(s.sourceShape) {
    packet.originalMesh.reset();
    if(!s.authoredUv) packet.originalTexture.reset();
    packet.validMask=15;packet.route=r::PacketRoute::Projected3D;
    packet.semanticReason=r::PacketSemanticReason::PolygonDefault;
    packet.fallbackReason=r::PacketFallbackReason::None;
    packet.state.depth.testEnabled=false;
    packet.state.depth.writeEnabled=!packet.state.blend.enabled;
    packet.state.depth.invalidateOnAccepted=packet.state.blend.enabled;
    packet.state.depth.compare=r::DepthCompare::Always;
    e.parameters[3]=fog;e.parameters[3][3]=paletteMode;
    for(unsigned k=0;k<4;++k) {
      auto &v=packet.vertices[k];const auto &p=s.view[k];
      v.w=v.rawDepth=p[2];v.nearDepth=1;
      v.x=p[0]*s.projection/p[2]+packet.state.drawOffset.x;
      v.y=p[1]*s.projection/p[2]+packet.state.drawOffset.y;
      v.qx=v.x*v.w;v.qy=v.y*v.w;
      // blastoff supplies real animated texture coordinates for its source
      // billboards. Only generated line/tile/ripple domains are synthetic.
      if(!s.authoredUv) {v.u=(k&1)*128;v.v=(k>>1)*128;}
      v.preciseXY=v.preciseX=v.preciseY=v.preciseUV=v.rawDepthValid=v.homogeneousQ=true;
      v.inventedW=v.reconciledX=v.reconciledY=v.nearSaturated=false;
    }
  }
  std::array<float,4> uv{1e9f,1e9f,-1e9f,-1e9f};
  for (const auto &v : packet.vertices) {
    if (!std::isfinite(v.u) || !std::isfinite(v.v)) return;
    uv[0] = std::min(uv[0], float(v.u)); uv[1] = std::min(uv[1], float(v.v));
    uv[2] = std::max(uv[2], float(v.u)); uv[3] = std::max(uv[3], float(v.v));
  }
  if (e.kind==SurfaceKind::Mosaic || e.kind==SurfaceKind::RadarJamming || e.kind==SurfaceKind::SignalLoss) uv={0,0,1,1};
  if (uv[2]<=uv[0] || uv[3]<=uv[1]) return;
  e.parameters[1] = uv;
  if(!s.sourceShape && (e.kind==SurfaceKind::Beam || e.kind==SurfaceKind::Spark || e.kind==SurfaceKind::Electricity)) {
    const auto &a=packet.vertices[0];
    const auto length=[&](unsigned k) {
      const auto &b=packet.vertices[k];return std::hypot(b.x-a.x,b.y-a.y);
    };
    const auto &b=packet.vertices[length(1)>=length(2) ? 1 : 2];
    e.parameters[2][1]=std::abs(b.u-a.u)>std::abs(b.v-a.v) ? 1.f : 0.f;
  }
  e.parameters[2][3] = float(e.actor);
  if(s.lighting && packet.originalMesh) {
    const auto positions=packet.originalMesh->lighting.positions;
    packet.originalMesh->lighting=*s.lighting;
    packet.originalMesh->lighting.positions=positions;
    for(auto &v:packet.vertices) v.color={128,128,128};
  }
  // Bubble plumes retain authored additive/quarter-additive energy. Opaque
  // alpha bubbles become a solid mass when the original clusters overlap.
  packet.surfaceEffect = e;
  if (e.kind == SurfaceKind::Explosion) {
    metrics::trace("mgs.effects.explosion", {
        {"actor",e.actor},{"command",commandAddress & 0x1fffffffu},
        {"ordering_raise",int64_t(e.parameters[2][0])},
        {"radius_x",int64_t(e.parameters[2][1])},
        {"radius_y",int64_t(e.parameters[2][2])},
        {"source_geometry",packet.originalMesh.has_value()},
        {"source_artwork",packet.originalTexture.has_value()}});
  }
  metrics::count("mgs.effects.surface.bound_packets");
  metrics::trace("mgs.effects.surface", {{"kind",int64_t(e.kind)}, {"actor",e.actor},
                                        {"command",commandAddress & 0x1fffffffu}});
}
 r::MgsSurfaceEffect surfaceEffectForObject(uint32_t object) {
  const auto found=objects.find(object&0x1fffffffu);
  auto effect=active() && found!=objects.end() ? found->second : r::MgsSurfaceEffect{};
  if(active()) effect.parameters[3]=fog;
  if(active()) effect.parameters[3][3]=paletteMode;
  return effect;
}
} // namespace ps1::games::mgs
