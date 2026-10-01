# tests/diff/well.dsc -- from the title to the Well (front state 5), the original's route.
#
# REACHED IN THE ORIGINAL (2026-10-01): front_state 3 -> 4 -> 5.
#   * 12500: the "Evergreen" row (msg 0x474, rect 160,144,263,177 in the oracle). FUN_0041D635
#     restarts the sound system twice (0x4C8 and MapLoader's FUN_00437461; each FUN_0046831C
#     waits one 100 ms poll of the music thread) and enters state 4, the +STORY scroller,
#     over art\splash.jpg.
#   * 14000: any mouse-up in state 4 ends the scroller (FUN_0041C2BC, "Saw Mouseup during
#     story state"). The next FrontEndTick dissolves for 1000 ms (FUN_0042198F, 999 blocks
#     at the harness's fixed rate) and posts 0x478: state 5, the Well, "Press NEW SOUL button".
# The world-list part is walk_path.dsc's route.
# The rng dumps at 15300 and 20000 cover scene 0 in the scene pane: SceneTick, the paint's
# breathing, idle poses and snow, the ACTOR seals and EnvSoundTick's order.
end 20500
at 100 click 320 240
at 1400 click 393 382
at 2800 click 150 124
at 5000 dialog 1005=click
at 6000 dialog 1=click
at 12000 dump front@worldlist
at 12500 click 211 160
at 13500 dump front@story
at 14000 click 320 240
at 14500 dump front@dissolve
at 15300 dump rng@well_entry
at 16000 dump front@well
at 16000 dump map@well
at 20000 dump rng@well20
