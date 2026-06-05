#include "who_ov5647_cam.hpp"
#include "camera.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "WhoOV5647Cam";

namespace who {
namespace cam {

WhoOV5647Cam::WhoOV5647Cam() :
    WhoCam(3, CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES, CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES),
    m_cam_handle(nullptr), m_active_idx(0)
{
    esp_cam_ctlr_trans_t trans;
    ESP_ERROR_CHECK(camera_init(&m_cam_handle, &trans));
    ESP_ERROR_CHECK(camera_start(m_cam_handle));
    ESP_LOGI(TAG, "OV5647 camera started (%dx%d RGB565)", m_fb_width, m_fb_height);
}

WhoOV5647Cam::~WhoOV5647Cam()
{
    if (m_cam_handle) {
        esp_cam_ctlr_stop(m_cam_handle);
        esp_cam_ctlr_del(m_cam_handle);
    }
}

cam_fb_t *WhoOV5647Cam::cam_fb_get()
{
    SemaphoreHandle_t sem = camera_get_frame_ready_sem();
    if (xSemaphoreTake(sem, portMAX_DELAY) != pdTRUE) {
        return nullptr;
    }

    volatile int *idx_ptr = camera_get_display_buf_idx_ptr();
    int idx = *idx_ptr;
    if (idx < 0) {
        return nullptr;
    }

    cam_fb_t *fb = &m_cam_fbs[0];
    fb->buf = camera_get_frame_buffer(idx);
    fb->len = camera_get_frame_buffer_size();
    fb->width = m_fb_width;
    fb->height = m_fb_height;
    fb->format = cam_fb_fmt_t::CAM_FB_FMT_RGB565;

    int64_t us = esp_timer_get_time();
    fb->timestamp.tv_sec = us / 1000000;
    fb->timestamp.tv_usec = us % 1000000;
    fb->ret = nullptr;

    return fb;
}

void WhoOV5647Cam::cam_fb_return(cam_fb_t *fb)
{
    // Double-buffered camera auto-switches buffers, nothing to do
}

} // namespace cam
} // namespace who
