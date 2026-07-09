/*
 * Web Server — Camera Stream + REST API for Mini-Program
 *
 * Endpoints:
 *   GET /         — HTML monitoring dashboard
 *   GET /status   — Camera + detection status JSON
 *   GET /stream   — MJPEG live stream
 *   GET /snapshot — Single JPEG frame
 *   + 18 REST API endpoints for mini-program (inventory, orders, sales, admin)
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "web_server.h"
#include "web_page.h"
#include "jpeg_encoder.h"

/* WHO framework headers for frame access */
#include "who_frame_cap.hpp"
#include "who_cam_define.hpp"

/* New components for REST API */
#include "inventory_manager.h"
#include "face_id_manager.h"
#include "admin_auth.h"
#include "order_manager.h"

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;

/* Config set by web_server_start() */
static web_server_config_t s_config;

/* Face count tracking (updated from main.cpp) */
static volatile int s_face_count = 0;

/* ── JSON buffer sizes ── */
#define JSON_BUF_SIZE   4096
#define SMALL_BUF_SIZE  512

/* ── Helper macro ── */
#define SET_JSON_HDR(req)  httpd_resp_set_type((req), "application/json")

/* ── Helper: get face count ── */
extern "C" void web_server_set_face_count(int count)
{
    s_face_count = count;
}

/* ===================================================================
 * Helper functions for REST API
 * =================================================================== */

static int json_escape(char *dst, const char *src, size_t max_len)
{
    int written = 0;
    while (*src && written < (int)(max_len - 2)) {
        switch (*src) {
            case '"':  dst[written++] = '\\'; dst[written++] = '"';  break;
            case '\\': dst[written++] = '\\'; dst[written++] = '\\'; break;
            case '\n': dst[written++] = '\\'; dst[written++] = 'n';  break;
            case '\r': dst[written++] = '\\'; dst[written++] = 'r';  break;
            case '\t': dst[written++] = '\\'; dst[written++] = 't';  break;
            default:   dst[written++] = *src; break;
        }
        src++;
    }
    dst[written] = '\0';
    return written;
}

static esp_err_t get_query_param(httpd_req_t *req, const char *key, char *value, size_t max_len)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len <= 1) return ESP_ERR_NOT_FOUND;

    char *query = (char *)malloc(query_len + 1);
    if (!query) return ESP_ERR_NO_MEM;

    esp_err_t ret = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (ret != ESP_OK) {
        free(query);
        return ret;
    }

    ret = httpd_query_key_value(query, key, value, max_len);
    free(query);
    return ret;
}

/* ===================================================================
 * GET / — Serve the HTML monitoring dashboard
 * =================================================================== */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, WEB_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ===================================================================
 * GET /status — Camera + detection JSON status (legacy)
 * =================================================================== */
static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[512];
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    size_t free_heap = esp_get_free_heap_size();
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    bool cam_running = (s_config.frame_cap_node != nullptr);
    bool det_running = (s_config.detector != nullptr);
    bool vending = s_config.vending_active ? *s_config.vending_active : false;

    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"camera\":{\"sensor\":\"OV5647\",\"resolution\":\"%dx%d\","
        "\"format\":\"RGB565\",\"fps\":30,\"running\":%s},"
        "\"detection\":{\"model\":\"MSRMNP_S8_V1\",\"fps\":5,"
        "\"running\":%s,\"faces\":%d},"
        "\"system\":{\"uptime_ms\":%lld,\"free_heap\":%u,"
        "\"psram_free\":%u,\"vending_active\":%s}"
        "}",
        s_config.cam_width, s_config.cam_height,
        cam_running ? "true" : "false",
        det_running ? "true" : "false",
        s_face_count,
        (long long)uptime_ms,
        (unsigned)free_heap,
        (unsigned)psram_free,
        vending ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ===================================================================
 * GET /snapshot — Single JPEG frame
 * =================================================================== */
static esp_err_t snapshot_handler(httpd_req_t *req)
{
    if (!s_config.frame_cap_node) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Camera not available", 20);
    }

    auto *node = (who::frame_cap::WhoFrameCapNode *)s_config.frame_cap_node;
    auto *fb = node->cam_fb_peek(-1);
    if (!fb || !fb->buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "No frame available", 19);
    }

    uint8_t *jpeg_buf = nullptr;
    size_t jpeg_len = 0;
    esp_err_t ret = rgb565_to_jpeg((const uint8_t *)fb->buf,
                                    fb->width, fb->height, 75,
                                    &jpeg_buf, &jpeg_len);
    if (ret != ESP_OK || !jpeg_buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "JPEG encode failed", 18);
    }

    httpd_resp_set_type(req, "image/jpeg");
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%u", (unsigned)jpeg_len);
    httpd_resp_set_hdr(req, "Content-Length", hdr);

    return httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);
}

/* ===================================================================
 * GET /stream — MJPEG multipart stream
 * =================================================================== */
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!s_config.frame_cap_node) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Camera not available", 20);
    }

    static const char *BOUNDARY = "frame";
    char part_hdr[128];
    esp_err_t ret;

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

    auto *node = (who::frame_cap::WhoFrameCapNode *)s_config.frame_cap_node;

    ESP_LOGI(TAG, "MJPEG stream started");

    while (true) {
        if (s_config.vending_active && *s_config.vending_active) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        auto *fb = node->cam_fb_peek(-1);
        if (!fb || !fb->buf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t *jpeg_buf = nullptr;
        size_t jpeg_len = 0;
        ret = rgb565_to_jpeg((const uint8_t *)fb->buf,
                              fb->width, fb->height, 75,
                              &jpeg_buf, &jpeg_len);
        if (ret != ESP_OK || !jpeg_buf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            BOUNDARY, (unsigned)jpeg_len);

        ret = httpd_resp_send_chunk(req, part_hdr, hdr_len);
        if (ret != ESP_OK) {
            free(jpeg_buf);
            break;
        }

        ret = httpd_resp_send_chunk(req, (const char *)jpeg_buf, jpeg_len);
        if (ret != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(125));
    }

    ESP_LOGI(TAG, "MJPEG stream ended");
    return ESP_OK;
}

/* ===================================================================
 * GET /api/system — System status
 * =================================================================== */
static esp_err_t api_system_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_s = uptime_us / 1000000;
    int32_t free_heap = esp_get_free_heap_size();
    int32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool vending = (s_config.vending_active && *s_config.vending_active);

    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"uptime_s\":%lld,"
        "\"free_heap\":%ld,"
        "\"free_psram\":%ld,"
        "\"vending_active\":%s,"
        "\"faces_registered\":%d,"
        "\"timestamp_us\":%lld"
        "}",
        (long long)uptime_s,
        (long)free_heap,
        (long)free_psram,
        vending ? "true" : "false",
        face_id_get_count(),
        (long long)uptime_us
    );

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/inventory — All products inventory
 * =================================================================== */
static esp_err_t api_inventory_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    product_inventory_t items[INVENTORY_MAX_PRODUCTS];
    int count = 0;

    esp_err_t ret = inventory_get_all(items, &count);
    if (ret != ESP_OK) {
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(ret));
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    int pos = snprintf(buf, sizeof(buf), "{\"products\":[");
    for (int i = 0; i < count && pos < (int)(sizeof(buf) - 200); i++) {
        int remaining = items[i].total_stock - items[i].sold_count;
        if (remaining < 0) remaining = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"name\":\"%s\",\"total\":%d,\"sold\":%d,\"remaining\":%d,\"price\":%.2f}",
            i > 0 ? "," : "",
            items[i].name,
            items[i].total_stock,
            items[i].sold_count,
            remaining,
            (double)items[i].price
        );
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"count\":%d,\"timestamp_us\":%lld}",
        count, (long long)esp_timer_get_time());

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/products — Product list for mini-program
 * =================================================================== */
static esp_err_t api_products_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    product_inventory_t items[INVENTORY_MAX_PRODUCTS];
    int count = 0;

    inventory_get_all(items, &count);

    int pos = snprintf(buf, sizeof(buf), "{\"products\":[");
    for (int i = 0; i < count && pos < (int)(sizeof(buf) - 200); i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"name\":\"%s\",\"price\":%.2f,\"stock\":%d,\"sold\":%d}",
            i > 0 ? "," : "",
            items[i].name, (double)items[i].price,
            items[i].total_stock, items[i].sold_count);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", count);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/sales/summary — Sales summary
 * =================================================================== */
static esp_err_t api_sales_summary_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    sales_summary_t summary;
    inventory_get_sales_summary(&summary);

    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"total_sales\":%d,"
        "\"total_revenue\":%.2f,"
        "\"today_sales\":%d,"
        "\"today_revenue\":%.2f,"
        "\"total_users\":%d"
        "}",
        summary.total_sales,
        (double)summary.total_revenue,
        summary.today_sales,
        (double)summary.today_revenue,
        summary.total_users
    );

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/sales/today — Today's sales
 * =================================================================== */
static esp_err_t api_sales_today_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    int pos = 0;
    int today_count = 0;
    float today_revenue = 0.0f;

    FILE *f = fopen("/sdcard/purchase_log.csv", "r");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"sales\":[");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            int64_t now_us = esp_timer_get_time();
            int64_t today_start_us = now_us - (now_us % (24LL * 3600 * 1000000));

            while (fgets(line, sizeof(line), f) &&
                   pos < (int)(sizeof(buf) - 256) && today_count < 50) {
                int64_t ts;
                int face_id;
                char product[64], price_str[32];
                if (sscanf(line, "%lld,%d,%63[^,],%31s",
                           (long long *)&ts, &face_id, product, price_str) >= 3) {
                    if (ts >= today_start_us) {
                        float price = 0.0f;
                        const char *p = price_str;
                        if (*p == '$') p++;
                        price = atof(p);

                        if (today_count > 0) {
                            pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                        }
                        pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "{\"time\":%lld,\"user\":%d,\"product\":\"%s\",\"price\":\"%s\"}",
                            (long long)ts, face_id, product, price_str);
                        today_count++;
                        today_revenue += price;
                    }
                }
            }
        }
        fclose(f);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"count\":%d,\"revenue\":%.2f}",
        today_count, (double)today_revenue);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/sales/ranking — Best sellers
 * =================================================================== */
static esp_err_t api_sales_ranking_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    product_inventory_t ranking[INVENTORY_MAX_PRODUCTS];
    int count = 0;

    inventory_get_ranking(ranking, &count);

    int pos = snprintf(buf, sizeof(buf), "{\"ranking\":[");
    for (int i = 0; i < count && pos < (int)(sizeof(buf) - 200); i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"rank\":%d,\"name\":\"%s\",\"sold\":%d,\"price\":%.2f,\"revenue\":%.2f}",
            i > 0 ? "," : "",
            i + 1,
            ranking[i].name,
            ranking[i].sold_count,
            (double)ranking[i].price,
            (double)(ranking[i].sold_count * ranking[i].price)
        );
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", count);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/sales/history?limit=20 — Recent purchase records
 * =================================================================== */
static esp_err_t api_sales_history_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    int limit = 20;

    char query_val[32];
    if (get_query_param(req, "limit", query_val, sizeof(query_val)) == ESP_OK) {
        limit = atoi(query_val);
        if (limit <= 0) limit = 20;
        if (limit > 100) limit = 100;
    }

    /* Load user name mapping */
    struct {
        int id;
        char name[64];
    } user_names[100] = {};
    int user_count = 0;
    FILE *uf = fopen("/sdcard/face_db.csv", "r");
    if (uf) {
        char uline[256];
        if (fgets(uline, sizeof(uline), uf)) {
            while (fgets(uline, sizeof(uline), uf) && user_count < 100) {
                int id;
                char name[64] = {0};
                if (sscanf(uline, "%d,%63[^,]", &id, name) >= 2) {
                    user_names[user_count].id = id;
                    strncpy(user_names[user_count].name, name, 63);
                    user_count++;
                }
            }
        }
        fclose(uf);
    }

    FILE *f = fopen("/sdcard/purchase_log.csv", "r");
    int pos = snprintf(buf, sizeof(buf), "{\"history\":[");
    int count = 0;

    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            struct {
                int64_t ts;
                int face_id;
                char product[64];
                char price[32];
            } records[100] = {};
            int total = 0;

            while (fgets(line, sizeof(line), f) && total < 100) {
                if (sscanf(line, "%lld,%d,%63[^,],%31s",
                           (long long *)&records[total].ts,
                           &records[total].face_id,
                           records[total].product,
                           records[total].price) >= 3) {
                    total++;
                }
            }

            int start = (total > limit) ? (total - limit) : 0;
            for (int i = start; i < total && pos < (int)(sizeof(buf) - 256); i++) {
                const char *user_name = "未知";
                for (int j = 0; j < user_count; j++) {
                    if (user_names[j].id == records[i].face_id) {
                        user_name = user_names[j].name;
                        break;
                    }
                }
                if (count > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"time\":%lld,\"user\":%d,\"user_name\":\"%s\",\"product\":\"%s\",\"price\":\"%s\"}",
                    (long long)records[i].ts,
                    records[i].face_id,
                    user_name,
                    records[i].product,
                    records[i].price);
                count++;
            }
        }
        fclose(f);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", count);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/users — Registered users
 * =================================================================== */
static esp_err_t api_users_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    int pos = snprintf(buf, sizeof(buf), "{\"users\":[");
    int count = 0;

    FILE *f = fopen("/sdcard/face_db.csv", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f) &&
                   pos < (int)(sizeof(buf) - 200) && count < 100) {
                int id = 0;
                char name[64] = {0};
                int64_t enroll_time = 0;
                if (sscanf(line, "%d,%63[^,],%lld", &id, name, (long long *)&enroll_time) >= 2) {
                    if (count > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{\"id\":%d,\"name\":\"%s\",\"enroll_time\":%lld}",
                        id, name, (long long)enroll_time);
                    count++;
                }
            }
        }
        fclose(f);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", count);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * GET /api/users/:id/purchases — User purchase history
 * =================================================================== */
static esp_err_t api_user_purchases_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    const char *uri = req->uri;

    int target_id = -1;
    const char *p = strstr(uri, "/api/users/");
    if (p) {
        target_id = atoi(p + 11);
    }

    if (target_id < 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Invalid user ID\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    int pos = snprintf(buf, sizeof(buf),
        "{\"user_id\":%d,\"purchases\":[", target_id);
    int count = 0;

    FILE *f = fopen("/sdcard/purchase_log.csv", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f) &&
                   pos < (int)(sizeof(buf) - 256) && count < 100) {
                int64_t ts;
                int face_id;
                char product[64], price[32];
                if (sscanf(line, "%lld,%d,%63[^,],%31s",
                           (long long *)&ts, &face_id, product, price) >= 3) {
                    if (face_id == target_id) {
                        if (count > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                        pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "{\"time\":%lld,\"product\":\"%s\",\"price\":\"%s\"}",
                            (long long)ts, product, price);
                        count++;
                    }
                }
            }
        }
        fclose(f);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", count);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * POST /api/inventory/restock — Restock product
 * =================================================================== */
static esp_err_t api_restock_handler(httpd_req_t *req)
{
    char buf[SMALL_BUF_SIZE];
    char body[256] = {0};

    int total_len = req->content_len;
    if (total_len > (int)(sizeof(body) - 1)) total_len = sizeof(body) - 1;
    int received = httpd_req_recv(req, body, total_len);
    if (received <= 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Empty body\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }
    body[received] = '\0';

    char product[64] = {0};
    int count = 0;

    char *pp = strstr(body, "\"product\"");
    if (pp) {
        pp = strchr(pp, ':');
        if (pp) {
            pp = strchr(pp, '"');
            if (pp) {
                pp++;
                int i = 0;
                while (*pp && *pp != '"' && i < 63) {
                    product[i++] = *pp++;
                }
                product[i] = '\0';
            }
        }
    }

    char *cp = strstr(body, "\"count\"");
    if (cp) {
        cp = strchr(cp, ':');
        if (cp) {
            count = atoi(cp + 1);
        }
    }

    if (product[0] == '\0' || count <= 0) {
        snprintf(buf, sizeof(buf),
            "{\"error\":\"Invalid request, need product and count\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    esp_err_t ret = inventory_restock(product, count);
    if (ret == ESP_OK) {
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"product\":\"%s\",\"added\":%d}", product, count);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"success\":false,\"error\":\"%s\"}", esp_err_to_name(ret));
    }

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ===================================================================
 * POST /api/order — Create order
 * =================================================================== */
static esp_err_t api_order_create_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    char body[512] = {0};

    int total_len = req->content_len;
    if (total_len > (int)(sizeof(body) - 1)) total_len = sizeof(body) - 1;
    int received = httpd_req_recv(req, body, total_len);
    if (received <= 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Empty body\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }
    body[received] = '\0';

    char product[64] = {0};
    int user_id = -1;
    int payment = 0;

    char *pp = strstr(body, "\"product\"");
    if (pp) { pp = strchr(pp, ':'); if (pp) { pp = strchr(pp, '"'); if (pp) {
        pp++; int i = 0; while (*pp && *pp != '"' && i < 63) product[i++] = *pp++; } } }

    char *up = strstr(body, "\"user_id\"");
    if (up) { up = strchr(up, ':'); if (up) user_id = atoi(up + 1); }

    char *mp = strstr(body, "\"payment_method\"");
    if (mp) { if (strstr(mp, "scan")) payment = 1; }

    if (product[0] == '\0' || user_id < 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Missing product or user_id\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    product_inventory_t item;
    float price = 0.0f;
    if (inventory_get_product(product, &item) == ESP_OK) {
        price = item.price;
    }

    char order_id[ORDER_ID_LEN];
    esp_err_t ret = order_create(user_id, product, price,
                                  (payment_method_t)payment, order_id, sizeof(order_id));

    if (ret == ESP_OK) {
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"order_id\":\"%s\",\"status\":\"pending\","
            "\"product\":\"%s\",\"price\":%.2f}",
            order_id, product, (double)price);
    } else {
        snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}",
                 esp_err_to_name(ret));
    }

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ===================================================================
 * /api/order/:id — Query / Cancel / Confirm / Ship
 * =================================================================== */
static esp_err_t api_order_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    const char *uri = req->uri;

    const char *p = strstr(uri, "/api/order/");
    if (!p) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Invalid URI\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }
    p += 11;

    char order_id[ORDER_ID_LEN] = {0};
    int i = 0;
    while (*p && *p != '/' && i < ORDER_ID_LEN - 1) {
        order_id[i++] = *p++;
    }
    order_id[i] = '\0';

    if (order_id[0] == '\0') {
        snprintf(buf, sizeof(buf), "{\"error\":\"Missing order_id\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    if (req->method == HTTP_POST) {
        if (strstr(uri, "/ship") || strstr(p, "ship")) {
            esp_err_t ret = order_ship(order_id);
            if (ret == ESP_OK) {
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"order_id\":\"%s\",\"status\":\"shipped\"}", order_id);
            } else {
                snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}",
                         esp_err_to_name(ret));
            }
        } else if (strstr(uri, "/confirm") || strstr(p, "confirm")) {
            esp_err_t ret = order_confirm(order_id);
            if (ret == ESP_OK) {
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"order_id\":\"%s\",\"status\":\"confirmed\"}", order_id);
            } else {
                snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}",
                         esp_err_to_name(ret));
            }
        } else {
            esp_err_t ret = order_cancel(order_id);
            if (ret == ESP_OK) {
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"order_id\":\"%s\",\"status\":\"cancelled\"}", order_id);
            } else {
                snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}",
                         esp_err_to_name(ret));
            }
        }
    } else {
        order_record_t record;
        esp_err_t ret = order_get(order_id, &record);
        if (ret == ESP_OK) {
            snprintf(buf, sizeof(buf),
                "{\"order_id\":\"%s\",\"user_id\":%d,\"product\":\"%s\","
                "\"price\":%.2f,\"payment\":\"%s\",\"status\":\"%s\","
                "\"created_at\":%lld}",
                record.order_id, record.user_id, record.product_name,
                (double)record.price,
                order_payment_str((payment_method_t)record.payment_method),
                order_status_str((order_status_t)record.status),
                (long long)record.created_at_us);
        } else {
            snprintf(buf, sizeof(buf), "{\"error\":\"Order not found\"}");
        }
    }

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ===================================================================
 * GET /api/recommend/:user_id — Personalized recommendations
 * =================================================================== */
static esp_err_t api_recommend_handler(httpd_req_t *req)
{
    char buf[JSON_BUF_SIZE];
    const char *uri = req->uri;

    const char *p = strstr(uri, "/api/recommend/");
    int user_id = -1;
    if (p) {
        user_id = atoi(p + 15);
    }

    if (user_id < 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Invalid user_id\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    recommend_item_t items[INVENTORY_MAX_PRODUCTS];
    int count = 0;
    inventory_get_recommend(user_id, items, &count);

    int pos = snprintf(buf, sizeof(buf),
        "{\"user_id\":%d,\"recommendations\":[", user_id);

    for (int i = 0; i < count && pos < (int)(sizeof(buf) - 200); i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"product_name\":\"%s\",\"price\":%.2f,\"stock\":%d,"
            "\"reason\":\"%s\",\"score\":%d,\"buy_freq\":%d}",
            i > 0 ? "," : "",
            items[i].product_name, (double)items[i].price,
            items[i].total_stock,
            items[i].reason, items[i].recommend_score,
            items[i].buy_freq);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"count\":%d}", count);

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ===================================================================
 * POST /api/face/register — Face registration from mini-program
 * =================================================================== */
static esp_err_t api_face_register_handler(httpd_req_t *req)
{
    char buf[SMALL_BUF_SIZE];
    char body[512] = {0};

    int total_len = req->content_len;
    if (total_len > (int)(sizeof(body) - 1)) total_len = sizeof(body) - 1;
    httpd_req_recv(req, body, total_len);

    int user_id = -1;
    char *up = strstr(body, "\"user_id\"");
    if (up) { up = strchr(up, ':'); if (up) user_id = atoi(up + 1); }

    if (user_id < 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"Missing user_id\"}");
        SET_JSON_HDR(req);
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    }

    if (strstr(body, "\"device\"") || strstr(body, "\"mode\"")) {
        FILE *f = fopen("/sdcard/pending_register.txt", "w");
        if (f) {
            fprintf(f, "%d\n", user_id);
            fclose(f);
        }
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"message\":\"Please look at the camera to complete\"}");
    } else {
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"message\":\"Face image received\"}");
    }

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ===================================================================
 * POST /api/admin/login — Admin login
 * =================================================================== */
static esp_err_t api_admin_login_handler(httpd_req_t *req)
{
    char buf[SMALL_BUF_SIZE];
    char body[256] = {0};

    int total_len = req->content_len;
    if (total_len > (int)(sizeof(body) - 1)) total_len = sizeof(body) - 1;
    httpd_req_recv(req, body, total_len);

    char password[64] = {0};
    char *pp = strstr(body, "\"password\"");
    if (pp) {
        pp = strchr(pp, ':');
        if (pp) { pp = strchr(pp, '"'); if (pp) {
            pp++; int i = 0;
            while (*pp && *pp != '"' && i < 63) password[i++] = *pp++;
        } }
    }

    bool ok = admin_auth_verify(password);
    snprintf(buf, sizeof(buf), "{\"success\":%s}", ok ? "true" : "false");

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ===================================================================
 * POST /api/admin/password — Change admin password
 * =================================================================== */
static esp_err_t api_admin_password_handler(httpd_req_t *req)
{
    char buf[SMALL_BUF_SIZE];
    char body[256] = {0};

    int total_len = req->content_len;
    if (total_len > (int)(sizeof(body) - 1)) total_len = sizeof(body) - 1;
    httpd_req_recv(req, body, total_len);

    char old_pwd[64] = {0}, new_pwd[64] = {0};

    char *pp = strstr(body, "\"old_password\"");
    if (pp) {
        pp = strchr(pp, ':');
        if (pp) { pp = strchr(pp, '"'); if (pp) {
            pp++; int i = 0;
            while (*pp && *pp != '"' && i < 63) old_pwd[i++] = *pp++;
        } }
    }

    pp = strstr(body, "\"new_password\"");
    if (pp) {
        pp = strchr(pp, ':');
        if (pp) { pp = strchr(pp, '"'); if (pp) {
            pp++; int i = 0;
            while (*pp && *pp != '"' && i < 63) new_pwd[i++] = *pp++;
        } }
    }

    esp_err_t ret = admin_auth_change_password(old_pwd, new_pwd);
    if (ret == ESP_OK) {
        snprintf(buf, sizeof(buf), "{\"success\":true}");
    } else {
        snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}",
                 esp_err_to_name(ret));
    }

    SET_JSON_HDR(req);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ===================================================================
 * URI handler tables
 * =================================================================== */

/* Fixed routes */
static const httpd_uri_t s_uri_handlers[] = {
    {.uri = "/",                        .method = HTTP_GET,  .handler = root_handler, .user_ctx = NULL},
    {.uri = "/status",                  .method = HTTP_GET,  .handler = status_handler, .user_ctx = NULL},
    {.uri = "/stream",                  .method = HTTP_GET,  .handler = stream_handler, .user_ctx = NULL},
    {.uri = "/snapshot",                .method = HTTP_GET,  .handler = snapshot_handler, .user_ctx = NULL},
    {.uri = "/api/system",              .method = HTTP_GET,  .handler = api_system_handler, .user_ctx = NULL},
    {.uri = "/api/inventory",           .method = HTTP_GET,  .handler = api_inventory_handler, .user_ctx = NULL},
    {.uri = "/api/inventory/restock",   .method = HTTP_POST, .handler = api_restock_handler, .user_ctx = NULL},
    {.uri = "/api/sales/summary",       .method = HTTP_GET,  .handler = api_sales_summary_handler, .user_ctx = NULL},
    {.uri = "/api/sales/today",         .method = HTTP_GET,  .handler = api_sales_today_handler, .user_ctx = NULL},
    {.uri = "/api/sales/ranking",       .method = HTTP_GET,  .handler = api_sales_ranking_handler, .user_ctx = NULL},
    {.uri = "/api/sales/history",       .method = HTTP_GET,  .handler = api_sales_history_handler, .user_ctx = NULL},
    {.uri = "/api/users",               .method = HTTP_GET,  .handler = api_users_handler, .user_ctx = NULL},
    {.uri = "/api/products",            .method = HTTP_GET,  .handler = api_products_handler, .user_ctx = NULL},
    {.uri = "/api/order",               .method = HTTP_POST, .handler = api_order_create_handler, .user_ctx = NULL},
    {.uri = "/api/face/register",       .method = HTTP_POST, .handler = api_face_register_handler, .user_ctx = NULL},
    {.uri = "/api/admin/login",         .method = HTTP_POST, .handler = api_admin_login_handler, .user_ctx = NULL},
    {.uri = "/api/admin/password",      .method = HTTP_POST, .handler = api_admin_password_handler, .user_ctx = NULL},
};

#define URI_COUNT (sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]))

/* Wildcard routes */
static const httpd_uri_t s_user_purchases_uri = {
    .uri = "/api/users/*",
    .method = HTTP_GET,
    .handler = api_user_purchases_handler, .user_ctx = NULL,
};

static const httpd_uri_t s_order_get_uri = {
    .uri = "/api/order/*",
    .method = HTTP_GET,
    .handler = api_order_handler, .user_ctx = NULL,
};

static const httpd_uri_t s_order_post_uri = {
    .uri = "/api/order/*",
    .method = HTTP_POST,
    .handler = api_order_handler, .user_ctx = NULL,
};

static const httpd_uri_t s_recommend_uri = {
    .uri = "/api/recommend/*",
    .method = HTTP_GET,
    .handler = api_recommend_handler, .user_ctx = NULL,
};

/* ===================================================================
 * Start / Stop
 * =================================================================== */
extern "C" esp_err_t web_server_start(const web_server_config_t *config)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));

    /* Initialize JPEG encoder for camera stream */
    esp_err_t ret = jpeg_encoder_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encoder init failed");
        return ret;
    }

    /* Configure HTTP server — larger stack for REST API handlers */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size = 32768;
    http_cfg.max_uri_handlers = 24;
    http_cfg.lru_purge_enable = true;

    ret = httpd_start(&s_server, &http_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        jpeg_encoder_deinit();
        return ret;
    }

    /* Register fixed routes */
    for (size_t i = 0; i < URI_COUNT; i++) {
        ret = httpd_register_uri_handler(s_server, &s_uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register %s: %s",
                     s_uri_handlers[i].uri, esp_err_to_name(ret));
        }
    }

    /* Register wildcard routes */
    httpd_register_uri_handler(s_server, &s_user_purchases_uri);
    httpd_register_uri_handler(s_server, &s_order_get_uri);
    httpd_register_uri_handler(s_server, &s_order_post_uri);
    httpd_register_uri_handler(s_server, &s_recommend_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d (%d endpoints + 4 wildcard)",
             http_cfg.server_port, (int)URI_COUNT);
    return ESP_OK;
}

extern "C" esp_err_t web_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;

    jpeg_encoder_deinit();

    memset(&s_config, 0, sizeof(s_config));

    ESP_LOGI(TAG, "HTTP server stopped");
    return ret;
}
