#pragma once
#include "who_cam_base.hpp"
#include "esp_cam_ctlr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace who {
namespace cam {

class WhoOV5647Cam : public WhoCam {
public:
    WhoOV5647Cam();
    ~WhoOV5647Cam();
    cam_fb_t *cam_fb_get() override;
    void cam_fb_return(cam_fb_t *fb) override;
    cam_fb_fmt_t get_fb_format() override { return cam_fb_fmt_t::CAM_FB_FMT_RGB565; }

private:
    esp_cam_ctlr_handle_t m_cam_handle;
    int m_active_idx;
};

} // namespace cam
} // namespace who
