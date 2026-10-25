#import <Foundation/Foundation.h>
#import "../../MotorStorm/MotorStorm iOS/OfflineDiagnostics.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_metrics.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_threads.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_waits.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include <unistd.h>

extern "C" int rsx_metal_passive_frame_sample(ps3_passive_frame *out) {
    *out={30,30,33.333,34,40,0}; return 1;
}
extern "C" int ms_rsx_phase_account_enabled(void) { return 0; }
extern "C" void ms_rsx_phase_take(uint64_t ns[19], uint32_t calls[19],
    uint64_t *draws, uint64_t *packed, uint64_t *fallback, uint64_t *vertices,
    uint64_t *packBytes, uint64_t *constBytes, uint64_t *indexBytes)
{
    memset(ns,0,19*sizeof(*ns)); memset(calls,0,19*sizeof(*calls));
    *draws=*packed=*fallback=*vertices=*packBytes=*constBytes=*indexBytes=0;
}
extern "C" void offline_test_cpu_worker(int role);

static NSURL *exportDiagnostics(NSURL *documents, BOOL expectSuccess)
{
    __block BOOL done=NO;
    __block NSURL *result=nil;
    MSDiagnosticsExport(documents, ^(NSURL *archive, NSError *error) {
        assert(NSThread.isMainThread);
        if (expectSuccess && error) fprintf(stderr,"Export error: %s\n",error.description.UTF8String);
        assert(expectSuccess ? (archive && !error) : (!archive && error));
        result=archive; done=YES;
    });
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:15];
    while (!done && deadline.timeIntervalSinceNow>0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(done);
    return result;
}

int main(int argc,char **argv) {
    assert(argc==2);
    // Snapshotting while producers run must not lose increments.
    ps3_metrics_enable(1);
    std::atomic<int> running{4}; std::vector<std::thread> threads;
    for (int t=0;t<4;t++) threads.emplace_back([&] {
        for (unsigned i=1;i<=25000;i++) { ps3_metric_add(PM_GPU_BUFFERS,1); ps3_metric_max(PM_GPU_MAX_NS,i); }
        --running;
    });
    uint64_t total=0;
    while (running.load()) { total+=ps3_metric_take(PM_GPU_BUFFERS); std::this_thread::yield(); }
    for (auto& t:threads) t.join();
    total+=ps3_metric_take(PM_GPU_BUFFERS);
    assert(total==100000 && ps3_metric_take(PM_GPU_MAX_NS)==25000);
    assert(ps3_metric_take(PM_GPU_BUFFERS)==0);
    ps3_metrics_enable(0); assert(!ps3_metric_begin());
    // Short-lived C worker threads finish between logger ticks. Their CPU
    // must be retained, slots reused, and a second snapshot must not recount it.
    std::thread disabled([]{offline_test_cpu_worker(PC_PPU);}); disabled.join();
    assert(ps3_metrics_sample_threads().started==0);
    ps3_metrics_enable(1);
    for (unsigned wave=0;wave<10;wave++) {
        threads.clear();
        for (unsigned i=0;i<16;i++) threads.emplace_back([i]{offline_test_cpu_worker(i&1 ? PC_SPURS : PC_SPU_AUDIO);});
        for (auto& t:threads) t.join();
    }
    ps3_thread_sample exited=ps3_metrics_sample_threads();
    assert(exited.started==160 && exited.exited==160 && !exited.active);
    assert(!exited.errors && !exited.dropped);
    assert(exited.cpu_ns[PC_SPURS]>200000000 && exited.cpu_ns[PC_SPU_AUDIO]>200000000);
    ps3_thread_sample empty=ps3_metrics_sample_threads();
    assert(!empty.cpu_ns[PC_SPURS] && !empty.cpu_ns[PC_SPU_AUDIO] && !empty.exited);
    // Also sample a live worker, then its exit without losing/doubling CPU.
    std::atomic<int> ready{0};
    std::thread live([&]{offline_test_cpu_worker(PC_PHYSICS); ready=1;
        while (ready==1) usleep(1000); offline_test_cpu_worker(PC_PHYSICS);});
    while (!ready) usleep(1000);
    ps3_thread_sample active=ps3_metrics_sample_threads();
    assert(active.active==1 && active.cpu_ns[PC_PHYSICS]>2000000);
    ready=2; live.join();
    ps3_thread_sample final=ps3_metrics_sample_threads();
    assert(final.active==0 && final.exited==1 && final.cpu_ns[PC_PHYSICS]>2000000);
    ps3_metrics_enable(0);
    @autoreleasepool {
        NSURL *documents=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
        int savedStderr=dup(STDERR_FILENO);
        NSFileManager *fm=NSFileManager.defaultManager;
        NSURL *parent=[documents URLByAppendingPathComponent:@"logs/performance"];
        exportDiagnostics(documents, NO);
        for (int i=0;i<7;i++) {
            NSURL *old=[parent URLByAppendingPathComponent:[NSString stringWithFormat:@"session-old-%d",i]];
            assert([fm createDirectoryAtURL:old withIntermediateDirectories:YES attributes:nil error:nil]);
            [@{@"owner":@"MotorStormOfflineDiagnostics-v1"} writeToURL:[old URLByAppendingPathComponent:@"owner.plist"] atomically:YES];
        }
        NSURL *unrelated=[parent URLByAppendingPathComponent:@"session-user-folder"];
        [fm createDirectoryAtURL:unrelated withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL *native=MSDiagnosticsStart(documents); assert(native && ps3_metrics_on());
        assert(freopen(native.fileSystemRepresentation,"a",stdout));
        assert(freopen(native.fileSystemRepresentation,"a",stderr));
        // Trigger console capping without recording any game data.
        std::vector<char> noise(2*1024*1024+100,'x'); fwrite(noise.data(),1,noise.size(),stdout);
        MSDiagnosticsEvent(@"start",@{});
        ps3_metric_add(PM_AUDIO_BLOCKS,100); ps3_metric_add(PM_AUDIO_SILENT,10);
        ps3_metric_add(PM_GPU_NS,10000000); ps3_metric_add(PM_GPU_BUFFERS,2);
        sleep(2); // exercise the actual dispatch timer and shared C/ObjC++ state
        NSString *payload=[@"x" stringByPaddingToLength:8192 withString:@"x" startingAtIndex:0];
        for (int i=0;i<500;i++) MSDiagnosticsEvent(@"rotation-test",@{@"payload":payload});
        MSDiagnosticsSetActive(NO); MSDiagnosticsSetActive(YES); MSDiagnosticsFinish(0);
        NSURL *session=native.URLByDeletingLastPathComponent;
        BOOL complete=NO;
        for (int attempt=0;attempt<100 && !complete;attempt++) {
            usleep(50000);
            for (int p=0;p<3;p++) {
                NSString *text=[NSString stringWithContentsOfURL:[session URLByAppendingPathComponent:[NSString stringWithFormat:@"samples-%d.jsonl",p]] encoding:NSUTF8StringEncoding error:nil];
                if ([text containsString:@"engine_returned"]) complete=YES;
            }
        }
        assert(complete && !ps3_metrics_on());
        NSArray *contents=[fm contentsOfDirectoryAtURL:parent includingPropertiesForKeys:nil options:0 error:nil];
        assert(contents.count==6 && [fm fileExistsAtPath:unrelated.path]); // five owned + unrelated
        unsigned files=0,events=0,samples=0;
        for (NSURL *file in [fm contentsOfDirectoryAtURL:session includingPropertiesForKeys:nil options:0 error:nil]) {
            if (![file.pathExtension isEqual:@"jsonl"]) continue;
            files++;
            assert([[fm attributesOfItemAtPath:file.path error:nil] fileSize]<=1024*1024);
            NSString *text=[NSString stringWithContentsOfURL:file encoding:NSUTF8StringEncoding error:nil];
            for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
                if (!line.length) continue;
                NSDictionary *row=[NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
                assert(row);
                if ([row[@"type"] isEqual:@"sample"]) {
                    samples++; assert([row[@"display_fps"] doubleValue]==30);
                    assert([row[@"cpu_by_role_ms"] count]==PC_COUNT);
                    assert(row[@"audio_sleep_overshoot_max_ms"] && row[@"thread_clock_errors"]);
                    assert(row[@"async_reused"] && row[@"async_threads_created"] && row[@"async_launch_cpu_ms"]);
                    assert([row[@"waits_by_role"] count]==PC_COUNT+1);
                    assert(row[@"waits_by_role"][@"main_game"][@"job_pause"][@"reasons"]);
                }
                else events++;
            }
        }
        assert(files==3 && events>0 && samples>0);
        unsigned long long before=[[fm attributesOfItemAtPath:native.path error:nil] fileSize];
        fputs("SHOULD_NOT_GROW",stdout); fflush(stdout);
        assert(before==[[fm attributesOfItemAtPath:native.path error:nil] fileSize]);
        dup2(savedStderr,STDERR_FILENO); close(savedStderr);
        NSDictionary *metadata=[NSJSONSerialization JSONObjectWithData:
            [NSData dataWithContentsOfURL:[session URLByAppendingPathComponent:@"session.json"]] options:0 error:nil];
        assert([[NSUUID alloc] initWithUUIDString:metadata[@"executable_uuid"]]);
        // Export must include only known diagnostics files, not sibling game
        // data, unowned sessions, or arbitrary files inside an owned session.
        [@"not diagnostics" writeToURL:[session URLByAppendingPathComponent:@"secret.dat"]
                           atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [fm createDirectoryAtURL:[documents URLByAppendingPathComponent:@"game"]
     withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL *archive=exportDiagnostics(documents, YES);
        assert([fm copyItemAtURL:archive toURL:[documents.URLByDeletingLastPathComponent
                                               URLByAppendingPathComponent:@"diagnostics.zip"] error:nil]);
        assert([fm removeItemAtURL:archive.URLByDeletingLastPathComponent error:nil]);
    }
    return 0;
}
