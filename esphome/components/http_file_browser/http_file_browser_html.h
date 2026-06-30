#pragma once

static constexpr const char HTTP_FILE_BROWSER_HTML_PRE_TITLE[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"UTF-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\">\n"
    "  <title>";

static constexpr const char HTTP_FILE_BROWSER_HTML_PRE_CSS[] =
    "</title>\n"
    "  <style>\n";

static constexpr const char HTTP_FILE_BROWSER_HTML_POST_CSS[] =
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "<div class=\"container\">\n";

static constexpr const char HTTP_FILE_BROWSER_HTML_PRE_MODAL[] =
    "</div>\n"
    "<div id=\"progressModal\" class=\"progress-modal\">\n"
    "  <div class=\"progress-content\">\n"
    "    <div class=\"progress-title\" id=\"progressTitle\">Processing...</div>\n"
    "    <div class=\"progress-bar-container\">\n"
    "      <div class=\"progress-bar\" id=\"progressBar\">0%</div>\n"
    "    </div>\n"
    "    <div class=\"progress-details\" id=\"progressDetails\">Initializing...</div>\n"
    "    <div class=\"progress-speed\" id=\"progressSpeed\"></div>\n"
    "    <div class=\"progress-file-info\">\n"
    "      <div><strong>From:</strong> <span id=\"progressSource\">-</span></div>\n"
    "      <div><strong>To:</strong> <span id=\"progressDest\">-</span></div>\n"
    "    </div>\n"
    "    <button id=\"cancelBtn\" class=\"delete\" onclick=\"cancelOperation()\">Cancel</button>\n"
    "  </div>\n"
    "</div>\n"
    "<script>\n"
    "const API_BASE = '";

// After API_BASE value is inserted, the JS content follows, then this closing fragment
static constexpr const char HTTP_FILE_BROWSER_HTML_PRE_JS[] = "';\n";

static constexpr const char HTTP_FILE_BROWSER_HTML_POST_JS[] =
    "</script>\n"
    "</body>\n"
    "</html>\n";
