# Assets

Images and recordings referenced by the top-level `README.md`.

## Files

| File | Description |
| --- | --- |
| `demo.gif` | Auto-rotating Phong demo used at the top of the README |
| `demo.cast` | asciinema source recording for `demo.gif` |
| `shading-wireframe.png` | Wireframe example |
| `shading-flat.png` | Flat-shading example |
| `shading-phong.png` | Phong-shading example |
| `stills.tape` | vhs script that regenerates the three examples |

## Regenerating the demo GIF

Render the existing recording with `agg`:

```sh
agg --last-frame-duration 0 assets/demo.cast assets/demo.gif
```

To trim the recording first, change `7.9` to the desired duration:

```sh
python3 - <<'PY'
import json

src = "assets/demo.cast"
with open(src) as f:
    lines = f.readlines()

header = json.loads(lines[0])
events = [json.loads(l) for l in lines[1:] if l.strip()]
trimmed = [e for e in events if e[0] <= 7.9]
header["duration"] = trimmed[-1][0]

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

## Regenerating the shading stills

```sh
vhs assets/stills.tape
```

Edit the model path, font size or camera movement in `stills.tape` before running it. vhs
also writes a temporary `/tmp/rasterminal-stills.gif`.
