/*
 * Web UI - Embedded gzipped SPA file accessors
 *
 * All files are pre-compressed with gzip and should be served
 * with Content-Encoding: gzip header.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the gzipped index.html content
 * @param len Output: length of the data in bytes
 * @return Pointer to gzipped data (embedded in flash)
 */
const uint8_t* web_ui_get_index_html_gz(size_t *len);

/**
 * Get the gzipped index.js bundle
 * @param len Output: length of the data in bytes
 * @return Pointer to gzipped data (embedded in flash)
 */
const uint8_t* web_ui_get_index_js_gz(size_t *len);

/**
 * Get the gzipped index.css bundle
 * @param len Output: length of the data in bytes
 * @return Pointer to gzipped data (embedded in flash)
 */
const uint8_t* web_ui_get_index_css_gz(size_t *len);

#ifdef __cplusplus
}
#endif
