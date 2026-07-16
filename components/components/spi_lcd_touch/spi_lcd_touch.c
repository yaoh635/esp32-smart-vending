/*
 * SPI LCD Touch Component Implementation
 * Reusable SPI LCD + Touch + LVGL integration for ESP-IDF
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "spi_lcd_touch.h"

/* Touch controller includes (conditional) */
#if CONFIG_SPI_LCD_TOUCH_CONTROLLER_STMPE610
#include "esp_lcd_touch_stmpe610.h"
#elif CONFIG_SPI_LCD_TOUCH_CONTROLLER_XPT2046
#include "esp_lcd_touch_xpt2046.h"
#endif

static const char *TAG = "spi_lcd_touch";

/* Use SPI2 as the LCD host */
#define LCD_HOST  SPI2_HOST

/* LVGL thread safety lock */
static _lock_t s_lvgl_api_lock;

/* Semaphore for synchronizing flush callback with DMA transfer completion */
static SemaphoreHandle_t s_lvgl_flush_sem = NULL;

/* Stored configuration for callbacks */
static spi_lcd_touch_config_t s_config;

/**
 * @brief DMA 传输完成通知回调
 *        当 LCD SPI DMA 传输完成时，由 ISR 调用，释放信号量以通知 LVGL 刷新可以继续。
 */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_lvgl_flush_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return false;
}

/**
 * @brief LVGL 显示旋转回调
 *        根据当前旋转角度设置 LCD 面板的 XY 交换和镜像方向。
 */
static void lvgl_port_update_callback(lv_display_t *disp)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISPLAY_ROTATION_90:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISPLAY_ROTATION_180:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    case LV_DISPLAY_ROTATION_270:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);
        break;
    }
}

/**
 * @brief LVGL 刷新回调 — 将渲染好的像素数据发送到 LCD
 *        交换 RGB565 字节序后通过 SPI DMA 发送到屏幕，然后等待传输完成。
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    /* SPI LCD uses big-endian byte order, swap RGB bytes */
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));

    /* Copy buffer to display region */
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);

    /* Wait for DMA transfer completion */
    if (xSemaphoreTake(s_lvgl_flush_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        static int s_flush_timeout_cnt = 0;
        s_flush_timeout_cnt++;
        ESP_LOGW(TAG, "Flush DMA timeout #%d", s_flush_timeout_cnt);
    }
    lv_display_flush_ready(disp);
}

/* 触摸输入回调（仅在启用触摸时编译） */
#if CONFIG_SPI_LCD_TOUCH_ENABLED
static esp_lcd_touch_handle_t s_touch_handle = NULL;  /**< 触摸控制器句柄 */
static int64_t s_last_touch_read_us = 0;              /**< 上次触摸读取时间戳 (微秒) */
static uint8_t s_debounce_cnt = 0;                    /**< 去抖计数器 */
static bool s_touch_pressed = false;                   /**< 当前是否处于按下状态 */

#define TOUCH_DEBOUNCE_PRESS   2   /* 连续读取多少次确认按下，减小可提升按下响应速度，最小值1 */
#define TOUCH_DEBOUNCE_RELEASE 3   /* 连续读取多少次确认释放，减小可提升释放响应速度，最小值1 */
#define TOUCH_READ_INTERVAL_US 5000 /* 两次SPI触摸读取的最小间隔(微秒)，减小可提升采样率，建议不低于5000 */

/**
 * @brief LVGL 触摸输入回调
 *        以固定频率读取触摸控制器数据，经过去抖处理后将坐标和状态报告给 LVGL。
 *        包含 SPI 读取频率限制，避免过于频繁的 SPI 通信阻塞显示刷新。
 */
static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_touch_handle == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int64_t now = esp_timer_get_time();

    /* Rate-limit SPI reads to avoid blocking display refresh too often */
    if (now - s_last_touch_read_us < TOUCH_READ_INTERVAL_US) {
        data->state = s_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    s_last_touch_read_us = now;

    /* Read touch data from XPT2046 via SPI */
    esp_lcd_touch_read_data(s_touch_handle);

    uint8_t touchpad_cnt = 0;
    esp_lcd_touch_point_data_t points[1] = {0};
    esp_lcd_touch_get_data(s_touch_handle, points, &touchpad_cnt, 1);

    /* Debounce logic */
    if (touchpad_cnt > 0) {
        s_debounce_cnt = (s_debounce_cnt < 255) ? s_debounce_cnt + 1 : 255;
        if (!s_touch_pressed && s_debounce_cnt >= TOUCH_DEBOUNCE_PRESS) {
            s_touch_pressed = true;
        }
    } else {
        s_debounce_cnt = (s_debounce_cnt > 0) ? s_debounce_cnt - 1 : 0;
        if (s_touch_pressed && s_debounce_cnt == 0) {
            s_touch_pressed = false;
        }
    }

    if (s_touch_pressed && touchpad_cnt > 0) {
        data->point.x = points[0].x;
        data->point.y = points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

/**
 * @brief LVGL 心跳递增函数
 *        由定时器周期调用，为 LVGL 提供时间基准。
 */
static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(s_config.tick_period_ms);
}

/**
 * @brief LVGL 主任务
 *        在独立的 FreeRTOS 任务中循环调用 lv_timer_handler() 驱动 LVGL，
 *        包含互斥锁保护和看门狗超时防护。
 */
static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    int64_t handler_start = 0;
    int64_t handler_elapsed = 0;

    while (1) {
        handler_start = esp_timer_get_time();
        _lock_acquire(&s_lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&s_lvgl_api_lock);
        handler_elapsed = (esp_timer_get_time() - handler_start) / 1000;

        if (handler_elapsed > 100) {
            ESP_LOGW(TAG, "lv_timer_handler took %lld ms (returned %lu ms)",
                     handler_elapsed, time_till_next_ms);
        }

        /* Prevent task watchdog timeout */
        uint32_t min_delay = 1000 / CONFIG_FREERTOS_HZ;
        time_till_next_ms = MAX(time_till_next_ms, min_delay);
        time_till_next_ms = MIN(time_till_next_ms, 500);
        usleep(1000 * time_till_next_ms);
    }
}

/**
 * @brief 从 Kconfig 获取默认配置
 *        读取 menuconfig 中定义的所有引脚和参数默认值，组装成配置结构体返回。
 */
spi_lcd_touch_config_t spi_lcd_touch_get_default_config(void)
{
    spi_lcd_touch_config_t config = {
        .sclk_gpio = CONFIG_SPI_LCD_TOUCH_SCLK_GPIO,
        .mosi_gpio = CONFIG_SPI_LCD_TOUCH_MOSI_GPIO,
        .miso_gpio = CONFIG_SPI_LCD_TOUCH_MISO_GPIO,
        .lcd_dc_gpio = CONFIG_SPI_LCD_TOUCH_LCD_DC_GPIO,
        .lcd_cs_gpio = CONFIG_SPI_LCD_TOUCH_LCD_CS_GPIO,
        .lcd_rst_gpio = CONFIG_SPI_LCD_TOUCH_LCD_RST_GPIO,
        .lcd_bk_light_gpio = CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO,
        .pixel_clock_hz = CONFIG_SPI_LCD_TOUCH_PIXEL_CLOCK_HZ,
        .h_res = CONFIG_SPI_LCD_TOUCH_H_RES,
        .v_res = CONFIG_SPI_LCD_TOUCH_V_RES,
        .draw_buf_lines = CONFIG_SPI_LCD_TOUCH_DRAW_BUF_LINES,
        .tick_period_ms = CONFIG_SPI_LCD_TOUCH_TICK_PERIOD_MS,
        .task_stack_size = CONFIG_SPI_LCD_TOUCH_TASK_STACK_SIZE,
        .task_priority = CONFIG_SPI_LCD_TOUCH_TASK_PRIORITY,
#if CONFIG_SPI_LCD_TOUCH_ENABLED
        .touch_enabled = true,
        .touch_cs_gpio = CONFIG_SPI_LCD_TOUCH_CS_GPIO,
        .touch_int_gpio = CONFIG_SPI_LCD_TOUCH_INT_GPIO,
#if CONFIG_SPI_LCD_TOUCH_MIRROR_Y
        .mirror_y = true,
#else
        .mirror_y = false,
#endif
#else
        .touch_enabled = false,
        .touch_cs_gpio = -1,
        .touch_int_gpio = -1,
        .mirror_y = false,
#endif
    };
    return config;
}

/**
 * @brief 初始化 SPI 总线、LCD 面板、触摸控制器和 LVGL
 *        完整初始化流程：配置背光 → 初始化 SPI 总线 → 安装 LCD 面板驱动 →
 *        初始化 LVGL → 创建双缓冲 DMA 绘图缓冲区 → 注册回调 → 初始化触摸控制器。
 *        注意：此函数不启动 LVGL 任务。
 */
esp_err_t spi_lcd_touch_init(const spi_lcd_touch_config_t *config,
                             lv_display_t **out_display)
{
    /* Use default config if none provided */
    if (config == NULL) {
        s_config = spi_lcd_touch_get_default_config();
    } else {
        s_config = *config;
    }

    /* Turn off LCD backlight initially */
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << s_config.lcd_bk_light_gpio
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* Initialize SPI bus */
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = s_config.sclk_gpio,
        .mosi_io_num = s_config.mosi_gpio,
        .miso_io_num = s_config.miso_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_config.h_res * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Install LCD panel IO */
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = s_config.lcd_dc_gpio,
        .cs_gpio_num = s_config.lcd_cs_gpio,
        .pclk_hz = s_config.pixel_clock_hz,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    /* Configure LCD panel */
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = s_config.lcd_rst_gpio,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };

    /* Install ST7789 driver */
    ESP_LOGI(TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* Clear display to black before turning on backlight (prevents garbage flash) */
    ESP_LOGI(TAG, "Clearing display...");
    uint16_t *black_buf = heap_caps_malloc(s_config.h_res * 80 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (black_buf) {
        memset(black_buf, 0, s_config.h_res * 80 * sizeof(uint16_t));
        for (int y = 0; y < s_config.v_res; y += 80) {
            int lines = (y + 80 > s_config.v_res) ? (s_config.v_res - y) : 80;
            esp_lcd_panel_draw_bitmap(panel_handle, 0, y, s_config.h_res, y + lines, black_buf);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        heap_caps_free(black_buf);
    }

    /* Turn on LCD backlight */
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(s_config.lcd_bk_light_gpio, 1);

    /* Initialize LVGL library */
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    /* Create semaphore for DMA transfer synchronization */
    s_lvgl_flush_sem = xSemaphoreCreateBinary();
    assert(s_lvgl_flush_sem);

    /* Create LVGL display object */
    lv_display_t *display = lv_display_create(s_config.h_res, s_config.v_res);

    /* Allocate LVGL draw buffers (double buffering) */
    size_t draw_buffer_sz = s_config.h_res * s_config.draw_buf_lines * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);

    /* Configure LVGL display */
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    /* Install LVGL tick timer */
    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, s_config.tick_period_ms * 1000));

    /* Register IO panel event callback for LVGL flush ready notification */
    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

#if CONFIG_SPI_LCD_TOUCH_ENABLED
    /* Initialize touch controller */
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_config =
#ifdef CONFIG_SPI_LCD_TOUCH_CONTROLLER_STMPE610
        ESP_LCD_TOUCH_IO_SPI_STMPE610_CONFIG(s_config.touch_cs_gpio);
#elif CONFIG_SPI_LCD_TOUCH_CONTROLLER_XPT2046
        ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(s_config.touch_cs_gpio);
#endif

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &tp_io_config, &tp_io_handle));

    /* Configure touch INT pin as input */
    if (s_config.touch_int_gpio >= 0) {
        gpio_config_t int_cfg = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << s_config.touch_int_gpio,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&int_cfg);
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = s_config.h_res,
        .y_max = s_config.v_res,
        .rst_gpio_num = -1,
        .int_gpio_num = s_config.touch_int_gpio,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = s_config.mirror_y ? 1 : 0,
        },
    };

#if CONFIG_SPI_LCD_TOUCH_CONTROLLER_STMPE610
    ESP_LOGI(TAG, "Initialize touch controller STMPE610");
    esp_err_t touch_err = esp_lcd_touch_new_spi_stmpe610(tp_io_handle, &tp_cfg, &s_touch_handle);
    if (touch_err != ESP_OK) {
        ESP_LOGE(TAG, "STMPE610 init failed: %s (0x%x)", esp_err_to_name(touch_err), touch_err);
        s_touch_handle = NULL;
    } else {
        ESP_LOGI(TAG, "STMPE610 initialized successfully");
    }
#elif CONFIG_SPI_LCD_TOUCH_CONTROLLER_XPT2046
    ESP_LOGI(TAG, "Initialize touch controller XPT2046");
    esp_err_t touch_err = esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &s_touch_handle);
    if (touch_err != ESP_OK) {
        ESP_LOGE(TAG, "XPT2046 init failed: %s (0x%x)", esp_err_to_name(touch_err), touch_err);
        s_touch_handle = NULL;
    } else {
        ESP_LOGI(TAG, "XPT2046 initialized successfully");
    }
#endif

    /* Create touch input device */
    static lv_indev_t *indev;
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_user_data(indev, s_touch_handle);
    lv_indev_set_read_cb(indev, lvgl_touch_cb);
#endif

    /* Return display handle */
    if (out_display != NULL) {
        *out_display = display;
    }

    return ESP_OK;
}

/**
 * @brief 启动 LVGL 任务
 *        必须在所有 LVGL UI 设置完成之后调用，创建一个独立的 FreeRTOS 任务来驱动 LVGL 渲染。
 */
void spi_lcd_touch_start_task(void)
{
    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", s_config.task_stack_size, NULL, s_config.task_priority, NULL);
}

/**
 * @brief 获取 LVGL API 互斥锁
 *        在非 LVGL 任务中操作 LVGL 对象前必须调用此函数加锁。
 */
void spi_lcd_touch_lock(void)
{
    _lock_acquire(&s_lvgl_api_lock);
}

/**
 * @brief 释放 LVGL API 互斥锁
 *        与 spi_lcd_touch_lock() 配对使用。
 */
void spi_lcd_touch_unlock(void)
{
    _lock_release(&s_lvgl_api_lock);
}
