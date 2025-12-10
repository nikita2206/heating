#pragma once

#include <string_view>
#include <array>

namespace web_ui {

// ============================================================================
// HTML Minification
// ============================================================================

constexpr bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr size_t html_minified_size(const char* str, size_t len) {
    size_t result = 0;
    bool in_space = false;
    bool prev_was_gt = false;

    for (size_t i = 0; i < len; ++i) {
        char c = str[i];
        bool next_is_lt = (i + 1 < len && str[i + 1] == '<');

        if (is_whitespace(c)) {
            if (prev_was_gt || next_is_lt) {
                continue;
            }
            if (!in_space) {
                result++;
                in_space = true;
            }
        } else {
            result++;
            in_space = false;
            prev_was_gt = (c == '>');
        }
    }

    return result;
}

template<size_t OutputSize>
constexpr auto html_minify_to_array(const char* str, size_t len) {
    std::array<char, OutputSize + 1> result{};

    size_t out_idx = 0;
    bool in_space = false;
    bool prev_was_gt = false;

    for (size_t i = 0; i < len; ++i) {
        char c = str[i];
        bool next_is_lt = (i + 1 < len && str[i + 1] == '<');

        if (is_whitespace(c)) {
            if (prev_was_gt || next_is_lt) {
                continue;
            }
            if (!in_space) {
                result[out_idx++] = ' ';
                in_space = true;
            }
        } else {
            result[out_idx++] = c;
            in_space = false;
            prev_was_gt = (c == '>');
        }
    }

    result[OutputSize] = '\0';
    return result;
}

// ============================================================================
// CSS Minification
// ============================================================================

constexpr bool is_css_punctuation(char c) {
    return c == '{' || c == '}' || c == ':' || c == ';' || c == ',';
}

constexpr size_t css_minified_size(const char* str, size_t len) {
    size_t result = 0;
    bool in_space = false;

    for (size_t i = 0; i < len; ++i) {
        char c = str[i];

        if (is_whitespace(c)) {
            // Check if prev or next char is punctuation - skip space entirely
            bool prev_is_punct = (i > 0 && is_css_punctuation(str[i - 1]));
            bool next_is_punct = (i + 1 < len && is_css_punctuation(str[i + 1]));
            // Also skip leading whitespace
            if (prev_is_punct || next_is_punct || result == 0) {
                continue;
            }
            // Skip whitespace before next whitespace (collapse)
            if (i + 1 < len && is_whitespace(str[i + 1])) {
                continue;
            }
            if (!in_space) {
                result++;
                in_space = true;
            }
        } else {
            result++;
            in_space = false;
        }
    }

    return result;
}

template<size_t OutputSize>
constexpr auto css_minify_to_array(const char* str, size_t len) {
    std::array<char, OutputSize + 1> result{};

    size_t out_idx = 0;
    bool in_space = false;

    for (size_t i = 0; i < len; ++i) {
        char c = str[i];

        if (is_whitespace(c)) {
            bool prev_is_punct = (i > 0 && is_css_punctuation(str[i - 1]));
            bool next_is_punct = (i + 1 < len && is_css_punctuation(str[i + 1]));
            if (prev_is_punct || next_is_punct || out_idx == 0) {
                continue;
            }
            if (i + 1 < len && is_whitespace(str[i + 1])) {
                continue;
            }
            if (!in_space) {
                result[out_idx++] = ' ';
                in_space = true;
            }
        } else {
            result[out_idx++] = c;
            in_space = false;
        }
    }

    result[OutputSize] = '\0';
    return result;
}

// ============================================================================
// Macros
// ============================================================================

#define MINIFY_HTML(expr) ([]() { \
    constexpr auto& _src = expr; \
    constexpr size_t _len = sizeof(_src) - 1; \
    constexpr size_t _out_size = web_ui::html_minified_size(_src, _len); \
    static constexpr auto _min = web_ui::html_minify_to_array<_out_size>(_src, _len); \
    return _min.data(); \
}())

#define MINIFY_CSS(expr) ([]() { \
    constexpr auto& _src = expr; \
    constexpr size_t _len = sizeof(_src) - 1; \
    constexpr size_t _out_size = web_ui::css_minified_size(_src, _len); \
    static constexpr auto _min = web_ui::css_minify_to_array<_out_size>(_src, _len); \
    return _min.data(); \
}())

} // namespace web_ui

