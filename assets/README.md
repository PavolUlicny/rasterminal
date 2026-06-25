# Assets

Images and recordings referenced by the top-level `README.md`.

## Files

| File | Description |
| --- | --- |
| `demo.gif` | Hero demo GIF — auto-spin in Phong shading. |
| `demo.cast` | asciinema recording the GIF was rendered from; re-render with `agg` (see below). |
| `shading-wireframe.png` | Wireframe shading still. |
| `shading-flat.png` | Flat shading still. |
| `shading-phong.png` | Phong shading still. |
| `stills.tape` | vhs tape that regenerates the three shading stills. |

## Regenerating the demo GIF

`demo.cast` is the source recording. Re-render it at any time:

```sh
agg --last-frame-duration 0 assets/demo.cast assets/demo.gif
```

To trim the cast to a specific duration before rendering (adjust `7.9` as needed):

```sh
python3 - << 'EOF'
import json
src = "assets/demo.cast"
with open(src) as f: lines = f.readlines()
header = json.loads(lines[0])
events = [json.loads(l) for l in lines[1:] if l.strip()]
trimmed = [e for e in events if e[0] <= 7.9]
header["duration"] = trimmed[-1][0]
with open(src, "w") as f:
    f.write(json.dumps(header) + "\n")
    for e in trimmed: f.write(json.dumps(e) + "\n")
EOF
agg --last-frame-duration 0 assets/demo.cast assets/demo.gif
```

To record a fresh take (launches rasterminal already spinning in Phong; press `q` to stop):

```sh
asciinema rec --overwrite \
  -c "./rasterminal --spin -s phong <model>" \
  assets/demo.cast
```

Then trim and render as above.

## Regenerating the shading stills

```sh
vhs assets/stills.tape   # -> assets/shading-{wireframe,flat,phong}.png
```

Edit the model path, font size, or camera nudge in `stills.tape`, then re-run. Also writes a
throwaway `/tmp/rasterminal-stills.gif` (vhs always emits a GIF); ignore or delete it.

## Alternative: GitHub user-attachments (no committed binaries)

If you would rather not commit large media into the repo, drag-and-drop the GIF/PNGs into a GitHub
issue, pull request, or the README editor on github.com. GitHub uploads them and gives back a
`https://github.com/user-attachments/...` URL. Swap the `assets/...` paths in `README.md` for those
URLs. This keeps clone sizes small at the cost of the images living outside the repo.
