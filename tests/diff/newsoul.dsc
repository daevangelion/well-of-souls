# tests/diff/newsoul.dsc -- title -> world list -> Well -> New Soul -> Incarnate -> the map.
#
# REACHED IN THE ORIGINAL (2026-10-01). The New Soul chain runs inside one message handler,
# WellCommand(3): DoModal 138 (name 1043, class row 1063), the Sage's PK review 164 (1091
# Yes), FUN_0041862F's random skin (rand() % the 82 skins), dialog 149 (1094 OK, then the
# Sage's "apply the rest randomly" 164, 1091 Yes), dialog 180 (skin list rows, 1235 "Use
# This Skin"), then WellCommand(0): the hero's combatant twice (SoulSelectionChanged and
# WellCommand, 37 draws each) and Incarnate. Incarnate's CloseOverlaysGoMap(1) puts the
# front end in state 6 and stamps DAT_004F2220; the 0x46A handler drops the hero ON link 0
# (179,219) with the latch set; the "Place Yourself On Gaiea!" Sage box stays up (modal),
# so from then on the world steps only from the 100 ms timer.
# rng@incarnated/map25/map30 pin all of it; front/map dumps pin state 6, the spawn, the
# latch and the encounter stamps.
at 100 click 320 240
at 1200 dump front@title
at 1400 click 393 382
at 1800 dump front@tos
at 2400 dump front@menu
at 2800 click 150 124
at 3200 dump front@entered
at 4000 dump front@srnet_modal
at 5000 dialog 1005=click
at 6000 dialog 1=click
at 6500 dump front@scanning
at 12000 dump front@worldlist
at 12500 click 211 160
at 13500 dump front@picked
at 14000 click 320 240
at 14500 dump front@skip500
at 15500 dump front@skip1500
at 16000 click 427 278
at 16500 dump front@ns_open
at 17000 dialog 138 1043=Walker 1063=sel:0 ok
at 17500 dialog 164 1091=click
at 18000 dump front@after164
at 18500 dialog 149 1094=click
at 19000 dialog 164 1091=click
at 19600 dialog 0 1190=sel:0
at 20000 dialog 0 1090=sel:0
at 20500 dialog 1235=click
at 22000 dump front@incarnated
at 22000 dump map@incarnated
at 22000 dump hero@incarnated
at 22000 dump rng@incarnated
at 25000 dump rng@map25
at 25000 dump map@map25
at 30000 dump rng@map30
at 30000 dump map@map30
at 30000 dump front@map30
end 30500
