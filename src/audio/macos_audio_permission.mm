#import <AVFoundation/AVFoundation.h>
#include <dispatch/dispatch.h>
#include <cstdio>

#include "macos_audio_permission.h"

void macos_request_microphone_permission(void)
{
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

    if (status == AVAuthorizationStatusAuthorized)
        return;

    if (status == AVAuthorizationStatusNotDetermined) {
        /* The async callback may arrive on any thread; use a semaphore to
           block here until the user responds to the system dialog. */
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                               completionHandler:^(BOOL granted) {
            if (!granted)
                fprintf(stderr,
                    "Microphone permission denied. "
                    "Grant access in System Settings > Privacy & Security > Microphone.\n");
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    } else {
        fprintf(stderr,
            "Microphone access denied or restricted. "
            "Grant access in System Settings > Privacy & Security > Microphone.\n");
    }
}
