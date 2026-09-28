# tests/diff/boot_only.dsc -- the boot, with NO input at all.
#
# REACHED IN THE ORIGINAL: trivially -- it dumps before anything is pressed. The value is
# what it ISOLATES. Every other script in this suite presses something within the first
# 200 ms, so on those the boot draws and the first input's draws arrive together and cannot
# be told apart. This one has no input, so `rng.calls` at the first dump is the boot total
# and nothing else.
#
# WHY IT EXISTS. The differential run reports rng.calls = 1414 (oracle) against 1418 (port)
# at t=150 on boot_newsoul.dsc, and 1418 is the port's ENTIRE boot total -- so the port has
# taken zero post-boot draws by t=150 and the difference is already present before the
# script does anything. That makes boot the place to look, and this script is the instrument:
# if the gap is present here with no input at all, it is a boot-sequence difference and no
# amount of tuning the pump will move it.
#
# The three dumps are at 150, 800 and 1400 so that a difference that GROWS with time is
# distinguishable from a constant offset -- on boot_newsoul the gap grew from 4 to 10,
# which is two separate problems rather than one, and that is only visible with more than
# one checkpoint.
#
# `end 1500` and no events after the last dump: the harness advances the clock to `end` and
# posts WM_QUIT once, so the run terminates on its own rather than at the timeout.
at 150 dump rng@boot
at 150 dump clock@boot
at 800 dump rng@mid
at 1400 dump rng@late
end 1500
