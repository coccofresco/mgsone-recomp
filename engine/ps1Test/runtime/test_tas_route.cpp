#include "runtime/input/tas_route.h"
#include <gtest/gtest.h>
#include <cstring>

using ps1::input::TasRoute;
namespace {
struct RouteFixture : testing::Test {
  std::vector<uint8_t> ram=std::vector<uint8_t>(2097152);
  void stage(const char* name) { std::memset(ram.data()+0xB7500,0,8); std::memcpy(ram.data()+0xB7500,name,std::strlen(name)); }
  void word(unsigned addr,uint32_t value) { for (unsigned i=0;i<4;++i) ram[addr+i]=uint8_t(value>>(8*i)); }
};
}
TEST_F(RouteFixture, ArmsOnStateAndReleasesImmediatelyWhenGoalArrives) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP arrive 100 1\nWHEN stage eq s00a 4294967295\nUNTIL story eq 28 4294967295\nINPUT 80 32768 8\nEND\n",error)) << error;
  stage("title");
  EXPECT_EQ(route.sample(ram.data(),ram.size(),1)[0],0);
  stage("s00a");
  EXPECT_EQ(route.sample(ram.data(),ram.size(),2),(std::array<uint16_t,2>{32768,8}));
  ram[0xB644A]=28;
  EXPECT_EQ(route.sample(ram.data(),ram.size(),3),(std::array<uint16_t,2>{0,0}));
  EXPECT_TRUE(route.done()); EXPECT_FALSE(route.failed());
}
TEST_F(RouteFixture, StopsOnGuardAndBudgetWithoutAdvancingGameplay) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP fight 3 1\nGUARD life min 1 4294967295\nUNTIL story eq 29 4294967295\nINPUT 20 32768 0\nEND\n",error));
  EXPECT_EQ(route.sample(ram.data(),ram.size(),1)[0],0);
  EXPECT_TRUE(route.failed());
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP wait 3 1\nUNTIL story eq 29 4294967295\nEND\n",error));
  for (int i=0;i<3;++i) route.sample(ram.data(),ram.size(),i+1);
  EXPECT_FALSE(route.failed());
  route.sample(ram.data(),ram.size(),4); EXPECT_TRUE(route.failed());
}
TEST_F(RouteFixture, ShortActionsRespectAttemptCountAndBothPorts) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP repeat 20 2\nUNTIL story eq 29 4294967295\nINPUT 2 8 16384\nINPUT 1 0 0\nEND\n",error));
  const std::array<int,8> expected{8,8,0,8,8,0,0,0};
  for (unsigned i=0;i<expected.size();++i) {
    auto pad=route.sample(ram.data(),ram.size(),i+1);
    EXPECT_EQ(pad[0],expected[i]); EXPECT_EQ(pad[1],expected[i]?16384:0);
  }
}
TEST_F(RouteFixture, SampledPadsFollowGameUpdatesAcrossVariableVideoCadence) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP sampled 100 1\nUNTIL story eq 29 4294967295\n"
    "CLOCK GAME\nINPUT 1 16 32768\nINPUT 2 128 0\nINPUT 1 0 8\nEND\n",error)) << error;
  word(0xADA48,100);
  for (int frame=1;frame<=3;++frame)
    EXPECT_EQ(route.sample(ram.data(),ram.size(),frame),(std::array<uint16_t,2>{16,32768}));
  word(0xADA48,101);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),4)[0],128);
  word(0xADA48,102);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),5)[0],128);
  word(0xADA48,103);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),6),(std::array<uint16_t,2>{0,8}));
  word(0xADA48,105); // never silently discard an unobserved input update
  EXPECT_EQ(route.sample(ram.data(),ram.size(),7),(std::array<uint16_t,2>{0,0}));
  EXPECT_TRUE(route.failed());
}
TEST_F(RouteFixture, MissingActorCannotSatisfyNegativeComparison) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP title 100 1\nUNTIL title_state ne 6 4294967295\nEND\n",error));
  stage("title"); route.sample(ram.data(),ram.size(),1); EXPECT_FALSE(route.done());
  word(0x10008,0x800D26E0); word(0x10014,0x80020000);
  std::memcpy(ram.data()+0x20000,"open.c",7);
  word(0x10A48,7);
  route.sample(ram.data(),ram.size(),2); EXPECT_TRUE(route.done());
}
TEST_F(RouteFixture, EffectCountsFollowCompleteActorListsAndRejectStaleAllocations) {
  for (unsigned level=0;level<9;++level) {
    const auto head=0xaf390+level*68;
    word(head+4,(head+32)|0x80000000);word(head+32,head|0x80000000);
  }
  constexpr uint32_t head=0xaf390+6*68,a=0x10000,b=0x10100;
  word(head+4,a|0x80000000);word(a,head|0x80000000);word(a+4,b|0x80000000);
  word(b,a|0x80000000);word(b+4,(head+32)|0x80000000);word(head+32,b|0x80000000);
  for (auto p : {a,b,0x10200u}) {
    word(p+8,0x80070000);word(p+20,0x80020000);
  }
  std::memcpy(ram.data()+0x20000,"bakudan.c",10);
  const auto original=ram;
  const auto observe=[&](const char* condition) {
    TasRoute route;std::string error;
    EXPECT_TRUE(route.parse(std::string("MGSROUTE 1\nSTEP effect 100 1\nUNTIL ")+condition+" 4294967295\nEND\n",error))<<error;
    route.sample(ram.data(),ram.size(),1);return route.done();
  };
  EXPECT_TRUE(observe("actor_bakudan_count eq 2"));
  EXPECT_TRUE(observe("actor_kogaku2_count eq 0"));
  EXPECT_EQ(ram,original);
  // A broken backwards link or a cycle invalidates absence as well as presence.
  word(b,0);EXPECT_FALSE(observe("actor_bakudan_count ne 0"));
  EXPECT_FALSE(observe("actor_kogaku2_count eq 0"));
  ram=original;word(b+4,a|0x80000000);EXPECT_FALSE(observe("actor_bakudan_count eq 2"));
  ram=original;word(head+32,head|0x80000000);EXPECT_FALSE(observe("actor_bakudan_count eq 2"));
  ram=original;word(a+20,0x801fffff);ram.back()='x';EXPECT_FALSE(observe("actor_bakudan_count ne 0"));
  ram=original;word(head+4,(head+32)|0x80000000);word(head+32,head|0x80000000);
  EXPECT_TRUE(observe("actor_bakudan_count eq 0")); // detached heap copies do not count
}
TEST_F(RouteFixture, ActorObservationNamesAreBoundedAndCannotReadArbitraryFields) {
  for (const auto name : {"actor__count","actor_kogaku2_timer","actor_../foo_count",
                          "actor_RedAlert_count"}) {
    TasRoute route;std::string error;
    EXPECT_FALSE(route.parse(std::string("MGSROUTE 1\nSTEP bad 10 1\nUNTIL ")+name+" eq 0 4294967295\nEND\n",error));
  }
}
TEST_F(RouteFixture, CameraAndPatrolNamesDoNotCollideAndCameraYawWraps) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP phase 100 1\n"
    "UNTIL camera_dd6d_facing eq 4066 4095\n"
    "UNTIL camera_dd6d_timer eq 65542 4294967295\n"
    "UNTIL watcher_dd6d_facing eq 99 4294967295\nEND\n",error)) << error;
  for (const auto base : {0x10000u,0x12000u}) {
    word(base,base|0x80000000); word(base+4,base|0x80000000);
    word(base+8,0x800c1000); word(base+20,(base+0x100)|0x80000000);
    word(base+0x50,0xdd6d);
    word(base+0x2a,base==0x10000 ? 99 : uint16_t(-30));
    std::strcpy(reinterpret_cast<char*>(ram.data()+base+0x100),
                base==0x10000 ? "watcher.c" : "camera.c");
  }
  word(0x12000+0x1ec,65542);
  const auto original=ram;
  route.sample(ram.data(),ram.size(),1);
  EXPECT_TRUE(route.done()); EXPECT_EQ(ram,original);
  EXPECT_FALSE(route.parse("MGSROUTE 1\nSTEP bad 2 1\n"
      "UNTIL camera_dd6d_route eq 0 4294967295\nEND\n",error));
}
TEST_F(RouteFixture, ElevatorArrivalWaitsForLiveNamedDoorStateWithoutChangingRam) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP lift 100 1\n"
    "UNTIL evpanel_7858_door_phase eq 2 4294967295\n"
    "UNTIL evpanel_7858_timer eq -1 4294967295\nEND\n",error)) << error;
  word(0x10000,0x80010000); word(0x10004,0x80010000);
  word(0x10008,0x800c1000); word(0x10014,0x80020000);
  std::memcpy(ram.data()+0x20000,"evpanel.c",10);
  word(0x1002c,0x00017858); word(0x10036,0x02580003);
  route.sample(ram.data(),ram.size(),1); EXPECT_FALSE(route.done());
  word(0x10042,2); word(0x10036,0x0258ffff);
  const auto original=ram;
  route.sample(ram.data(),ram.size(),2);
  EXPECT_TRUE(route.done()); EXPECT_EQ(ram,original);
  EXPECT_FALSE(route.parse("MGSROUTE 1\nSTEP bad 2 1\n"
    "UNTIL evpanel_7858_facing eq 0 4294967295\nEND\n",error));
}
TEST_F(RouteFixture, PrisonGuardTargetLifeRequiresCorrectOverlayAndValidPointer) {
  TasRoute route; std::string error;
  const std::string program="MGSROUTE 1\nSTEP guard 100 1\n"
    "UNTIL zako_41e2_life max 192 4294967295\n"
    "UNTIL ambush_phase eq 12 4294967295\nUNTIL ambush_kills eq 15 4294967295\nEND\n";
  ASSERT_TRUE(route.parse(program,error)) << error;
  word(0x10000,0x80010000); word(0x10004,0x80010000);
  word(0x10008,0x800c1000); word(0x10014,0x80020000);
  std::memcpy(ram.data()+0x20000,"zako.c",7); word(0x10050,0x41e2);
  word(0xdfb70,12); word(0xdfbe8,15);
  stage("s03d");
  route.sample(ram.data(),ram.size(),1); EXPECT_FALSE(route.done());
  word(0x108fc,0x80030000); word(0x30026,192);
  stage("s11e");
  route.sample(ram.data(),ram.size(),2); EXPECT_FALSE(route.done());
  stage("s03d"); const auto original=ram;
  route.sample(ram.data(),ram.size(),3);
  EXPECT_TRUE(route.done()); EXPECT_EQ(ram,original);
}
TEST_F(RouteFixture, OcelotLayoutUsesNamedBattleActorAndFullWidthPhase) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP boss 100 1\n"
    "UNTIL revolver_09fb_x eq -5826 4294967295\n"
    "UNTIL revolver_09fb_life eq 657 4294967295\n"
    "UNTIL revolver_09fb_phase eq 2 4294967295\n"
    "UNTIL revolver_09fb_timer eq 65540 4294967295\nEND\n",error)) << error;
  word(0x10000,0x80010000); word(0x10004,0x80010000);
  word(0x10008,0x800d811c); word(0x10014,0x80020000);
  std::memcpy(ram.data()+0x20000,"revolver.c",11);
  word(0x10054,0x09fb); word(0x10024,uint16_t(-5826));
  word(0x10854,657); word(0x108fc,2); word(0x10900,65540);
  stage("s11e"); route.sample(ram.data(),ram.size(),1); EXPECT_FALSE(route.done());
  stage("s04b"); const auto original=ram;
  route.sample(ram.data(),ram.size(),2);
  EXPECT_TRUE(route.done()); EXPECT_EQ(ram,original);
}
TEST_F(RouteFixture, MissionFailureAbortsEvenWhileSnakeIsAlive) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP mission 100 1\n"
    "GUARD game_over_timer eq 0 4294967295\n"
    "UNTIL life min 1 4294967295\nEND\n",error)) << error;
  word(0xb7526,256); word(0xadb44,uint32_t(-2));
  const auto original=ram;
  route.sample(ram.data(),ram.size(),1);
  EXPECT_FALSE(route.done()); EXPECT_FALSE(route.error().empty()); EXPECT_EQ(ram,original);
}
TEST_F(RouteFixture, RejectsMalformedRecipesAndSystemSizedNumbers) {
  for (const auto& bad : {"MGSROUTE 1\nSTEP a 10 1\nEND\n",
       "MGSROUTE 1\nSTEP a -1 1\nUNTIL story eq 29 4294967295\nEND\n",
       "MGSROUTE 1\nSTEP a 10 1\nUNTIL mystery eq 1 4294967295\nEND\n",
       "MGSROUTE 1\nSTEP a 10 1\nUNTIL story eq 29 4294967295\nINPUT 3 65536 0\nEND\n",
       "MGSROUTE 1\nSTEP a 10 1\nUNTIL story eq 29 4294967295\n"}) {
    TasRoute route; std::string error;
    EXPECT_FALSE(route.parse(bad,error)); EXPECT_FALSE(error.empty());
  }
}

TEST_F(RouteFixture, RetailGclOperandsReadSignedValuesFlagsAndLinkedState) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP original-script 20 1\n"
    "GUARD gcl_11800002 eq 3 4294967295\n"
    "UNTIL gcl_1402002d eq 1 4294967295\n"
    "UNTIL gcl_14040067 eq 1 4294967295\n"
    "UNTIL gcl_1100000a eq -1 4294967295\nEND\n",error)) << error;
  ram[0xB7512]=3;
  ram[0xB646D]=4;
  ram[0xB644A]=ram[0xB644B]=255;
  route.sample(ram.data(),ram.size(),1);
  EXPECT_FALSE(route.done()); EXPECT_FALSE(route.failed());
  ram[0xB64A7]=16;
  const auto original=ram;
  route.sample(ram.data(),ram.size(),2);
  EXPECT_TRUE(route.done()); EXPECT_EQ(ram,original);
  const auto events=route.takeEvents();
  ASSERT_EQ(events.size(),1);
  EXPECT_NE(events[0].find("gcl_14040067=1"),std::string::npos);
  EXPECT_NE(events[0].find("gcl_1100000a=-1"),std::string::npos);
  EXPECT_NE(events[0].find("difficulty=3"),std::string::npos);
  for (const auto& field : {"gcl_118000c0", "gcl_110007ff", "gcl_19000000",
                           "gcl_14880000", "gcl_11100000"}) {
    EXPECT_FALSE(route.parse(std::string("MGSROUTE 1\nSTEP bad 20 1\nUNTIL ")+field+
                            " eq 0 4294967295\nEND\n",error));
  }
}

TEST_F(RouteFixture, PatrolGateRequiresLiveUniqueNamedActor) {
  TasRoute route; std::string error;
  const auto program="MGSROUTE 1\nSTEP patrol 40 1\n"
    "UNTIL watcher_1466_target_z eq -16000 4294967295\n"
    "UNTIL watcher_1466_vision_range eq 0 4294967295\n"
    "UNTIL watcher_1466_route eq 13 4294967295\nEND\n";
  ASSERT_TRUE(route.parse(program,error)) << error;
  route.sample(ram.data(),ram.size(),1); EXPECT_FALSE(route.done());
  std::memcpy(ram.data()+0x20000,"watcher.c",10);
  for (const auto base : {0x10000u,0x12000u}) {
    word(base,base|0x80000000); word(base+4,base|0x80000000);
    word(base+8,0x800c1000); word(base+20,0x80020000);
    word(base+0x50,0x1466); word(base+0xbe4,uint16_t(-16000));
    word(base+0xb90,8000);
    ram[base+0xb7d]=13;
  }
  route.sample(ram.data(),ram.size(),2); EXPECT_FALSE(route.done());
  word(0x12000,0); // unlinked stale actor
  route.sample(ram.data(),ram.size(),3); EXPECT_FALSE(route.done()); // still awake
  word(0x10000+0xb90,0);
  const auto original=ram;
  route.sample(ram.data(),ram.size(),4);
  EXPECT_TRUE(route.done()); EXPECT_EQ(ram,original);
  for (const auto& field : {"watcher_1466_fake","watcher_x466_x","watcher_14661_x"})
    EXPECT_FALSE(route.parse(std::string("MGSROUTE 1\nSTEP bad 2 1\nUNTIL ")+field+
                            " eq 0 4294967295\nEND\n",error));
}

TEST_F(RouteFixture, MovementUsesCameraOriginAndWaitsForGuestControl) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP go 200 1\nUNTIL x min 900 4294967295\nMOVE 1000 0\nEND\n",error));
  EXPECT_EQ(route.sample(ram.data(),ram.size(),1)[0],0); // no actor
  word(0xAE16C,0x80100000);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),2)[0],0x20); // right
  word(0xADAA8,1024);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),3)[0],0x40); // down in rotated camera
  word(0xADB3C,0x10000000);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),4)[0],0); // pad released by script
  word(0xAE0A0,1);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),5)[0],0x4008); // Codec interrupt
  word(0xAE0A0,0); word(0xADB3C,0); word(0xAE188,950);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),6)[0],0);
  EXPECT_TRUE(route.done());
  for (const auto& bad : {"MOVE 32768 0", "MOVE 0 0\nINPUT 3 16 0", "MOVE 0 0\nMOVE 1 1"})
    EXPECT_FALSE(route.parse(std::string("MGSROUTE 1\nSTEP bad 2 1\nUNTIL x eq 0 4294967295\n")+bad+"\nEND\n",error));
}

TEST_F(RouteFixture, StableGoalRejectsTransientReadyState) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP ready 20 1\nSTABLE 3\n"
                         "UNTIL control ne 0 4294967295\nEND\n",error));
  word(0xAE16C,0x80100000);
  route.sample(ram.data(),ram.size(),1); EXPECT_FALSE(route.done());
  word(0xAE16C,0);
  route.sample(ram.data(),ram.size(),2); EXPECT_FALSE(route.done());
  word(0xAE16C,0x80100000);
  route.sample(ram.data(),ram.size(),3); EXPECT_FALSE(route.done());
  route.sample(ram.data(),ram.size(),4); EXPECT_FALSE(route.done());
  route.sample(ram.data(),ram.size(),5); EXPECT_TRUE(route.done());
  EXPECT_FALSE(route.failed());
  EXPECT_FALSE(route.parse("MGSROUTE 1\nSTEP bad 2 1\nSTABLE 0\n"
                          "UNTIL life eq 1 4294967295\nEND\n",error));
}

TEST_F(RouteFixture, DuctMovementTurnsRelativeToSnakeAndDoesNotWriteGuestState) {
  TasRoute route; std::string error;
  ASSERT_TRUE(route.parse("MGSROUTE 1\nSTEP duct 200 1\nUNTIL x min 900 4294967295\nMOVE 1000 0\nEND\n",error));
  word(0xAE16C,0x80100000); word(0xAE1C8,2); word(0xADAA8,2048);
  word(0x10000A,2048);
  const auto before=ram;
  EXPECT_EQ(route.sample(ram.data(),ram.size(),1)[0],0x20); // right turns 2048 towards 1024
  EXPECT_EQ(ram,before);
  word(0x10000A,1024);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),2)[0],0x10); // aligned: crawl forwards
  word(0x10000A,0);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),3)[0],0x80); // left turns 0 towards 1024
  word(0xAE16C,0x801ffff8);
  EXPECT_EQ(route.sample(ram.data(),ram.size(),4)[0],0); // invalid control pointer
}
