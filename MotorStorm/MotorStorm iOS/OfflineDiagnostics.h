#import <Foundation/Foundation.h>

#ifndef MOTORSTORM_IOS_DIAGNOSTICS
#define MOTORSTORM_IOS_DIAGNOSTICS 0
#endif

#if MOTORSTORM_IOS_DIAGNOSTICS

NS_ASSUME_NONNULL_BEGIN

// Start once before guest threads. Returns nil if diagnostics cannot start.
NSURL * _Nullable MSDiagnosticsStart(NSURL *documents);
void MSDiagnosticsSetActive(BOOL active);
void MSDiagnosticsEvent(NSString *name, NSDictionary *details);
void MSDiagnosticsFinish(int result);
// Called on the diagnostics queue about once a second with guest frames in the
// last window. The handler must not sample those counters again.
void MSDiagnosticsObserveGuestFPS(void (^_Nullable handler)(double guestFPS));

// Snapshot the retained sessions into a temporary ZIP, without game files.
// Completion runs on the main queue. Remove the archive's parent after sharing.
// The caller must serialize exports and prevent a game launch during export.
void MSDiagnosticsExport(NSURL *documents,
    void (^completion)(NSURL * _Nullable archive, NSError * _Nullable error));

NS_ASSUME_NONNULL_END
#else
// No argument evaluation, Objective-C objects, timers or file work in play builds.
#define MSDiagnosticsSetActive(...) ((void)0)
#define MSDiagnosticsEvent(...) ((void)0)
#define MSDiagnosticsFinish(...) ((void)0)
#endif
