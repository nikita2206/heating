# Web UI Component

Shared web UI framework for the OpenTherm Gateway. Provides common styles, navigation, page rendering, and page content.


## API

### Page Rendering

```c
#include "web_ui.h"
#include "web_ui_pages.h"

// Build navigation with active page highlighted
char nav[256];
web_ui_build_nav(nav, sizeof(nav), WEB_NAV_DASHBOARD);

// Render complete HTML page
char page[8192];
web_ui_render_page(page, sizeof(page),
    "Page Title",
    WEB_UI_DASHBOARD_STYLES,  // page-specific CSS
    nav,
    WEB_UI_DASHBOARD_BODY);   // page body HTML
```

### Available Pages

- `WEB_UI_DASHBOARD_*` - Home/dashboard
- `WEB_UI_LOGS_*` - Live OpenTherm logs
- `WEB_UI_DIAGNOSTICS_*` - Boiler diagnostics
- `WEB_UI_MQTT_*` - MQTT configuration
- `WEB_UI_WRITE_*` - Manual write frames
- `WEB_UI_OTA_*` - Firmware updates

Each page has `_STYLES` and `_BODY` variants.

## Adding Pages

1. Add content to `web_ui_pages.cpp`:
```cpp
constexpr auto mypage_styles = R"(
    .my-class { color: red; }
)";

constexpr auto mypage_body = R"(
    <div class='container'>...</div>
)";
```

2. Export in `extern "C"` block:
```cpp
const char* WEB_UI_MYPAGE_STYLES = MINIFY_CSS(web_ui::mypage_styles);
const char* WEB_UI_MYPAGE_BODY = MINIFY_HTML(web_ui::mypage_body);
```

3. Declare in `web_ui_pages.h`

## Minification

Content is minified at compile time:
- `MINIFY_HTML` - removes whitespace around `<>` tags
- `MINIFY_CSS` - removes whitespace around `{}:;,`
