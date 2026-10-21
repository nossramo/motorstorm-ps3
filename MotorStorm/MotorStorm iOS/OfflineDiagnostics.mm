#import "OfflineDiagnostics.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_metrics.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_threads.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_waits.h"
#include <mach/mach.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <cstdio>

// Three 1 MiB circular sample files per session; retain five sessions.
// No network, video, audio samples, stack sampling, or GPU readback.
static const NSUInteger kPartLimit = 1024 * 1024;
static const NSUInteger kNativeLimit = 2 * 1024 * 1024;

static double cpuSeconds(clockid_t clock)
{
    timespec t;
    return clock_gettime(clock, &t) == 0 ? t.tv_sec + t.tv_nsec * 1e-9 : -1;
}

@interface MSOfflineDiagnostics : NSObject {
    dispatch_queue_t _queue;
    dispatch_source_t _timer;
    NSURL *_directory;
    FILE *_file;
    NSUInteger _part, _bytes, _sequence;
    double _start, _lastWall, _lastCPU, _lastFlush, _loggerCPU;
    BOOL _active, _finished, _nativeCapped;
}
- (instancetype)initWithDocuments:(NSURL *)documents;
- (void)sample;
- (void)event:(NSString *)name details:(NSDictionary *)details;
- (void)setActive:(BOOL)active;
- (void)finish:(int)result;
- (NSURL *)nativeLog;
@end

@implementation MSOfflineDiagnostics
- (void)openPart
{
    if (_file) fclose(_file);
    NSURL *url = [_directory URLByAppendingPathComponent:[NSString stringWithFormat:@"samples-%lu.jsonl", (unsigned long)_part]];
    _file = fopen(url.fileSystemRepresentation, "w");
    if (_file) setvbuf(_file, nullptr, _IOFBF, 16 * 1024);
    _bytes = 0;
}
- (void)writeRow:(NSDictionary *)row
{
    if (!_file) return;
    NSData *data = [NSJSONSerialization dataWithJSONObject:row options:0 error:nil];
    if (!data) return;
    if (_bytes + data.length + 1 > kPartLimit) {
        _part = (_part + 1) % 3;
        [self openPart];
        if (!_file) return;
    }
    if (fwrite(data.bytes, 1, data.length, _file) != data.length || fputc('\n', _file) == EOF) {
        fclose(_file); _file = nullptr; return;
    }
    _bytes += data.length + 1;
}
- (instancetype)initWithDocuments:(NSURL *)documents
{
    if (!(self = [super init])) return nil;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *parent = [documents URLByAppendingPathComponent:@"logs/performance" isDirectory:YES];
    if (![fm createDirectoryAtURL:parent withIntermediateDirectories:YES attributes:nil error:nil]) return nil;
    NSArray<NSURL *> *entries = [fm contentsOfDirectoryAtURL:parent includingPropertiesForKeys:@[NSURLContentModificationDateKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
    NSMutableArray<NSURL *> *sessions = [NSMutableArray new];
    for (NSURL *entry in entries) {
        if (![entry.lastPathComponent hasPrefix:@"session-"]) continue;
        NSDictionary *saved = [NSDictionary dictionaryWithContentsOfURL:[entry URLByAppendingPathComponent:@"owner.plist"]];
        if ([saved[@"owner"] isEqual:@"MotorStormOfflineDiagnostics-v1"]) [sessions addObject:entry];
    }
    [sessions sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent compare:b.lastPathComponent];
    }];
    while (sessions.count >= 5) { [fm removeItemAtURL:sessions.firstObject error:nil]; [sessions removeObjectAtIndex:0]; }
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    formatter.dateFormat = @"yyyyMMdd'T'HHmmss'Z'";
    NSString *name = [NSString stringWithFormat:@"session-%@-%@", [formatter stringFromDate:NSDate.date], NSUUID.UUID.UUIDString];
    _directory = [parent URLByAppendingPathComponent:name isDirectory:YES];
    if (![fm createDirectoryAtURL:_directory withIntermediateDirectories:NO attributes:nil error:nil]) return nil;
    [@{@"owner":@"MotorStormOfflineDiagnostics-v1"} writeToURL:[_directory URLByAppendingPathComponent:@"owner.plist"] atomically:YES];
    struct utsname device; uname(&device);
    // Only selected non-sensitive engine switches, never the full environment.
    NSMutableDictionary *runtimeOptions = [NSMutableDictionary new];
    for (NSString *key in @[@"MOTORSTORM_NATIVE_JOBS", @"MOTORSTORM_AUDIO_MERGE", @"MOTORSTORM_JOB_SCOPES", @"SPU_LOCKLINE_HOST", @"PS3_AUDIO_OUTPUT_QOS", @"PS3_GAMEPLAY_QOS",
                            @"SPURS_TARGETED_WAKE", @"SPU_LEAN_DISPATCH", @"SPU_ASYNC_REUSE", @"MOTORSTORM_SPU_SEMAPHORE_WAIT",
                            @"CELL_SYNC_MUTEX_PARK", @"SPU_LOCKLINE_STRIPES"]) {
        const char *value = getenv(key.UTF8String);
        runtimeOptions[key] = value ? [NSString stringWithUTF8String:value] : @"unset";
    }
    NSData *launchData = [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"MotorStormLaunch" withExtension:@"json"]];
    NSDictionary *launch = launchData ? [NSJSONSerialization JSONObjectWithData:launchData options:0 error:nil] : nil;
    NSDictionary *metadata = @{
        @"schema":@4, @"session":name, @"started_utc":[formatter stringFromDate:NSDate.date],
        @"device":[NSString stringWithUTF8String:device.machine],
        @"os":NSProcessInfo.processInfo.operatingSystemVersionString,
        @"version":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"test",
        @"build":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"test",
        @"logical_cpu_count":@(NSProcessInfo.processInfo.processorCount),
        @"runtime_options":runtimeOptions,
        @"build_options":launch[@"build_options"] ?: @{},
        @"internal_resolution":@[@1280,@720], @"interval_seconds":@1,
        @"flush_seconds":@5, @"sample_files":@3, @"sample_file_limit_bytes":@(kPartLimit),
        @"native_log_limit_bytes":@(kNativeLimit),
        @"notes":@[
            @"CPU percent: 100 is one fully used core; measured for this process only.",
            @"GPU values cover completed engine command buffers, attributed when completed. They overlap CPU work and are not total device GPU utilization.",
            @"No new GPU waits or texture readbacks are introduced. Encode CPU is a subset of process CPU; shader/pipeline timings are wall time.",
            @"Empty audio queue observations are starvation indicators, not an exact count of audible glitches. Silent blocks may be intentional.",
            @"Counters are sampled independently at one-second boundaries. Long operations are attributed on completion.",
            @"CPU roles use kernel user+system thread clocks. TLS cleanup accounts for exited threads. Roles are nonoverlapping, but unregistered helper threads remain unattributed. They do not distinguish useful work from spinning.",
            @"Metal encoding CPU overlaps role totals. SPU roles include compiled code and interpreter fallback; no per-instruction profiling is performed.",
            @"Audio sleep overshoot includes timer coalescing and scheduling delay. Service wall time includes locks, submission and preemption.",
            @"Async launch CPU/wall time covers thread creation or idle-thread handoff only, excluding image lookup, job execution and thread retirement. Counters apply to POSIX async tasks.",
            @"waits_by_role contains completed wall intervals, including preemption. job_pause is nested in job_total; never add them. job_pause reasons: complete, notified/spurious, timeout, capacity. sync_mutex covers contended ticket wait, including spin; other kinds use reason 0. flip_api is command enqueue/reserve time, NOT the full guest presentation dependency. GPU read includes render-queue delay. Missing/unregistered roles appear as unknown.",
            @"The last five seconds may be lost on an abrupt kill. Circular sample files must be sorted by sequence.",
            @"logger_cpu_ms_previous_sample measures the logger thread only, not all instrumentation overhead."
        ]
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToURL:[_directory URLByAppendingPathComponent:@"session.json"] atomically:YES];
    _start = _lastWall = ps3_metric_now() * 1e-9;
    _lastCPU = cpuSeconds(CLOCK_PROCESS_CPUTIME_ID);
    _lastFlush = _start; _active = YES;
    [self openPart];
    if (!_file) return nil;
    _queue = dispatch_queue_create("MotorStorm.offline-diagnostics", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    ps3_metrics_enable(1);
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
    dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC), NSEC_PER_SEC, 250*NSEC_PER_MSEC);
    __weak MSOfflineDiagnostics *weakSelf = self;
    dispatch_source_set_event_handler(_timer, ^{ @autoreleasepool { [weakSelf sample]; } });
    dispatch_resume(_timer);
    return self;
}
- (NSURL *)nativeLog { return [_directory URLByAppendingPathComponent:@"native.log"]; }
- (void)flush
{
    if (_file) fflush(_file);
    if (!_nativeCapped) {
        // These are buffered app log streams. Lock them only once per flush,
        // never in an audio callback or render submission.
        flockfile(stdout); flockfile(stderr);
        fflush(stdout); fflush(stderr);
        struct stat st;
        if (fstat(fileno(stdout), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= (off_t)kNativeLimit) {
            freopen("/dev/null", "a", stdout);
            freopen("/dev/null", "a", stderr);
            _nativeCapped = YES;
        }
        funlockfile(stderr); funlockfile(stdout);
    }
}
- (void)sample
{
    if (_finished) return;
    const double loggerStart = cpuSeconds(CLOCK_THREAD_CPUTIME_ID);
    const double now = ps3_metric_now() * 1e-9;
    const double cpu = cpuSeconds(CLOCK_PROCESS_CPUTIME_ID);
    const double elapsed = now - _lastWall;
    if (elapsed <= 0) return;
    uint64_t v[PM_COUNT];
    const ps3_thread_sample threads=ps3_metrics_sample_threads();
    for (int i=0;i<PM_COUNT;i++) v[i] = ps3_metric_take((ps3_metric)i);
    NSArray<NSString *> *roleNames=@[@"main_game",@"ppu_other",@"physics",@"ppu_audio_streaming",@"spurs_workers",@"spurs_coordinators",@"spu_audio",@"spu_async_tasks",@"audio_output",@"render",@"vblank",@"gcm_callbacks"];
    NSMutableDictionary *roles=[NSMutableDictionary new];
    for (int i=0;i<PC_COUNT;i++) roles[roleNames[i]]=@(threads.cpu_ns[i]/1e6);
    NSArray *waitNames=@[@"job_total",@"job_pause",@"gpu_read",@"flip_api",@"sync_mutex"];
    NSMutableDictionary *waits=[NSMutableDictionary new];
    for (unsigned role=0;role<=PC_COUNT;role++) {
        NSMutableDictionary *kinds=[NSMutableDictionary new];
        for (unsigned kind=0;kind<PW_COUNT;kind++) {
            ps3_wait_counter w=ps3_wait_take(role,kind);
            kinds[waitNames[kind]]=@{@"wall_ms":@(w.ns/1e6),@"count":@(w.count),
                @"max_ms":@(w.max_ns/1e6),@"reasons":@[@(w.reason[0]),@(w.reason[1]),@(w.reason[2]),@(w.reason[3])]};
        }
        waits[role<PC_COUNT ? roleNames[role] : @"unknown"]=kinds;
    }
    ps3_passive_frame frames = {};
    BOOL haveFrames = rsx_metal_passive_frame_sample(&frames) != 0;
    task_vm_info_data_t memory = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    BOOL haveMemory = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&memory, &count) == KERN_SUCCESS;
    NSMutableDictionary *row = [@{
        @"type":@"sample", @"seq":@(_sequence++), @"elapsed_s":@(now-_start), @"window_s":@(elapsed),
        @"active":@(_active), @"thermal_state":@(NSProcessInfo.processInfo.thermalState),
        @"low_power":@(NSProcessInfo.processInfo.lowPowerModeEnabled),
        @"logger_cpu_ms_previous_sample":@(_loggerCPU*1000),
        @"cpu_percent":cpu>=0 && _lastCPU>=0 ? @((cpu-_lastCPU)*100/elapsed) : NSNull.null,
        @"cpu_by_role_ms":roles, @"tracked_threads":@(threads.active),
        @"waits_by_role":waits,
        @"tracked_threads_started":@(threads.started), @"tracked_threads_exited":@(threads.exited),
        @"thread_clock_errors":@(threads.errors), @"thread_registration_drops":@(threads.dropped),
        @"async_dispatches":@(v[PM_ASYNC_DISPATCHES]), @"async_reused":@(v[PM_ASYNC_REUSED]),
        @"async_threads_created":@(v[PM_ASYNC_THREADS_CREATED]), @"async_launch_failures":@(v[PM_ASYNC_LAUNCH_FAILURES]),
        @"async_launch_cpu_ms":@(v[PM_ASYNC_LAUNCH_CPU_NS]/1e6), @"async_launch_wall_ms":@(v[PM_ASYNC_LAUNCH_WALL_NS]/1e6),
        @"memory_footprint_mb":haveMemory ? @(memory.phys_footprint/1048576.0) : NSNull.null,
        @"display_fps":haveFrames ? @(frames.display_fps) : NSNull.null,
        @"guest_fps":haveFrames ? @(frames.guest_fps) : NSNull.null,
        @"frame_avg_ms":haveFrames ? @(frames.average_ms) : NSNull.null,
        @"frame_p95_ms":haveFrames ? @(frames.p95_ms) : NSNull.null,
        @"frame_max_ms":haveFrames ? @(frames.worst_ms) : NSNull.null,
        @"last_present_age_s":haveFrames ? @(frames.age_seconds) : NSNull.null,
        @"gpu_completed_buffers":@(v[PM_GPU_BUFFERS]), @"gpu_completed_ms":@(v[PM_GPU_NS]/1e6),
        @"gpu_longest_buffer_ms":@(v[PM_GPU_MAX_NS]/1e6), @"gpu_errors":@(v[PM_GPU_ERRORS]),
        @"gpu_missing_timestamps":@(v[PM_GPU_NO_TIME]),
        @"metal_encode_cpu_ms":@(v[PM_ENCODE_CPU_NS]/1e6), @"metal_encode_wall_ms":@(v[PM_ENCODE_WALL_NS]/1e6),
        @"gpu_wait_wall_ms":@(v[PM_GPU_WAIT_NS]/1e6), @"drawable_wait_wall_ms":@(v[PM_DRAWABLE_WAIT_NS]/1e6),
        @"shader_builds":@(v[PM_SHADER_BUILDS]), @"shader_build_wall_ms":@(v[PM_SHADER_NS]/1e6),
        @"pipeline_builds":@(v[PM_PIPELINE_BUILDS]), @"pipeline_build_wall_ms":@(v[PM_PIPELINE_NS]/1e6),
        @"audio_blocks":@(v[PM_AUDIO_BLOCKS]), @"audio_blocks_per_s":@(v[PM_AUDIO_BLOCKS]/elapsed),
        @"audio_silent_blocks":@(v[PM_AUDIO_SILENT]), @"audio_empty_queue_observations":@(v[PM_AUDIO_EMPTY_QUEUE]),
        @"audio_submit_errors":@(v[PM_AUDIO_SUBMIT_ERRORS]),
        @"audio_queue_mean_frames":v[PM_AUDIO_BLOCKS] ? @(v[PM_AUDIO_QUEUE_SUM]/(double)v[PM_AUDIO_BLOCKS]) : NSNull.null,
        @"audio_queue_max_frames":@(v[PM_AUDIO_QUEUE_MAX]), @"audio_submit_max_gap_ms":@(v[PM_AUDIO_GAP_MAX_NS]/1e6),
        @"audio_service_wall_ms":@(v[PM_AUDIO_SERVICE_NS]/1e6), @"audio_service_max_ms":@(v[PM_AUDIO_SERVICE_MAX_NS]/1e6),
        @"audio_mix_lock_wait_ms":@(v[PM_AUDIO_LOCK_NS]/1e6),
        @"audio_sleep_count":@(v[PM_AUDIO_SLEEP_COUNT]),
        @"audio_sleep_requested_ms":@(v[PM_AUDIO_SLEEP_REQUEST_NS]/1e6),
        @"audio_sleep_actual_ms":@(v[PM_AUDIO_SLEEP_ACTUAL_NS]/1e6),
        @"audio_sleep_overshoot_ms":@(v[PM_AUDIO_SLEEP_OVERSHOOT_NS]/1e6),
        @"audio_sleep_overshoot_max_ms":@(v[PM_AUDIO_SLEEP_OVERSHOOT_MAX_NS]/1e6),
        @"audio_sleep_late_5ms":@(v[PM_AUDIO_SLEEP_LATE_5MS]), @"audio_sleep_late_20ms":@(v[PM_AUDIO_SLEEP_LATE_20MS])
    } mutableCopy];
    [self writeRow:row];
    _lastWall=now; _lastCPU=cpu;
    if (now-_lastFlush >= 5) { [self flush]; _lastFlush=now; }
    double end=cpuSeconds(CLOCK_THREAD_CPUTIME_ID);
    _loggerCPU=loggerStart>=0 && end>=loggerStart ? end-loggerStart : 0;
}
- (void)event:(NSString *)name details:(NSDictionary *)details
{
    dispatch_async(_queue, ^{
        if (self->_finished) return;
        [self writeRow:@{@"type":@"event",@"seq":@(self->_sequence++),@"elapsed_s":@(ps3_metric_now()*1e-9-self->_start),@"event":name,@"details":details}];
        [self flush];
    });
}
- (void)setActive:(BOOL)active
{
    dispatch_async(_queue, ^{ self->_active=active; });
    [self event:active ? @"foreground" : @"inactive" details:@{}];
}
- (void)finish:(int)result
{
    dispatch_async(_queue, ^{
        if (self->_finished) return;
        [self sample];
        [self writeRow:@{@"type":@"event",@"seq":@(self->_sequence++),@"elapsed_s":@(ps3_metric_now()*1e-9-self->_start),@"event":@"engine_returned",@"result":@(result)}];
        [self flush]; self->_finished=YES; ps3_metrics_enable(0);
        dispatch_source_cancel(self->_timer); self->_timer=nil;
        if (self->_file) { fclose(self->_file); self->_file=nullptr; }
    });
}
@end

static MSOfflineDiagnostics *s_diagnostics;
NSURL *MSDiagnosticsStart(NSURL *documents)
{
    if (!s_diagnostics) s_diagnostics=[[MSOfflineDiagnostics alloc] initWithDocuments:documents];
    return [s_diagnostics nativeLog];
}
void MSDiagnosticsSetActive(BOOL active) { [s_diagnostics setActive:active]; }
void MSDiagnosticsEvent(NSString *name, NSDictionary *details) { [s_diagnostics event:name details:details]; }
void MSDiagnosticsFinish(int result) { [s_diagnostics finish:result]; }
