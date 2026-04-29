# lfe — LFE (Lightweight Fixed-point Engine)

A portable C library for offline sample synthesis and effect processing,
intended for use as the sound-design backend of the maxtracker NDS music
tracker.

The library is **portable C** — no libnds, no NDS hardware registers, no
dependency on the rest of maxtracker. It can be built and tested standalone
on any host with a C99 compiler. Platform-specific optimizations (e.g.
ARM9 ITCM placement) are opt-in via compile-time gates and fall back to
portable C when the target isn't recognized.

## Status

Complete. Six generators (test tone, drawn wavetable, drum, subtractive
synth, 4-op FM, Braids with 18 shapes) and eight effects (distortion,
filter, delay, envelope shaper, normalize, OTT, reverse, bitcrush).

## Building

### Host (development)

```sh
make           # build liblfe.a
make test      # build everything, run tests, write WAV outputs
make clean
```

The host build uses your system `gcc` (or override with `CC=clang make`).
Test outputs go to `test/output/*.wav` so you can listen to the generated
waveforms — open them in any audio player.

### Nintendo DS (production)

The NDS build is invoked from the top-level maxtracker Makefile when
`LFE_ENABLED=1`. It uses devkitARM and produces `build_nds/liblfe.a`,
which then gets linked into the maxtracker ARM9 binary.

```sh
# from the maxtracker top-level
make emulator           # LFE included
make emulator-nosynth   # LFE excluded
```

You can also build the NDS library standalone:

```sh
make -f Makefile_arm
```

## Public API

The single public header is `include/lfe.h`. Callers include it and
link against `liblfe.a`. Everything else under `src/` is internal —
do not include `src/util/*.h` from outside the library.

The library is consumed in two contexts:

- From maxtracker on Nintendo DS, when `MAXTRACKER_LFE` is defined.
  Maxtracker's `arm9/source/ui/waveform_view.c` is the only file in
  the editor that includes `lfe.h` directly.
- From the standalone host test binary, which links the library and
  exercises each generator with deterministic inputs.

## Numeric conventions

All math is **fixed-point**. ARM9 has no FPU; software floating point
would be 50–200× slower than integer for the same DSP work. The
conventions used throughout the library:

| Format  | Type     | Range            | Used for                            |
|---------|----------|------------------|-------------------------------------|
| Q15     | int16_t  | [-1.0, +1.0)     | Audio samples, filter coefficients  |
| Q31     | int32_t  | [-1.0, +1.0)     | Accumulators, intermediate results  |
| Q24.8   | uint32_t | [0, 16M)         | Frequencies in Hz with sub-Hz prec. |
| Q16.16  | int32_t  | [-32K, +32K)     | Envelope levels, generic fixed      |
| Phase   | uint32_t | [0, 2^32)        | Oscillator phase accumulators       |

See `src/util/fixed.h` for the macros and inline helpers.

## Credits

The Braids generator (`src/gen/braids/`) is a fixed-point port of
algorithms from [Mutable Instruments Braids](https://mutable-instruments.net/),
designed by Emilie Gillet. Original source released under GPLv3.

The Calvario oscillator-combine mode in the synth generator is based on
[Calvario](https://github.com/HydrangeaSystems/Calvario) by Hydrangea Systems.

## Licensing

LFE is **GPL-3.0-or-later**. See the `LICENSE` file for the full text.

The license choice is deliberate: porting algorithms from Mutable
Instruments Braids (which is GPLv3) requires GPL-compatibility.

## Layout

```
lfe/
├── LICENSE                 → GPLv3
├── README.md               → this file
├── Makefile                → host build + test runner
├── Makefile_arm            → NDS build (devkitARM)
├── include/
│   ├── lfe.h               → public API (the only header outsiders include)
│   └── lfe_dbmath.h        → dB conversion utilities
├── src/
│   ├── lfe.c               → init / shutdown / version
│   ├── util/               → internal utilities (fixed-point, filters, etc.)
│   ├── gen/                → generators (test_tone, drawn, drum, synth, fm4)
│   │   └── braids/         → Mutable Instruments Braids port (18 shapes)
│   └── fx/                 → effects (distortion, filter, delay, OTT, etc.)
└── test/
    ├── test_main.{h,c}     → test runner + assert macros
    ├── test_*.c             → per-module tests
    ├── util/
    │   └── wav.{h,c}       → WAV writer for test outputs
    └── output/             → test WAV outputs (created by `make test`)
```
