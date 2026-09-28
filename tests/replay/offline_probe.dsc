# Deterministic probe of the .dsc script path (Core's --script mode).
# Unlike offline.rpl this schedule is in VIRTUAL MILLISECONDS, so the same file
# produces the same run every time: the clock only moves to a scheduled event or
# to the next 20 ms idle boundary, never from the host.
#
# It exercises: the parser, the schedule, the 20 ms idle cadence, --dump and the
# `at <ms> dump <label>` op. It asserts nothing about game rules, so it stays
# valid while the rule modules migrate.
at 0 dump clock
at 0 key ESCAPE
at 100 dump rng
at 200 dump hero
at 300 dump map
at 1000 dump clock
at 1000 dump battle
at 2000 dump scene
end 2500
