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

// tachi 的头像（tools/mk_badge_avatar.py 从 desktop 的 app 图标生成）。
// 声明放在这里而不是头文件里：它是匿名结构体 typedef，没法前向声明，
// 而这个头会被主机测试包含（那边没有 LVGL 的头）。
extern const lv_image_dsc_t badge_avatar;

static lv_obj_t *s_status_scr;
static lv_obj_t *s_ask_scr;

static lv_obj_t *s_st_battery;
static lv_obj_t *s_st_dot;
static lv_obj_t *s_st_state;
static lv_obj_t *s_st_route;
static lv_obj_t *s_st_detail;
static lv_obj_t *s_st_footer;

static lv_obj_t *s_ak_heading;
static lv_obj_t *s_ak_battery;
static lv_obj_t *s_ak_subject;
static lv_obj_t *s_ak_body;
static lv_obj_t *s_ak_options[BADGE_MAX_OPTIONS];
static lv_obj_t *s_ak_labels[BADGE_MAX_OPTIONS];
static lv_obj_t *s_ak_footer;

static badge_ui_view_t s_view;

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

// at_most_lines 允许最多 lines 行，超出在最后一行打点。题目正文与命令预览是唯一
// 允许多行的区域，而它的高度就是面板留给它的那块空间——同样不能撑破面板压到选项上。
static void at_most_lines(lv_obj_t *label, int width, int lines)
{
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, (int32_t)badge_font_16.line_height * lines);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

// 跑马灯速度（px/s）。它填进的是样式的 anim_duration——在 LVGL 9.5 里那个样式承载
// 的是**速度**（LVGL 自己就这么用：把速度与时长上下限编码进一个值），不是一趟的毫秒
// 数。默认 40 对这块小屏偏慢，长一点的选项要等好几秒才滚完。
#define MARQUEE_SPEED_PX_S 60

// marquee 让一行文字横向循环滚动，只用在**选中的那一行**上：五个选项同时滚会谁也看
// 不清，而黄色高亮就是「你在看这一行」。文字放得下时 LVGL 不会启动动画。
static void marquee(lv_obj_t *label, bool on)
{
    if (on) {
        lv_obj_set_style_anim_duration(label,
                                       lv_anim_speed_clamped(MARQUEE_SPEED_PX_S, 300, 10000), 0);
    }
    lv_label_set_long_mode(label,
                           on ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS);
}

static void build_status_screen(void)
{
    s_status_scr = make_screen();

    lv_obj_t *brand = make_label(s_status_scr, COL_MUTED);

    lv_label_set_text(brand, "TACHI");
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 20, 16);

    s_st_battery = make_label(s_status_scr, COL_MUTED);
    lv_obj_align(s_st_battery, LV_ALIGN_TOP_RIGHT, -20, 16);

    // 头像占原来那个圆的位置。它不再靠颜色说话——一张脸比一个色块更像「有人在」；
    // 颜色改由下面那颗小圆点承担（见 render_status）。
    lv_obj_t *avatar = lv_image_create(s_status_scr);

    lv_image_set_src(avatar, &badge_avatar);
    lv_obj_align(avatar, LV_ALIGN_TOP_MID, 0, 48);

    s_st_dot = plain(s_status_scr);
    lv_obj_set_size(s_st_dot, 10, 10);
    lv_obj_set_style_radius(s_st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_st_dot, lv_color_hex(COL_THINK), 0);
    lv_obj_align(s_st_dot, LV_ALIGN_TOP_MID, 0, 176);

    s_st_state = make_label(s_status_scr, COL_TEXT);
    one_line(s_st_state, 200, true);
    lv_obj_align(s_st_state, LV_ALIGN_TOP_MID, 0, 194);

    s_st_route = make_label(s_status_scr, COL_MUTED);
    one_line(s_st_route, 200, true);
    lv_obj_align(s_st_route, LV_ALIGN_TOP_MID, 0, 224);

    s_st_detail = make_label(s_status_scr, COL_MUTED);
    one_line(s_st_detail, 200, true);
    lv_obj_align(s_st_detail, LV_ALIGN_TOP_MID, 0, 248);

    s_st_footer = make_label(s_status_scr, COL_MUTED);
    one_line(s_st_footer, 220, true);
    lv_obj_align(s_st_footer, LV_ALIGN_BOTTOM_MID, 0, -34);
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

    // 面板内高 84px，主题占掉 22px，正文最多 3 行（3×19=57）——再多就要压到选项上。
    s_ak_body = make_label(panel, COL_TEXT);
    at_most_lines(s_ak_body, 200, 3);
    lv_obj_align(s_ak_body, LV_ALIGN_TOP_LEFT, 0, 22);

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

void badge_ui_init(void)
{
    build_status_screen();
    build_ask_screen();
    s_view = BADGE_UI_STATUS;
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

static void render_status(const badge_ui_snapshot_t *snapshot)
{
    char footer[64];

    set_battery(s_st_battery, snapshot);

    if (!snapshot->connected) {
        // 链路断了：屏幕上必须说清楚，否则「空闲」会被读成「agent 没在干活」。
        lv_label_set_text(s_st_state, "等电脑");
        lv_obj_set_style_bg_color(s_st_dot, lv_color_hex(COL_MUTED), 0);
        lv_label_set_text(s_st_route, "");
        lv_label_set_text(s_st_detail, "还没有连上 tachi");
        lv_label_set_text(s_st_footer, "");
        return;
    }

    lv_obj_set_style_bg_color(s_st_dot, lv_color_hex(status_color(snapshot)), 0);

    if (!snapshot->has_session) {
        lv_label_set_text(s_st_state, "空闲");
        lv_label_set_text(s_st_route, "");
        lv_label_set_text(s_st_detail, "电脑上没有会话在跑");
    } else {
        lv_label_set_text(s_st_state, snapshot->state_label);
        lv_label_set_text(s_st_route, snapshot->session_title);
        lv_label_set_text(s_st_detail, snapshot->state_detail);
    }

    // 按主机报的总数算，不按这里放得下的那几个：放不下的会话同样是「另有」。
    if (snapshot->session_total > 1U) {
        (void)snprintf(footer, sizeof(footer), "另有 %u 个会话在跑",
                       (unsigned)(snapshot->session_total - 1U));
    } else {
        footer[0] = '\0';
    }
    lv_label_set_text(s_st_footer, footer);
}

static bool option_checked(const badge_ui_snapshot_t *snapshot, size_t index)
{
    return (snapshot->checked & (uint8_t)(1U << index)) != 0U;
}

static void render_ask(const badge_ui_snapshot_t *snapshot)
{
    const badge_msg_t *ask = snapshot->ask;
    size_t i;
    bool questions = (ask->ask_kind == BADGE_ASK_QUESTIONS);
    const badge_question_t *question = NULL;

    set_battery(s_ak_battery, snapshot);

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
    if (snapshot == NULL) {
        return;
    }
    if (snapshot->view != s_view) {
        s_view = snapshot->view;
        lv_screen_load(s_view == BADGE_UI_ASK ? s_ask_scr : s_status_scr);
    }
    if (s_view == BADGE_UI_ASK) {
        render_ask(snapshot);
    } else {
        render_status(snapshot);
    }
}

void badge_ui_scroll(int lines)
{
    (void)lines;
    // 待答屏的内容目前靠换行整段放下（正文框 100px 高，够放常见的问题与命令）。
    // 真正需要滚动的是异常长的一屏，而那已经超出「一眼能扫完」的范围——与其做半个
    // 滚动，不如把这件事留给屏幕上的提示：长内容最终还是要在电脑上看。
}
