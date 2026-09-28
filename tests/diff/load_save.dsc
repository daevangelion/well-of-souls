# tests/diff/load_save.dsc -- load a .her the original itself saved and re-dump the hero record
#
# TRANSLATED FROM tests/replay/offline.rpl. The port's replay language has `expect`/`wait`
# in FRAMES at 60 Hz, so `wait 30` is 500 ms and `wait 5` is 83 ms; the timings below are
# that conversion, rounded to whole milliseconds. The route itself is the port's.
#
# THE FRONT END IS EXPRESSED IN THE ORIGINAL'S TERMS, NOT THE PORT'S. Two measured facts
# force this, and both are in docs/re/oracle.md:
#   * `key RETURN` is a PORT-ONLY shortcut. Four RETURNs leave the running original at
#     front state 1,1,1,1 -- the front view's message map at 0x4C898C has entries for
#     WM_LBUTTONDOWN (0x201), WM_LBUTTONUP (0x202) and WM_MOUSEMOVE (0x200) and nothing
#     else, so there is no key path into the front end at all.
#   * `key SPACE` activates "Depart this realm" and EXITS the process.
# So the three RETURNs the replays spend on menu -> splash -> world list are clicks on the
# front end's hotspot table (DAT_005339F8, 100 slots of 0xBC, hit-tested by FUN_00405765
# at 0x405765). The coordinates are the settled rects measured in
# tests/diff/front_hotspots.dsc, not coordinates read off a screenshot, because the rects
# ANIMATE: "Play now" is at 591,120,864,162 at t=1200 and at 79,120,352,162 once settled.
#
# VERIFICATION STATUS: NOT REACHED IN THE ORIGINAL. Everything up to the main menu is measured and works;
# past it is not, and the reason is recorded in docs/re/oracle.md section 7: the
# state-2 screen ("Where Do You Want To Play Today?") registers ONE hotspot, which is a
# non-clickable label (msg=0x0000), so there is no button to press and the transition
# out of it is not reachable by the means this suite has. The clicks below the front
# end are therefore UNVERIFIED against the original: they are the port's route, and
# the oracle run for this script is expected to stop in the front end.
#
# The golden-rule ASK is answered with `y` and the button-bar slots are at 640-51-51*i.
end 5800
at 100 click 320 240
at 1200 dump front@title
at 2200 dump front@menu
at 2600 click 215 141
at 3000 dump front@entered
at 4000 dump hero@before
at 5000 dump hero@loaded
at 5400 dump items@loaded
