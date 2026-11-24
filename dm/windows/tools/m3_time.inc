// SDK clock snapshots, including the IDs and contexts used to compare saved snapshots.
#include <time.h>
struct steady_point { int64_t seconds; uint8_t source[16]; };
struct system_context { int64_t offset; struct steady_point point; };
struct calendar { int16_t year; int8_t month, day, hour, minute, second, pad; };
struct calendar_info { int32_t weekday, yearday; char zone[8]; int32_t dst, offset; };
struct clock_snapshot {
    struct system_context user_ctx, network_ctx;
    int64_t user, network;
    struct calendar local, net_local;
    struct calendar_info info, net_info;
    struct steady_point steady;
    char location[36]; uint8_t automatic, type, pad[2];
};
static_assert(sizeof(struct clock_snapshot) == 0xd0 && offsetof(struct clock_snapshot, steady) == 0x90,
              "Horizon ClockSnapshot layout");
static uint64_t wall_units(void) {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    return ((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime) - UINT64_C(116444736000000000);
}
int64_t time_wall_now(void) { return (int64_t)(wall_units() / UINT64_C(10000000)); }
int64_t time_steady_now(void) {
    ULONGLONG t;
    QueryInterruptTimePrecise(&t);
    return (int64_t)(t / UINT64_C(10000000));
}
uint32_t time_posix(int64_t *out) {
    if (!dm_guest_range(out, sizeof(*out), 1)) dm_fail("time: invalid PosixTime output");
    *out = time_wall_now();
    return 0;
}
static int local_calendar(int64_t seconds, struct calendar *date, struct calendar_info *info) {
    struct tm t;
    if (_localtime64_s(&t, &seconds)) return 0;
    *date = (struct calendar){(int16_t)(1900 + t.tm_year), (int8_t)(t.tm_mon + 1), (int8_t)t.tm_mday,
                              (int8_t)t.tm_hour, (int8_t)t.tm_min, (int8_t)t.tm_sec, 0};
    *info = (struct calendar_info){.weekday = t.tm_wday, .yearday = t.tm_yday, .dst = t.tm_isdst > 0};
    info->offset = (int32_t)(_mkgmtime64(&t) - seconds);
    if (!info->offset) memcpy(info->zone, "UTC", 4);
    else {
        int minutes = abs(info->offset / 60);
        snprintf(info->zone, sizeof(info->zone), "%c%02d%02d", info->offset < 0 ? '-' : '+', minutes / 60, minutes % 60);
    }
    return 1;
}
uint32_t time_to_calendar(struct calendar *date, struct calendar_info *info, const int64_t *seconds) {
    if (!dm_guest_range(seconds, 8, 0) || (date && !dm_guest_range(date, sizeof(*date), 1)) ||
        (info && !dm_guest_range(info, sizeof(*info), 1))) dm_fail("time: invalid calendar arguments");
    struct calendar d; struct calendar_info i;
    if (!local_calendar(*seconds, &d, &i)) return 0x16e8;
    if (date) *date = d;
    if (info) *info = i;
    return 0;
}
uint32_t time_snapshot(struct clock_snapshot *s) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static struct system_context anchor;
    static int initialized;
    if (!dm_guest_range(s, sizeof(*s), 1)) dm_fail("time: invalid snapshot output");
    AcquireSRWLockExclusive(&lock);
    ULONGLONG before, after;
    QueryInterruptTimePrecise(&before);
    uint64_t wall = wall_units();
    QueryInterruptTimePrecise(&after);
    uint64_t steady = before + (after - before) / 2;
    int64_t offset = ((int64_t)wall - (int64_t)steady + 5000000) / 10000000;
    if (!initialized) {
        // A new process is a new clock source. Cross-launch snapshots fall back
        // to their absolute time, never subtract uptime across Windows reboots.
        if (BCryptGenRandom(NULL, anchor.point.source, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            dm_fail("time: cannot create steady clock source ID");
    }
    if (!initialized || llabs(offset - anchor.offset) > 1) {
        anchor.offset = offset;
        anchor.point.seconds = (int64_t)(steady / 10000000);
        initialized = 1;
    }
    struct system_context ctx = anchor;
    ReleaseSRWLockExclusive(&lock);
    *s = (struct clock_snapshot){.user_ctx = ctx, .network_ctx = ctx, .steady = ctx.point, .type = 1};
    s->steady.seconds = (int64_t)(steady / 10000000);
    s->user = s->network = (int64_t)(wall / 10000000);
    if (!local_calendar(s->user, &s->local, &s->info)) dm_fail("time: local calendar conversion failed");
    s->net_local = s->local; s->net_info = s->info;
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    if (GetDynamicTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID && tz.TimeZoneKeyName[0])
        WideCharToMultiByte(CP_UTF8, 0, tz.TimeZoneKeyName, -1, s->location, sizeof(s->location), NULL, NULL);
    if (!s->location[0]) memcpy(s->location, s->info.zone, sizeof(s->info.zone));
    return 0;
}
static int span_ns(int64_t a, int64_t b, int64_t *out) {
    uint64_t distance = b >= a ? (uint64_t)b - (uint64_t)a : (uint64_t)a - (uint64_t)b;
    if (distance > INT64_MAX / UINT64_C(1000000000)) return 0;
    *out = (b - a) * INT64_C(1000000000);
    return 1;
}
static void check_clocks(const struct clock_snapshot *a, const struct clock_snapshot *b) {
    if (!dm_guest_range(a, sizeof(*a), 0) || !dm_guest_range(b, sizeof(*b), 0)) dm_fail("time: invalid saved snapshots");
}
int time_snapshot_span(int64_t *out, const struct clock_snapshot *a, const struct clock_snapshot *b) {
    check_clocks(a, b);
    if (!dm_guest_range(out, 8, 1)) dm_fail("time: invalid span output");
    if (!memcmp(a->steady.source, b->steady.source, 16)) return span_ns(a->steady.seconds, b->steady.seconds, out);
    return a->network && b->network && span_ns(a->network, b->network, out);
}
int64_t time_user_difference(const struct clock_snapshot *a, const struct clock_snapshot *b) {
    int64_t ns = 0;
    check_clocks(a, b);
    if (!(a->automatic && b->automatic) && !memcmp(a->user_ctx.point.source, b->user_ctx.point.source, 16))
        span_ns(a->user_ctx.offset, b->user_ctx.offset, &ns);
    return ns;
}
