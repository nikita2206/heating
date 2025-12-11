#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Common CSS shared across all web pages
static const char WEB_UI_COMMON_STYLES[] =
    ":root{--bg:#0f0f13;--card:#1a1a24;--accent:#00d4aa;--accent2:#7c3aed;--text:#e4e4e7;--muted:#71717a;--border:#27272a}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:'SF Mono',Monaco,Consolas,monospace;background:var(--bg);color:var(--text);min-height:100vh}"
    ".container{max-width:1200px;margin:0 auto;padding:24px}"
    "nav{background:var(--card);border-bottom:1px solid var(--border);padding:16px 24px;display:flex;align-items:center;gap:32px}"
    ".logo{font-size:18px;font-weight:700;color:var(--accent);text-decoration:none;display:flex;align-items:center;gap:8px}"
    ".logo svg{width:24px;height:24px}"
    ".nav-links{display:flex;gap:24px}"
    ".nav-links a{color:var(--muted);text-decoration:none;font-size:14px;transition:color 0.2s}"
    ".nav-links a:hover,.nav-links a.active{color:var(--accent)}"
    "h1{font-size:28px;margin-bottom:8px;background:linear-gradient(135deg,var(--accent),var(--accent2));-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
    ".subtitle{color:var(--muted);font-size:14px;margin-bottom:32px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:24px;margin-bottom:24px}"
    ".card-title{font-size:16px;font-weight:600;margin-bottom:16px;display:flex;align-items:center;gap:8px}"
    ".badge{display:inline-flex;align-items:center;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:500}"
    ".badge.green{background:rgba(0,212,170,0.15);color:var(--accent)}"
    ".badge.yellow{background:rgba(250,204,21,0.15);color:#facc15}"
    ".badge.red{background:rgba(239,68,68,0.15);color:#ef4444}"
    ".badge.purple{background:rgba(124,58,237,0.15);color:var(--accent2)}"
    "button,.btn{padding:10px 20px;border:none;border-radius:8px;cursor:pointer;font-size:14px;font-family:inherit;font-weight:500;transition:all 0.2s}"
    ".btn-primary{background:var(--accent);color:var(--bg)}"
    ".btn-primary:hover{background:#00e6b8;transform:translateY(-1px)}"
    ".btn-danger{background:#dc2626;color:white}"
    ".btn-danger:hover{background:#ef4444}"
    ".btn-secondary{background:var(--border);color:var(--text)}"
    ".btn-secondary:hover{background:#3f3f46}";

typedef enum {
    WEB_NAV_DASHBOARD = 0,
    WEB_NAV_LOGS,
    WEB_NAV_DIAGNOSTICS,
    WEB_NAV_MQTT,
    WEB_NAV_WRITE,
    WEB_NAV_OTA
} web_nav_page_t;

static const char WEB_UI_NAV_TEMPLATE[] =
    "<nav><a href='/' class='logo'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5'/></svg>OT Gateway</a>"
    "<div class='nav-links'>"
    "<a href='/' class='%s'>Dashboard</a>"
    "<a href='/logs' class='%s'>Logs</a>"
    "<a href='/diagnostics' class='%s'>Diagnostics</a>"
    "<a href='/mqtt' class='%s'>MQTT</a>"
    "<a href='/write' class='%s'>Manual Write</a>"
    "<a href='/ota' class='%s'>OTA Update</a>"
    "</div></nav>";

// Enough room for the full nav + all "active" classes and terminator
#define WEB_UI_NAV_MAX_LEN 512
#define WEB_UI_PAGE_DEFAULT_CAP 32768

static inline void web_ui_build_nav(char *out, size_t out_sz, web_nav_page_t active)
{
    const char *dashboard = active == WEB_NAV_DASHBOARD ? "active" : "";
    const char *logs = active == WEB_NAV_LOGS ? "active" : "";
    const char *diagnostics = active == WEB_NAV_DIAGNOSTICS ? "active" : "";
    const char *mqtt = active == WEB_NAV_MQTT ? "active" : "";
    const char *write = active == WEB_NAV_WRITE ? "active" : "";
    const char *ota = active == WEB_NAV_OTA ? "active" : "";
    snprintf(out, out_sz, WEB_UI_NAV_TEMPLATE, dashboard, logs, diagnostics, mqtt, write, ota);
}

static inline int web_ui_render_page(char *out,
                                     size_t out_sz,
                                     const char *title,
                                     const char *page_styles,
                                     const char *nav_html,
                                     const char *body_html)
{
    return snprintf(out,
                    out_sz,
                    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>%s</title><style>%s%s</style></head><body>%s%s</body></html>",
                    title,
                    WEB_UI_COMMON_STYLES,
                    page_styles ? page_styles : "",
                    nav_html ? nav_html : "",
                    body_html ? body_html : "");
}

// Allocate a page buffer sized to the provided parts. Returns malloc'd buffer
// (caller must free) or NULL on size/capacity errors. A cap lets us avoid
// unbounded allocations if a caller passes unexpected input sizes.
static inline char *web_ui_alloc_page(size_t cap_max,
                                      size_t *out_len,
                                      const char *title,
                                      const char *page_styles,
                                      const char *nav_html,
                                      const char *body_html)
{
    if (cap_max == 0) {
        cap_max = WEB_UI_PAGE_DEFAULT_CAP;
    }

    const size_t len_title = title ? strlen(title) : 0;
    const size_t len_common = sizeof(WEB_UI_COMMON_STYLES) - 1;
    const size_t len_styles = page_styles ? strlen(page_styles) : 0;
    const size_t len_nav = nav_html ? strlen(nav_html) : 0;
    const size_t len_body = body_html ? strlen(body_html) : 0;

    // Constant HTML wrappers (without placeholders)
    static const char prefix1[] = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>";
    static const char prefix2[] = "</title><style>";
    static const char prefix3[] = "</style></head><body>";
    static const char suffix[] = "</body></html>";

    size_t total = strlen(prefix1) + len_title +
                   strlen(prefix2) + len_common + len_styles +
                   strlen(prefix3) + len_nav + len_body +
                   strlen(suffix);

    if (total + 1 > cap_max) {
        return NULL;
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        return NULL;
    }

    char *p = buf;
    memcpy(p, prefix1, strlen(prefix1)); p += strlen(prefix1);
    if (len_title) { memcpy(p, title, len_title); p += len_title; }
    memcpy(p, prefix2, strlen(prefix2)); p += strlen(prefix2);
    memcpy(p, WEB_UI_COMMON_STYLES, len_common); p += len_common;
    if (len_styles) { memcpy(p, page_styles, len_styles); p += len_styles; }
    memcpy(p, prefix3, strlen(prefix3)); p += strlen(prefix3);
    if (len_nav) { memcpy(p, nav_html, len_nav); p += len_nav; }
    if (len_body) { memcpy(p, body_html, len_body); p += len_body; }
    memcpy(p, suffix, strlen(suffix)); p += strlen(suffix);
    *p = '\0';

    if (out_len) {
        *out_len = total;
    }
    return buf;
}

