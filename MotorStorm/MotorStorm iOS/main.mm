#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFAudio/AVFAudio.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "OfflineDiagnostics.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" int motorstorm_main(int, char **);
extern "C" void motorstorm_set_active(int);
extern "C" int rsx_metal_backend_attach_layer(void *);
extern "C" void rsx_metal_backend_on_first_presented(void (^handler)(void));
extern "C" void rsx_metal_touch_key(unsigned, int);
extern "C" void rsx_metal_touch_clear(void);
extern "C" void SDL_SetMainReady(void);

@interface MotorStormMetalView : UIView
@end
@implementation MotorStormMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

static NSDictionary *launchConfiguration(void)
{
    NSURL *url = [NSBundle.mainBundle URLForResource:@"MotorStormLaunch" withExtension:@"json"];
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    id config = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [config isKindOfClass:NSDictionary.class] ? config : nil;
}

// File validation can touch thousands of files. Keep it off UIKit's thread so
// the logo is visible and responsive while local/bookmarked storage is checked.
static NSURL *validatedGameRoot(NSURL *root, NSDictionary *files)
{
    if (!root || ![files isKindOfClass:NSDictionary.class] || !files.count) return nil;
    if ([root.lastPathComponent isEqualToString:@"PS3_GAME"]) root = root.URLByDeletingLastPathComponent;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm isReadableFileAtPath:[root URLByAppendingPathComponent:@"PS3_GAME/USRDIR/EBOOT.elf"].path]) return nil;
    for (NSString *relative in files) {
        NSDictionary *attributes = [fm attributesOfItemAtPath:[root URLByAppendingPathComponent:relative].path error:nil];
        if (!attributes || [attributes fileSize] != [files[relative] unsignedLongLongValue]) return nil;
    }
    return root;
}

@interface MotorStormController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) MotorStormMetalView *gameView;
@property(nonatomic, strong) UIImageView *logo;
@property(nonatomic, strong) UIView *controls;
@property(nonatomic, strong) NSURL *gameRoot;
@property(nonatomic, strong) NSURL *scopedURL;
@property(nonatomic) BOOL attemptedLaunch;
@property(nonatomic) BOOL launchPending;
@property(nonatomic) BOOL started;
@property(nonatomic) BOOL gameRunning;
@property(nonatomic) BOOL fillGameScreen;
- (void)gameReturned:(int)result;
@end

struct BootArguments { char *eboot; void *controller; };
static void *runGame(void *opaque)
{
    @autoreleasepool {
        BootArguments *boot = static_cast<BootArguments *>(opaque);
        MotorStormController *controller = (__bridge_transfer MotorStormController *)boot->controller;
        AVAudioSession *audio = AVAudioSession.sharedInstance;
        NSError *audioError = nil;
        BOOL configured = [audio setCategory:AVAudioSessionCategoryPlayback
                                      mode:AVAudioSessionModeDefault options:0 error:&audioError];
        BOOL activated = configured && [audio setActive:YES error:&audioError];
        if (!activated) NSLog(@"MotorStorm audio activation failed: %@", audioError);
#if MOTORSTORM_IOS_DIAGNOSTICS
        MSDiagnosticsEvent(@"audio_session", @{@"active":@(activated), @"main_thread":@(NSThread.isMainThread),
            @"sample_rate":@(audio.sampleRate), @"io_buffer_ms":@(audio.IOBufferDuration*1000),
            @"error":audioError.localizedDescription ?: @""});
#endif
        char name[] = "MotorStorm";
        char *args[] = {name, boot->eboot, nullptr};
        int result = motorstorm_main(2, args);
        free(boot->eboot);
        delete boot;
        dispatch_async(dispatch_get_main_queue(), ^{ [controller gameReturned:result]; });
    }
    return nullptr;
}

@implementation MotorStormController
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.view.multipleTouchEnabled = YES;
    self.gameView = [MotorStormMetalView new];
    self.gameView.backgroundColor = UIColor.blackColor;
    self.gameView.userInteractionEnabled = NO;
    // Only stretch the presented texture. The renderer still owns drawableSize
    // at the guest resolution; the controls and logo are separate sibling views.
    self.gameView.layer.contentsGravity = kCAGravityResize;
    const char *fillScreen = getenv("MOTORSTORM_IOS_FILL_SCREEN");
    self.fillGameScreen = !fillScreen || strcmp(fillScreen, "0") != 0;
    [self.view addSubview:self.gameView];
    self.controls = [UIView new];
    self.controls.hidden = YES;
    self.controls.multipleTouchEnabled = YES;
    [self.view addSubview:self.controls];
    [self addControl:@"◀" key:123];
    [self addControl:@"▶" key:124];
    [self addControl:@"↑" key:126];
    [self addControl:@"↓" key:125];
    [self addControl:@"BOOST" key:49];
    [self addControl:@"H-BRAKE" key:56];
    [self addControl:@"✓" key:36];
    [self addControl:@"Ⅱ / Back" key:53];
    [self addControl:@"Camera" key:8];
    self.logo = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"LaunchLogo"]];
    self.logo.contentMode = UIViewContentModeScaleAspectFit;
    self.logo.accessibilityLabel = @"MotorStorm";
    self.logo.isAccessibilityElement = YES;
    [self.view addSubview:self.logo];
}
- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    if (self.attemptedLaunch) return;
    self.attemptedLaunch = YES;
    // The Metal layer now has a window and laid-out bounds. No artificial delay
    // or developer launch argument is needed for an ordinary app-icon launch.
    [self locateGameAndStart:nil];
}
- (void)addControl:(NSString *)title key:(unsigned)key
{
    UIButton *button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.tag = key;
    button.accessibilityIdentifier = [NSString stringWithFormat:@"pad-%u", key];
    button.accessibilityLabel = title;
    button.backgroundColor = [UIColor.blackColor colorWithAlphaComponent:0.55];
    button.layer.cornerRadius = 15;
    button.layer.borderWidth = 1;
    button.layer.borderColor = [UIColor.whiteColor colorWithAlphaComponent:0.45].CGColor;
    button.titleLabel.font = [UIFont boldSystemFontOfSize:17];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:@selector(controlDown:) forControlEvents:UIControlEventTouchDown | UIControlEventTouchDragEnter];
    [button addTarget:self action:@selector(controlUp:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel | UIControlEventTouchDragExit];
    [self.controls addSubview:button];
}
- (void)controlDown:(UIButton *)sender { rsx_metal_touch_key((unsigned)sender.tag, 1); sender.alpha = 0.55; }
- (void)controlUp:(UIButton *)sender { rsx_metal_touch_key((unsigned)sender.tag, 0); sender.alpha = 1; }
- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGRect bounds = self.view.bounds;
    CGFloat scale = MIN(bounds.size.width / 1280.0, bounds.size.height / 720.0);
    CGSize size = CGSizeMake(1280 * scale, 720 * scale);
    // Fill Duo's taller landscape display. Conventional 16:9/wider displays
    // retain the previous fit. A launch override of 0 restores fit everywhere.
    BOOL tallerLandscape = bounds.size.width > bounds.size.height &&
                           bounds.size.height > bounds.size.width * (720.0 / 1280.0);
    self.gameView.frame = self.fillGameScreen && tallerLandscape ? bounds :
        CGRectMake((bounds.size.width-size.width)/2, (bounds.size.height-size.height)/2, size.width, size.height);
    self.controls.frame = bounds;
    CGFloat width = MIN(bounds.size.width * 0.72, 720.0);
    CGSize logoSize = self.logo.image.size;
    CGFloat height = logoSize.width > 0 ? width * logoSize.height / logoSize.width : 0;
    self.logo.frame = CGRectMake((bounds.size.width-width)/2, (bounds.size.height-height)/2, width, height);
    UIEdgeInsets safe = self.view.safeAreaInsets;
    CGFloat left = safe.left + 12, right = bounds.size.width - safe.right - 12;
    CGFloat bottom = bounds.size.height - safe.bottom - 12;
    for (UIButton *b in self.controls.subviews) {
        switch (b.tag) {
            case 123: b.frame=CGRectMake(left,bottom-64,64,64); break;
            case 124: b.frame=CGRectMake(left+76,bottom-64,64,64); break;
            case 126: b.frame=CGRectMake(right-64,bottom-140,64,64); break;
            case 125: b.frame=CGRectMake(right-64,bottom-64,64,64); break;
            case 49: b.frame=CGRectMake(right-154,bottom-140,78,64); break;
            case 56: b.frame=CGRectMake(right-154,bottom-64,78,64); break;
            case 36: b.frame=CGRectMake(right-64,safe.top+12,64,44); break;
            case 53: b.frame=CGRectMake(left,safe.top+12,100,44); break;
            case 8: b.frame=CGRectMake(left+112,safe.top+12,88,44); break;
        }
    }
}
- (NSURL *)documents
{
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
}
- (void)showLaunchError:(NSString *)message allowFolder:(BOOL)allowFolder
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Unable to start MotorStorm"
        message:message preferredStyle:UIAlertControllerStyleAlert];
    if (allowFolder) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Choose game folder" style:UIAlertActionStyleDefault
            handler:^(UIAlertAction *action) { [self chooseGame]; }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Try again" style:UIAlertActionStyleDefault
            handler:^(UIAlertAction *action) { [self locateGameAndStart:nil]; }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)locateGameAndStart:(NSURL *)picked
{
    if (self.started || self.launchPending) return;
    self.launchPending = YES;
    [self.scopedURL stopAccessingSecurityScopedResource]; self.scopedURL = nil;
    NSURL *documents = [self documents];
    NSData *bookmark = [NSUserDefaults.standardUserDefaults dataForKey:@"GameFolder"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSDictionary *config = launchConfiguration();
            NSDictionary *files = config[@"files"];
            NSURL *access = nil;
            NSURL *root = nil;
            NSURL *chosen = picked;
            if (!picked) root = validatedGameRoot([documents URLByAppendingPathComponent:@"game" isDirectory:YES], files);
            if (!root && !chosen && bookmark) {
                BOOL stale = NO;
                chosen = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
            }
            if (!root && chosen) {
                if ([chosen startAccessingSecurityScopedResource]) access = chosen;
                root = validatedGameRoot(chosen, files);
                if (!root) { [access stopAccessingSecurityScopedResource]; access = nil; }
            }
            NSData *newBookmark = picked && root ? [picked bookmarkDataWithOptions:0
                includingResourceValuesForKeys:nil relativeToURL:nil error:nil] : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                self.launchPending = NO;
                self.scopedURL = access;
                self.gameRoot = root;
                if (newBookmark) [NSUserDefaults.standardUserDefaults setObject:newBookmark forKey:@"GameFolder"];
                if (root) [self startGame];
                else [self showLaunchError:@"Game files are missing or still copying. Finish copying the game folder into MotorStorm in Files, or choose an existing game folder."
                    allowFolder:YES];
            });
        }
    });
}
- (void)chooseGame
{
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    NSURL *url = urls.firstObject;
    [controller dismissViewControllerAnimated:YES completion:^{ if (url) [self locateGameAndStart:url]; }];
}
- (void)startGame
{
    if (self.started || !self.gameRoot) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *documents = [self documents];
    NSURL *cache = [fm URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    NSDictionary *dirs = @{@"PS3_HDD0_ROOT":[documents URLByAppendingPathComponent:@"gamedata/dev_hdd0"],
                          @"PS3_HDD1_ROOT":[cache URLByAppendingPathComponent:@"dev_hdd1"],
                          @"PS3RECOMP_MSL_CACHE":[cache URLByAppendingPathComponent:@"msl-ios"]};
    NSDictionary *config = launchConfiguration();
    if (![config[@"environment"] isKindOfClass:NSDictionary.class]) {
        [self showLaunchError:@"Launch settings are missing. Reinstall the app." allowFolder:NO]; return;
    }
    for (NSString *key in config[@"environment"]) setenv(key.UTF8String, [config[@"environment"][key] UTF8String], 0);
    for (NSString *key in dirs) {
        NSURL *url = dirs[key]; NSError *error = nil;
        if (![fm createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:&error]) {
            [self showLaunchError:error.localizedDescription allowFolder:NO]; return;
        }
        setenv(key.UTF8String, url.fileSystemRepresentation, 1);
    }
    setenv("PS3_VFS_ROOT", self.gameRoot.fileSystemRepresentation, 1);
    if (chdir(documents.fileSystemRepresentation) != 0) {
        [self showLaunchError:@"Cannot open app storage." allowFolder:NO]; return;
    }
    SDL_SetMainReady();
    if (rsx_metal_backend_attach_layer((__bridge void *)self.gameView.layer) != 0) {
        [self showLaunchError:@"Could not attach the Metal display." allowFolder:NO]; return;
    }
#if MOTORSTORM_IOS_DIAGNOSTICS
    NSURL *offlineLog = MSDiagnosticsStart(documents);
    const char *log = offlineLog ? offlineLog.fileSystemRepresentation : "/dev/null";
#else
    // Normal gameplay has no session logger, sampling timer or growing log file.
    const char *log = "/dev/null";
#endif
    if (freopen(log,"a",stdout)) setvbuf(stdout,nullptr,_IOFBF,64*1024);
    if (freopen(log,"a",stderr)) setvbuf(stderr,nullptr,_IOFBF,16*1024);
    setenv("MOTORSTORM_BUFFERED_LOG","1",1);
    MSDiagnosticsEvent(@"start", @{});
    self.started = YES;
    self.gameRunning = YES;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    __weak MotorStormController *weakSelf = self;
    rsx_metal_backend_on_first_presented(^{
        MotorStormController *controller = weakSelf;
        if (!controller.gameRunning) return;
        controller.logo.hidden = YES;
        controller.controls.hidden = NO;
    });
    BootArguments *boot = new BootArguments{strdup([self.gameRoot URLByAppendingPathComponent:@"PS3_GAME/USRDIR/EBOOT.elf"].fileSystemRepresentation), (__bridge_retained void *)self};
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
    pthread_t thread;
    int result = pthread_create(&thread,&attr,runGame,boot);
    pthread_attr_destroy(&attr);
    if (result==0) pthread_detach(thread);
    else { CFBridgingRelease(boot->controller); free(boot->eboot); delete boot; [self gameReturned:result]; }
}
- (void)dealloc { [_scopedURL stopAccessingSecurityScopedResource]; }
- (void)gameReturned:(int)result
{
    self.gameRunning = NO;
    MSDiagnosticsFinish(result);
    self.logo.hidden = NO;
    self.controls.hidden = YES;
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    [self showLaunchError:[NSString stringWithFormat:@"Game stopped (%d). Close and reopen MotorStorm to try again.",result] allowFolder:NO];
}
@end

@interface MotorStormSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation MotorStormSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController=[MotorStormController new];
    [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)scene
{ motorstorm_set_active(0); rsx_metal_touch_clear(); MSDiagnosticsSetActive(NO); }
- (void)sceneDidBecomeActive:(UIScene *)scene
{ motorstorm_set_active(1); MSDiagnosticsSetActive(YES); }
@end

@interface MotorStormAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation MotorStormAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    UISceneConfiguration *configuration=[[UISceneConfiguration alloc] initWithName:@"MotorStorm" sessionRole:session.role];
    configuration.delegateClass=MotorStormSceneDelegate.class;
    return configuration;
}
@end

int main(int argc,char **argv)
{
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(MotorStormAppDelegate.class)); }
}
