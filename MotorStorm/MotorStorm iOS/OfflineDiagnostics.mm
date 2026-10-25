#import "OfflineDiagnostics.h"
#if MOTORSTORM_IOS_DIAGNOSTICS
#include "../../vendor/ps3recomp/include/ps3emu/passive_metrics.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_threads.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_waits.h"
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <cstdint>
#include <cstdio>

extern "C" {
void ms_rsx_phase_take(uint64_t ns[19], uint32_t calls[19],
                       uint64_t* draws, uint64_t* packed, uint64_t* fallback,
                       uint64_t* vertices, uint64_t* pack_bytes,
                       uint64_t* const_bytes, uint64_t* index_bytes);
int ms_rsx_phase_account_enabled(void);
}

// Three 1 MiB circular sample files per session; retain five sessions.
// No network, video, audio samples, stack sampling, or GPU readback.
static const NSUInteger kPartLimit = 1024 * 1024;
static const NSUInteger kNativeLimit = 2 * 1024 * 1024;
static void (^s_guestFPS)(double);

static NSString *executableUUID(void)
{
    const mach_header *header = _dyld_get_image_header(0);
    if (!header || header->magic != MH_MAGIC_64) return @"unknown";
    const uint8_t *cursor = (const uint8_t *)header + sizeof(mach_header_64);
    const uint8_t *end = cursor + header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds && cursor + sizeof(load_command) <= end; ++i) {
        const load_command *command = (const load_command *)cursor;
        if (command->cmdsize < sizeof(load_command) || command->cmdsize > (size_t)(end-cursor)) break;
        if (command->cmd == LC_UUID && command->cmdsize >= sizeof(uuid_command))
            return [[NSUUID alloc] initWithUUIDBytes:((const uuid_command *)command)->uuid].UUIDString;
        cursor += command->cmdsize;
    }
    return @"unknown";
}

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
- (void)afterFlush:(dispatch_block_t)block;
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
                            @"CELL_SYNC_MUTEX_PARK", @"SPU_LOCKLINE_STRIPES", @"MOTORSTORM_AUDIO_SKIP",                             @"MOTORSTORM_JOB_BOOST",
                            @"MOTORSTORM_JOB_CHAIN", @"MOTORSTORM_DMA_REPEAT", @"MOTORSTORM_JOB_MERGE", @"MOTORSTORM_NATIVE_GEOMETRY",
                            @"RSX_METAL_DIRECT_VERTEX_SNAPSHOT", @"RSX_VERTEX_TOPOLOGY_CACHE",
                            @"MOTORSTORM_AUDIO_FILTER", @"MOTORSTORM_AUDIO_BLOCK", @"MOTORSTORM_COUNTING_WAIT", @"MOTORSTORM_SPU_PIPELINE",
                            @"MOTORSTORM_WWS_CHAIN", @"MOTORSTORM_WWS_CAPTURE"]) {
        const char *value = getenv(key.UTF8String);
        runtimeOptions[key] = value ? [NSString stringWithUTF8String:value] : @"unset";
    }
    runtimeOptions[@"MOTORSTORM_GEOMETRY_CAPTURE"] = getenv("MOTORSTORM_GEOMETRY_CAPTURE") ? @"enabled" : @"off";
    NSData *launchData = [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"MotorStormLaunch" withExtension:@"json"]];
    NSDictionary *launch = launchData ? [NSJSONSerialization JSONObjectWithData:launchData options:0 error:nil] : nil;
    NSDictionary *metadata = @{
        @"schema":@4, @"session":name, @"started_utc":[formatter stringFromDate:NSDate.date],
        @"device":[NSString stringWithUTF8String:device.machine],
        @"os":NSProcessInfo.processInfo.operatingSystemVersionString,
        @"version":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"test",
        @"build":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"test",
        @"executable_uuid":executableUUID(),
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
    if (haveFrames && s_guestFPS) s_guestFPS(frames.guest_fps);
    uint64_t phaseNs[19] = {};
    uint32_t phaseCalls[19] = {};
    uint64_t rsxDraws = 0, rsxPacked = 0, rsxFallback = 0, rsxVertices = 0;
    uint64_t rsxPackBytes = 0, rsxConstBytes = 0, rsxIndexBytes = 0;
    ms_rsx_phase_take(phaseNs, phaseCalls, &rsxDraws, &rsxPacked, &rsxFallback,
                      &rsxVertices, &rsxPackBytes, &rsxConstBytes, &rsxIndexBytes);
    static NSArray<NSString *> *phaseNames = @[
        @"unused", @"vertex", @"state", @"pipeline", @"textures", @"submit",
        @"layout", @"index_walk", @"remap", @"fetch_plan", @"pack", @"fetch_one",
        @"const_index", @"prepare", @"bind", @"draw_copy", @"vertex_hash",
        @"surface_a", @"surface_b"];
    NSMutableDictionary *rsxPhase = [NSMutableDictionary new];
    for (int i = 1; i < 19; i++) if (phaseCalls[i])
        rsxPhase[phaseNames[i]] = @{@"ms": @(phaseNs[i] / 1e6), @"calls": @(phaseCalls[i])};
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
    row[@"rsx_phase_account"] = @(ms_rsx_phase_account_enabled());
    row[@"rsx_draws"] = @(rsxDraws);
    row[@"rsx_packed_draws"] = @(rsxPacked);
    row[@"rsx_fallback_draws"] = @(rsxFallback);
    row[@"rsx_vertices"] = @(rsxVertices);
    row[@"rsx_pack_bytes"] = @(rsxPackBytes);
    row[@"rsx_const_bytes"] = @(rsxConstBytes);
    row[@"rsx_index_bytes"] = @(rsxIndexBytes);
    row[@"rsx_phase"] = rsxPhase;
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
- (void)afterFlush:(dispatch_block_t)block
{
    dispatch_async(_queue, ^{ [self flush]; block(); });
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
void MSDiagnosticsObserveGuestFPS(void (^handler)(double guestFPS)) { s_guestFPS = [handler copy]; }

static NSError *exportError(NSString *message)
{
    return [NSError errorWithDomain:@"MotorStormDiagnostics" code:1
                          userInfo:@{NSLocalizedDescriptionKey:message}];
}

static void zip16(NSMutableData *data, uint16_t value)
{
    const uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8)};
    [data appendBytes:bytes length:sizeof bytes];
}
static void zip32(NSMutableData *data, uint32_t value)
{
    zip16(data, (uint16_t)value); zip16(data, (uint16_t)(value >> 16));
}
static uint32_t zipCRC(NSData *data)
{
    static uint32_t table[256];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        for (uint32_t i=0; i<256; ++i) {
            uint32_t c=i;
            for (unsigned bit=0; bit<8; ++bit) c=(c>>1)^((c&1)?0xedb88320u:0);
            table[i]=c;
        }
    });
    uint32_t crc=0xffffffffu;
    const uint8_t *bytes=(const uint8_t *)data.bytes;
    for (NSUInteger i=0; i<data.length; ++i) crc=table[(crc^bytes[i])&255]^(crc>>8);
    return crc^0xffffffffu;
}
static BOOL writeDiagnosticsZIP(NSURL *snapshot, NSURL *archive, NSError **error)
{
    // Small, bounded stored ZIP: no archive service, game data or compression
    // dependency. Work runs only on explicit export, never during measurement.
    NSMutableData *zip=[NSMutableData new], *central=[NSMutableData new];
    NSDirectoryEnumerator<NSURL *> *files=[NSFileManager.defaultManager
        enumeratorAtURL:snapshot includingPropertiesForKeys:@[NSURLIsRegularFileKey]
        options:0 errorHandler:nil];
    uint16_t count=0;
    for (NSURL *url in files) {
        NSNumber *regular=nil; [url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
        if (!regular.boolValue) continue;
        NSString *relative=[url.path substringFromIndex:snapshot.path.length+1];
        NSData *name=[[@"performance/" stringByAppendingString:relative] dataUsingEncoding:NSUTF8StringEncoding];
        NSData *data=[NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
        if (!data) return NO;
        if (++count>40 || name.length>UINT16_MAX || zip.length+data.length>64*1024*1024) {
            if (error) *error=exportError(@"Diagnostics exceed the export size limit.");
            return NO;
        }
        uint32_t offset=(uint32_t)zip.length, size=(uint32_t)data.length, crc=zipCRC(data);
        zip32(zip,0x04034b50); zip16(zip,20); zip16(zip,0x800); zip16(zip,0);
        zip16(zip,0); zip16(zip,33); zip32(zip,crc); zip32(zip,size); zip32(zip,size);
        zip16(zip,(uint16_t)name.length); zip16(zip,0); [zip appendData:name]; [zip appendData:data];
        zip32(central,0x02014b50); zip16(central,20); zip16(central,20); zip16(central,0x800);
        zip16(central,0); zip16(central,0); zip16(central,33); zip32(central,crc);
        zip32(central,size); zip32(central,size); zip16(central,(uint16_t)name.length);
        zip16(central,0); zip16(central,0); zip16(central,0); zip16(central,0);
        zip32(central,0); zip32(central,offset); [central appendData:name];
    }
    uint32_t offset=(uint32_t)zip.length;
    [zip appendData:central];
    zip32(zip,0x06054b50); zip16(zip,0); zip16(zip,0); zip16(zip,count); zip16(zip,count);
    zip32(zip,(uint32_t)central.length); zip32(zip,offset); zip16(zip,0);
    return [zip writeToURL:archive options:NSDataWritingAtomic error:error];
}

static NSURL *createDiagnosticsArchive(NSURL *documents, NSError **error)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *exports = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
                     URLByAppendingPathComponent:@"MotorStormDiagnosticsExport" isDirectory:YES];
    // This directory belongs only to this exporter. Remove a previous share
    // left behind by an interrupted app launch, keeping temporary storage bounded.
    if ([fm fileExistsAtPath:exports.path] && ![fm removeItemAtURL:exports error:error]) return nil;
    NSURL *work = [exports URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    NSURL *snapshot = [work URLByAppendingPathComponent:@"performance" isDirectory:YES];
    if (![fm createDirectoryAtURL:snapshot withIntermediateDirectories:YES attributes:nil error:error]) return nil;
    NSURL *source = [documents URLByAppendingPathComponent:@"logs/performance" isDirectory:YES];
    NSArray<NSURL *> *entries = [fm contentsOfDirectoryAtURL:source includingPropertiesForKeys:nil options:0 error:nil];
    entries = [entries sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [b.lastPathComponent compare:a.lastPathComponent];
    }];
    NSUInteger copied = 0;
    // An explicit allowlist excludes game data, unrelated logs and developer
    // job dumps, even if they were placed inside a diagnostics session folder.
    NSArray<NSString *> *names = @[@"session.json", @"owner.plist", @"samples-0.jsonl",
                                   @"samples-1.jsonl", @"samples-2.jsonl", @"native.log"];
    for (NSURL *session in entries) {
        if (copied == 5) break;
        if (![session.lastPathComponent hasPrefix:@"session-"]) continue;
        NSDictionary *attributes = [fm attributesOfItemAtPath:session.path error:nil];
        if (![attributes[NSFileType] isEqual:NSFileTypeDirectory]) continue;
        NSDictionary *owner = [NSDictionary dictionaryWithContentsOfURL:[session URLByAppendingPathComponent:@"owner.plist"]];
        if (![owner[@"owner"] isEqual:@"MotorStormOfflineDiagnostics-v1"]) continue;
        NSURL *metadata = [session URLByAppendingPathComponent:@"session.json"];
        attributes = [fm attributesOfItemAtPath:metadata.path error:nil];
        if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes fileSize] > 1024*1024) continue;
        NSURL *destination = [snapshot URLByAppendingPathComponent:session.lastPathComponent isDirectory:YES];
        if (![fm createDirectoryAtURL:destination withIntermediateDirectories:NO attributes:nil error:error]) goto failed;
        for (NSString *name in names) {
            NSURL *file = [session URLByAppendingPathComponent:name];
            attributes = [fm attributesOfItemAtPath:file.path error:nil];
            if (!attributes) continue;
            // Do not follow symlinks or include unexpectedly large files.
            if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes fileSize] > 4*1024*1024) continue;
            if (![fm copyItemAtURL:file toURL:[destination URLByAppendingPathComponent:name] error:error]) goto failed;
        }
        ++copied;
    }
    if (!copied) {
        if (error) *error = exportError(@"No recorded sessions yet. Play a race, then close and reopen the app to share diagnostics.");
        goto failed;
    }
    {
        NSURL *archive = [work URLByAppendingPathComponent:@"MotorStorm-diagnostics.zip"];
        if (!writeDiagnosticsZIP(snapshot, archive, error)) goto failed;
        [fm removeItemAtURL:snapshot error:nil];
        return archive;
    }
failed:
    [fm removeItemAtURL:work error:nil];
    return nil;
}

void MSDiagnosticsExport(NSURL *documents, void (^completion)(NSURL *, NSError *))
{
    dispatch_block_t prepare = ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSError *error = nil;
            NSURL *archive = createDiagnosticsArchive(documents, &error);
            dispatch_async(dispatch_get_main_queue(), ^{ completion(archive, error); });
        });
    };
    if (s_diagnostics) [s_diagnostics afterFlush:prepare];
    else prepare();
}

extern "C" const char* motorstorm_job_chain_dir(void)
{
    static char path[1024];
    if (path[0]) return path;
    NSArray<NSString *> *docs = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    if (docs.count == 0) return NULL;
    NSString *dir = [docs[0] stringByAppendingPathComponent:@"logs/performance"];
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil])
        return NULL;
    if (dir.length + 1 > sizeof path) return NULL;
    memcpy(path, dir.UTF8String, dir.length + 1);
    return path;
}
#else
extern "C" const char* motorstorm_job_chain_dir(void) { return nullptr; }
#endif
