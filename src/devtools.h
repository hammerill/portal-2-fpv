// Tools for testing without hands, through files in fpv-data (next to the DLLs):
//
//   status.txt    rewritten twice a second: map, drone, view, pose, flags,
//                 portals, what the gameplay did last.
//   dump.txt      write class names into it (one per line, or "player" /
//                 "weapon"): every field of the first such entity goes to
//                 dump-<class>.txt; the file is deleted.
//   autotest.txt  a scripted flight, one step per line:
//                   secs=2 arm=1 throttle=-0.55 pitch=0 roll=0 yaw=0 angle=1
//                 (sticks -1..1); "launch" launches the drone first. Channels
//                 go to libfpv instead of the radio until the last step ends;
//                 the file is deleted then. Delete it to stop early. Words
//                 without "=" are actions (devtools.cpp run_action).
//
// The loader adds cmd.txt (console commands) and console.log; render.cpp
// adds shot.txt (screenshot to shots/).
#pragma once

namespace devtools {

void tick();          // the server frame
void frame(float dt); // every rendered frame (main thread), for the autotest's channels

}  // namespace devtools
