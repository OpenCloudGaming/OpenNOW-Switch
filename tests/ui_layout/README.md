# Native UI layout regression test

This target renders the production `TopBarFrame`, browser header, game cards and
game detail view with the vendored Borealis SDL/OpenGL backend. It is not an HTML
mock or a full application build. Only account fixtures, cover loading, persistence
and network/launch actions are stubbed, so it needs no NVIDIA account and writes no
runtime account files. The placeholder cover and blank avatar are intentional.

On Ubuntu, install the host dependencies and build outside the repository:

```sh
sudo apt-get install cmake ninja-build libsdl2-dev libgl1-mesa-dev libcurl4-openssl-dev xvfb xauth
cmake -S tests/ui_layout -B /tmp/opennow-ui-layout -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/opennow-ui-layout -j4
bash tests/ui_layout/run.sh /tmp/opennow-ui-layout/ui_layout /tmp/opennow-ui-evidence
```

The runner tests every supported interface language with a long account name,
subscription chips and a minimized queue at 1280×720 and 1920×1080 physical
resolution. Both must retain the same 1280×720 logical coordinates. Guest and game
detail cases also run at both sizes. Each run checks child containment, horizontal
sibling separation, one-line tabs, card widths and vertical grid navigation routes.
The two-pixel tolerance allows Borealis/Yoga text and pixel rounding. Scrolling
frame scrollbars intentionally overlay content and are excluded from sibling checks.
The programmatic resize also updates the OpenGL viewport, matching Borealis'
native resize callback. The test checks drawable dimensions, viewport coverage
and the bottom-right framebuffer pixel before accepting a capture.

Logs contain the measured rectangles, and `.ppm` files contain native framebuffer
captures. View them directly or convert them with ImageMagick:

```sh
convert /tmp/opennow-ui-evidence/fr-1280.ppm /tmp/opennow-ui-evidence/fr-1280.png
```

The executable accepts `language account-name screenshot-path physical-width
[detail]`. Use `guest` as the account name for the signed-out fixture. A nonzero exit
means initialization or a geometry assertion failed. Switch/Deko3D rendering,
real controller input, asynchronous cover loading, settings pages and live catalog
contents still require separate application and hardware verification.

The Linux Borealis backend does not load a system Chinese fallback font. Some
Chinese glyphs appear as missing-character boxes in these captures. The Chinese
cases check geometry, not glyph coverage or the Switch shared-font fallback.
