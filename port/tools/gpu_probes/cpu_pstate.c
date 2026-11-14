// SPDX-License-Identifier: GPL-2.0-or-later
// CPU cluster performance state residency (as gpu_pstate.c, for PCPU*/ECPU* of "CPU Stats"): whether a slow
//   clang -O2 cpu_pstate.c -o cpu_pstate -framework CoreFoundation -lIOReport && ./cpu_pstate [seconds]
// Prints the share of each GPU P-state (OFF, P1 = slowest ... Pn = fastest) per second and the mean
// active state: whether a frame-rate drop is the GPU clocking down, not more work. Also the GPU
// power of that second (IOReport "Energy Model"; CPU energy is not there on an M3 Max), before the "|".
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct IOReportSubscription *IOReportSubscriptionRef;
extern CFDictionaryRef IOReportCopyChannelsInGroup(CFStringRef group, CFStringRef subgroup, uint64_t a, uint64_t b, uint64_t c);
extern IOReportSubscriptionRef IOReportCreateSubscription(void *a, CFMutableDictionaryRef channels, CFMutableDictionaryRef *out, uint64_t d, CFTypeRef e);
extern CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef sub, CFMutableDictionaryRef channels, CFTypeRef a);
extern CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef a, CFDictionaryRef b, CFTypeRef c);
extern CFStringRef IOReportChannelGetChannelName(CFDictionaryRef channel);
extern int IOReportStateGetCount(CFDictionaryRef channel);
extern CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef channel, int index);
extern int64_t IOReportStateGetResidency(CFDictionaryRef channel, int index);
extern void IOReportMergeChannels(CFDictionaryRef a, CFDictionaryRef b, CFTypeRef c);
extern int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef channel, int32_t index);
extern CFStringRef IOReportChannelGetUnitLabel(CFDictionaryRef channel);

// Joules in one Energy Model sample, from its unit label (mJ, uJ, nJ).
static double joules(CFDictionaryRef ch) {
    char unit[16] = "";
    CFStringRef u = IOReportChannelGetUnitLabel(ch);
    if (u) CFStringGetCString(u, unit, sizeof unit, kCFStringEncodingUTF8);
    const double v = (double)IOReportSimpleGetIntegerValue(ch, 0);
    if (!strcmp(unit, "mJ")) return v / 1e3;
    if (!strcmp(unit, "uJ")) return v / 1e6;
    if (!strcmp(unit, "nJ")) return v / 1e9;
    return 0;
}

int main(int argc, char **argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 10;
    CFDictionaryRef found = IOReportCopyChannelsInGroup(CFSTR("CPU Stats"), CFSTR("CPU Complex Performance States"), 0, 0, 0);
    if (!found) { fprintf(stderr, "no CPU Complex Performance States channels\n"); return 1; }
    CFMutableDictionaryRef channels = CFDictionaryCreateMutableCopy(NULL, 0, found), subscribed = NULL;
    CFDictionaryRef energy = IOReportCopyChannelsInGroup(CFSTR("Energy Model"), NULL, 0, 0, 0);
    if (energy) IOReportMergeChannels(channels, energy, NULL);
    IOReportSubscriptionRef sub = IOReportCreateSubscription(NULL, channels, &subscribed, 0, NULL);
    if (!sub) { fprintf(stderr, "no subscription\n"); return 1; }
    CFDictionaryRef before = IOReportCreateSamples(sub, subscribed, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int s = 0; s < seconds; ++s) {
        sleep(1);
        CFDictionaryRef now = IOReportCreateSamples(sub, subscribed, NULL);
        CFDictionaryRef delta = IOReportCreateSamplesDelta(before, now, NULL);
        CFArrayRef list = CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
        double gpu_j = 0;
        for (CFIndex i = 0; list && i < CFArrayGetCount(list); ++i) {
            CFDictionaryRef ch = CFArrayGetValueAtIndex(list, i);
            char name[64] = "";
            CFStringRef n = IOReportChannelGetChannelName(ch);
            if (n) CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8);
            if (!strcmp(name, "GPU Energy")) gpu_j += joules(ch);
        }
        for (CFIndex i = 0; list && i < CFArrayGetCount(list); ++i) {
            CFDictionaryRef ch = CFArrayGetValueAtIndex(list, i);
            char name[64] = "";
            CFStringRef n = IOReportChannelGetChannelName(ch);
            if (n) CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8);
            if (strcmp(name, "PCPU") != 0 && strcmp(name, "PCPU1") != 0 && strcmp(name, "ECPU") != 0) continue; printf("%s ", name);
            const int count = IOReportStateGetCount(ch);
            int64_t total = 0, active = 0; double weighted = 0;
            for (int k = 0; k < count; ++k) total += IOReportStateGetResidency(ch, k);
            printf("%3d s:", s + 1);
            for (int k = 0; k < count; ++k) {
                char state[16] = "";
                CFStringRef sn = IOReportStateGetNameForIndex(ch, k);
                if (sn) CFStringGetCString(sn, state, sizeof state, kCFStringEncodingUTF8);
                const int64_t r = IOReportStateGetResidency(ch, k);
                if (k > 0) { active += r; weighted += (double)r * k; }
                if (total && r * 100 / total >= 1) printf(" %s %lld%%", state, (long long)(r * 100 / total));
            }
            printf(" GPU %.1f W | active %lld%%, mean state %.1f of %d\n", gpu_j, total ? (long long)(active * 100 / total) : 0,
                   active ? weighted / (double)active : 0.0, count - 1);
        }
        CFRelease(delta);
        CFRelease(before);
        before = now;
    }
    return 0;
}
