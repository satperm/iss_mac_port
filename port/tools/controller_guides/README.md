# Controller guides

`make_guides.py <controller images dir> <output dir>` draws the six guide images shown in the Qt
launcher (Controller guides screen): Xbox, Nintendo Switch Pro and 8BitDo, each in pt-BR and en-US.
Needs Pillow and the three base controller pictures (`1.webp` Xbox, `2.png` Switch Pro, `3.png`
8BitDo); the base pictures are not kept in this repository, only the generated guides.

Put the output in `port/launcher/qt/assets/controller_guides/` as
`ISS-controle-{xbox,switch,8bitdo}-{pt-BR,en-US}.png`. The launcher reads them at run time and
shows "images not included" when they are missing, so they can be replaced by your own
illustrations (same file names) without touching the code.
