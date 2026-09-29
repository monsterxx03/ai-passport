#include "badge_power.h"

void badge_power_init(badge_power_t *power, uint32_t now_ms)
{
    power->screen_on = true;
    power->last_activity_ms = now_ms;
}

bool badge_power_activity(badge_power_t *power, uint32_t now_ms)
{
    const bool woke = !power->screen_on;

    power->screen_on = true;
    power->last_activity_ms = now_ms;
    return woke;
}

bool badge_power_tick(badge_power_t *power, uint32_t now_ms, bool has_pending_ask)
{
    if (has_pending_ask) {
        // 有待答项：点亮并保持。这里顺带把空闲计时重置，所以「答完最后一条」之后
        // 还会完整地亮 BADGE_SCREEN_OFF_MS，而不是刚答完就黑掉。
        const bool woke = !power->screen_on;

        power->screen_on = true;
        power->last_activity_ms = now_ms;
        return woke;
    }
    if (power->screen_on && (uint32_t)(now_ms - power->last_activity_ms) >= BADGE_SCREEN_OFF_MS) {
        power->screen_on = false;
        return true;
    }
    return false;
}
