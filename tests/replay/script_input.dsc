# Regression: a --script run must deliver input. The diff suite reported 22
# front@* mismatches that were all ONE cause: the script branch applied the op to
# the Input struct and then continued past screen->update(), so the next
# iteration's input_begin() cleared the edges and the event was never consumed.
# `at 100 click 320 240` must move the front end 0 -> 1 under --script.
# The ORIGINAL leaves the title 230 ms after the click, not 1200, so the assertion is
# a deadline and not "eventually". A fix that delivers the input on a later frame
# would pass a loose test and still not match. 400 ms is the first checkpoint after
# 230 at which the front end is settled; the 150 ms dump is kept to show the state
# mid-transition rather than only at the deadline.
at 0 dump front@pre
at 100 click 320 240
at 150 dump front@mid
at 400 dump front@deadline
end 500
