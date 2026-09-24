# WebP host harness

Two small programs that compile against this repo's vendored libwebp so the
decoder can be measured and visually inspected **on a workstation instead of by
flashing the board**. Every number quoted in `README.md` and `HANDOFF.md` about
the decoder's memory requirement came from here, and the correctness of
`main/gfx.c`'s frame compositing was established here.

This exists because the board is awkward to iterate on: the HD-WF1 has no serial
console, flashing needs the GPIO0 pads bridged, and the panel comes up collapsed
after a flash until it is power-cycled. Measuring the decoder on the host costs
seconds.

## Build

```sh
./build.sh          # produces ./measure and ./render
```

Run it once after `idf.py build`, so the component manager has fetched
`managed_components/libwebp`.

## `measure` — how much heap does a decode need?

```
./measure ../../tools/config.webp ../../components/assets/tronbyt.webp
```

For the first frame of each file it reports the **peak transient heap** that
libwebp allocates inside a single `WebPDecode()` call (malloc is intercepted, so
this is measured, not inferred), under several decoder option sets: baseline
RGB/RGBA, cropping, and scaling.

That is the number that matters on this board. For a 64×32 lossless frame it is
**~18–26 KB**, in two allocations that are live at once — a 12,544-byte ARGB
working buffer that must be *contiguous*, plus ~5–12 KB of Huffman tables.

`measure` is also what closes off dead ends: cropping and scaling both sound like
they should shrink the working set, and neither does. Cropping is applied on the
way out (peak unchanged); scaling makes it *worse* (18,200 → 19,456 B) because
the rescaler adds memory without shrinking the full-size pixel buffer.

## `render` — is the compositing actually right?

```
./render ../../components/assets/tronbyt.webp tronbyt
./ppm2png.py                      # convert every *.ppm to .png
```

Writes four montages, one per decode strategy, each showing the first 20 frames
scaled up side by side:

| File | Strategy |
| ---- | -------- |
| `render_TAG_ref.ppm` | libwebp's own `WebPAnimDecoder` — the reference |
| `render_TAG_naive.ppm` | every frame at the canvas origin (the original `gfx.c` bug) |
| `render_TAG_offset.ppm` | each frame at its own x/y offset, no compositing |
| `render_TAG_scratch.ppm` | per-frame RGBA scratch + key-frame/blend/dispose — what `gfx.c` now does |

`render_TAG_scratch` is **byte-identical** to `render_TAG_ref` for the still and
animated assets, which is what makes it safe to say the decode path in `gfx.c`
is correct and that the remaining blocker is purely memory. Diff them directly:

```sh
cmp render_parrot_ref.ppm render_parrot_scratch.ppm && echo identical
```

(The one exception is lossy content: `ref` uses libwebp's default fancy
upsampling while `gfx.c` sets `no_fancy_upsampling`, so those differ by a couple
of counts per channel. Set `FANCY=1` to confirm that is the whole difference.)

`offset` is worth looking at: it is perfect for the colour-cycling parrot, but it
loses text and detail on `tronbyt.webp`, because frames whose rectangles contain
transparent pixels get copied rather than blended. That is the trade the
`GFX_DECODE_HEADROOM` gate in `gfx.c` makes when the heap cannot afford the
scratch.
