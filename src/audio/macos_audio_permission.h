#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* On macOS 10.14+, request microphone access via AVFoundation before opening
   any audio input stream.  Blocks until the user responds to the system dialog
   (or returns immediately if permission was already granted or denied).
   No-op on platforms other than macOS. */
void macos_request_microphone_permission(void);

#ifdef __cplusplus
}
#endif
