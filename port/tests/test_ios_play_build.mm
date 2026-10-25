#import "../../MotorStorm/MotorStorm iOS/OfflineDiagnostics.h"
#include "../../vendor/ps3recomp/include/ps3emu/passive_waits.h"
#include <cassert>

extern "C" const char *motorstorm_job_chain_dir(void);

int main()
{
    @autoreleasepool {
        // A release build cannot turn recording on, even through an old call.
        ps3_metrics_enable(1);
        assert(!ps3_metrics_on());
        assert(ps3_metric_begin() == 0);
        ps3_metric_add(PM_GPU_BUFFERS, 99);
        ps3_metric_max(PM_GPU_MAX_NS, 99);
        assert(ps3_metric_take(PM_GPU_BUFFERS) == 0);
        ps3_wait_record(PW_JOB_TOTAL, 1, 0);
        ps3_wait_counter waits = ps3_wait_take(PC_MAIN, PW_JOB_TOTAL);
        assert(!waits.ns && !waits.count && !waits.max_ns);
        ps3_metrics_thread_role(PC_MAIN);
        ps3_thread_sample threads = ps3_metrics_sample_threads();
        assert(!threads.active && !threads.started && !threads.exited);
        assert(ps3_metrics_current_role() == PC_COUNT);
        assert(motorstorm_job_chain_dir() == nullptr);

        int evaluated = 0;
        MSDiagnosticsEvent(@"ignored", @{ @"value": @(++evaluated) });
        MSDiagnosticsSetActive(++evaluated);
        MSDiagnosticsFinish(++evaluated);
        assert(evaluated == 0); // No hidden Objective-C allocation/evaluation.
    }
}
