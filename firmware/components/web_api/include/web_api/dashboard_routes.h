/**
 * @file dashboard_routes.h
 * @brief T041/T042/T043/T044/T045: live web dashboard - WebSocket telemetry
 *        broadcast, a control-command POST endpoint, and the dashboard page
 *        itself (chart + controls + Emergency Stop + alarm banner), served
 *        entirely inline (no external CDN/JS libraries, per FR-021 - the
 *        device must work fully offline/on its own AP), same convention as
 *        wifi_setup_routes.c.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Shared Material-Design-inspired dark theme (no external CDN - the device
 * must work fully offline/on its own AP per FR-021) reused by every
 * "connected" web page (dashboard, history, presets, wifi) so they look
 * consistent without duplicating the whole stylesheet in every routes
 * file. Layout mirrors the on-device display's nav_shell.c: a fixed left
 * sidebar (icon + label nav items, active-item accent highlight) with the
 * page content filling the rest of the viewport - collapses to a
 * horizontal top bar on narrow/phone screens via a media query. Every page
 * wraps its markup as `<div class='app'><nav class='sidebar'>...</nav>
 * <main class='content'>...page content...</main></div>` (see
 * web_ui_send_nav_bar() below for the sidebar itself). */
#define WEB_UI_STYLE \
    "<style>" \
    ":root{--bg:#121212;--surface:#1e1e1e;--sidebar:#181818;--primary:#FF9746;--secondary:#616161;" \
    "--danger:#B3261E;--on-surface:#e0e0e0;--muted:#9e9e9e;--fan:#66BB6A;}" \
    "*{box-sizing:border-box;}" \
    "html,body{height:100%;margin:0;}" \
    "body{background:var(--bg);color:var(--on-surface);font-family:Roboto,'Segoe UI',Arial,sans-serif;}" \
    ".app{display:flex;min-height:100vh;}" \
    ".sidebar{width:220px;flex-shrink:0;background:var(--sidebar);padding:20px 0;" \
    "box-shadow:2px 0 8px rgba(0,0,0,.4);}" \
    ".sidebar .brand{color:var(--primary);font-size:18px;font-weight:700;padding:0 20px 20px;" \
    "letter-spacing:.5px;}" \
    ".sidebar a{display:flex;align-items:center;gap:14px;padding:14px 20px;color:var(--muted);" \
    "text-decoration:none;font-size:14px;border-left:3px solid transparent;}" \
    ".sidebar a .navicon{font-size:18px;width:22px;text-align:center;}" \
    ".sidebar a.active{color:var(--primary);background:rgba(255,151,70,.08);" \
    "border-left:3px solid var(--primary);font-weight:500;}" \
    ".content{flex:1;padding:24px;min-width:0;}" \
    "@media (max-width:700px){.app{flex-direction:column;}" \
    ".sidebar{width:100%;padding:8px 0;display:flex;overflow-x:auto;box-shadow:0 2px 8px rgba(0,0,0,.4);}" \
    ".sidebar .brand{display:none;}" \
    ".sidebar a{flex-direction:column;gap:2px;padding:8px 16px;border-left:none;" \
    "border-bottom:3px solid transparent;white-space:nowrap;}" \
    ".sidebar a.active{border-left:none;border-bottom:3px solid var(--primary);}" \
    ".content{padding:16px;max-width:100%;}}" \
    ".card{background:var(--surface);border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.6);" \
    "padding:16px;margin:0 0 16px;}" \
    "h1{font-size:22px;font-weight:500;margin:0 0 4px;}" \
    ".sub{color:var(--muted);font-size:13px;font-weight:400;margin:0 0 8px;}" \
    "#alarmBanner{display:none;background:var(--danger);color:#fff;border-radius:8px;padding:12px 16px;" \
    "margin:0 0 16px;align-items:center;justify-content:space-between;gap:12px;}" \
    "canvas{width:100%;height:180px;background:#0d0d0d;border-radius:6px;display:block;}" \
    ".legend{color:var(--muted);font-size:12px;margin-top:6px;}" \
    ".legend .bt{color:var(--primary);} .legend .fan{color:var(--fan);}" \
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:12px;}" \
    ".stat{background:#2a2a2a;border-radius:6px;padding:8px 12px;}" \
    ".stat .label{color:var(--muted);font-size:12px;} .stat .value{font-size:16px;font-weight:500;}" \
    ".btnrow{display:flex;gap:8px;margin-top:16px;flex-wrap:wrap;}" \
    "button,.btnlink{border:none;border-radius:6px;padding:10px 16px;font-size:14px;font-weight:500;" \
    "color:#fff;background:var(--secondary);cursor:pointer;text-decoration:none;display:inline-block;}" \
    "button:active,.btnlink:active{opacity:.85;}" \
    ".primary{background:var(--primary);} .danger{background:var(--danger);}" \
    "#estopBtn{background:var(--danger);font-weight:700;letter-spacing:.5px;}" \
    "input[type=range]{width:100%;}" \
    ".sliderrow{margin-top:12px;} .sliderrow .label{color:var(--muted);font-size:13px;" \
    "display:flex;justify-content:space-between;}" \
    ".row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;" \
    "border-bottom:1px solid #2a2a2a;gap:8px;}" \
    ".row:last-child{border-bottom:none;}" \
    ".row .name{flex:1;} .row .selected{color:var(--primary);}" \
    "h2{font-size:15px;font-weight:500;margin:20px 0 4px;color:var(--primary);}" \
    "table{width:100%;border-collapse:collapse;font-size:13px;margin-top:4px;}" \
    "table td,table th{text-align:left;padding:6px 8px;border-bottom:1px solid #2a2a2a;" \
    "overflow-wrap:anywhere;}" \
    "table th{color:var(--muted);font-weight:500;}" \
    "table tr:last-child td{border-bottom:none;}" \
    "</style>"

/** Sends the shared LEFT SIDEBAR navigation (Dashboard/Roast History/
 * Presets/Wi-Fi Setup - collapses to a horizontal top bar on narrow
 * screens via WEB_UI_STYLE's media query), mirroring the on-device
 * display's nav_shell.c sidebar, as a chunked HTTP response fragment.
 * `active_page` should be one of "dashboard"/"history"/"presets"/"wifi" so
 * that page's item gets the accent-highlighted "active" style. Callers
 * must wrap it as `<div class='app'>` + web_ui_send_nav_bar() +
 * `<main class='content'>`...page content...`</main></div>`. */
void web_ui_send_nav_bar(httpd_req_t *req, const char *active_page);

/**
 * Disables Nagle's algorithm (sets TCP_NODELAY) on the request's underlying
 * socket. Every multi-chunk page in web_api (dashboard/history/presets/
 * wifi status) sends its HTML as many small httpd_resp_send_chunk() pieces
 * (nav bar links, stat cards, etc.) - without this, Nagle's algorithm
 * interacting with the browser's delayed ACKs can add up to ~200ms of
 * stall PER CHUNK, compounding into several real seconds of perceived
 * "the page is slow to load" for a page with 10-20+ chunks. Call once at
 * the top of any handler that will send multiple chunks. Safe to call even
 * if it fails (logged at debug level only, never fails the request).
 */
void web_ui_enable_low_latency(httpd_req_t *req);

/** Registers "/ws/telemetry" (WebSocket) and "/api/control" (POST), and starts the periodic broadcast timer. Call once from web_api_server_init(). */
esp_err_t dashboard_routes_register(httpd_handle_t server);

/** Sends the full dashboard HTML page (chart + controls) - called by wifi_setup_routes.c's "/" GET handler once WiFi is connected (STA mode), replacing the old simple status card. */
void dashboard_routes_send_page(httpd_req_t *req);

