# Web UI component - embeds pre-built gzipped SPA files
#
# IMPORTANT: Run `npm run build` in web-ui/ directory before building ESP-IDF project.
# Or use: idf.py build-web && idf.py build

set(WEB_UI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../web-ui")
set(WEB_UI_DIST "${WEB_UI_DIR}/dist")

# Verify the built files exist
set(WEB_UI_HTML "${WEB_UI_DIST}/index.html.gz")
set(WEB_UI_JS "${WEB_UI_DIST}/assets/index.js.gz")
set(WEB_UI_CSS "${WEB_UI_DIST}/assets/index.css.gz")

if(NOT EXISTS "${WEB_UI_HTML}" OR NOT EXISTS "${WEB_UI_JS}" OR NOT EXISTS "${WEB_UI_CSS}")
    message(FATAL_ERROR
        "Web UI build output not found!\n"
        "Please build the web UI first:\n"
        "  cd ${WEB_UI_DIR}\n"
        "  npm install\n"
        "  npm run build\n"
        "Then run idf.py build again."
    )
endif()

idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "${WEB_UI_HTML}"
        "${WEB_UI_JS}"
        "${WEB_UI_CSS}"
)
