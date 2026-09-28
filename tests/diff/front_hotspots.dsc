# tests/diff/front_hotspots.dsc -- the front end's clickable regions, as the original
# has them at the main menu.
#
# REACHED IN THE ORIGINAL. Evidence: front@title has oracle.front_state=0 and no live
# hotspots, front@menu has oracle.front_state=1 and front.hotspot_count=6, and the six
# labels are the real menu ("Check On-Line for New Worlds", "Play now (it's free!)",
# "Buy a Mug!", "Read the attractive help file.", "Visit synthetic-reality.com.",
# "Depart this realm."). The two dumps of the menu show the slide-in animation
# progressing, which is the measurement the later scripts' click coordinates come from.
#
# WHY THIS SCRIPT EXISTS RATHER THAN A NAVIGATION ONE. The front end's buttons are not
# child windows: FUN_004056E7 (0x4056E7) registers each one into a 100-slot table at
# DAT_005339F8 and FUN_00405765 (0x405765) hit-tests a click against it. So the table IS
# the front end's state, dumping it is a real `front.*` read-out rather than a
# screenshot, and it is the only reliable source of a click coordinate: the rects are
# computed from GetClientRect at the moment each state is entered and then ANIMATED
# (FUN_004054A8 lerps from a "from" to a "to" position over the entry's t_len), so the
# same button is at a different pixel at a different moment. Measured on this run:
#
#     t=1200  "Play now" rect = 591,120,864,162   (mid slide-in, right of the client)
#     t=2200  "Play now" rect = 194,120,467,162
#     t=2600  "Play now" rect =  79,120,352,162   (settled, fully on screen)
#
# and the settled centre (215,141) is inside the client while the mid-animation centre is
# not. That difference is why a coordinate read off a screenshot at the wrong moment
# misses, and why the click coordinates for the rest of this suite have to come from
# here.
#
# NOTE the coordinate space: the layout is in per-mille of the client and the pixel
# anchor is anchor*client/1000, so the original's menu text is sized for a much larger
# window than the 640x480 both sides run at. Several rects therefore run past x=640 and
# are only partly reachable; that is a property of the original at this size, not a
# harness artefact, and it is recorded in docs/re/oracle.md section 5.4.
at 100 click 320 240
at 900 dump front@title
at 1200 dump front@menu_early
at 2200 dump front@menu_mid
at 2600 dump front@menu_settled
at 2800 dump options
end 3000
