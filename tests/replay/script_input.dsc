# Regression: a --script run must deliver input. The diff suite reported 22
# front@* mismatches that were all ONE cause: the script branch applied the op to
# the Input struct and then continued past screen->update(), so the next
# iteration's input_begin() cleared the edges and the event was never consumed.
# `at 100 click 320 240` must move the front end 0 -> 1 under --script.
at 0 dump front@pre
at 100 click 320 240
at 150 dump front@post
end 300
