# tests/diff/boot_newsoul.dsc -- boot the original up to the world list.
#
# The front end is FUN_0041B891 (boot_flow.md section 1): state 0 is the title,
# 1 the main menu, 2 the "Where Do You Want To Play Today?" splash, 3 the world
# list, 4 the story, 5 the Well.  State 0 advances to 1 on ANY mouse-down
# (FUN_0041C1CD case 0), and every state after that takes RETURN, which is what
# the port's own tests/replay/gap_*.rpl do to walk the same path.
#
# NOTE: no trailing comments.  The port's dscript parser strips only whole-line
# comments, so a `#` after a statement is a parse error on that line -- the two
# sides must read the same file, and only the port gets to define the grammar.
#
# A label is a MODULE name, not a free tag: src/game_main.c's dump table has
# exactly ten (clock rng hero map scene battle panels items minigame options) and
# `dump_one()` answers a dump_unknown for anything else.  So a script may dump each
# module ONCE, and a repeated label would overwrite the earlier one on BOTH sides
# and make the diff compare the last occurrence while looking successful.
#
# A dump after every step is not decoration: a key that is constant across a run
# compares equal twice without proving anything.
at 0 dump clock
at 200 click 320 240
at 400 dump hero
at 600 key RETURN
at 800 dump rng
at 1000 key RETURN
at 1200 dump map
end 1400
