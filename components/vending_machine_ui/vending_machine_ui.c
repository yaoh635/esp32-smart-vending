/*
 * Vending Machine UI Component Implementation
 * Reusable vending machine interface for ESP32 SPI LCD Touch
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "vending_machine.h"
#include "voice_control.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "vending_machine";

/* ── 全局状态变量 ── */
static lv_disp_t *g_disp;                              /**< LVGL 显示句柄 */
static vending_state_t g_state = STATE_PRODUCT_SELECT;  /**< 当前售货机状态 */
static int g_selected = 0;                              /**< 当前选中的商品索引 */
static int g_motor_gpio = VENDING_MOTOR_GPIO;           /**< 电机控制 GPIO 引脚 */

/* ── 舵机控制 ── */
static int g_servo_gpios[VENDING_SERVO_COUNT] = {-1, -1, -1, -1, -1}; /**< 舵机 GPIO 引脚 */
static bool g_servo_initialized = false; /**< 舵机是否已初始化 */

/* 舵机 PWM 参数（50Hz，0.5ms~2.5ms 对应 0°~180°） */
#define SERVO_FREQ_HZ       50
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_SPEED_MODE    LEDC_LOW_SPEED_MODE
#define SERVO_MIN_PULSE_US  500    /* 0° = 0.5ms */
#define SERVO_MAX_PULSE_US  2500   /* 180° = 2.5ms */
#define SERVO_DEG_0         0      /* 初始位置 */
#define SERVO_DEG_90        90     /* 出货位置 */

/**
 * @brief 将角度转换为 LEDC 占空比
 *        0° = 0.5ms, 180° = 2.5ms, 周期 = 20ms (50Hz)
 */
static uint32_t servo_angle_to_duty(int angle)
{
    /* 限制角度范围 */
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    /* 计算脉宽（微秒） */
    uint32_t pulse_us = SERVO_MIN_PULSE_US +
                        (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * angle / 180;

    /* 转换为占空比（14位分辨率） */
    uint32_t duty = (pulse_us * (1 << 14)) / 20000;  /* 20ms = 20000us */
    return duty;
}

/**
 * @brief 初始化舵机 PWM
 */
static esp_err_t servos_init(const int *gpio_pins, int count)
{
    ESP_LOGI(TAG, "Initializing %d servos", count);

    /* 配置 LEDC 定时器 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode = SERVO_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置每个舵机的 LEDC 通道 */
    for (int i = 0; i < count; i++) {
        if (gpio_pins[i] < 0) continue;

        ledc_channel_config_t ch_cfg = {
            .gpio_num = gpio_pins[i],
            .speed_mode = SERVO_SPEED_MODE,
            .channel = LEDC_CHANNEL_0 + i,
            .timer_sel = SERVO_TIMER,
            .duty = servo_angle_to_duty(SERVO_DEG_0),  /* 初始位置 0° */
            .hpoint = 0,
        };
        ret = ledc_channel_config(&ch_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Servo %d (GPIO%d) config failed: %s",
                     i, gpio_pins[i], esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Servo %d: GPIO%d initialized (0°)", i, gpio_pins[i]);
    }

    g_servo_initialized = true;
    return ESP_OK;
}

/**
 * @brief 设置舵机角度
 */
static void servo_set_angle(int servo_idx, int angle)
{
    if (!g_servo_initialized || servo_idx < 0 || servo_idx >= VENDING_SERVO_COUNT) {
        return;
    }
    if (g_servo_gpios[servo_idx] < 0) return;

    uint32_t duty = servo_angle_to_duty(angle);
    ledc_set_duty(SERVO_SPEED_MODE, LEDC_CHANNEL_0 + servo_idx, duty);
    ledc_update_duty(SERVO_SPEED_MODE, LEDC_CHANNEL_0 + servo_idx);

    ESP_LOGI(TAG, "Servo %d (GPIO%d): %d° (duty=%lu)",
             servo_idx, g_servo_gpios[servo_idx], angle, duty);
}

/**
 * @brief 出货：转动对应商品的舵机
 */
static void dispense_product(int product_idx)
{
    if (product_idx < 0 || product_idx >= VENDING_SERVO_COUNT) {
        ESP_LOGE(TAG, "Invalid product index: %d", product_idx);
        return;
    }

    ESP_LOGI(TAG, "Dispensing product %d via servo %d (GPIO%d)",
             product_idx, product_idx, g_servo_gpios[product_idx]);

    /* 转到 90° 出货位置 */
    servo_set_angle(product_idx, SERVO_DEG_90);

    /* 1.5 秒后转回 0° */
    vTaskDelay(pdMS_TO_TICKS(1500));
    servo_set_angle(product_idx, SERVO_DEG_0);
}

/* ── 默认商品目录 ── */
static const product_t default_products[VENDING_PRODUCT_COUNT] = {
    {"Cola",   "$2.00", .color = LV_COLOR_MAKE(0xE8, 0x04, 0x04), LV_SYMBOL_TINT},
    {"Water",  "$1.50", .color = LV_COLOR_MAKE(0x29, 0x65, 0xD5), LV_SYMBOL_TINT},
    {"Chips",  "$3.00", .color = LV_COLOR_MAKE(0xFB, 0xE0, 0x00), LV_SYMBOL_BARS},
    {"Candy",  "$2.50", .color = LV_COLOR_MAKE(0xFC, 0x9F, 0x7F), LV_SYMBOL_BELL},
};

/* 商品目录指针 */
static const product_t *g_products = NULL;       /**< 当前使用的商品目录 */
static uint8_t g_product_count = VENDING_PRODUCT_COUNT; /**< 当前商品数量 */

/* 购买完成回调 */
static vending_purchase_cb_t g_purchase_cb = NULL;        /**< 购买完成回调 */
static void *g_purchase_cb_user_data = NULL;               /**< 回调用户数据 */

/* ── Forward declarations ── */
static void create_product_select_screen(void);
static void create_order_confirm_screen(void);
static void create_payment_screen(void);
static void create_dispensing_screen(void);
static void create_done_screen(void);

/* ===================================================================
 * 语音命令回调
 * =================================================================== */
/**
 * @brief 语音命令回调函数
 *        当检测到语音命令时，自动选择对应商品并跳转到确认界面
 */
static void voice_cmd_callback(int cmd_id, const char *product, void *user_data)
{
    (void)user_data;
    (void)product;

    /* 只在商品选择界面响应语音命令 */
    if (g_state != STATE_PRODUCT_SELECT) {
        ESP_LOGI(TAG, "Voice command ignored (state=%d)", g_state);
        return;
    }

    int product_idx = voice_cmd_to_product_index(cmd_id);
    if (product_idx >= 0 && product_idx < g_product_count) {
        ESP_LOGI(TAG, "Voice selected product %d: %s", product_idx, g_products[product_idx].name);
        g_selected = product_idx;
        create_order_confirm_screen();
    } else {
        ESP_LOGW(TAG, "Unknown voice command: %d", cmd_id);
    }
}

/* 定时器回调封装（lv_timer_cb_t 要求 lv_timer_t* 参数） */
/**
 * @brief 定时器回调：跳转到出货界面
 */
static void to_dispensing_cb(lv_timer_t *t) { lv_timer_delete(t); create_dispensing_screen(); }

/**
 * @brief 安全切换屏幕
 *        加载新屏幕并删除旧屏幕，避免内存泄漏。
 */
static void load_screen(lv_obj_t *new_scr)
{
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(new_scr);
    if (old) {
        lv_obj_delete(old);
    }
}

/**
 * @brief 创建带样式的按钮
 *        创建一个带圆角、指定背景色和居中文字的按钮。
 *
 * @param parent  父对象
 * @param text    按钮文字
 * @param bg      背景颜色
 * @param align   对齐方式
 * @param x_ofs   X 偏移量
 * @param y_ofs   Y 偏移量
 * @return 按钮对象指针
 */
static lv_obj_t *create_btn(lv_obj_t *parent, const char *text,
                             lv_color_t bg, lv_align_t align,
                             int32_t x_ofs, int32_t y_ofs)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 130, 45);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_align(btn, align, x_ofs, y_ofs);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    return btn;
}

/* ===================================================================
 * 界面 A：商品选择（2x2 卡片网格）
 * =================================================================== */
/**
 * @brief 商品卡片点击回调
 *        记录被点击的商品索引，跳转到订单确认界面。
 */
static void product_card_cb(lv_event_t *e)
{
    g_selected = (int)(intptr_t)lv_event_get_user_data(e);
    create_order_confirm_screen();
}

/**
 * @brief 创建商品选择界面
 *        显示标题栏和 2x2 商品卡片网格，每个卡片包含图标、名称和价格。
 */
static void create_product_select_screen(void)
{
    g_state = STATE_PRODUCT_SELECT;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Title bar */
    lv_obj_t *title_bar = lv_obj_create(scr);
    lv_obj_set_size(title_bar, 320, 44);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_remove_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_bar);
    lv_label_set_text(title, "VENDING MACHINE");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_center(title);

    /* Grid container */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, 300, 410);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < g_product_count; i++) {
        const product_t *p = &g_products[i];

        /* Card */
        lv_obj_t *card = lv_obj_create(grid);
        lv_obj_set_size(card, 140, 188);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 8, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xE0E0E0), LV_STATE_PRESSED);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, product_card_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        /* Icon panel */
        lv_obj_t *icon = lv_obj_create(card);
        lv_obj_set_size(icon, 100, 100);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(icon, p->color, 0);
        lv_obj_set_style_radius(icon, 12, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sym = lv_label_create(icon);
        lv_label_set_text(sym, p->symbol);
        lv_obj_set_style_text_color(sym, lv_color_white(), 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);
        lv_obj_center(sym);

        /* Name */
        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, p->name);
        lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 110);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x1D2024), 0);

        /* Price */
        lv_obj_t *price_lbl = lv_label_create(card);
        lv_label_set_text(price_lbl, p->price_text);
        lv_obj_align(price_lbl, LV_ALIGN_TOP_MID, 0, 138);
        lv_obj_set_style_text_color(price_lbl, lv_color_hex(0xFF9800), 0);
        lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_20, 0);
    }

    load_screen(scr);
}

/* ===================================================================
 * 界面 B：订单确认
 * =================================================================== */
/**
 * @brief 确认按钮回调 — 跳转到支付界面
 */
static void confirm_cb(lv_event_t *e)
{
    (void)e;
    create_payment_screen();
}

/**
 * @brief 取消按钮回调 — 返回商品选择界面
 */
static void cancel_cb(lv_event_t *e)
{
    (void)e;
    create_product_select_screen();
}

/**
 * @brief 创建订单确认界面
 *        显示选中商品的大图标、名称、价格，以及确认和取消按钮。
 */
static void create_order_confirm_screen(void)
{
    g_state = STATE_ORDER_CONFIRM;
    const product_t *p = &g_products[g_selected];

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Order Details");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* Large icon */
    lv_obj_t *icon = lv_obj_create(scr);
    lv_obj_set_size(icon, 120, 120);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(icon, p->color, 0);
    lv_obj_set_style_radius(icon, 20, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sym = lv_label_create(icon);
    lv_label_set_text(sym, p->symbol);
    lv_obj_set_style_text_color(sym, lv_color_white(), 0);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);
    lv_obj_center(sym);

    /* Product name */
    lv_obj_t *name_lbl = lv_label_create(scr);
    lv_label_set_text(name_lbl, p->name);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 220);

    /* Price */
    lv_obj_t *price_lbl = lv_label_create(scr);
    lv_label_set_text(price_lbl, p->price_text);
    lv_obj_set_style_text_color(price_lbl, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(price_lbl, LV_ALIGN_TOP_MID, 0, 260);

    /* Confirm button */
    lv_obj_t *btn_ok = create_btn(scr, "CONFIRM", lv_color_hex(0x4CAF50),
                                  LV_ALIGN_BOTTOM_LEFT, 30, -30);
    lv_obj_add_event_cb(btn_ok, confirm_cb, LV_EVENT_CLICKED, NULL);

    /* Cancel button */
    lv_obj_t *btn_cancel = create_btn(scr, "CANCEL", lv_color_hex(0xF44336),
                                      LV_ALIGN_BOTTOM_RIGHT, -30, -30);
    lv_obj_add_event_cb(btn_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

    load_screen(scr);
}

/* ===================================================================
 * 界面 C：支付（自动跳转）
 * =================================================================== */
/**
 * @brief 支付成功显示回调
 *        清除加载动画，显示成功图标和提示文字，1.5 秒后自动跳转到出货界面。
 */
static void payment_success_show(lv_timer_t *timer)
{
    lv_obj_t *scr = lv_timer_get_user_data(timer);
    lv_timer_delete(timer);

    /* Clear spinner area */
    lv_obj_clean(scr);

    /* Success icon */
    lv_obj_t *ok_icon = lv_label_create(scr);
    lv_label_set_text(ok_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(ok_icon, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_text_font(ok_icon, &lv_font_montserrat_48, 0);
    lv_obj_align(ok_icon, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Payment Successful!");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 30);

    /* Transition to dispensing after 1.5s */
    lv_timer_t *t = lv_timer_create(to_dispensing_cb, 1500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/**
 * @brief 创建支付处理界面
 *        显示加载动画和"处理中"提示，2 秒后自动显示支付成功并跳转出货。
 */
static void create_payment_screen(void)
{
    g_state = STATE_PAYMENT;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Spinner */
    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 80, 80);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -30);

    /* Label */
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Processing Payment...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 30);

    load_screen(scr);

    /* Auto-transition: show success after 2s */
    lv_timer_t *t = lv_timer_create(payment_success_show, 2000, scr);
    lv_timer_set_repeat_count(t, 1);
}

/* ===================================================================
 * 界面 D：出货（舵机控制）
 * =================================================================== */
/**
 * @brief 出货完成回调
 *        舵机已转回初始位置，跳转到完成界面。
 */
static void dispense_done_cb(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    create_done_screen();
}

/**
 * @brief 开始出货回调
 *        清除加载动画，显示"请取货"提示，激活对应商品的舵机出货。
 */
static void dispense_start(lv_timer_t *timer)
{
    lv_obj_t *scr = lv_timer_get_user_data(timer);
    lv_timer_delete(timer);

    /* Update UI */
    lv_obj_clean(scr);

    lv_obj_t *ok_icon = lv_label_create(scr);
    lv_label_set_text(ok_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(ok_icon, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_text_font(ok_icon, &lv_font_montserrat_48, 0);
    lv_obj_align(ok_icon, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Please collect\nyour item!");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 30);

    /* 激活对应商品的舵机出货 */
    dispense_product(g_selected);

    /* 2 秒后跳转到完成界面 */
    lv_timer_t *t = lv_timer_create(dispense_done_cb, 2000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/**
 * @brief 创建出货界面
 *        显示加载动画和"出货中"提示，2 秒后开始出货流程。
 */
static void create_dispensing_screen(void)
{
    g_state = STATE_DISPENSING;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Spinner */
    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 80, 80);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Dispensing...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 30);

    load_screen(scr);

    /* After 2s animation, start motor */
    lv_timer_t *t = lv_timer_create(dispense_start, 2000, scr);
    lv_timer_set_repeat_count(t, 1);
}

/* ===================================================================
 * 界面 E：完成（自动返回）
 * =================================================================== */
/**
 * @brief 完成界面自动返回回调
 *        2 秒后自动返回商品选择界面，形成循环。
 */
static void done_return_cb(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    create_product_select_screen();
}

/**
 * @brief 创建完成/感谢界面
 *        显示成功图标和"Thank you!"提示，2 秒后自动返回商品选择界面。
 */
static void create_done_screen(void)
{
    g_state = STATE_DONE;

    /* 触发购买完成回调 */
    if (g_purchase_cb && g_products) {
        g_purchase_cb(g_products[g_selected].name,
                      g_products[g_selected].price_text,
                      g_purchase_cb_user_data);
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *heart = lv_label_create(scr);
    lv_label_set_text(heart, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(heart, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_text_font(heart, &lv_font_montserrat_48, 0);
    lv_obj_align(heart, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Thank you!");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 30);

    load_screen(scr);

    /* Return to product selection after 2s */
    lv_timer_t *t = lv_timer_create(done_return_cb, 2000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ===================================================================
 * 公共 API
 * =================================================================== */

/**
 * @brief 获取自动售货机默认配置
 *        返回包含默认电机 GPIO、舵机 GPIO 和默认商品目录的配置结构体。
 */
vending_machine_config_t vending_machine_get_default_config(void)
{
    vending_machine_config_t config = {
        .motor_gpio = VENDING_MOTOR_GPIO,
        .servo_gpios = {47, 27, 53, 46, 48},  /* 5个商品对应的舵机 GPIO */
        .products = default_products,
        .product_count = VENDING_PRODUCT_COUNT,
    };
    return config;
}

/**
 * @brief 初始化并启动自动售货机 UI
 *        保存显示句柄，应用配置（或使用默认值），初始化舵机 GPIO，
 *        然后创建商品选择界面启动完整的售货机交互流程。
 */
esp_err_t vending_machine_start(lv_display_t *display,
                                const vending_machine_config_t *config)
{
    if (display == NULL) {
        ESP_LOGE(TAG, "Display handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    g_disp = display;

    /* Apply configuration */
    if (config != NULL) {
        g_motor_gpio = config->motor_gpio;
        memcpy(g_servo_gpios, config->servo_gpios, sizeof(g_servo_gpios));
        g_products = config->products;
        g_product_count = config->product_count;
        g_purchase_cb = config->purchase_cb;
        g_purchase_cb_user_data = config->purchase_cb_user_data;
    } else {
        /* Use defaults */
        g_motor_gpio = VENDING_MOTOR_GPIO;
        g_servo_gpios[0] = 47;
        g_servo_gpios[1] = 27;
        g_servo_gpios[2] = 53;
        g_servo_gpios[3] = 46;
        g_servo_gpios[4] = 48;
        g_products = default_products;
        g_product_count = VENDING_PRODUCT_COUNT;
        g_purchase_cb = NULL;
        g_purchase_cb_user_data = NULL;
    }

    /* Initialize servos */
    esp_err_t ret = servos_init(g_servo_gpios, VENDING_SERVO_COUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Servo initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start with product selection screen */
    ESP_LOGI(TAG, "Starting vending machine UI");
    create_product_select_screen();

    /* 启动语音控制 */
    voice_control_set_cb(voice_cmd_callback, NULL);
    voice_control_start();
    ESP_LOGI(TAG, "Voice control started");

    return ESP_OK;
}

/**
 * @brief 获取当前售货机状态
 * @return 当前状态枚举值
 */
vending_state_t vending_machine_get_state(void)
{
    return g_state;
}
