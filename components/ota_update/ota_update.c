/*
 * OTA Update Support for OpenTherm Gateway
 */

#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"
#include "esp_system.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "OTA";

// OTA state
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;
static bool ota_in_progress = false;
static size_t ota_bytes_written = 0;

// Common CSS (must match websocket_server.c)
static const char *common_styles = 
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

// OTA page HTML
static const char *ota_page = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OTA Update - OpenTherm Gateway</title><style>%s"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(350px,1fr));gap:24px}"
    ".info-grid{display:grid;grid-template-columns:140px 1fr;gap:8px 16px;font-size:14px}"
    ".info-label{color:var(--muted)}"
    ".info-value{font-weight:500}"
    ".upload-zone{border:2px dashed var(--border);border-radius:12px;padding:48px 24px;text-align:center;transition:all 0.3s;cursor:pointer}"
    ".upload-zone:hover,.upload-zone.dragover{border-color:var(--accent);background:rgba(0,212,170,0.05)}"
    ".upload-zone.uploading{border-color:var(--accent2);background:rgba(124,58,237,0.05)}"
    ".upload-icon{font-size:48px;margin-bottom:16px;opacity:0.5}"
    ".upload-text{color:var(--muted);margin-bottom:8px}"
    ".upload-hint{font-size:12px;color:var(--muted)}"
    "#file-input{display:none}"
    ".progress-container{margin-top:24px;display:none}"
    ".progress-bar{height:8px;background:var(--border);border-radius:4px;overflow:hidden}"
    ".progress-fill{height:100%%;background:linear-gradient(90deg,var(--accent),var(--accent2));width:0%%;transition:width 0.3s}"
    ".progress-text{margin-top:8px;font-size:13px;color:var(--muted);text-align:center}"
    ".status-msg{margin-top:16px;padding:12px 16px;border-radius:8px;font-size:14px;display:none}"
    ".status-msg.success{display:block;background:rgba(0,212,170,0.15);color:var(--accent);border:1px solid rgba(0,212,170,0.3)}"
    ".status-msg.error{display:block;background:rgba(239,68,68,0.15);color:#ef4444;border:1px solid rgba(239,68,68,0.3)}"
    ".actions{display:flex;gap:12px;margin-top:24px;flex-wrap:wrap}"
    ".partition-visual{display:flex;gap:8px;margin-top:16px}"
    ".partition-box{flex:1;padding:12px;border-radius:8px;text-align:center;font-size:12px}"
    ".partition-box.active{background:rgba(0,212,170,0.15);border:1px solid var(--accent)}"
    ".partition-box.inactive{background:var(--bg);border:1px solid var(--border)}"
    ".partition-name{font-weight:600;margin-bottom:4px}"
    ".partition-addr{color:var(--muted);font-size:11px}"
    ".warning{background:rgba(250,204,21,0.1);border:1px solid rgba(250,204,21,0.3);border-radius:8px;padding:12px 16px;margin-bottom:24px;font-size:13px;color:#facc15}"
    "</style></head><body>"
    "<nav><a href='/' class='logo'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5'/></svg>OT Gateway</a>"
    "<div class='nav-links'><a href='/'>Dashboard</a><a href='/logs'>Logs</a><a href='/ota' class='active'>OTA Update</a></div></nav>"
    "<div class='container'><h1>Firmware Update</h1><p class='subtitle'>Over-the-air firmware management</p>"
    "<div id='pending-warning' class='warning' style='display:none'>⚠️ Firmware is pending verification. If you don't confirm it, the device will rollback on next reboot.</div>"
    "<div class='grid'>"
    // Current firmware card
    "<div class='card'><div class='card-title'>📦 Current Firmware</div>"
    "<div class='info-grid'>"
    "<span class='info-label'>Version</span><span class='info-value' id='version'>--</span>"
    "<span class='info-label'>Project</span><span class='info-value' id='project'>--</span>"
    "<span class='info-label'>Compiled</span><span class='info-value' id='compile-time'>--</span>"
    "<span class='info-label'>IDF Version</span><span class='info-value' id='idf-ver'>--</span>"
    "<span class='info-label'>State</span><span class='info-value'><span class='badge green' id='state'>--</span></span>"
    "</div>"
    "<div class='partition-visual'>"
    "<div class='partition-box' id='ota0-box'><div class='partition-name'>ota_0</div><div class='partition-addr' id='ota0-addr'>--</div></div>"
    "<div class='partition-box' id='ota1-box'><div class='partition-name'>ota_1</div><div class='partition-addr' id='ota1-addr'>--</div></div>"
    "</div>"
    "<div class='actions'>"
    "<button class='btn btn-secondary' onclick='confirmFirmware()' id='confirm-btn' style='display:none'>✓ Confirm Firmware</button>"
    "<button class='btn btn-danger' onclick='rollback()' id='rollback-btn'>↩ Rollback</button>"
    "</div></div>"
    // Upload card
    "<div class='card'><div class='card-title'>⬆️ Upload New Firmware</div>"
    "<div class='upload-zone' id='upload-zone' onclick='document.getElementById(\"file-input\").click()'>"
    "<div class='upload-icon'>📁</div>"
    "<div class='upload-text'>Drop firmware.bin here or click to browse</div>"
    "<div class='upload-hint'>Max size: ~1.7 MB (partition size)</div>"
    "</div>"
    "<input type='file' id='file-input' accept='.bin' onchange='handleFile(this.files[0])'>"
    "<div class='progress-container' id='progress-container'>"
    "<div class='progress-bar'><div class='progress-fill' id='progress-fill'></div></div>"
    "<div class='progress-text' id='progress-text'>Uploading... 0%%</div></div>"
    "<div class='status-msg' id='status-msg'></div>"
    "</div></div></div>"
    "<script>"
    "let zone=document.getElementById('upload-zone'),uploading=false,statusInterval=null;"
    "zone.addEventListener('dragover',e=>{e.preventDefault();zone.classList.add('dragover')});"
    "zone.addEventListener('dragleave',()=>zone.classList.remove('dragover'));"
    "zone.addEventListener('drop',e=>{e.preventDefault();zone.classList.remove('dragover');if(e.dataTransfer.files.length)handleFile(e.dataTransfer.files[0])});"
    "function handleFile(file){if(!file||!file.name.endsWith('.bin')){showStatus('Please select a .bin file','error');return;}"
    "if(file.size>1800000){showStatus('File too large for partition','error');return;}"
    "uploadFirmware(file);}"
    "function uploadFirmware(file){if(uploading)return;uploading=true;"
    "if(statusInterval){clearInterval(statusInterval);statusInterval=null;}"
    "let xhr=new XMLHttpRequest();let progress=document.getElementById('progress-container');"
    "let fill=document.getElementById('progress-fill');let text=document.getElementById('progress-text');"
    "progress.style.display='block';zone.classList.add('uploading');hideStatus();"
    "xhr.upload.onprogress=e=>{if(e.lengthComputable){let pct=Math.round(e.loaded/e.total*100);fill.style.width=pct+'%%';text.textContent='Uploading... '+pct+'%%';}};"
    "xhr.onload=()=>{zone.classList.remove('uploading');uploading=false;"
    "if(xhr.status===200){let r=JSON.parse(xhr.responseText);showStatus('✓ '+r.message,'success');text.textContent='Complete! Restarting...';setTimeout(()=>location.reload(),5000);}"
    "else{showStatus('✗ Upload failed: '+xhr.responseText,'error');progress.style.display='none';startStatusPolling();}};"
    "xhr.onerror=()=>{zone.classList.remove('uploading');uploading=false;showStatus('✗ Network error','error');progress.style.display='none';startStatusPolling();};"
    "xhr.open('POST','/ota');xhr.timeout=120000;xhr.send(file);}"
    "function showStatus(msg,type){let el=document.getElementById('status-msg');el.textContent=msg;el.className='status-msg '+type;}"
    "function hideStatus(){document.getElementById('status-msg').className='status-msg';}"
    "function rollback(){if(!confirm('Rollback to previous firmware? Device will restart.'))return;"
    "fetch('/ota/rollback',{method:'POST'}).then(r=>r.json()).then(d=>{showStatus(d.message,'success');setTimeout(()=>location.reload(),3000)}).catch(e=>showStatus('Rollback failed','error'));}"
    "function confirmFirmware(){fetch('/ota/confirm',{method:'POST'}).then(r=>r.json()).then(d=>{showStatus(d.message,'success');loadStatus()}).catch(e=>showStatus('Confirm failed','error'));}"
    "function loadStatus(){if(uploading)return;fetch('/ota/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('version').textContent=d.version;"
    "document.getElementById('project').textContent=d.project_name;"
    "document.getElementById('compile-time').textContent=d.compile_time;"
    "document.getElementById('idf-ver').textContent=d.idf_ver;"
    "let state=document.getElementById('state');"
    "state.textContent=d.ota_state;state.className='badge '+(d.ota_state==='valid'?'green':d.ota_state==='pending_verify'?'yellow':'purple');"
    "let run=d.running_partition;document.getElementById('ota0-box').className='partition-box '+(run==='ota_0'?'active':'inactive');"
    "document.getElementById('ota1-box').className='partition-box '+(run==='ota_1'?'active':'inactive');"
    "document.getElementById('ota0-addr').textContent=run==='ota_0'?'Running':'Next update';"
    "document.getElementById('ota1-addr').textContent=run==='ota_1'?'Running':'Next update';"
    "if(d.ota_state==='pending_verify'){document.getElementById('pending-warning').style.display='block';document.getElementById('confirm-btn').style.display='inline-flex';}"
    "else{document.getElementById('pending-warning').style.display='none';document.getElementById('confirm-btn').style.display='none';}"
    "}).catch(()=>{})};"
    "function startStatusPolling(){if(!statusInterval)statusInterval=setInterval(loadStatus,10000);}"
    "loadStatus();startStatusPolling();"
    "</script></body></html>";

/**
 * GET /ota - OTA management page
 */
static esp_err_t ota_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    char *page = malloc(12288);  // 12KB should be enough
    if (page) {
        snprintf(page, 12288, ota_page, common_styles);
        httpd_resp_send(req, page, strlen(page));
        free(page);
        return ESP_OK;
    }
    return httpd_resp_send_500(req);
}

/**
 * POST /ota - Upload firmware binary
 * 
 * Expects raw binary data in request body.
 * Response: JSON with status
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    esp_err_t err;
    char buf[4096];  // Larger buffer for efficient flash writes
    int received;
    int remaining = req->content_len;
    bool first_chunk = true;
    
    ESP_LOGI(TAG, "OTA update started, content length: %d", req->content_len);
    
    // Don't allow concurrent OTA updates
    if (ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already in progress");
        return ESP_FAIL;
    }
    
    ota_in_progress = true;
    ota_bytes_written = 0;
    
    // Get the next OTA partition to write to
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        ota_in_progress = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%" PRIx32,
             update_partition->subtype, update_partition->address);
    
    // Receive and write firmware chunks
    while (remaining > 0) {
        int to_read = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        
        received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // Retry on timeout
            }
            ESP_LOGE(TAG, "File receive failed");
            if (ota_handle) {
                esp_ota_abort(ota_handle);
                ota_handle = 0;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            ota_in_progress = false;
            return ESP_FAIL;
        }
        
        // On first chunk, validate the image header and start OTA
        if (first_chunk) {
            first_chunk = false;
            
            // Validate header
            if (received < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                ESP_LOGE(TAG, "First chunk too small for header validation");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
                ota_in_progress = false;
                return ESP_FAIL;
            }
            
            // Check new firmware version
            esp_app_desc_t new_app_info;
            memcpy(&new_app_info, 
                   buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                   sizeof(esp_app_desc_t));
            ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
            
            // Log current version
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_app_desc_t running_app_info;
            if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
            }
            
            // Check if this version was previously marked invalid
            const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
            if (last_invalid != NULL) {
                esp_app_desc_t invalid_app_info;
                if (esp_ota_get_partition_description(last_invalid, &invalid_app_info) == ESP_OK) {
                    if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                        ESP_LOGW(TAG, "Rejecting firmware version %s - previously marked invalid", new_app_info.version);
                        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                            "This firmware version was previously rejected after failed validation");
                        ota_in_progress = false;
                        return ESP_FAIL;
                    }
                }
            }
            
            // Start OTA
            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to begin OTA");
                ota_in_progress = false;
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "OTA begin succeeded");
        }
        
        // Write chunk to flash
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            ota_handle = 0;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write firmware");
            ota_in_progress = false;
            return ESP_FAIL;
        }
        
        ota_bytes_written += received;
        remaining -= received;
        
        // Log progress periodically
        if (ota_bytes_written % (64 * 1024) == 0 || remaining == 0) {
            ESP_LOGI(TAG, "Written %zu bytes, %d remaining", ota_bytes_written, remaining);
        }
    }
    
    // Finalize OTA
    err = esp_ota_end(ota_handle);
    ota_handle = 0;
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image validation failed");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to finalize OTA");
        }
        ota_in_progress = false;
        return ESP_FAIL;
    }
    
    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        ota_in_progress = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OTA update successful! Written %zu bytes. Preparing to restart...", ota_bytes_written);
    
    // Send success response
    httpd_resp_set_type(req, "application/json");
    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"success\",\"message\":\"OTA update complete, restarting...\",\"bytes_written\":%zu}",
             ota_bytes_written);
    httpd_resp_send(req, response, strlen(response));
    
    ota_in_progress = false;
    
    // Restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/**
 * GET /ota/status - Get OTA and firmware status
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next_update = esp_ota_get_next_update_partition(NULL);
    
    esp_app_desc_t app_info;
    esp_ota_get_partition_description(running, &app_info);
    
    esp_ota_img_states_t ota_state;
    esp_ota_get_state_partition(running, &ota_state);
    
    const char *state_str = "unknown";
    switch (ota_state) {
        case ESP_OTA_IMG_NEW: state_str = "new"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: state_str = "pending_verify"; break;
        case ESP_OTA_IMG_VALID: state_str = "valid"; break;
        case ESP_OTA_IMG_INVALID: state_str = "invalid"; break;
        case ESP_OTA_IMG_ABORTED: state_str = "aborted"; break;
        case ESP_OTA_IMG_UNDEFINED: state_str = "undefined"; break;
    }
    
    char response[512];
    snprintf(response, sizeof(response),
             "{"
             "\"version\":\"%s\","
             "\"project_name\":\"%s\","
             "\"compile_time\":\"%s %s\","
             "\"idf_ver\":\"%s\","
             "\"running_partition\":\"%s\","
             "\"running_offset\":\"0x%" PRIx32 "\","
             "\"boot_partition\":\"%s\","
             "\"next_update_partition\":\"%s\","
             "\"ota_state\":\"%s\","
             "\"ota_in_progress\":%s"
             "}",
             app_info.version,
             app_info.project_name,
             app_info.date, app_info.time,
             app_info.idf_ver,
             running->label,
             running->address,
             boot ? boot->label : "none",
             next_update ? next_update->label : "none",
             state_str,
             ota_in_progress ? "true" : "false");
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

/**
 * POST /ota/rollback - Manually trigger rollback to previous firmware
 */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    if (!esp_ota_check_rollback_is_possible()) {
        ESP_LOGW(TAG, "Rollback not possible");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rollback not possible - no valid previous firmware");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Manual rollback requested, restarting...");
    
    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"success\",\"message\":\"Rolling back and restarting...\"}";
    httpd_resp_send(req, response, strlen(response));
    
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_ota_mark_app_invalid_rollback_and_reboot();
    
    return ESP_OK;
}

/**
 * POST /ota/confirm - Confirm current firmware is working (cancels rollback)
 */
static esp_err_t ota_confirm_handler(httpd_req_t *req)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm app: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to confirm firmware");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Firmware confirmed as valid");
    
    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"success\",\"message\":\"Firmware confirmed as valid\"}";
    return httpd_resp_send(req, response, strlen(response));
}

esp_err_t ota_update_register_handlers(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering OTA HTTP handlers");
    
    // GET /ota - OTA management page
    httpd_uri_t ota_page_uri = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_page_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_page_uri);
    
    // POST /ota - Upload firmware
    httpd_uri_t ota_upload = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_upload);
    
    // GET /ota/status - Get status
    httpd_uri_t ota_status = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_status);
    
    // POST /ota/rollback - Manual rollback
    httpd_uri_t ota_rollback = {
        .uri = "/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_rollback);
    
    // POST /ota/confirm - Confirm firmware
    httpd_uri_t ota_confirm = {
        .uri = "/ota/confirm",
        .method = HTTP_POST,
        .handler = ota_confirm_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_confirm);
    
    ESP_LOGI(TAG, "OTA handlers registered: GET/POST /ota, GET /ota/status, POST /ota/rollback, POST /ota/confirm");
    
    return ESP_OK;
}

esp_err_t ota_update_validate_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    ESP_LOGI(TAG, "Running from partition: %s at offset 0x%" PRIx32, 
             running->label, running->address);
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA state: pending verification");
            ESP_LOGI(TAG, "New firmware booted successfully, marking as valid...");
            
            // For this project, we auto-confirm after successful boot
            // In a more critical application, you might want to run diagnostics first
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
                return ESP_FAIL;
            }
            
            ESP_LOGI(TAG, "Firmware marked as valid, rollback cancelled");
        } else {
            ESP_LOGI(TAG, "OTA state: %d (not pending verification)", ota_state);
        }
    }
    
    return ESP_OK;
}

const char *ota_update_get_version(void)
{
    static esp_app_desc_t app_info;
    static bool initialized = false;
    
    if (!initialized) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_get_partition_description(running, &app_info);
        initialized = true;
    }
    
    return app_info.version;
}

