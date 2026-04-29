# LFE (Lightweight Fixed-point Engine)

A portable C library for offline sample synthesis and effect processing,
used as the sound-design backend of the maxtracker NDS music tracker.

The library is pure portable C with no libnds or NDS hardware dependencies.
It builds and tests standalone on any host with a C99 compiler.
Platform-specific optimizations (e.g. ARM9 ITCM placement) are opt-in
via compile-time gates and fall back to portable C otherwise.

## Building

### Host (development)

```sh
make           # build liblfe.a
make test      # build everything, run tests, write WAV outputs
make clean
```

The host build uses your system `gcc` (or override with `CC=clang make`).
Test outputs go to `test/output/*.wav`.

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

The single public header is `include/lfe.h`. Link against `liblfe.a`.
Everything under `src/` is internal.

- maxtracker on NDS includes `lfe.h` from `arm9/source/ui/waveform_view.c`
  when `MAXTRACKER_LFE` is defined.
- The standalone host test binary links the library and exercises each
  generator with deterministic inputs.

## Credits

The Braids generator (`src/gen/braids/`) is a fixed-point port of
algorithms from [Mutable Instruments Braids](https://mutable-instruments.net/),
designed by Emilie Gillet. Original source released under GPLv3.

The Calvario oscillator-combine mode in the synth generator is based on
[Calvario](https://github.com/HydrangeaSystems/Calvario) by Hydrangea Systems.

The OTT multiband compressor effect (`src/fx/fx_ott.c`) is inspired by
[OTT](https://xferrecords.com/freeware) by Steve Duda / Xfer Records.

Biquad filter coefficients (`src/util/biquad.c`) are derived from the
[Audio EQ Cookbook](https://www.w3.org/2011/audio/audio-eq-cookbook.html)
by Robert Bristow-Johnson.

## Licensing

LFE is **GPL-3.0-or-later**. See the `LICENSE` file for the full text.

