/*---------------------------------------------------------------------------*\

  device_picker.h

  Minimal terminal UI for choosing an audio device from a list.  Uses raw
  termios mode and ANSI escapes, so it needs no curses dependency.

\*---------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

#include "audio/audio_stream.h"

namespace device_picker {

/* Return values from pick() other than a valid index into the device list */
constexpr int PICK_NONE   = -1;   // user chose the "None" entry
constexpr int PICK_CANCEL = -2;   // user pressed q / Esc, or stdin closed

/* True when stdin and stderr are both attached to a terminal, i.e. when the
   picker can actually be driven by a human. */
bool available();

/* Draw an interactive list and return the chosen index.  `title` names the
   setting being chosen, `hint` is a one-line explanation shown beneath it.
   The frame is replaced by a single summary line before returning. */
int pick(const std::string& title, const std::string& hint,
         const std::vector<AudioDevice>& devices);

}  // namespace device_picker
