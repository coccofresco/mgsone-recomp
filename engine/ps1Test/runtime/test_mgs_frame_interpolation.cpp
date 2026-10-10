#include <gtest/gtest.h>
#include "runtime/games/mgs_frame_interpolation.h"
using namespace ps1::gpu::replay;
using ps1::games::mgs::FrameInterpolator;
namespace {
VulkanReplayBakedDraw face(unsigned id,unsigned frame,float x,unsigned page=0) {
  VulkanReplayBakedDraw d;
  d.sourceTopology=true; d.segmentCount=1;
  d.mgsTemporalIdentity={1,1,5,id}; d.mgsTemporalFrame=frame;
  d.mgsTemporalCamera={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  d.scissor={0,page,320,224};
  auto &u=d.segments[0].uniforms;
  u.mgsProjection={320,160,float(page+112),1};
  u.mgsViewRows[0]={1,0,0,x};
  return d;
}
VulkanReplayBakedBatch batch(std::initializer_list<VulkanReplayBakedDraw> faces) {
  VulkanReplayBakedBatch b; b.internalScale=1;
  for (auto &d:faces) { VulkanReplayBakedCommand c; c.payload=d; b.commands.push_back(c); }
  return b;
}
}
TEST(MgsFrameInterpolation, MatchesProducerIdentityAcrossReorderingAndPageFlips) {
  FrameInterpolator h;
  ASSERT_FALSE(h.advance(batch({face(1,10,0),face(2,10,100)}),10));
  auto p=h.advance(batch({face(2,12,120,256),face(1,12,20,256)}),12);
  ASSERT_TRUE(p); EXPECT_EQ(p->steps,2); EXPECT_EQ(p->pageY,256);
  ASSERT_EQ(p->phases[0].size(),2);
  EXPECT_FLOAT_EQ(p->phases[0][0].mgsViewRows[0][3],110);
  EXPECT_FLOAT_EQ(p->phases[0][1].mgsViewRows[0][3],10);
  EXPECT_FLOAT_EQ(p->phases[0][1].mgsProjection[2],368);
}
TEST(MgsFrameInterpolation, NativeTwentyHzProducesTwoSourceGeometryPhases) {
  FrameInterpolator h; h.advance(batch({face(1,10,0)}),10);
  auto p=h.advance(batch({face(1,13,90)}),13); ASSERT_TRUE(p);
  EXPECT_EQ(p->steps,3);
  EXPECT_FLOAT_EQ(p->phases[0][0].mgsViewRows[0][3],30);
  EXPECT_FLOAT_EQ(p->phases[1][0].mgsViewRows[0][3],60);
}
TEST(MgsFrameInterpolation, ReusedObjectLifetimeAndUnmatchedFacesSnap) {
  FrameInterpolator h; h.advance(batch({face(1,10,0),face(2,10,0)}),10);
  auto recycled=face(2,12,80); recycled.mgsTemporalIdentity[1]=2;
  auto p=h.advance(batch({face(1,12,20),recycled,face(3,12,160)}),12);
  ASSERT_TRUE(p); EXPECT_EQ(p->matchedFaces,1);
  EXPECT_FLOAT_EQ(p->phases[0][1].mgsViewRows[0][3],80);
  EXPECT_FLOAT_EQ(p->phases[0][2].mgsViewRows[0][3],160);
}
TEST(MgsFrameInterpolation, CutsEpochsGapsAndRewindsAreBarriers) {
  for (unsigned kind=0;kind<5;++kind) {
    FrameInterpolator h; h.advance(batch({face(1,10,0)}),10);
    auto d=face(1,12,20); unsigned frame=12;
    if (kind==0) d.mgsTemporalIdentity[0]++;
    if (kind==1) d.mgsTemporalCamera[0]=-1;
    if (kind==2) d.mgsTemporalCamera[12]=3000;
    if (kind==3) frame=d.mgsTemporalFrame=15;
    if (kind==4) frame=d.mgsTemporalFrame=8;
    EXPECT_FALSE(h.advance(batch({d}),frame));
  }
}
TEST(MgsFrameInterpolation, DuplicateIdentityAndMixedSnapshotsFailClosed) {
  FrameInterpolator h; h.advance(batch({face(1,10,0)}),10);
  EXPECT_FALSE(h.advance(batch({face(1,12,20),face(1,12,20)}),12));
  h.reset(); h.advance(batch({face(1,10,0)}),10);
  EXPECT_FALSE(h.advance(batch({face(1,12,20),face(2,11,20)}),12));
  EXPECT_FALSE(h.advance(batch({face(1,14,40)}),14));
}
TEST(MgsFrameInterpolation, TwoDimensionalDrawEndsHistoryButEmptyLatchDoesNot) {
  FrameInterpolator h; h.advance(batch({face(1,10,0)}),10);
  h.advance(batch({}),11);
  EXPECT_TRUE(h.advance(batch({face(1,12,20)}),12));
  auto ui=face(1,13,0); ui.sourceTopology=false;
  h.advance(batch({ui}),13);
  EXPECT_FALSE(h.advance(batch({face(1,14,40)}),14));
}
