# char-spectrogram

A scrolling spectrogram plug-in, drawn on the GPU with WebGPU/Dawn. It passes
audio through and shows how its spectrum changes over time, with an FFT or a
constant-Q transform. It is built on
[char-clap-template](https://github.com/charCulbert/char-clap-template)'s
libraries, and its interface is made only of chardraw's parts.

## Download

| Platform | Formats | Download |
|---|---|---|
| macOS | CLAP, VST3, AU | [char-spectrogram-macOS.zip](https://github.com/charCulbert/char-spectrogram/releases/latest/download/char-spectrogram-macOS.zip) |
| Windows | CLAP, VST3 | [char-spectrogram-Windows.zip](https://github.com/charCulbert/char-spectrogram/releases/latest/download/char-spectrogram-Windows.zip) |
| Linux | CLAP, VST3 | [char-spectrogram-Linux.zip](https://github.com/charCulbert/char-spectrogram/releases/latest/download/char-spectrogram-Linux.zip) |
| Browser hosts | WCLAP | [char-spectrogram.wclap.tar.gz](https://github.com/charCulbert/char-spectrogram/releases/latest/download/char-spectrogram.wclap.tar.gz) |

All releases are on the [releases page](https://github.com/charCulbert/char-spectrogram/releases).

## Settings

The spectrogram's settings, with their ranges and defaults. They are saved
in the plugin's state, not CLAP parameters, since hosts have no use for
automating them.

| Setting | Values | Default |
|---|---|---|
| FFT size | 256 to 16384, powers of two | 2048 |
| Hop size | 128 samples (and a 480th of the sample rate) to the FFT size | 512 |
| Window | rectangular, Hann, Hamming or Blackman | Hann |
| Channel | Mix, Left or Right | Mix |
| Frequency range | lower 0 to 500 Hz, upper 100 Hz to Nyquist | 20 Hz to 24 kHz |
| Level range | floor −160 to −20 dB, ceiling −20 to 12 dB, the floor 10 dB under the ceiling at least | −100 to 0 dB |
| Scale | linear or log | log |
| Colours | Magma, Viridis, Inferno, Plasma, Cividis, Turbo or Grayscale | Magma |
| Scroll rate | 25 to 200 points a second, whatever the hop | 100 |
| Piano strip, snap, cursor readout, labels, click tone | on or off | on |
| Transform | FFT or CQT | FFT |
| CQT bands per octave | 12, 24 or 36 | 24 |
| A4 tuning | 400 to 480 Hz | 440 Hz |
| CQT frequency limits | lower 27.5 Hz (A0) to 220 Hz, upper 1 kHz to 0.4 × the sample rate | 55 Hz to 16 kHz |
| CQT update rate | 15, 30 or 60 a second | 30 |
| CQT reading | Mean, Peak or Centre | Mean |

With CQT as the transform the spectrogram shows a constant-Q transform:
bands spaced by pitch, A4 tuning × 2^(k / bands per octave), so the low end
has resolution without an enormous FFT. Its settings show in place of the
FFT size, hop and window. The plot's frequency range stops at its bands, and
the cursor readout, snap, piano strip and its tone all go by the same
bands and tuning. A band's level over each update is its power over that
time (Mean), its loudest moment (Peak, which shows a click at its own level
but reads noise higher in the high bands), or its value at the update's
middle (Centre). The panel shows the delay from a
sound to its column: half the transform's block, about 341 ms at 48 kHz with
the defaults (the block holds 24 periods of 55 Hz). A lower floor, down to
A0 (27.5 Hz), resolves the bass's lowest notes at a cost: the block, and so
the delay, doubles below about 35 Hz at 24 bands an octave (683 ms). The
display's upper limit goes down to 100 Hz so the plot can zoom in on them;
the range stays an octave at least. The CQT runs from 44.1 to 96 kHz; at
other sample rates the plugin shows the FFT and the panel says so. A4 tuning also sets the piano strip's keys and the pitch names with the
FFT.

The analysis and display settings are in a drawer. The tab in the window's
top right corner (18 by 36 points, faint until the pointer is over it or it
has focus, named **Settings** for screen readers and in its tooltip) brings
its panel down over the plot, in that corner and right of the piano strip,
which keeps its size, so the time axis and scrolling stay as they are. The
panel is see-through (75% opaque) and as big as what's in it: a column to
each group (Analysis, CQT while it's the transform, Display), one setting a
row, 24 points apart. The tab then rides on the panel's lower edge and
closes it again; so does Escape from anywhere in the panel, and focus goes
back to the tab. Tab goes from the tab into the panel. Whether it is open is
saved with the plugin's state.

With CQT chosen, the CQT column ends with the delay from a sound to its
column, or why the FFT shows instead: the sample rate is outside 44.1 to
96 kHz, or the CQT's settings make no bands.

Right-click the plot
for what's shown over it: the piano strip, click tone, snap, cursor readout
and labels, each checked when it's on, which choosing switches; the colours
and the frequency scale, which choosing steps to the next; and Pause. The
menu is slightly see-through (90%). From the keyboard, Tab to the plot (the
first stop; an outline shows it has focus) and press the Menu key or
Shift+F10; screen readers offer the menu on the plot too.

The lists (Transform, FFT size, Window, Channel and the CQT's) are readouts:
drag one up or down or scroll over it to change it, or click it to step to
the next. The numbers are number boxes: drag one (Shift for fine), or focus
it and type a value, Enter to set it. Escape during a drag puts it back.
Double-click either, press Delete or Backspace, or choose Reset to Default
from its context menu, to reset it. Focused, the arrows step it. Where the
sample rate makes the hop used longer than the one set, Hop shows both:
`128 → 200` at 96 kHz.

Hovering over the spectrogram shows the frequency, the nearest pitch in cents
and the level under the pointer. Snap follows the nearest peak in that
column within 6 points (favouring one it follows by 2); hold
Command to bypass it.

Pause, from the plot's menu, freezes the display; the analysis keeps
running. A **Paused** button shows in the top right corner (below the
drawer while it's open): click it, or choose Resume from the menu, to start
again. The colours, level and frequency ranges and the scale still apply to
the frozen picture, and resuming starts a fresh history. Screen readers hear
"Display paused" and "Display resumed".

Hold a key of the piano strip to hear a quiet sine (−18 dBFS) at its pitch,
drag to another key to change note, and let go to stop; a line marks the
exact pitch across the plot while it's held. A plugin has no audio device of
its own, so the tone is mixed into the plugin's output, fading in and out over a
few milliseconds. The analysis takes its input before the tone is added, so
the spectrogram never shows it. To screen readers the strip is one picture.

With **Click tone** on, holding the pointer anywhere on the plot plays the
same sine at the pitch under it, snapped to a peak as the cursor readout is
(Command bypasses the snap); drag to change it, and let go to stop.

**Labels** turns the frequency and time scales off. The colours are
chardraw's ready-made schemes (`gpu::ColourScheme`), each sampled at nine
steps. All but Turbo and Grayscale are matplotlib's: perceptually even, and
readable with colour blindness.

A short hop with a large FFT is more work than the main thread can do in
real time: the plugin analyses at most 8 columns each time it runs (60 times
a second) and skips older ones, so the picture keeps time.

## Build

```sh
git clone https://github.com/charCulbert/char-spectrogram.git
cd char-spectrogram
cmake -B build
cmake --build build
```

That builds CLAP, VST3, AU (macOS) and a standalone app into `build/`. It needs
CMake 3.28 or later and a C++20 compiler; on Linux also `libasound2-dev`,
`libx11-dev` and `libxrandr-dev`.

CMake downloads the template's libraries (`libs/`) at the version pinned in
`CMakeLists.txt` (`GIT_TAG`), and they pin everything else they download. To
build against a local checkout of the template instead, add
`-DFETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE=<path to char-clap-template>` when
configuring.

WCLAP, for browser hosts, needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases)
and [Emscripten](https://emscripten.org) (`emcmake` on your `PATH`):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/CharSpectrogram.wclap.tar.gz
```

To run the tests after building: `ctest --test-dir build`.

## Source

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, state, the hand-off of
  the analysis to the interface, and the `clap.gui` and `clap.webview` extensions.
- `signalsmith-dsp.cmake`: pins Signalsmith Audio's DSP headers, for their FFT.
- `SpectrogramAnalysis.h`: the analysis, made of chardsp's parts: the
  channel (left, right or their mix), levels in dB relative to a full-scale
  sine, and time smoothing, over either transform. The audio thread only
  pushes both channels into a buffer; the main thread sets up the transform
  and reads the buffer a hop at a time.
- `chardsp_AnalysisBuffer.h`, `chardsp_Spectrum.h`, `chardsp_ConstantQ.h`,
  `chardsp_ConstantQSpectrogram.h`: copies of chardsp's (commit 87bfc1e), with
  only their include of Signalsmith's FFT changed to this project's
  `dsp/fft.h`. The buffer brings the audio from the audio thread; Spectrum is
  the windowed FFT (the short-time Fourier transform);
  ConstantQ and ConstantQSpectrogram are the constant-Q transform, which
  follows [CiCueTea](https://github.com/jdsierral/CiCueTea) (MIT, Juan
  Sierra): `licenses/CiCueTea.txt` is its licence, which ships with the
  plugin's other third-party licences.
- `ui/SpectrogramView.cpp`: the interface, a `gpu::View`: a
  `gpu::Spectrogram` (`chardraw/views.h`) fills the window, frequencies up its
  left edge and seconds along the bottom drawn over it, and the settings
  drawer's tab (`Ui::drawer`, an overlay) on its top edge. Open, the
  drawer's panel comes down over the plot, see-through, with the analysis
  and display settings as readouts (`Ui::readout`) and number boxes
  (`Ui::numberBox`); the plot's context menu has the rest (the plot is a
  stop in the Tab order, `Ui::tabStop`, so the keys reach it).
- `resources/`: the files the plugin ships (its font, IBM Plex Mono). Add images, sounds or
  anything else here and read them with `core::readResource("/path")` (on the
  main thread), or `gpu::loadAsset` in the view.
- Natively `libs/gpu` draws the view with Dawn in the host's window. In WCLAP
  the same view is compiled with Emscripten into the page the host shows.
- `tests/spectrogram_test.cpp`: the analysis's tests.
