# Optional add-ons

None of this is needed for anyaudio. These are settings that pair well with it; the work
belongs to the projects credited below. Lines go into your `input.conf` (in Nuvio:
`nuvio\input.conf`, where key names are limited to `F1`..`F12` and `CTRL+<0-9|A-Z>`).

## Audio normalization (loudnorm)

Evens out loud and quiet scenes, useful when the added audio track is mixed differently
from the one you are used to. One key turns it on and off:

```
CTRL+9 af toggle "lavfi=[loudnorm=I=-22:TP=-1.5:LRA=2]"; show-text "Normalization: ${af}"
```

Credit: the `loudnorm` filter is part of [FFmpeg](https://ffmpeg.org/ffmpeg-filters.html#loudnorm),
an implementation of EBU R128 by Kyle Swanson.

## Anime4K upscaling

Real-time upscaling shaders for animation.

1. Download the shaders from [bloc97/Anime4K](https://github.com/bloc97/Anime4K/releases)
   and extract them to a folder.
2. Bind a key to a shader chain, using the full path to that folder, for example Mode A:

```
CTRL+1 no-osd change-list glsl-shaders set "C:/shaders/Anime4K_Clamp_Highlights.glsl;C:/shaders/Anime4K_Restore_CNN_VL.glsl;C:/shaders/Anime4K_Upscale_CNN_x2_VL.glsl;C:/shaders/Anime4K_AutoDownscalePre_x2.glsl;C:/shaders/Anime4K_AutoDownscalePre_x4.glsl;C:/shaders/Anime4K_Upscale_CNN_x2_M.glsl"; show-text "Anime4K: Mode A"
CTRL+0 no-osd change-list glsl-shaders clr ""; show-text "Shaders off"
```

The other modes (B, C, A+A...) are listed in the
[Anime4K mpv instructions](https://github.com/bloc97/Anime4K/blob/master/md/GLSL_Instructions_Advanced.md).

Credit: [Anime4K](https://github.com/bloc97/Anime4K) by bloc97 and contributors, MIT
License. The shaders are not included in this repository.
