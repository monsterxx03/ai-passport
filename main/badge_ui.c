#include "badge_ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

// 配色。深色是为了佩戴/桌面场景：屏幕大部分时间只是静静显示一句话，
// 亮底在暗环境里刺眼，而"有人等你"这件事要用颜色说清楚——琥珀是待答，
// 绿是放行，红是拒绝。
#define COL_BG 0x0F1319
#define COL_PANEL 0x191F28
#define COL_TEXT 0xE8EBEF
#define COL_MUTED 0x7C8798
#define COL_LINE 0x252C36
#define COL_ACCENT 0xFFB020
#define COL_THINK 0x5B9BD5
#define COL_ALLOW 0x46B87A
#define COL_DENY 0xE2554E

// 中文必须显式选字体：默认的 Montserrat 不含中文字形，而 UTF-8 正确、编译通过
// 都不代表屏幕上能看见——缺字会是空白或方框（见仓库的中文字体检查清单）。
//
// 用的是自己生成的字体（tools/gen_badge_font.sh），不是 LVGL 内置的 CJK 子集：
// 那个子集小到连「脑」「还」「连」都不含，而这块屏要显示的是 tachi 下发的任意
// 中文（会话标题、命令预览、模型提的问题），缺一个字就是方框。
#define FONT_UI (&badge_font_16)
// 顶栏那两枚传输图标走单独的图标字体（见 badge_ui.h 的声明）。
#define FONT_ICON (&badge_font_icon_16)

// tachi 的头像（tools/mk_badge_avatar.py 从 desktop 的 app 图标生成）。
// 声明放在这里而不是头文件里：它是匿名结构体 typedef，没法前向声明，
// 而这个头会被主机测试包含（那边没有 LVGL 的头）。
extern const lv_image_dsc_t badge_avatar;

static lv_obj_t *s_status_scr;
static lv_obj_t *s_ask_scr;
static lv_obj_t *s_pair_scr;

static lv_obj_t *s_st_battery;
static lv_obj_t *s_st_transport;
static lv_obj_t *s_st_state;
static lv_obj_t *s_st_title;
static lv_obj_t *s_st_detail;
static lv_obj_t *s_st_tools;
static lv_obj_t *s_st_footer;

static lv_obj_t *s_ak_heading;
static lv_obj_t *s_ak_battery;
static lv_obj_t *s_ak_subject;
static lv_obj_t *s_ak_body;
static lv_obj_t *s_ak_viewport;          // 正文的可滚容器：长题面靠它看全文
static const badge_msg_t *s_ak_body_ask; // 正文此刻属于哪条等待
static size_t s_ak_body_index;           // ...以及哪一题（换题要把正文滚回顶部）
static lv_obj_t *s_ak_options[BADGE_MAX_OPTIONS];
static lv_obj_t *s_ak_labels[BADGE_MAX_OPTIONS];
static lv_obj_t *s_ak_footer;

// 配对屏：用户要在另一台设备上照着敲这串数字，所以它值得独占一屏。
static lv_obj_t *s_pair_battery;
static lv_obj_t *s_pair_code;

static lv_obj_t *s_current_scr;

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);

    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_style_text_font(label, FONT_UI, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// one_line 把 label 约束成「一行，超出打点」。
//
// 光设宽度不够：LVGL 的 LV_LABEL_LONG_MODE_DOTS 是「先按对象尺寸换行，再在最后一行
// 打点」，高度不定死就会撑出第二行，压到下面那一行上去——这块屏上相邻元素只隔 24px
// （选项之间甚至贴身），两行文本必然重叠。
static void one_line(lv_obj_t *label, int width, bool centered)
{
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, (int32_t)badge_font_16.line_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    if (centered) {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

// 跑马灯速度（px/s）。它填进的是样式的 anim_duration——在 LVGL 9.5 里那个样式承载
// 的是**速度**（LVGL 自己就这么用：把速度与时长上下限编码进一个值），不是一趟的毫秒
// 数。默认 40 对这块小屏偏慢，长一点的选项要等好几秒才滚完。
#define MARQUEE_SPEED_PX_S 60

// marquee 让一行文字横向循环滚动。文字放得下时 LVGL 不会启动动画，所以它可以常开着。
//
// 「模式没变就直接返回」不是优化，是必要条件：lv_label_set_long_mode 每次都无条件删掉
// 动画并把 offset 归零（源码里第一件事就是 lv_anim_delete + lv_point_set），而状态屏
// 每 10 秒会收到一次心跳、因而重画一次——不挡这一下，长文案会每 10 秒从头开始，永远
// 滚不到后半句。
static void marquee(lv_obj_t *label, bool on)
{
    lv_label_long_mode_t mode =
        on ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS;

    if (lv_label_get_long_mode(label) == mode) {
        return;
    }
    if (on) {
        lv_obj_set_style_anim_duration(label,
                                       lv_anim_speed_clamped(MARQUEE_SPEED_PX_S, 300, 10000), 0);
    }
    lv_label_set_long_mode(label, mode);
}

static void build_status_screen(void)
{
    s_status_scr = make_screen();

    // 顶栏就是标题栏：左边是**当前会话的名字**（没有会话时回退成品牌名，这一栏不空
    // 着），右边是传输图标 + 电量。会话名放这里而不是主体里，是因为它属于「这是哪一
    // 个窗口」这一层，而主体那几行要说的是「它此刻在干什么、做到哪了」。
    s_st_title = make_label(s_status_scr, COL_MUTED);
    // 宽度按右侧那一簇留出来：图标 ~14 + 间距 8 + 电量 ~34 + 右边距 20。
    one_line(s_st_title, 140, false);
    lv_obj_align(s_st_title, LV_ALIGN_TOP_LEFT, 20, 12);

    s_st_battery = make_label(s_status_scr, COL_MUTED);
    lv_obj_align(s_st_battery, LV_ALIGN_TOP_RIGHT, -20, 12);

    // 传输指示贴在电量左边成一簇：屏幕上画的和实际走的必须是同一条判据（见 main.c 的
    // active_transport）。跟着标题排版的话，标题一长就会把它顶出屏幕。两枚图标走图标
    // 字体（USB / 蓝牙的 logo 没有 Unicode 码位，见 badge_ui.h）。
    s_st_transport = make_label(s_status_scr, COL_MUTED);
    lv_obj_set_style_text_font(s_st_transport, FONT_ICON, 0);
    lv_obj_align_to(s_st_transport, s_st_battery, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    // 头像就是原来那个圆的位置，它不再靠颜色说话——一张脸比一个色块更像「有人在」。
    //
    // 状态色**现在落在下面那行文字上**（见 render_status）：这块屏上从 44 到 261 只有
    // 217px，而头像（120）加三行字（3×31=93）正好是 213 —— 再单占一行去放一颗色点
    // 就没地方了，何况那颗点和「执行 / 思考」本来就是同一件事。
    lv_obj_t *avatar = lv_image_create(s_status_scr);

    lv_image_set_src(avatar, &badge_avatar);
    lv_obj_align(avatar, LV_ALIGN_TOP_MID, 0, 44);

    s_st_state = make_label(s_status_scr, COL_TEXT);
    one_line(s_st_state, 200, true);
    lv_obj_align(s_st_state, LV_ALIGN_TOP_MID, 0, 168);

    // 三行挨着排：状态 → 它此刻在干什么 → 本轮做到第几个工具。行盒是字体的
    // line_height（31），本身已经含了行距，所以行间没有额外空隙——上面那两行原本就
    // 是这么排的。
    s_st_detail = make_label(s_status_scr, COL_MUTED);
    one_line(s_st_detail, 200, true);
    lv_obj_align(s_st_detail, LV_ALIGN_TOP_MID, 0, 199);

    s_st_tools = make_label(s_status_scr, COL_MUTED);
    one_line(s_st_tools, 200, true);
    lv_obj_align(s_st_tools, LV_ALIGN_TOP_MID, 0, 230);

    s_st_footer = make_label(s_status_scr, COL_MUTED);
    one_line(s_st_footer, 220, true);
    lv_obj_align(s_st_footer, LV_ALIGN_BOTTOM_MID, 0, -24);
}

static void build_ask_screen(void)
{
    size_t i;

    s_ask_scr = make_screen();

    s_ak_heading = make_label(s_ask_scr, COL_ACCENT);
    // 宽度 150 是给右上角的电量留位置：标题从 x=20 起，电量从右侧 220 起。
    one_line(s_ak_heading, 150, false);
    lv_obj_align(s_ak_heading, LV_ALIGN_TOP_LEFT, 20, 14);

    s_ak_battery = make_label(s_ask_scr, COL_MUTED);
    lv_obj_align(s_ak_battery, LV_ALIGN_TOP_RIGHT, -20, 14);

    // 主体区：命令预览，或模型的提问。它自带边框，和下面的选项分开——
    // 「要你看的东西」和「要你按的东西」在这块小屏上必须是两个区块。
    lv_obj_t *panel = plain(s_ask_scr);

    lv_obj_set_pos(panel, 12, 42);
    lv_obj_set_size(panel, 216, 100);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    s_ak_subject = make_label(panel, COL_MUTED);
    one_line(s_ak_subject, 200, false);
    lv_obj_align(s_ak_subject, LV_ALIGN_TOP_LEFT, 0, 0);

    // 面板内高 84px，主题占掉 22px，正文视口最多 3 行（3×19=57）——再多就要压到选项上。
    // 装不下的正文不再打点，而是**滚**：三枚按键是这块屏唯一的输入，而正文是长题面
    // 与长命令预览里唯一没法压缩的东西（见 badge_ui_scroll）。
    s_ak_viewport = plain(panel);
    lv_obj_add_flag(s_ak_viewport, LV_OBJ_FLAG_SCROLLABLE); // plain 默认把它去掉
    lv_obj_set_pos(s_ak_viewport, 0, 22);
    lv_obj_set_size(s_ak_viewport, 200, (int32_t)badge_font_16.line_height * 3);
    lv_obj_set_style_bg_opa(s_ak_viewport, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_ak_viewport, LV_DIR_VER);
    // 滚动条只在滚动时出现（ACTIVE）：它的作用是「还能往下」的提示，而常显一条竖线
    // 会压在这块小屏的正文上。
    lv_obj_set_scrollbar_mode(s_ak_viewport, LV_SCROLLBAR_MODE_ACTIVE);
    // 正文比视口窄 10px：滚动条画在容器右缘，留出这条槽它才不盖住最后一列字。
    s_ak_body = make_label(s_ak_viewport, COL_TEXT);
    lv_obj_set_width(s_ak_body, 190);
    // 不再打点、也不定高：标签自然长高，容器才滚得动（打点会把内容截掉，没什么可滚）。
    lv_label_set_long_mode(s_ak_body, LV_LABEL_LONG_MODE_WRAP);

    for (i = 0; i < BADGE_MAX_OPTIONS; ++i) {
        lv_obj_t *row = plain(s_ask_scr);

        // 选项区从 146 起、每行 24px：5 行正好到 266，给底部的按键提示留出位置。
        // 早先按 30px 排，最后一行会压在提示上（两者都在屏幕底部 30px 内）。
        lv_obj_set_pos(row, 12, 146 + (int)i * 24);
        lv_obj_set_size(row, 216, 22);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(row, 10, 0);

        s_ak_options[i] = row;
        s_ak_labels[i] = make_label(row, COL_TEXT);
        // 一行定死：选项行本身只有 22px 高、行距 24px，标签一旦换行就会压到下一行上。
        one_line(s_ak_labels[i], 190, false);
        lv_obj_align(s_ak_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    s_ak_footer = make_label(s_ask_scr, COL_MUTED);
    one_line(s_ak_footer, 220, true);
    lv_obj_align(s_ak_footer, LV_ALIGN_BOTTOM_MID, 0, -34);
}

static void build_pair_screen(void)
{
    s_pair_scr = make_screen();

    lv_obj_t *brand = make_label(s_pair_scr, COL_MUTED);

    lv_label_set_text(brand, "TACHI");
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 20, 16);

    s_pair_battery = make_label(s_pair_scr, COL_MUTED);
    lv_obj_align(s_pair_battery, LV_ALIGN_TOP_RIGHT, -20, 16);

    lv_obj_t *hint = make_label(s_pair_scr, COL_TEXT);

    lv_label_set_text(hint, "在电脑上输入这个码");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 84);

    // 48px：这是整块屏幕上唯一需要「隔着一米也看得清」的东西——用户要把它念到、
    // 或者抄进另一台设备。用 LVGL 内置的 Montserrat：它不含中文，而配对码正好只有
    // 数字，所以不必为它再生成一套中文字形。
    s_pair_code = make_label(s_pair_scr, COL_ACCENT);
    lv_obj_set_style_text_font(s_pair_code, &lv_font_montserrat_48, 0);
    lv_obj_align(s_pair_code, LV_ALIGN_TOP_MID, 0, 122);

    lv_obj_t *footer = make_label(s_pair_scr, COL_MUTED);

    lv_label_set_text(footer, "输完它就会自动连上");
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -34);
}

void badge_ui_init(void)
{
    build_status_screen();
    build_ask_screen();
    build_pair_screen();
    s_current_scr = s_status_scr;
    lv_screen_load(s_status_scr);
}

static void set_battery(lv_obj_t *label, const badge_ui_snapshot_t *snapshot)
{
    char text[16];

    if (snapshot->battery_percent < 0) {
        // 读不到电量是正常情况（芯片不应答），界面上留空而不是显示 0%——
        // 一个假的 0% 会让人以为设备快关机了。
        lv_label_set_text(label, "");
        return;
    }
    (void)snprintf(text, sizeof(text), "%d%%", snapshot->battery_percent);
    lv_label_set_text(label, text);
}

// 状态屏空闲时底栏轮换的两条提示。
//
// 两条都是**隐藏手势**：长按上 = 忘记那台电脑、按住确定 = 说话（按下开始录，松开发送）。
// 屏幕是它们唯一的说明书——不写出来没人猜得到。而底栏只有一行（220px ≈ 13 个汉字），两条
// 合起来 15 个，所以按相位轮换（见 badge_state_hint_phase）。
//
// ⚠ 下标 0 必须是「忘记电脑」：链路断着的时候只显示它（见 set_status_footer）——那一刻
// 说话没有去处，而「电脑那边还留着旧配对」正是要靠这一下拉回来的处境。换文案可以，别换
// 顺序。
// 键名（上 / 确定）和动作词都跟这块屏别处的叫法一致（「再长按一次上：忘记电脑」）。
static const char *const STATUS_HINTS[2] = {"长按上：忘记电脑", "按住确定：说话"};

// 小窗开着时底栏那一句（见 set_status_footer）。它必须短——底栏一行只放得下 220px 左右，
// 而这句话还兼着「小窗开着」这个状态说明（屏幕上没有别的地方能说这件事）。
#define STATUS_HUD_HINT "上下翻 · 确定关小窗"

// 状态屏底栏。一次性提示优先——它说的是刚发生的事（「已忘记那台电脑」），而这块
// 屏幕上是唯一能把它讲出来的地方；没有提示时才轮到「另有 N 个会话在跑」。
static void set_status_footer(const badge_ui_snapshot_t *snapshot)
{
    char footer[64];

    if (snapshot->recording) {
        // 录音优先于一切，包括上面那条一次性提示：此刻最要紧的是「它在录、录了多久、
        // 离上限还有多远」。一条上一句话的提示（「已发送：…」）停在那里几秒，会让正在
        // 录音的人以为自己按下去没生效。
        const unsigned seconds = (unsigned)(snapshot->record_ms / 1000U);
        const unsigned tenths = (unsigned)((snapshot->record_ms % 1000U) / 100U);
        const unsigned limit = (unsigned)(snapshot->record_limit_ms / 1000U);

        (void)snprintf(footer, sizeof(footer), LV_SYMBOL_BULLET " 录音中 %u.%us / %us",
                       seconds, tenths, limit);
        lv_label_set_text(s_st_footer, footer);
        return;
    }
    if (snapshot->notice != NULL) {
        lv_label_set_text(s_st_footer, snapshot->notice);
        return;
    }
    // 按主机报的总数算，不按这里放得下的那几个：放不下的会话同样是「另有」。
    if (snapshot->session_total > 1U) {
        (void)snprintf(footer, sizeof(footer), "另有 %u 个会话在跑",
                       (unsigned)(snapshot->session_total - 1U));
        lv_label_set_text(s_st_footer, footer);
        return;
    }
    // 小窗（桌上的置顶面板）开着：这一格改成它的按键说明。上下/确定此刻的意思变了
    // （见 main.c 的按键路由），而屏幕上本来没有任何东西表明它开着——这一句既是「它开着」
    // 也是「这几个键现在干什么」。
    //
    // 它抢的只是**空闲提示**这一格（下面那两条轮换），不是录音 / 一次性提示 / 「另有 N 个
    // 会话」：那三样说的是刚发生的事和别处在跑什么，都比一句用法要紧，而空闲时刻这一格
    // 本来也没有别的用处。
    if (snapshot->hud_open) {
        lv_label_set_text(s_st_footer, STATUS_HUD_HINT);
        return;
    }
    // 空闲：没有提示、没有别的会话、也没在录音。此刻这块屏唯一还能告诉用户的，就是那两个
    // 长按能干什么——它们是隐藏手势，屏幕不说就没人知道。相位由状态机推进（按相位选一条，
    // 而不是把两条都塞进去：底栏放不下）。
    //
    // 链路断着时只说「忘记电脑」：那一刻说话没有去处（录了也发不出去），而电脑上留着一份
    // 旧配对正是要靠这一下才能清掉——这一条恰恰在最需要它的时候最有价值。
    const size_t hint = (snapshot->connected && snapshot->hint_phase != 0U) ? 1U : 0U;

    lv_label_set_text(s_st_footer, STATUS_HINTS[hint]);
}

static uint32_t status_color(const badge_ui_snapshot_t *snapshot)
{
    if (!snapshot->connected) {
        return COL_MUTED;
    }
    if (strcmp(snapshot->state_label, "提问") == 0) {
        return COL_ACCENT;
    }
    if (strcmp(snapshot->state_label, "执行") == 0) {
        return COL_ALLOW;
    }
    if (strcmp(snapshot->state_label, "空闲") == 0) {
        return COL_LINE;
    }
    return COL_THINK;
}

static void render_pair(const badge_ui_snapshot_t *snapshot)
{
    char spaced[16];

    set_battery(s_pair_battery, snapshot);

    // 「463160」切成「463 160」：六位数字连成一片容易被抄错一位，而抄错一位的后果
    // 是配对失败、用户完全不知道错在哪。分成两组之后眼睛能自己对上。
    if (strlen(snapshot->passkey) == 6U) {
        (void)snprintf(spaced, sizeof(spaced), "%.3s %.3s", snapshot->passkey,
                       snapshot->passkey + 3);
        lv_label_set_text(s_pair_code, spaced);
    } else {
        // 协议栈给的一定是 6 位（badge_ble 用 %06u 生成），这里只是不替它假设。
        lv_label_set_text(s_pair_code, snapshot->passkey);
    }
}

// render_tool_line 画「第 N 个工具调用」那一行。
//
// 0 就不画：它要么是这一轮还没碰过工具，要么是主机根本不发这个字段（老主机），而两种
// 情况在屏幕上是同一件事——没有进度可报。留着上一次的读数最坏，那个数字会被读成
// 「还在跑」，而它其实早跑完了。
//
// 只在状态屏的主路径上画：链路断了、或者根本没有会话时，清掉。
static void render_tool_line(const badge_ui_snapshot_t *snapshot)
{
    char line[32];

    if (!snapshot->connected || !snapshot->has_session || snapshot->tool_calls == 0UL) {
        lv_label_set_text(s_st_tools, "");
        return;
    }
    (void)snprintf(line, sizeof(line), "第 %lu 个工具调用", snapshot->tool_calls);
    lv_label_set_text(s_st_tools, line);
}

static void render_status(const badge_ui_snapshot_t *snapshot)
{
    set_battery(s_st_battery, snapshot);
    // 写完电量再对一次位：`lv_obj_align_to` 算的是**一次性**的位置，而电量那行在 build
    // 时还是空文本——等它写上「82%」，盒子会往左长出来，图标和它之间的那条缝就跟着变成
    // 一个洞（而且位数变化时洞的宽度还会变）。
    lv_obj_align_to(s_st_transport, s_st_battery, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    // 出方向走哪条链路是**路由事实**，不是「连没连上」：蓝牙没到「已认证加密」时
    // 每一行都从串口出去，屏幕上就该这么画。字形来自图标字体（LV_SYMBOL_* 与
    // tools/gen_badge_font.sh 里那两个码位是一对）。
    lv_label_set_text(s_st_transport,
                      snapshot->transport == BADGE_UI_TRANSPORT_BLE ? LV_SYMBOL_BLUETOOTH
                                                                     : LV_SYMBOL_USB);

    // 标题栏那行最可能放不下（会话名是主机下发的，长度不归我们管），所以它自己跑马灯；
    // 状态说明虽然是自己写的，也留出滚动的余地。息屏时必须停——黑屏背后的动画纯粹在
    // 烧电，还会让 LVGL 任务一直重绘。
    lv_label_set_text(s_st_title,
                      snapshot->has_session && snapshot->session_title[0] != '\0'
                          ? snapshot->session_title
                          : "TACHI");
    marquee(s_st_title, snapshot->screen_on);
    marquee(s_st_detail, snapshot->screen_on);
    render_tool_line(snapshot);

    if (!snapshot->connected) {
        // 链路断了：屏幕上必须说清楚，否则「空闲」会被读成「agent 没在干活」。
        lv_label_set_text(s_st_state, "等电脑");
        // 状态色在这块屏上落在那一行字上（那颗色点已经让位给第三行，见 build_status_screen）。
        lv_obj_set_style_text_color(s_st_state, lv_color_hex(COL_MUTED), 0);
        // 「等电脑」底下藏着四种处境，用户要做的动作完全不同——糊成一句「还没有
        // 连上 tachi」等于什么都没说，而这块屏幕存在的意义就是回答「现在需要你
        // 做什么」。
        //
        // 每句都压在一行放得下的长度里（200px ≈ 12 个汉字）：这几句是常驻说明，
        // 一眼看完比让它滚起来好——滚动是留给放不下的东西的。
        switch (snapshot->link) {
        case BADGE_UI_LINK_PAIRED:
            lv_label_set_text(s_st_detail, "打开电脑上的 tachi");
            break;
        case BADGE_UI_LINK_SECURED:
            lv_label_set_text(s_st_detail, "在 tachi 里选这台设备");
            break;
        case BADGE_UI_LINK_PAIRING:
            // 正常走不到这里：passkey 一到就会切到配对屏（见 badge_ui_render）。
            // 配对刚开始的那一帧除外，那时还没有码可显示。
            lv_label_set_text(s_st_detail, "正在配对…");
            break;
        case BADGE_UI_LINK_UNPAIRED:
        default:
            lv_label_set_text(s_st_detail, "在电脑上搜 Tachi-Badge");
            break;
        }
        set_status_footer(snapshot);
        return;
    }
    lv_obj_set_style_text_color(s_st_state, lv_color_hex(status_color(snapshot)), 0);

    if (!snapshot->has_session) {
        lv_label_set_text(s_st_state, "空闲");
        lv_label_set_text(s_st_detail, "电脑上没有会话在跑");
    } else {
        lv_label_set_text(s_st_state, snapshot->state_label);
        lv_label_set_text(s_st_detail, snapshot->state_detail);
    }

    set_status_footer(snapshot);
}

static bool option_checked(const badge_ui_snapshot_t *snapshot, size_t index)
{
    return (snapshot->checked & (uint8_t)(1U << index)) != 0U;
}

// ask_body_can_scroll 问正文视口还能不能往这个方向翻（direction < 0 往回翻）。
//
// 这是界面里唯一判断「装不装得下」的地方——LVGL 量出来的比任何按字符数估的都准，
// 而正文里中文、ASCII、制表符的宽度差得很远。
static bool ask_body_can_scroll(int direction)
{
    if (s_ak_viewport == NULL) {
        return false;
    }
    // 先让布局算完再问：正文刚换过文本时几何还是上一屏的，那样问出来的答案是慢一拍的
    // （底栏提示会跟着错）。
    lv_obj_update_layout(s_ak_viewport);
    return (direction < 0) ? lv_obj_get_scroll_top(s_ak_viewport) > 0
                           : lv_obj_get_scroll_bottom(s_ak_viewport) > 0;
}

static void render_ask(const badge_ui_snapshot_t *snapshot)
{
    const badge_msg_t *ask = snapshot->ask;
    size_t i;
    bool questions = (ask->ask_kind == BADGE_ASK_QUESTIONS);
    const badge_question_t *question = NULL;

    set_battery(s_ak_battery, snapshot);

    // 换题、或者换了一条等待：正文滚回顶部。残留的偏移会让新题从中段开始显示，而那种
    // 「一上来就是半句话」看起来像设备坏了。
    if (ask != s_ak_body_ask || snapshot->question_index != s_ak_body_index) {
        s_ak_body_ask = ask;
        s_ak_body_index = snapshot->question_index;
        lv_obj_scroll_to_y(s_ak_viewport, 0, LV_ANIM_OFF);
    }

    if (questions) {
        if (snapshot->question_index < ask->question_count) {
            question = &ask->questions[snapshot->question_index];
        }
        if (ask->question_count > 1U) {
            char heading[BADGE_TITLE_MAX + 16];

            // 题号写在**前面**：标题长了会被 one_line 截掉尾巴，而「第几题」正是
            // 换题时唯一需要一直看得见的东西（当前题的主题就在下面那行）。
            (void)snprintf(heading, sizeof(heading), "(%u/%u) %s",
                           (unsigned)(snapshot->question_index + 1U),
                           (unsigned)ask->question_count, ask->title);
            lv_label_set_text(s_ak_heading, heading);
        } else {
            lv_label_set_text(s_ak_heading, ask->title);
        }
        lv_label_set_text(s_ak_subject,
                          question != NULL ? question->header : "");
        lv_label_set_text(s_ak_body, question != NULL ? question->question : "");
    } else {
        // 权限确认：标题说这是要放行一条命令，正文是 agent 自己渲染的预览——
        // 原样显示，不在设备上重新解析它。
        lv_label_set_text(s_ak_heading, "要执行命令");
        lv_label_set_text(s_ak_subject, ask->title);
        lv_label_set_text(s_ak_body, ask->body);
    }

    // 这一屏答不了的情况都要说出来，而不是留一块空白让人以为设备卡住了：
    //   - 无选项的自由输入题：三键打不了字；
    //   - 一道题都上不了屏（题面长到装不下，见 badge_proto 的 questions_total）。
    if (questions && (question == NULL || question->option_count == 0U)) {
        lv_obj_remove_flag(s_ak_options[0], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ak_labels[0],
                          question != NULL ? "需要在电脑上回答" : "这题要到电脑上答");
        marquee(s_ak_labels[0], false);
        lv_obj_set_style_bg_opa(s_ak_options[0], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_ak_labels[0], lv_color_hex(COL_ACCENT), 0);
        for (i = 1; i < BADGE_MAX_OPTIONS; ++i) {
            lv_obj_add_flag(s_ak_options[i], LV_OBJ_FLAG_HIDDEN);
            // 藏起来的行也要退出跑马灯：看不见的动画照样让 LVGL 一直重绘。
            marquee(s_ak_labels[i], false);
        }
        lv_label_set_text(s_ak_footer, question != NULL ? "这题要用文字回答"
                                                        : "题面太长，电脑上看全文");
        return;
    }

    for (i = 0; i < BADGE_MAX_OPTIONS; ++i) {
        size_t count = questions ? (question != NULL ? question->option_count : 0U)
                                 : ask->option_count;
        char text[BADGE_TITLE_MAX + 8];

        if (i >= count) {
            lv_obj_add_flag(s_ak_options[i], LV_OBJ_FLAG_HIDDEN);
            marquee(s_ak_labels[i], false); // 看不见的动画也在让 LVGL 一直重绘
            continue;
        }
        lv_obj_remove_flag(s_ak_options[i], LV_OBJ_FLAG_HIDDEN);

        if (questions && question->multi_select) {
            // 多选要能看出「已勾」和「只是高亮」的区别：这是两种不同的状态，
            // 而三键设备上它们只差一次按键。
            (void)snprintf(text, sizeof(text), "%s %s",
                           option_checked(snapshot, i) ? "[x]" : "[ ]",
                           question->options[i].label);
        } else {
            (void)snprintf(text, sizeof(text), "%s",
                           questions ? question->options[i].label
                                     : ask->options[i].label);
        }
        lv_label_set_text(s_ak_labels[i], text);

        if (i == snapshot->selection) {
            lv_obj_set_style_bg_opa(s_ak_options[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_ak_options[i], lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_text_color(s_ak_labels[i], lv_color_hex(COL_BG), 0);
        } else {
            lv_obj_set_style_bg_opa(s_ak_options[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(s_ak_labels[i], lv_color_hex(COL_TEXT), 0);
        }
        // 只有选中的那一行滚：放得下时 LVGL 不启动动画，短选项不受影响。
        // 熄屏时必须停掉——屏幕黑着还在滚的动画纯属烧电。
        marquee(s_ak_labels[i], i == snapshot->selection && snapshot->screen_on);
    }

    // 放不下的选项：**留一行说真话**。解析层已经保证「有溢出时 option_count ≤ 上限-1」，
    // 所以这里一定有空位；说得含糊（"更多…"）没有意义，用户要知道少了几个。
    {
        size_t shown = questions
                           ? (question != NULL ? question->option_count : 0U)
                           : ask->option_count;
        size_t total = questions
                           ? (question != NULL ? question->options_total : 0U)
                           : ask->options_total;

        if (total > shown && shown < BADGE_MAX_OPTIONS) {
            char note[48];

            (void)snprintf(note, sizeof(note), "…还有 %u 项在电脑上", (unsigned)(total - shown));
            lv_obj_remove_flag(s_ak_options[shown], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(s_ak_options[shown], LV_OPA_TRANSP, 0);
            lv_label_set_text(s_ak_labels[shown], note);
            marquee(s_ak_labels[shown], false);
            lv_obj_set_style_text_color(s_ak_labels[shown], lv_color_hex(COL_MUTED), 0);
        }
    }

    // 底栏：先报按键，再报其它。题号不在这里重复——标题写成「(1/2) 主题」，
    // 而这块屏一行放不下「第几题 + 换题 + 提交」三件事。
    if (snapshot->notice != NULL) {
        lv_label_set_text(s_ak_footer, snapshot->notice);
    } else if (ask_body_can_scroll(1)) {
        // 提示跟着**手势此刻的作用**走：还能往下翻时，长按上/下是翻屏而不是换题，
        // 所以这里说翻屏；翻到底之后下面那些分支会自己把换题/提交的话接回来。
        lv_label_set_text(s_ak_footer, "长按翻屏看全文");
    } else if (questions && ask->question_count > 1U) {
        lv_label_set_text(s_ak_footer,
                          (question != NULL && question->multi_select)
                              ? "上/下长按换题 · 确定长按提交"
                              : "上/下长按换题 · 确定下一题");
    } else if (questions && question != NULL && question->multi_select) {
        lv_label_set_text(s_ak_footer, "确定勾选 · 长按提交");
    } else {
        lv_label_set_text(s_ak_footer, "上下选择 · 确定提交");
    }
}

void badge_ui_render(const badge_ui_snapshot_t *snapshot)
{
    lv_obj_t *wanted;

    if (snapshot == NULL) {
        return;
    }
    // 配对压倒一切：此刻用户唯一的任务是把屏幕上那 6 位数字敲进电脑，会话列表、
    // 电量、别的任何信息都是干扰。配对结束（passkey 变空）后自动走下面那两条路。
    if (snapshot->passkey != NULL && snapshot->passkey[0] != '\0') {
        render_pair(snapshot);
        wanted = s_pair_scr;
    } else if (snapshot->view == BADGE_UI_ASK) {
        render_ask(snapshot);
        wanted = s_ask_scr;
    } else {
        render_status(snapshot);
        wanted = s_status_scr;
    }
    if (wanted != s_current_scr) {
        s_current_scr = wanted;
        lv_screen_load(wanted);
    }
}

bool badge_ui_scroll(int direction)
{
    const bool down = direction >= 0;
    int32_t page;
    int32_t target;

    // 先问边界的账，而不是「翻完看看位置变了没」：那样写会把一次真的翻动判成「没翻」，
    // 于是按键被当成换题。
    if (!ask_body_can_scroll(down ? 1 : -1)) {
        return false;
    }
    // 一次一屏（视口高度 = 3 行）：一次一行读长题面太磨人，而这屏上唯一的输入就是长按
    // ——每一次长按都得值回票价。
    page = lv_obj_get_height(s_ak_viewport);
    // 走 scroll_to_y 而不是 scroll_by，方向按 LVGL 自己的约定来（它自己的按键处理就是
    // 「往下 = 在当前 scroll_y 上加」）：公开的 scroll_y 越大越往下读。别用 scroll_by——
    // 那个走 raw 路径，符号是反的，而且不替我们夹边界（一次翻过内容底部会露出空白）。
    target = lv_obj_get_scroll_y(s_ak_viewport) + (down ? page : -page);
    lv_obj_scroll_to_y(s_ak_viewport, target, LV_ANIM_OFF);
    return true;
}
