/**
 * @file roast_profile.c
 * @brief See header.
 */
#include "roast_core/roast_profile.h"

uint32_t roast_profile_total_duration_s(const roast_profile_t *profile)
{
    uint32_t total = 0;
    for (uint8_t i = 0; i < profile->point_count; i++) {
        total += profile->points[i].duration_s;
    }
    return total;
}

/* Finds which segment `elapsed_s` falls into - clamped to the last segment
 * once elapsed_s reaches/exceeds the profile's total duration. */
static uint8_t locate_segment(const roast_profile_t *profile, uint32_t elapsed_s)
{
    uint32_t cursor = 0;
    for (uint8_t i = 0; i < profile->point_count; i++) {
        uint32_t seg_end = cursor + profile->points[i].duration_s;
        if (elapsed_s < seg_end || i == profile->point_count - 1) {
            return i;
        }
        cursor = seg_end;
    }
    /* Empty profile - shouldn't happen, but stay defensive. */
    return 0;
}

/* Per operator requirement: target temp/fan must transition IMMEDIATELY at
 * a segment boundary (a step function) - NOT ramp/smooth gradually from the
 * previous segment's target over the new segment's duration. Each segment
 * simply holds its own configured target flat for its whole duration. */
float roast_profile_get_target_temp_c(const roast_profile_t *profile, uint32_t elapsed_s)
{
    if (profile->point_count == 0) {
        return 0.0f;
    }
    uint8_t idx = locate_segment(profile, elapsed_s);
    return profile->points[idx].target_temp_c;
}

uint8_t roast_profile_get_target_fan_pct(const roast_profile_t *profile, uint32_t elapsed_s)
{
    if (profile->point_count == 0) {
        return 0;
    }
    uint8_t idx = locate_segment(profile, elapsed_s);
    return profile->points[idx].target_fan_pct;
}

uint8_t roast_profile_get_segment_index(const roast_profile_t *profile, uint32_t elapsed_s)
{
    if (profile->point_count == 0) {
        return 0;
    }
    return locate_segment(profile, elapsed_s);
}

/* Default duration for an auto-appended/converted mandatory trailing
 * Cooling segment - a reasonable middle-of-the-road cool-down time. */
#define DEFAULT_COOLING_DURATION_S 300

void roast_profile_ensure_trailing_cooling(roast_profile_t *profile)
{
    if (profile == NULL) {
        return;
    }

    if (profile->point_count == 0) {
        profile->points[0].duration_s = DEFAULT_COOLING_DURATION_S;
        profile->points[0].target_temp_c = ROAST_PROFILE_COOLING_TEMP_C;
        profile->points[0].target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT;
        profile->points[0].is_cooling = true;
        profile->point_count = 1;
        return;
    }

    /* Per operator requirement: Cooling is now ALWAYS exactly the LAST
     * segment - normalize any legacy/imported/hand-crafted data that might
     * have it elsewhere (or nowhere, or in more than one place). */
    for (uint8_t i = 0; i < profile->point_count - 1; i++) {
        if (profile->points[i].is_cooling) {
            /* An intermediate segment was marked cooling by older data -
             * demote it back to a normal (heating) segment with a
             * reasonable target, since Cooling is no longer a per-segment
             * choice. */
            profile->points[i].is_cooling = false;
            if (profile->points[i].target_fan_pct < ROAST_PROFILE_FAN_MIN_PCT) {
                profile->points[i].target_fan_pct = ROAST_PROFILE_FAN_MIN_PCT;
            }
        }
    }

    roast_profile_point_t *last = &profile->points[profile->point_count - 1];
    if (last->is_cooling) {
        return; /* Already conforms. */
    }

    if (profile->point_count < ROAST_PROFILE_MAX_POINTS) {
        /* Room to append a fresh, dedicated Cooling segment. */
        roast_profile_point_t *cooling = &profile->points[profile->point_count];
        cooling->duration_s = DEFAULT_COOLING_DURATION_S;
        cooling->target_temp_c = ROAST_PROFILE_COOLING_TEMP_C;
        cooling->target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT;
        cooling->is_cooling = true;
        profile->point_count++;
    } else {
        /* Already at the max segment count - convert the last existing
         * segment into the mandatory Cooling one rather than silently
         * dropping the requirement (keeps its own duration, since that's
         * still a reasonable operator-configured value). */
        last->target_temp_c = ROAST_PROFILE_COOLING_TEMP_C;
        last->target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT;
        last->is_cooling = true;
    }
}
