# Regenerate the three shading-mode stills:  vhs assets/stills.tape
#
# Produces assets/shading-{wireframe,flat,phong}.png.
# vhs handles these fine (a still is one static frame — only the live spin
# overwhelmed it, which is why the demo GIF uses asciinema + agg instead; see
# this directory's README). rasterminal auto-fits the camera per model, so with
# no orbit every mode below shares the exact same angle.
#
# Requires the model path below to exist. Output is a throwaway GIF (vhs always
# writes one); the real artifacts are the Screenshot PNGs.

Output "/tmp/rasterminal-stills.gif"

# Resolution levers: larger Width/Height = bigger PNG AND a more detailed render
# (more terminal cells -> more pixels, since each cell is 2 vertical pixels).
# A smaller FontSize packs even more cells in for the same canvas. Bump these if
# the stills still look low-res.
Set FontSize 12
Set Width 2400
Set Height 1600
Set Padding 10

Hide
Type "./rasterminal Duck.glb --no-hud --fps 30"
Enter
Sleep 5s
# Optional: nudge to a 3/4 angle. Same keys are sent before every mode, so the
# angle stays identical across all three stills. Remove for the default view.
Type "wwaa"
Sleep 3s
Show

Type "1"
Sleep 3s
Screenshot assets/shading-wireframe.png

Type "2"
Sleep 3s
Screenshot assets/shading-flat.png

Type "3"
Sleep 3s
Screenshot assets/shading-phong.png

Type "q"
