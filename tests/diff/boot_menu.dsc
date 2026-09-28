# tests/diff/boot_menu.dsc -- boot the original from the title to the main menu.
#
# REACHED IN THE ORIGINAL. Evidence: tools/oracle/run.sh on this file writes
# front@title and front@menu; oracle.front_state is 0 in the first and 1 in the second,
# and the two screenshots differ (the title art art\title.jpg versus the main menu
# art\art_beg.jpg). The only input is one click.
#
# WHY THE CLICK IS A CLICK AND NOT A KEY. The port's own tests/replay/*.rpl advance the
# front end with `key RETURN`, and that is a PORT-ONLY shortcut: measured against the
# running original, RETURN does nothing at any front-end state (four RETURNs, states
# 1,1,1,1). The original has no key handler in the front view's message map -- the
# entries at 0x4C898C are WM_LBUTTONDOWN (0x201), WM_LBUTTONUP (0x202) and WM_MOUSEMOVE
# (0x200) and nothing else. SPACE is worse than useless: it activates "Depart this
# realm" and the process exits, which is why an early probe produced one dump and no
# more.
#
# WHY (320,240) AND NOT A COORDINATE READ OFF THE ART. FUN_0041C1CD (0x41C1CD) case 0
# advances the state on ANY mouse-down, with no hit test, so the coordinate only has to
# land inside the frame's client area. What it must NOT land on is another window: the
# MFC MDI tree under the main frame contains panes that are INVISIBLE but still own the
# whole client rect, and a click routed to one of those never reaches the front end.
# The harness used to route by WindowFromPoint, which ignores WS_VISIBLE; it now
# descends only into visible, enabled windows, and this is the first script that works
# because of it.
#
# The `front.*` keys are the front end's own hotspot table (DAT_005339F8, 100 slots of
# 0xBC bytes), which is also how the click coordinates for the later scripts were found.
at 100 click 320 240
at 600 dump front@title
# The title dump is BEFORE the click, deliberately: a dump after it would show the main
# menu under the name "title", which is the kind of label that makes a broken script look
# like a working one. With this order the two dumps are a real before and after --
# oracle.front_state 0 then 1 -- and the two screenshots are the two different artworks.
at 50 dump front@title
at 100 click 320 240
at 900 dump front@menu
end 1000
