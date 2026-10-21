#import <AppKit/AppKit.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern "C" int motorstorm_main(int argc, char** argv);
extern "C" void port_jobmods_register_all(void);

static int launchError(NSString *message)
{
    fprintf(stderr, "[app] %s\n", message.UTF8String);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"MotorStorm could not start";
    alert.informativeText = message;
    [alert addButtonWithTitle:@"OK"];
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
    return 1;
}

int main(int argc, char **argv)
{
    // The pinned runtime registers native jobs in a static constructor only
    // when the launch environment already exists (as it does in Terminal).
    // AppKit loads the bundle profile below, so finish that same registration
    // once here when the constructor skipped it. Keep this in the macOS app,
    // without importing the iOS runtime's startup/diagnostics changes.
    const bool jobsRegisteredBeforeMain = getenv("MOTORSTORM_NATIVE_JOBS") != nullptr;
    @autoreleasepool {
        // Build-time local paths stay inside the ignored app product. The app
        // reads the existing game/install/saves; it never bundles the PS3 data.
        NSURL *url = [[NSBundle mainBundle] URLForResource:@"MotorStormLaunch" withExtension:@"json"];
        NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
        NSDictionary *config = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (![config isKindOfClass:[NSDictionary class]])
            return launchError(@"Launch configuration is missing. Build the MotorStorm scheme again in Xcode.");
        NSString *root = config[@"root"];
        NSDictionary *environment = config[@"environment"];
        if (![root isKindOfClass:[NSString class]] || ![environment isKindOfClass:[NSDictionary class]])
            return launchError(@"Invalid launch configuration. Rebuild the MotorStorm scheme.");
        NSString *eboot = [root stringByAppendingPathComponent:@"game/PS3_GAME/USRDIR/EBOOT.elf"];
        if (![[NSFileManager defaultManager] isReadableFileAtPath:eboot])
            return launchError([NSString stringWithFormat:@"The local game executable is missing:\n%@\n\nRestore your game files and rebuild if the project folder has moved.", eboot]);
        if (chdir(root.fileSystemRepresentation) != 0)
            return launchError(@"The project folder cannot be opened. Rebuild from its current location.");
        for (NSString *key in environment) {
            NSString *value = environment[key];
            if (![key isKindOfClass:[NSString class]] || ![value isKindOfClass:[NSString class]])
                return launchError(@"Invalid environment in the launch configuration.");
            // Scheme overrides remain useful for Instruments and GPU captures.
            setenv(key.UTF8String, value.UTF8String, 0);
        }
        if (!jobsRegisteredBeforeMain && getenv("MOTORSTORM_NATIVE_JOBS"))
            port_jobmods_register_all();
        const char *cache = getenv("PS3RECOMP_MSL_CACHE");
        NSError *error = nil;
        if (cache && ![[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:cache]
                withIntermediateDirectories:YES attributes:nil error:&error])
            return launchError([NSString stringWithFormat:@"Cannot create the shader cache: %@", error.localizedDescription]);
        fprintf(stderr, "[app] native runtime; project=%s\n", root.fileSystemRepresentation);
        NSString *revision = config[@"source_commit"];
        if ([revision isKindOfClass:[NSString class]])
            fprintf(stderr, "[app] macOS source baseline=%s\n", revision.UTF8String);

        [NSApplication sharedApplication];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"MotorStorm"];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"MotorStorm"];
        [appMenu addItemWithTitle:@"Quit MotorStorm" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        [menu addItem:appItem];
        NSApp.mainMenu = menu;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // The custom event pump does not call -run, so complete Cocoa's app
        // launch explicitly before entering the native game runtime.
        [NSApp finishLaunching];
        // The runtime owns the Metal window and main-thread event pump.
        char *gameArgs[] = {argv[0], const_cast<char *>(eboot.fileSystemRepresentation), nullptr};
        return motorstorm_main(2, gameArgs);
    }
}
