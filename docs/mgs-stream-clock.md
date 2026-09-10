# MGS cutscene audio clock

The USA v1.0 runtime previously redirected `InterruptCallback(9, handler)`
to slot 0 because its shared interrupt/DMA table accepted only eight slots.
The native `SpuSetIRQCallback` wrapper at `0x80099724` supplies index 9;
`UserSpuIRQProc` is `0x80085150`. Consequently that SPU callback ran once per
VBlank rather than on audio IRQs. The MTS scheduler is separately installed
through `VSyncCallback`, so other actors/effects continued while streamed
animation followed the wrong clock.

The original sound driver clocks streaming using voice 23, a silent 512-byte
ADPCM loop at pitch 0x1000, alternating the IRQ address between its 256-byte
halves. Each half is 16 blocks * 28 samples = 448 samples; at 44,100 Hz this
produces 98.4375 IRQs/s. `FS_StreamGetTick` converts the sound counter using
`counter * 64 / 105 + 1`, yielding a 60 Hz timeline. Its USA entry is
`0x800257CC`, with the returned tick observable at `0x8002587C`. The previous
60 Hz callback could supply only about 36.57 stream ticks/s when continuously
incrementing, even though the video/actor timeline advanced at 60 Hz.

Reference implementation descriptions:
- [Sound IRQ and stream counter](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/sound/sd_str.c)
- [Stream tick conversion](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libfs/stream.c)
- [Streamed demo frame selection](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/kojo/demothrd.c)

## Runtime change

IRQ slots 0–10 are distinct from the eight DMA callback slots. Out-of-range
registrations return zero and cannot overwrite VBlank. For the MGS profile,
the host contributes 735 SPU sample credits per NTSC VBlank. The game thread
synthesizes samples only up to the next audio IRQ, delivers the registered
callback, and allows native MTS tasks to run before consuming more credit.
SDL only consumes a bounded PCM queue; its buffer size, mute status or lack
of an audio device cannot advance the guest sound clock. Unused PCM is dropped
if the presentation queue exceeds 250 ms, without dropping guest clock credit.

This is guest-clock synchronization, not a CPU overclock multiplier. The donor
165% setting remains separate from host-native execution. Hardware-quantized
30/20 fps animation is retained; interpolation is not enabled.

Savestate v7 preserves the v6 disk prefix and adds IRQ slots 8–10 and pending
SPU sample credit. It clears queued presentation PCM on restore. A v6 state
loaded in the MGS guest-clock profile migrates the known `0x80085150` callback
from slot 0 to slot 9; the independent MTS VSync callback is preserved. Existing
user files are not rewritten during migration.

## Validation

- 111 runtime tests passed, including alternating silent-voice IRQ edges every
  448 samples, SDL consumption without guest-clock advancement, credit restore,
  separate VBlank/SPU delivery, v7 roundtrip and v6 migration.
- A real v6 dock checkpoint loaded into a fresh process and advanced 1500
  VBlanks without interpreter counters.
- A fresh boot advanced through 14,000 VBlanks and created a v7 checkpoint in
  a cutscene. Three consecutive loads of this checkpoint then advanced another
  1800 VBlanks without interpreter counters.
- Normal NTSC pacing: 900 VBlanks after a cutscene load took 14,993,739 us.
  The original stream-tick return was traced 450 times. There were nine repeated
  values immediately after load (about 0.3 seconds); all remaining returns
  advanced by 1–3 ticks. This verifies the repaired clock path, not absence of
  every possible animation hold in every scene. Display captures show the
  original dock dialogue scene with the two guards.
- Accelerated 1 ms runs above validate execution and state continuity, not
  real-time frame rate. The normal-pacing run used software scanout with the
  presentation window disabled and dummy audio; perceived smoothness in the
  user's Vulkan window still needs confirmation.

Evidence: [mgs-stream-clock-evidence.json](mgs-stream-clock-evidence.json).
The previous interactive process was left running to preserve unsaved progress;
new launches use the corrected runtime. Interactive launches should omit the
periodic metrics dump and tracing environment variables.
