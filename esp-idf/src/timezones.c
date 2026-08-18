/* tzLookup — binary search over the generated zone table (timezones_gen.c),
 * which is strcmp-sorted by name. Everything stays in flash. */
#include "timezones.h"
#include <string.h>

const char* tzLookup(const char* iana) {
    int lo = 0, hi = TZ_COUNT - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(iana, TZ_NAMES[mid]);
        if (c == 0) return TZ_POSIX[mid];
        if (c < 0) hi = mid - 1;
        else       lo = mid + 1;
    }
    return 0;
}
