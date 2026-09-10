# RecompOne reuse

Source: https://github.com/BlackLabelHQ/RecompOne
Revision: `0409bc227ab41149e27985383eafd0492f2e6afc`.
License: MIT, copyright (c) 2026 flaffy; complete notice in LICENSE.

`engine/ps1Runtime/src/input/memory_card_files.cpp` adapts the directory,
allocation-chain and file algorithms from
`RecompOne.Runtime/Hardware/MemoryCard.cs` into our existing C++ MemoryCard.
Formatting in `input.cpp` follows the same layout. Added validation rejects
cyclic chains and wrong-sized images; host writes use temporary-file replacement.

USA pad addresses were checked with this revision's
`RecompOne.Recompiler/AutoConfigure/signatures/psyq.json`. The existing C++ pad
HLE is connected at these addresses; no OpenGTPS1 implementation was imported.

BIOS file operations use RecompOne's documented card contract as a reference.
MGS's per-sector completion count was verified independently against its
`source/memcard/memcard.c`: one HwCARD event per 128 bytes, then SwCARD completion.
