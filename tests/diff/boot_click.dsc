# tests/diff/boot_click.dsc -- ONE click, bracketed by dumps on both sides.
#
# REACHED IN THE ORIGINAL. The click is the title-to-main-menu advance: FUN_0041C1CD
# (0x41C1CD) case 0 calls FUN_0041B891(1) on any mouse-down, with no hit test, and the two
# dumps show oracle.front_state 0 then 1 with different artwork. One event, two checkpoints.
#
# WHY IT EXISTS. On boot_newsoul.dsc the rng.calls gap GROWS: 4 draws at t=150, 10 at
# t=800. A single cause cannot produce a growing gap, so there are at least two. The window
# between those two checkpoints contains exactly one input -- the click at 320,240 -- and
# this script dumps immediately before and immediately after it, so the delta across the
# click is attributable to the click and to nothing else.
#
# Read it as three numbers: rng.calls@before, rng.calls@after, and their difference. That
# difference is the number of rand() draws the ORIGINAL spends turning one title click into
# the main menu, and it is directly comparable to the port's. Everything before @before is
# boot, which boot_only.dsc isolates separately.
#
# The click must be a click and not a key. `key RETURN` is a PORT-ONLY shortcut: measured
# against the running original, four RETURNs at states 1,1,1,1 change nothing, because the
# front view's message map has entries for WM_LBUTTONDOWN/UP and WM_MOUSEMOVE and nothing
# else. `key SPACE` is worse -- it activates "Depart this realm" and the process exits.
#
# The click must also land on the frame's own client area and not on an invisible MFC pane.
# The MDI tree under the main frame contains windows that are hidden but still own the whole
# client rect, and the harness used to route by WindowFromPoint, which ignores WS_VISIBLE.
# It now descends only into visible, enabled windows; this is the script that proves it.
at 150 dump rng@before
at 150 dump clock@before
at 200 click 320 240
at 400 dump rng@after
at 400 dump front@after
end 500
