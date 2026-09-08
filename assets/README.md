# Assets

Images and recordings used in the [project README](../README.md). Run the commands
below from the repository root. Build the viewer before recording new material.

## Files

| File | Description |
| --- | --- |
| `demo.gif` | Auto-rotating Phong demo used at the top of the README |
| `demo.cast` | asciinema source recording for `demo.gif` |
| `shading-wireframe.png` | Wireframe example |
| `shading-flat.png` | Flat-shading example |
| `shading-phong.png` | Phong-shading example |
| `stills.tape` | vhs script that regenerates the three examples |

## Regenerate the demo GIF

Render the existing recording with `agg`:

```sh
agg --last-frame-duration 0 assets/demo.cast assets/demo.gif
```

To shorten the recording, set the cutoff in seconds below. This overwrites
`assets/demo.cast`; keep a copy if you need the full recording:

```sh
python3 - <<'PY'
import json

src = "assets/demo.cast"
with open(src) as f:
    lines = f.readlines()

header = json.loads(lines[0])
events = [json.loads(l) for l in lines[1:] if l.strip()]
trimmed = [e for e in events if e[0] <= 7.9]
header["duration"] = trimmed[-1][0] if trimmed else 0

with open(src, "w") as f:
    f.write(json.dumps(header) + "\n")
    for event in trimmed:
        f.write(json.dumps(event) + "\n")
PY
agg --last-frame-duration 0 assets/demo.cast assets/demo.gif
```

To record a new take, replace `<model>` and press `Q` when finished:

```sh
asciinema rec --overwrite \
  -c "./build/rasterminal --spin -s phong --no-hud --graphics blocks <model>" \
  assets/demo.cast
```

Then render `demo.cast` with `agg`.

## Regenerate the shading stills

```sh
vhs assets/stills.tape
```

The script expects `Duck.glb` in the repository root. Use the download command in
the [quick start](../README.md#quick-start), or edit the model path in
[`stills.tape`](stills.tape). You can also adjust its font size and camera movement.
vhs also writes a temporary `/tmp/rasterminal-stills.gif`.

## Recording provenance

`demo.cast` uses asciinema file format version 2 and records a 274-column, 75-row
terminal. These files do not record the original asciinema, agg or vhs tool versions. When replacing these assets, record the tool versions and exact commands
here so the captures can be reproduced.
