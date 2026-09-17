/**
 * 电池电压到电量的换算（battery_curve.c，无 IDF 依赖）。
 *
 * 屏幕上的百分比与上发主机的电量等级都出自这一处换算，表里写错一个点
 * 就是「放到 3.8 V 电量还不动」或「满电只显示 90%」这类只能靠长时间
 * 观察才发现的问题，因此把曲线形状、插值与钳位钉在用例里。
 */
#include "host_test.h"

#include "battery_curve.h"

/** 曲线两端：实测满电电压 4.07V (4070mV) 及以上（插电到 4.15V）恒 100%，截止电压 2.87V (2870mV) 以下恒 0%。 */
static void clamps_outside_curve(void)
{
    CHECK_EQ(battery_percent_from_mv(4070), 100);
    CHECK_EQ(battery_percent_from_mv(4150), 100);
    CHECK_EQ(battery_percent_from_mv(5000), 100);
    CHECK_EQ(battery_percent_from_mv(2870), 0);
    CHECK_EQ(battery_percent_from_mv(2500), 0);
    CHECK_EQ(battery_percent_from_mv(0), 0);
}

/** 表上给出的点原值命中（插值不能把这些点带偏）。 */
static void hits_table_points(void)
{
    CHECK_EQ(battery_percent_from_mv(4005), 95);
    CHECK_EQ(battery_percent_from_mv(3838), 80);
    CHECK_EQ(battery_percent_from_mv(3644), 60);
    CHECK_EQ(battery_percent_from_mv(3605), 50);
    CHECK_EQ(battery_percent_from_mv(3489), 25);
    CHECK_EQ(battery_percent_from_mv(3412), 10);
}

/** 两点之间线性过渡，结果不会越过相邻档位。 */
static void interpolates_between_table_points(void)
{
    /* 3605 mV = 50%，3618 mV = 55%：过渡点不越界。 */
    CHECK_EQ(battery_percent_from_mv(3611), 52);
    CHECK_EQ(battery_percent_from_mv(3612), 53);
    const uint8_t low = battery_percent_from_mv(3608);
    const uint8_t high = battery_percent_from_mv(3615);
    CHECK(low >= 50 && low <= 55);
    CHECK(high >= 50 && high <= 55);
    CHECK(low <= high);
}

/**
 * 放电时电量只降不升：电压从满电一路扫到空电，百分比必须单调不增。
 * 表里出现逆序点（后一档比前一档高）时会在这里被抓住。
 */
static void percentage_never_rises_while_voltage_falls(void)
{
    uint8_t previous = 100;
    for (uint32_t mv = 4300; mv >= 2800; mv--) {
        const uint8_t percentage = battery_percent_from_mv(mv);
        CHECK(percentage <= previous);
        previous = percentage;
    }
    CHECK_EQ(previous, 0);
}

/** NS2 报告电量等级 0-9：每 10% 一档，满电为 9。 */
static void maps_ns2_level(void)
{
    CHECK_EQ(battery_ns2_level_from_percent(0), 0);
    CHECK_EQ(battery_ns2_level_from_percent(9), 0);
    CHECK_EQ(battery_ns2_level_from_percent(10), 1);
    CHECK_EQ(battery_ns2_level_from_percent(55), 5);
    CHECK_EQ(battery_ns2_level_from_percent(90), 9);
    CHECK_EQ(battery_ns2_level_from_percent(100), 9);
    /* 越界输入按满电处理，不上溢到 4 位字段之外。 */
    CHECK_EQ(battery_ns2_level_from_percent(120), 9);
}

HOST_TEST_SUITE(suite_battery, "battery",
                {"曲线两端钳位在 0-100%", clamps_outside_curve},
                {"表上的电压点原值命中", hits_table_points},
                {"两点之间线性过渡且不越档", interpolates_between_table_points},
                {"放电过程中电量单调不回升", percentage_never_rises_while_voltage_falls},
                {"NS2 电量等级按 10% 一档映射", maps_ns2_level});
