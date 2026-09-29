// badge_ui —— 三块屏：等谁、要你按什么、按得怎么样。
//
// 这是二次开发应用自己的界面，**不沿用基线的 demo 菜单外壳**（规范里的硬要求）：
// 没有菜单、没有卡片、没有那只像素吉祥物——设备只有一件事要做，就是把「现在
// 需要你」这件事说清楚，所以界面按这个目的重新设计：一块静态的状态屏，和一块
// 只在你必须做决定时才出现的待答屏。
//
// 本文件只负责画：一切状态由 badge_state 算好放进 snapshot 再交进来。所以它没有
// 自己的业务逻辑，也就没有需要测试的判断——真机上要看的是布局和字，那是人工验收。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "badge_proto.h"

// 只前向声明 lv_font_t，不 include lvgl.h：这个头也被主机测试包含（状态机那套），
// 而主机上没有 LVGL 的头路径——一个 #include 就会让整组 host test 编译不过。
typedef struct _lv_font_t lv_font_t;

// 生成的中文字体（tools/gen_badge_font.sh）。声明放在头文件里而不是某个 .c 里：
// 界面和启动自检都要用它，而一个藏在别的编译单元里的 extern 只有那个文件看得见。
extern const lv_font_t badge_font_16;

// 生成的图标字体（同一个脚本）：顶栏那两枚传输图标（USB / 蓝牙）。它们是图标字体的
// 私有区字形（U+F287 / U+F293）——那两个 logo 在 Unicode 里没有码位，所以只能这样来。
extern const lv_font_t badge_font_icon_16;

typedef enum {
    BADGE_UI_STATUS = 0, // 平时：谁在跑、跑到哪一步
    BADGE_UI_ASK,        // 有事等你：权限确认，或模型提的问题
    BADGE_UI_INFO,       // 双击确定看的那一眼：模型、上下文、花费
    BADGE_UI_VIEW_COUNT, // 计数用（不是一屏）
} badge_ui_view_t;

// 链路的四种状态。刻意不 include badge_ble.h：那个头带着 esp_err.h（ESP-IDF 专有），
// 而本头会被主机测试包含（那边没有 ESP-IDF 的头路径）。
//
// 这四态值得各占一行界面，因为用户要做的事完全不同：去配对 / 把屏幕上的码敲进电脑 /
// 打开 tachi / 什么都不用做。只画一个「等电脑」会把它们全糊成同一件事。
typedef enum {
    BADGE_UI_LINK_UNPAIRED = 0, // 没有 bond：在广播，等电脑来配
    BADGE_UI_LINK_PAIRING,      // 正在配对：屏幕上必须显示那 6 位码
    BADGE_UI_LINK_PAIRED,       // 有 bond，但没连上
    BADGE_UI_LINK_SECURED,      // 连上了，而且已认证加密
} badge_ui_link_t;

// 出方向此刻走哪条链路。界面只负责显示它，而**判据只有一份**（main.c 的
// active_transport）：同一条判据既决定 send_line 往哪儿发，也决定顶栏写什么。
// 分成两份的话，屏幕上写着「BLE」、消息却从串口出去这种事只是时间问题。
typedef enum {
    BADGE_UI_TRANSPORT_USB = 0, // USB-Serial-JTAG，有线那条
    BADGE_UI_TRANSPORT_BLE,     // 蓝牙：只有「已认证加密」时才算可用
} badge_ui_transport_t;

// BADGE_UI_MAX_CHECKED 是勾选位掩码的宽度，与选项上限一致。
#define BADGE_UI_MAX_CHECKED BADGE_MAX_OPTIONS

typedef struct {
    badge_ui_view_t view;

    bool connected;      // 电脑那边的链路活着
    int battery_percent; // -1 = 读不到（BSP 读失败）

    // 状态屏
    bool has_session;
    char session_title[BADGE_TEXT_MAX];
    char state_label[BADGE_META_MAX];
    char state_detail[BADGE_DETAIL_MAX];
    size_t session_count; // >1 时底栏说明还有别人
    size_t session_total; // 主机报的会话数（≥ session_count）：底栏按它算，才不会少报

    // 待答屏。ask 为 NULL 时这一屏不该被显示（状态机不会切过来）。
    const badge_msg_t *ask;
    size_t selection;      // 高亮的选项下标（提问屏里是当前题目的选项）
    size_t question_index; // 第几道题
    uint8_t checked;       // 多选：按选项下标的位掩码

    // 一次性提示：回答已发出、主机拒绝了回答、这个问题要在电脑上答。
    const char *notice;

    // 会话信息屏那几行（非空 = 显示那一屏）。是**指针**：数据和字符串都活在状态机里，
    // 界面只读它——复制一份只会多一个会失同步的地方。
    const badge_info_t *info;

    // 录音中：已录多久、上限多少（毫秒）。recording 为 false 时这两个数没有意义。
    //
    // 它是这一屏上**唯一在动的数字**：30 秒的录音里，人得知道自己按下去之后它真的在录、
    // 以及离上限还有多远——否则按下去之后屏幕上什么都不动，看起来就是没生效。
    bool recording;
    uint32_t record_ms;
    uint32_t record_limit_ms;

    // 屏幕（背光）此刻亮不亮。界面唯一需要它的地方是「选中的那一行要不要跑马灯」：
    // 熄灭的屏幕背后还在动的动画纯粹是在烧电，而它还会让 LVGL 任务一直重绘。
    bool screen_on;

    // 蓝牙链路当前的状态，以及正在配对时那 6 位码（不在配对态是空串）。
    // passkey 非空时界面**忽略 view**、直接显示配对屏：这一刻用户唯一的任务就是
    // 照着这串数字敲进电脑，别的信息都是干扰。
    badge_ui_link_t link;
    const char *passkey;

    // 状态屏顶栏显示的那三个字母：出方向此刻走哪条链路（见 main.c 的 active_transport）。
    badge_ui_transport_t transport;
} badge_ui_snapshot_t;

// badge_ui_init 建好对象树并停在状态屏。必须在 LVGL 初始化之后、持锁调用。
void badge_ui_init(void);

// badge_ui_render 按快照刷新当前屏。持锁调用；调用方负责只在快照变化后调用它。
void badge_ui_render(const badge_ui_snapshot_t *snapshot);

// badge_ui_scroll 在待答屏上翻动超屏的正文，返回「这次真的翻了吗」。
//
// 返回值是给按键路径用的：长按上/下先问这里一句——翻了就吞掉这次按键；没翻（正文放
// 得下，或者已经翻到那一头）才交给状态机去换题。判据放在界面里，是因为「放不放得下」
// 是布局事实（LVGL 自己量得最准），而状态机是纯逻辑、只管按键的语义。
//
// direction > 0 往下读（正文往上滑，露出后面的内容），< 0 往回读。持锁调用。
bool badge_ui_scroll(int direction);
