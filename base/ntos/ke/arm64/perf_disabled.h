/* This early kernel does not initialize the WMI performance logger. Exclude
 * optional instrumentation at compile time; do not supply fake log/timestamp
 * routines. Timer expiration, wait satisfaction and DPC calls stay original. */
#ifndef XP_ARM64_PERF_DISABLED_H
#define XP_ARM64_PERF_DISABLED_H
#undef PERFINFO_IS_GROUP_ON
#define PERFINFO_IS_GROUP_ON(Group) FALSE
#endif
