# OpenNOW UI fonts

The client bundles static Nunito Medium, SemiBold, Bold and ExtraBold faces at
weights 500, 600, 700 and 800. NanoVG does not select variable-font weight axes,
so the original variable font must not replace these static files. IBM Plex Mono
Medium is used for tabular stream diagnostics.

OpenNOW-CJK is a weight-500 Noto Sans SC derivative containing the GB2312
repertoire. It supplies common Simplified Chinese glyphs without depending on
fonts installed on a development machine. Each application font also receives
direct Switch shared-font fallbacks for additional Chinese, Korean and controller
symbols. The inherited Switch fonts remain available as fallbacks.

The three OFL files in this directory cover the bundled assets. The Nunito and
IBM Plex inputs came from OpenCloudGaming/OpenNOW commit
`6e00a91dcaac53a9510f2ebf31d0e812f5b8e615`, under
`opennow-qt/res/fonts`. The Noto Sans SC input came from the Google Fonts
`ofl/notosanssc` directory. Input SHA-256 values are:

| Input | SHA-256 |
| --- | --- |
| Nunito-Variable.ttf | `bb55a5ca5c2042335b3991af27c4d0705d0ef41cac6164ac737fd8f2a1e85207` |
| IBMPlexMono-Medium.ttf | `a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755` |
| NotoSansSC variable | `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da` |

Normal Switch builds use the committed static assets. To regenerate them,
install FontTools in a separate Python environment and run:

```sh
python scripts/build-ui-fonts.py \
    --nunito /path/to/Nunito-Variable.ttf \
    --mono /path/to/IBMPlexMono-Medium.ttf \
    --cjk /path/to/NotoSansSC-Variable.ttf \
    --output resources/font
python tests/ui_font_assets_test.py
```

The generator preserves font copyright and license records and renames the
subset CJK family. Runtime registration fails explicitly if a required bundled
font cannot be loaded. CMake copies this directory into the NRO's RomFS.
