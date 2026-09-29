// badge_power 的主机测试。这里守的是一条红线：屏幕灭着藏一条「有人在等你」。
#include <assert.h>
#include <stdio.h>

#include "badge_power.h"

static void test_starts_on_and_blanks_after_the_idle_window(void)
{
    badge_power_t power;

    badge_power_init(&power, 1000U);
    assert(power.screen_on);

    // 差一毫秒不熄。
    assert(!badge_power_tick(&power, 1000U + BADGE_SCREEN_OFF_MS - 1U, false));
    assert(power.screen_on);
    // 正好到点就熄。
    assert(badge_power_tick(&power, 1000U + BADGE_SCREEN_OFF_MS, false));
    assert(!power.screen_on);
    // 熄着的时候再 tick 不算翻转：调用方不该每 100ms 重复写一次背光。
    assert(!badge_power_tick(&power, 1000U + BADGE_SCREEN_OFF_MS * 3U, false));
}

static void test_pending_ask_never_blanks(void)
{
    badge_power_t power;

    badge_power_init(&power, 0U);
    assert(!badge_power_tick(&power, 10U * 60U * 1000U, true)); // 十分钟没人动它
    assert(power.screen_on);
}

static void test_pending_ask_wakes_a_blanked_screen(void)
{
    badge_power_t power;

    badge_power_init(&power, 0U);
    (void)badge_power_tick(&power, BADGE_SCREEN_OFF_MS, false);
    assert(!power.screen_on);

    // 主机递进来一条 ask：屏幕必须立刻亮，而不是等下一次按键。
    assert(badge_power_tick(&power, BADGE_SCREEN_OFF_MS + 100U, true));
    assert(power.screen_on);
}

static void test_activity_wakes_and_resets_the_window(void)
{
    badge_power_t power;

    badge_power_init(&power, 0U);
    (void)badge_power_tick(&power, BADGE_SCREEN_OFF_MS, false);
    assert(!power.screen_on);

    assert(badge_power_activity(&power, BADGE_SCREEN_OFF_MS));
    assert(power.screen_on);

    // 亮着时的一次活动不翻转，但把窗口重新起算。
    assert(!badge_power_activity(&power, BADGE_SCREEN_OFF_MS + 5000U));
    assert(!badge_power_tick(&power, BADGE_SCREEN_OFF_MS * 2U - 1U, false));
    assert(badge_power_tick(&power, BADGE_SCREEN_OFF_MS * 2U + 5000U, false));
}

static void test_tick_alone_does_not_count_as_activity(void)
{
    badge_power_t power;
    uint32_t now;

    badge_power_init(&power, 0U);
    // 每 100ms tick 一次并跨过整个窗口：tick 本身不是活动，屏幕必须熄。
    for (now = 100U; now <= BADGE_SCREEN_OFF_MS + 100U; now += 100U) {
        (void)badge_power_tick(&power, now, false);
    }
    assert(!power.screen_on);
}

static void test_wrap_around_does_not_mistake_the_window(void)
{
    badge_power_t power;
    const uint32_t near_wrap = 0xFFFFFFFFU - 1000U;

    // 起点贴着回绕处：now 回绕成小值之后，空闲窗口仍按无符号差算。
    badge_power_init(&power, near_wrap);
    assert(!badge_power_tick(&power, near_wrap + 1000U, false));
    assert(badge_power_tick(&power, near_wrap + BADGE_SCREEN_OFF_MS, false));
    assert(!power.screen_on);
}

int main(void)
{
    test_starts_on_and_blanks_after_the_idle_window();
    test_pending_ask_never_blanks();
    test_pending_ask_wakes_a_blanked_screen();
    test_activity_wakes_and_resets_the_window();
    test_tick_alone_does_not_count_as_activity();
    test_wrap_around_does_not_mistake_the_window();
    puts("test_badge_power: OK");
    return 0;
}
