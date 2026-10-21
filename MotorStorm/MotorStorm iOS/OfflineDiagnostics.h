#import <Foundation/Foundation.h>

// Start once before guest threads. Returns the bounded native log's URL.
NSURL *MSDiagnosticsStart(NSURL *documents);
void MSDiagnosticsSetActive(BOOL active);
void MSDiagnosticsEvent(NSString *name, NSDictionary *details);
void MSDiagnosticsFinish(int result);
