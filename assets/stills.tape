# Run with `vhs assets/stills.tape` to regenerate the three shading PNGs.
# Duck.glb must exist. VHS also writes the disposable GIF below.

Output "/tmp/rasterminal-stills.gif"

# A larger canvas or smaller font gives rasterminal more terminal pixels.
Set FontSize 12
Set Width 2400
Set Height 1600
Set Padding 10

Hide
Type "./build/rasterminal Duck.glb --no-hud --fps 30"
Enter
Sleep 5s
# Set the shared camera angle before capturing each shading mode.
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
