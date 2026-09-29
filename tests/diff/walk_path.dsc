# tests/diff/walk_path.dsc -- walk a path on the world map and reach a new map cell
#
# TRANSLATED FROM tests/replay/gap_path.rpl. The port's replay language has `expect`/`wait`
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
# REACHED IN THE ORIGINAL, PAST THE FRONT END. The state-2 screen ("Where Do You Want To
# Play Today?") is not left by a click, a key or a timer. `FUN_0041F699` (0x41F699, the
# 0x46B handler) makes a SYNCHRONOUS `SendMessageA(frame, 0x46F, 0, 0)` at 0x41F6DC, and
# 0x46F's pfn is 0x42AA10 -- the SRNet open, which puts up its OWN modal and waits for the
# user. Measured with WOS_WINTREE during the call, that modal is a top-level #32770
# captioned "Where would you like to play today? (tm)" at screen (221,256)-(688,473):
#
#     id 1005  "Solo Game -- Play alone by yourself (no network required)."  (250,313)-(549,336)
#     id 1007  "Multiplayer Game -- Play with others, using a network or modem."
#     id 1000  ComboBox "Any Public MIX Game Server"
#     id 1006  "Configure Network Options"      id 1 "Play Game"    id 2 "Cancel"
#     id 1008  "Select Game Arena"   id 1009 "Help"   id 1042 "Bio"
#
# BOTH buttons are needed and in this order: 1005 alone leaves the dialog up, and
# `dialog 1=click` alone is a no-op because nothing is selected. With the pair, 0x46F
# RETURNS 1 at 6020 ms instead of at the script's `end`, FUN_0041D374 registers the
# "..Scanning..............." label, the solo stepper FUN_00438E8E runs ~30 times, and at
# 8340 ms FUN_0041B891(3) puts the front into state 3 -- the world list -- which
# FUN_0041D717 enumerates and draws as "Choose Your World..." / "Evergreen" /
# "... or Create Your Own World." (the 0x474 row hotspot).
#
# The `dialog` op takes the control ID, not a caption, and its syntax is `dialog <id>=click`
# -- `dialog <id> click` parses as a default-OK/cancel verb and is silently dropped, which
# looks exactly like a click that did nothing. Control ids and true SCREEN rects are what
# WOS_WINTREE now emits; the main-client rect it also prints is meaningless for a top-level
# modal, because a modal is not positioned relative to the main window's client.
#
# The golden-rule ASK is answered with `y` and the button-bar slots are at 640-51-51*i.
# That part is still UNVERIFIED against the original: reaching it needs a world to be
# chosen first (the 0x474 row click, `FUN_0041D635`), and this script stops at the world
# list. Everything above the ASK is now measured on the original; see below.
# NEXT STEP, measured and not yet scripted: the world row is hotspot 1 in front@worldlist,
# a real clickable 0x474 entry at rect 160,144,263,177 (centre 211,160), label "Evergreen",
# hwnd 65658 class AfxFrameOrView42, target 0. Its handler is FUN_0041D635 (0x41D635), which
# copies the name to DAT_004E0BD0 and calls FUN_0041B891(4). Past that is the "Pick a Soul"
# CListCtrl (control ids 0x531 / 0x464).
end 14000
at 100 click 320 240
at 1200 dump front@title
at 1400 click 393 382
at 1800 dump front@tos
at 2400 dump front@menu
at 2800 click 150 124
at 3200 dump front@entered
at 4000 dump front@srnet_modal
# SRNet's own modal. 1005 selects Solo Game, then 1 is "Play Game" -- measured, and the
# second click only works once the first has been delivered. 0x46F returns at 6020 ms.
at 5000 dialog 1005=click
at 6000 dialog 1=click
at 6500 dump front@scanning
# state 3, "Choose Your World...". FUN_0041B891(3) at 8340 ms, rows drawn at 8490 ms.
at 12000 dump front@worldlist
at 13600 dump map@worldlist
