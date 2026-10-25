#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFAudio/AVFAudio.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "OfflineDiagnostics.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern "C" int motorstorm_main(int, char **);
extern "C" void motorstorm_set_active(int);
extern "C" int rsx_metal_backend_attach_layer(void *);
extern "C" void rsx_metal_touch_key(unsigned, int);
extern "C" void rsx_metal_touch_clear(void);
extern "C" void SDL_SetMainReady(void);
extern "C" int motorstorm_job_benchmark(const char*, const char*);
extern "C" int motorstorm_audio_benchmark(const char*, const char*);
extern "C" int motorstorm_vmx_benchmark(const char*);

@interface MotorStormMetalView : UIView
@end
@implementation MotorStormMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface MotorStormController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) MotorStormMetalView *gameView;
@property(nonatomic, strong) UIStackView *setup;
@property(nonatomic, strong) UILabel *status;
@property(nonatomic, strong) UILabel *fpsLabel;
@property(nonatomic, strong) UIButton *start;
@property(nonatomic, strong) UIButton *shareDiagnostics;
@property(nonatomic, strong) UIView *controls;
@property(nonatomic, strong) NSURL *gameRoot;
@property(nonatomic, strong) NSURL *scopedURL;
@property(nonatomic) BOOL started;
- (void)gameReturned:(int)result;
@end

struct BootArguments { char *eboot; void *controller; };
static void *runGame(void *opaque)
{
    @autoreleasepool {
        BootArguments *boot = static_cast<BootArguments *>(opaque);
        MotorStormController *controller = (__bridge_transfer MotorStormController *)boot->controller;
        // Audio activation may block on the audio service. This is the engine
        // thread, so UIKit remains responsive; finish before SDL opens audio.
        AVAudioSession *audio = AVAudioSession.sharedInstance;
        NSError *audioError = nil;
        BOOL configured = [audio setCategory:AVAudioSessionCategoryPlayback
                                      mode:AVAudioSessionModeDefault options:0 error:&audioError];
        BOOL activated = configured && [audio setActive:YES error:&audioError];
        fprintf(stderr, "[iOS audio] active=%d main_thread=%d sample_rate=%.0f io_buffer_ms=%.2f\n",
                activated, NSThread.isMainThread, audio.sampleRate, audio.IOBufferDuration * 1000);
        if (!activated) NSLog(@"MotorStorm audio activation failed: %@", audioError);
        MSDiagnosticsEvent(@"audio_session", @{@"active":@(activated), @"main_thread":@(NSThread.isMainThread),
            @"sample_rate":@(audio.sampleRate), @"io_buffer_ms":@(audio.IOBufferDuration*1000),
            @"error":audioError.localizedDescription ?: @""});
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
    [self.view addSubview:self.gameView];

    self.status = [UILabel new];
    self.status.textColor = UIColor.whiteColor;
    self.status.numberOfLines = 0;
    self.status.textAlignment = NSTextAlignmentCenter;
    UILabel *title = [UILabel new];
    title.text = @"MotorStorm";
    title.textColor = UIColor.systemYellowColor;
    title.font = [UIFont boldSystemFontOfSize:36];
    title.textAlignment = NSTextAlignmentCenter;
    self.start = [UIButton buttonWithType:UIButtonTypeSystem];
    self.start.configuration = [UIButtonConfiguration filledButtonConfiguration];
    [self.start setTitle:@"Start game" forState:UIControlStateNormal];
    [self.start addTarget:self action:@selector(startGame) forControlEvents:UIControlEventTouchUpInside];
    UIButton *choose = [UIButton buttonWithType:UIButtonTypeSystem];
    choose.configuration = [UIButtonConfiguration borderedButtonConfiguration];
    [choose setTitle:@"Choose game folder" forState:UIControlStateNormal];
    [choose addTarget:self action:@selector(chooseGame) forControlEvents:UIControlEventTouchUpInside];
    self.shareDiagnostics = [UIButton buttonWithType:UIButtonTypeSystem];
    self.shareDiagnostics.configuration = [UIButtonConfiguration borderedButtonConfiguration];
    [self.shareDiagnostics setTitle:@"Share diagnostics" forState:UIControlStateNormal];
    self.shareDiagnostics.accessibilityIdentifier = @"share-diagnostics";
    [self.shareDiagnostics addTarget:self action:@selector(exportDiagnostics) forControlEvents:UIControlEventTouchUpInside];
    self.setup = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.status, self.start, choose, self.shareDiagnostics]];
    self.setup.axis = UILayoutConstraintAxisVertical;
    self.setup.spacing = 16;
    self.setup.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.setup];
    [NSLayoutConstraint activateConstraints:@[
        [self.setup.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.setup.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [self.setup.widthAnchor constraintLessThanOrEqualToConstant:540],
        [self.setup.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24],
        [self.setup.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24]
    ]];
    [self restoreGameFolder];
    self.controls = [UIView new];
    self.controls.hidden = YES;
    self.controls.multipleTouchEnabled = YES;
    [self.view addSubview:self.controls];
    self.fpsLabel = [UILabel new];
    self.fpsLabel.hidden = YES;
    self.fpsLabel.userInteractionEnabled = NO;
    self.fpsLabel.textAlignment = NSTextAlignmentCenter;
    self.fpsLabel.textColor = UIColor.whiteColor;
    self.fpsLabel.font = [UIFont monospacedDigitSystemFontOfSize:28 weight:UIFontWeightBold];
    self.fpsLabel.backgroundColor = [UIColor.blackColor colorWithAlphaComponent:0.62];
    self.fpsLabel.layer.cornerRadius = 8;
    self.fpsLabel.layer.masksToBounds = YES;
    self.fpsLabel.text = @"— FPS";
    [self.view addSubview:self.fpsLabel];
    [self addControl:@"◀" key:123];
    [self addControl:@"▶" key:124];
    [self addControl:@"↑" key:126];
    [self addControl:@"↓" key:125];
    [self addControl:@"BOOST" key:49];
    [self addControl:@"H-BRAKE" key:56];
    [self addControl:@"✓" key:36];
    [self addControl:@"Ⅱ / Back" key:53];
    [self addControl:@"Camera" key:8];
    BOOL audioBenchmark=[NSProcessInfo.processInfo.arguments containsObject:@"--audio-worker-benchmark"];
    BOOL vmxBenchmark=[NSProcessInfo.processInfo.arguments containsObject:@"--vmx-benchmark"];
    if (vmxBenchmark || audioBenchmark || [NSProcessInfo.processInfo.arguments containsObject:@"--job-arithmetic-benchmark"]) {
        // Explicit developer launch only; never mixed with a running game.
        self.setup.userInteractionEnabled = NO;
        self.status.text = @"Testing saved calculations…";
        UIApplication.sharedApplication.idleTimerDisabled = YES;
        NSURL* logs=[[self documents] URLByAppendingPathComponent:@"logs"];
        [NSFileManager.defaultManager createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL* inputs=[logs URLByAppendingPathComponent:audioBenchmark ? @"audio-inputs" : @"job-inputs"];
        NSString* kind=vmxBenchmark ? @"vmx-benchmark" : (audioBenchmark ? @"audio-benchmark" : @"job-arithmetic");
        NSURL* report=[logs URLByAppendingPathComponent:[kind stringByAppendingString:@"-result.txt"]];
        NSInteger before=NSProcessInfo.processInfo.thermalState;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            int result=vmxBenchmark ? motorstorm_vmx_benchmark(report.fileSystemRepresentation)
                : (audioBenchmark ? motorstorm_audio_benchmark(inputs.fileSystemRepresentation,report.fileSystemRepresentation)
                : motorstorm_job_benchmark(inputs.fileSystemRepresentation,report.fileSystemRepresentation));
            NSDictionary* metadata=@{@"result":@(result),@"thermal_before":@(before),
                @"thermal_after":@(NSProcessInfo.processInfo.thermalState),
                @"date":NSDate.date.description,@"game_running":@NO};
            NSData* data=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:nil];
            [data writeToURL:[logs URLByAppendingPathComponent:[kind stringByAppendingString:@"-status.json"]] atomically:YES];
            dispatch_async(dispatch_get_main_queue(), ^{
                self.status.text=result==0 ? @"Test complete. Close and reopen to play." : @"Test failed. Close and reopen to play.";
                UIApplication.sharedApplication.idleTimerDisabled = NO;
            });
        });
    } else if ([NSProcessInfo.processInfo.arguments containsObject:@"--start-game"]) {
        // Opt-in device testing. Keep the ordinary setup screen unchanged and
        // let UIKit finish installing/layouting the view before attaching Metal.
        dispatch_async(dispatch_get_main_queue(), ^{ [self startGame]; });
    }
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
    self.gameView.frame = CGRectMake((bounds.size.width-size.width)/2, (bounds.size.height-size.height)/2, size.width, size.height);
    self.controls.frame = bounds;
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
    self.fpsLabel.frame = CGRectMake((bounds.size.width - 148) / 2, safe.top + 8, 148, 40);
    [self.view bringSubviewToFront:self.fpsLabel];
}
- (NSURL *)documents
{
    return [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
}
- (NSURL *)executableAtRoot:(NSURL *)root
{
    return [root URLByAppendingPathComponent:@"PS3_GAME/USRDIR/EBOOT.elf"];
}
- (void)useGameRoot:(NSURL *)root
{
    if ([root.lastPathComponent isEqualToString:@"PS3_GAME"]) root = root.URLByDeletingLastPathComponent;
    BOOL valid = [[NSFileManager defaultManager] isReadableFileAtPath:[self executableAtRoot:root].path];
    NSData *data = [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"MotorStormLaunch" withExtension:@"json"]];
    NSDictionary *config = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSDictionary *files = config[@"files"];
    if (![files isKindOfClass:NSDictionary.class] || !files.count) valid = NO;
    if (valid) for (NSString *relative in files) {
        NSString *path = [root URLByAppendingPathComponent:relative].path;
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
        if (!attributes || [attributes fileSize] != [files[relative] unsignedLongLongValue]) {
            valid = NO; break;
        }
    }
    self.gameRoot = valid ? root : nil;
    self.start.enabled = valid;
    self.status.text = valid ? @"Game files ready. Use the arrows to drive or navigate menus; ✓ confirms a selection."
        : @"Game files are missing or still copying. Finish copying the game folder into MotorStorm in Files, then reopen the app. You can also choose an existing game folder.";
}
- (void)restoreGameFolder
{
    NSURL *local = [[self documents] URLByAppendingPathComponent:@"game" isDirectory:YES];
    [self useGameRoot:local];
    if (self.gameRoot) return;
    NSData *bookmark = [[NSUserDefaults standardUserDefaults] dataForKey:@"GameFolder"];
    if (!bookmark) return;
    BOOL stale = NO;
    NSURL *saved = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
    if (saved) {
        if ([saved startAccessingSecurityScopedResource]) self.scopedURL = saved;
        [self useGameRoot:saved];
    }
}
- (void)chooseGame
{
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)exportDiagnostics
{
    if (self.setup.hidden || !self.setup.userInteractionEnabled) return;
    self.setup.userInteractionEnabled = NO;
    NSString *previousStatus = self.status.text;
    self.status.text = @"Preparing recorded sessions…";
    MSDiagnosticsExport([self documents], ^(NSURL *archive, NSError *error) {
        self.setup.userInteractionEnabled = YES;
        self.status.text = previousStatus;
        if (!archive) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Diagnostics"
                message:error.localizedDescription ?: @"Could not prepare diagnostics."
                preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
            return;
        }
        UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[archive] applicationActivities:nil];
        share.popoverPresentationController.sourceView = self.shareDiagnostics;
        share.popoverPresentationController.sourceRect = self.shareDiagnostics.bounds;
        share.completionWithItemsHandler = ^(UIActivityType type, BOOL completed, NSArray *items, NSError *shareError) {
            [NSFileManager.defaultManager removeItemAtURL:archive.URLByDeletingLastPathComponent error:nil];
        };
        [self presentViewController:share animated:YES completion:nil];
    });
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    NSURL *url = urls.firstObject;
    if (!url) return;
    [self.scopedURL stopAccessingSecurityScopedResource];
    self.scopedURL = [url startAccessingSecurityScopedResource] ? url : nil;
    [self useGameRoot:url];
    if (self.gameRoot) {
        NSData *bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (bookmark) [[NSUserDefaults standardUserDefaults] setObject:bookmark forKey:@"GameFolder"];
    }
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
    NSData *data = [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"MotorStormLaunch" withExtension:@"json"]];
    NSDictionary *config = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![config[@"environment"] isKindOfClass:NSDictionary.class]) {
        self.status.text = @"Launch settings are missing. Rebuild the iOS target."; return;
    }
    for (NSString *key in config[@"environment"]) setenv(key.UTF8String, [config[@"environment"][key] UTF8String], 0);
    for (NSString *key in dirs) {
        NSURL *url = dirs[key]; NSError *error = nil;
        if (![fm createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:&error]) {
            self.status.text=error.localizedDescription; return;
        }
        setenv(key.UTF8String, url.fileSystemRepresentation, 1);
    }
    setenv("PS3_VFS_ROOT", self.gameRoot.fileSystemRepresentation, 1);
    if (chdir(documents.fileSystemRepresentation) != 0) { self.status.text=@"Cannot open app storage."; return; }
    SDL_SetMainReady();
    if (rsx_metal_backend_attach_layer((__bridge void *)self.gameView.layer) != 0) {
        self.status.text=@"Could not attach the Metal display."; return;
    }
    NSURL *logs=[documents URLByAppendingPathComponent:@"logs" isDirectory:YES];
    [fm createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:nil];
    self.fpsLabel.hidden = NO;
    self.fpsLabel.text = @"— FPS";
    __weak MotorStormController *controller = self;
    MSDiagnosticsObserveGuestFPS(^(double guestFPS) {
        dispatch_async(dispatch_get_main_queue(), ^{
            controller.fpsLabel.text = [NSString stringWithFormat:@"%.0f FPS", guestFPS];
        });
    });
    NSURL *offlineLog=MSDiagnosticsStart(documents);
    NSString *log=[(offlineLog ?: [logs URLByAppendingPathComponent:@"latest.log"]) path];
    if (freopen(log.fileSystemRepresentation,"a",stdout)) setvbuf(stdout,nullptr,_IOFBF,64*1024);
    if (freopen(log.fileSystemRepresentation,"a",stderr)) setvbuf(stderr,nullptr,_IOFBF,16*1024);
    setenv("MOTORSTORM_BUFFERED_LOG","1",1);
    NSLog(@"MotorStorm runtime log: %@",log);
    MSDiagnosticsEvent(@"start", @{});
    const char *diagnostics[] = {"PS3_FTRACE", "AUDIO_RATE", "MOTORSTORM_AUDIO_WORKER_TIMING"};
    for (const char *key : diagnostics) {
        const char *value = getenv(key);
        fprintf(stderr, "[iOS diagnostics] %s=%s\n", key, value ? value : "off");
    }
    self.started = YES;
    self.setup.hidden = YES;
    self.controls.hidden = NO;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    BootArguments *boot = new BootArguments{strdup([self executableAtRoot:self.gameRoot].fileSystemRepresentation), (__bridge_retained void *)self};
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
    pthread_t thread;
    int result=pthread_create(&thread,&attr,runGame,boot);
    pthread_attr_destroy(&attr);
    if (result==0) pthread_detach(thread);
    else { CFBridgingRelease(boot->controller); free(boot->eboot); delete boot; [self gameReturned:result]; }
}
- (void)dealloc { [_scopedURL stopAccessingSecurityScopedResource]; }
- (void)gameReturned:(int)result
{
    MSDiagnosticsObserveGuestFPS(nil);
    MSDiagnosticsFinish(result);
    self.fpsLabel.hidden = YES;
    self.setup.hidden=NO; self.controls.hidden=YES; self.start.enabled=NO;
    self.status.text=[NSString stringWithFormat:@"Game stopped (%d). Close and reopen MotorStorm to try again. Logs are saved in MotorStorm’s Files folder.",result];
    UIApplication.sharedApplication.idleTimerDisabled=NO;
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
