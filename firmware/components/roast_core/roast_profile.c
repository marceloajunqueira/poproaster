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
