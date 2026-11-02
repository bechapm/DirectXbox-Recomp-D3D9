/*
 * kernel_tz_setting - the EEPROM time zone (src/kernel/kernel_xbox.c, after
 * upstream #216).
 *
 * The bias used to be a fixed -300 for every player. A bias is UTC minus
 * local time, so that was UTC+5, not the Eastern time its comment named, and
 * it was wrong everywhere else too: XAPI turns the UTC clock into local time
 * with it, so saves and clocks were stamped hours off. Checked:
 *   - XC_TIMEZONE_BIAS is the host's. On POSIX, TZ pins the host zone to a
 *     known one first, so the sign is checked too: TZ=XST+5 is five hours
 *     behind UTC, a bias of +300.
 *   - XC_MAX_OS, the user-settings block XAPI reads the time zone through,
 *     agrees with the single queries, field by field, and its checksum
 *     still holds.
 *
 * No files: RECOMP_CONSOLE_DIR=off, so no player EEPROM is read.
 */
#include "kernel.h"
#include "xbox_console_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* This settings-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)

static void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static LONG query_long(ULONG index)
{
    ULONG type = 0, len = 0;
    LONG v = 0x5A5A5A5A;
    xbox_ExQueryNonVolatileSetting(index, &type, &v, sizeof v, &len);
    return v;
}

int main(void)
{
    uint8_t blk[0x60];
    ULONG type = 0, len = 0;
    LONG bias, want;
    uint32_t sum;

    printf("kernel_tz_setting: running\n");
    set_env("RECOMP_CONSOLE_DIR", "off");
#if defined(_WIN32)
    {
        TIME_ZONE_INFORMATION tzi;
        memset(&tzi, 0, sizeof tzi);
        GetTimeZoneInformation(&tzi);
        want = tzi.Bias;
    }
#else
    /* Pinned before the first query: the zone is read once per run. */
    set_env("TZ", "XST+5");
    tzset();
    want = 300;
#endif

    bias = query_long(XC_TIMEZONE_BIAS);
    printf("  bias %ld, want %ld\n", (long)bias, (long)want);
    CHECK("XC_TIMEZONE_BIAS is the host's, with UTC = local + bias", bias == want);

    memset(blk, 0xCC, sizeof blk);
    CHECK("XC_MAX_OS succeeds",
          xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, blk, sizeof blk, &len) == STATUS_SUCCESS);
    CHECK("XC_MAX_OS is a 0x60-byte binary block", type == 3 && len == 0x60);
    {
        LONG v;
        memcpy(&v, blk + 0x04, 4);
        CHECK("the block's bias is the single query's", v == bias);
        memcpy(&v, blk + 0x28, 4);
        CHECK("the block's standard bias is the single query's", v == query_long(XC_TZ_STD_BIAS));
        memcpy(&v, blk + 0x2C, 4);
        CHECK("the block's daylight bias is the single query's", v == query_long(XC_TZ_DLT_BIAS));
        memcpy(&v, blk + 0x08, 4);
        CHECK("the block's standard name is the single query's", v == query_long(XC_TZ_STD_NAME));
        memcpy(&v, blk + 0x30, 4);
        CHECK("language is still at +0x30", v == query_long(XC_LANGUAGE));
        memcpy(&v, blk + 0x5C, 4);
        CHECK("DVD region is still at +0x5C", v == query_long(XC_DVD_REGION));
    }
    memcpy(&sum, blk, 4);
    CHECK("the block's checksum holds", sum == xbox_eeprom_checksum(blk + 4, 0x5C));

    printf("kernel_tz_setting: %s (%d failure%s)\n", failures ? "FAILED" : "OK",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
