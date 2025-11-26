#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "wifi_manager.h"
#include "book_index.h"
#include "gui.h"

static const char *TAG = "WEB";
extern WifiManager wifiManager;
extern BookIndex bookIndex;
extern GUI gui;

#define MIN(a,b) ((a) < (b) ? (a) : (b))


#define MIN(a,b) ((a) < (b) ? (a) : (b))

// --- HTML Content ---

static const char* CAPTIVE_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>M5Paper Setup</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; padding: 20px; background: #f2f2f7; color: #333; }
.card { background: white; padding: 20px; border-radius: 12px; margin-bottom: 20px; box-shadow: 0 2px 8px rgba(0,0,0,0.05); }
h2 { margin-top: 0; color: #1c1c1e; }
button { background: #007aff; color: white; border: none; padding: 12px 20px; border-radius: 8px; font-size: 16px; font-weight: 600; cursor: pointer; width: 100%; }
input { padding: 12px; border: 1px solid #d1d1d6; border-radius: 8px; width: 100%; box-sizing: border-box; margin-bottom: 15px; font-size: 16px; }
</style>
</head>
<body>
<div class="card">
  <h2>WiFi Setup</h2>
  <p>Connect M5Paper to your local network.</p>
  <input id="ssid" placeholder="WiFi Name (SSID)">
  <input id="pass" type="password" placeholder="Password">
  <button onclick="saveWifi()">Connect & Restart</button>
</div>
<script>
function saveWifi() {
    const ssid = document.getElementById('ssid').value;
    const pass = document.getElementById('pass').value;
    if(!ssid) return alert('SSID required');
    
    fetch('/wifi', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
    }).then(r => {
        alert('Credentials saved. Device restarting...');
    });
}
</script>
</body>
</html>
)rawliteral";

static const char* MANAGER_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>M5Paper Library</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; padding: 20px; background: #f2f2f7; color: #333; }
.card { background: white; padding: 20px; border-radius: 12px; margin-bottom: 20px; box-shadow: 0 2px 8px rgba(0,0,0,0.05); }
h2 { margin-top: 0; color: #1c1c1e; }
ul { list-style: none; padding: 0; }
li { padding: 12px 0; border-bottom: 1px solid #eee; display: flex; justify-content: space-between; align-items: center; }
li:last-child { border-bottom: none; }
button { background: #007aff; color: white; border: none; padding: 10px 20px; border-radius: 8px; font-size: 16px; font-weight: 600; cursor: pointer; }
button:disabled { opacity: 0.5; }
button.del { background: #ff3b30; padding: 6px 12px; font-size: 14px; }
input { padding: 12px; border: 1px solid #d1d1d6; border-radius: 8px; width: 100%; box-sizing: border-box; margin-bottom: 15px; font-size: 16px; }
.progress { height: 4px; background: #eee; margin-top: 10px; border-radius: 2px; overflow: hidden; display: none; }
.bar { height: 100%; background: #34c759; width: 0%; transition: width 0.2s; }
</style>
</head>
<body>
<div class="card">
  <h2>Settings</h2>
  <div style="margin-bottom: 15px;">
    <label>Font Size: <span id="sizeVal"></span></label>
    <div style="display: flex; gap: 10px; margin-top: 5px;">
        <button onclick="changeSize(-0.1)">-</button>
        <input type="number" id="customSize" step="0.1" style="width: 80px; margin:0;" onchange="setCustomSize()">
        <button onclick="changeSize(0.1)">+</button>
    </div>
  </div>
  <div>
    <label>Font Family:</label>
    <select id="fontSel" onchange="changeFont()" style="width: 100%; padding: 10px; margin-top: 5px; border-radius: 8px; border: 1px solid #d1d1d6;">
        <option value="Default">Default</option>
        <option value="Hebrew">Hebrew</option>
        <option value="Roboto">Roboto</option>
    </select>
  </div>
</div>

<div class="card">
  <h2>Library</h2>
  <ul id="list">Loading...</ul>
  <div style="margin-top: 20px;">
    <input type="file" id="upfile" accept=".epub">
    <button onclick="upload()" id="upbtn">Upload Book</button>
    <div class="progress" id="progress"><div class="bar" id="bar"></div></div>
    <p id="status"></p>
  </div>
</div>
<script>
let currentSize = 1.0;

function fetchSettings() {
    fetch('/api/settings').then(r => r.json()).then(s => {
        currentSize = s.fontSize;
        document.getElementById('sizeVal').innerText = currentSize.toFixed(1);
        document.getElementById('customSize').value = currentSize.toFixed(1);
        document.getElementById('fontSel').value = s.font;
    });
}

function changeSize(delta) {
    currentSize += delta;
    if(currentSize < 0.5) currentSize = 0.5;
    if(currentSize > 5.0) currentSize = 5.0;
    updateSettings();
}

function setCustomSize() {
    const val = parseFloat(document.getElementById('customSize').value);
    if(!isNaN(val)) {
        currentSize = val;
        if(currentSize < 0.5) currentSize = 0.5;
        if(currentSize > 5.0) currentSize = 5.0;
        updateSettings();
    }
}

function changeFont() {
    updateSettings();
}

function updateSettings() {
    const font = document.getElementById('fontSel').value;
    document.getElementById('sizeVal').innerText = currentSize.toFixed(1);
    document.getElementById('customSize').value = currentSize.toFixed(1);
    
    fetch('/api/settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({fontSize: currentSize, font: font})
    });
}

function fetchList() {
    fetch('/api/list').then(r => r.json()).then(files => {
        const list = document.getElementById('list');
        list.innerHTML = '';
        if(files.length === 0) list.innerHTML = '<li>No books found</li>';
        files.forEach(f => {
            const li = document.createElement('li');
            li.innerHTML = `<span>${f.name}</span><button class="del" onclick="del(${f.id})">Delete</button>`;
            list.appendChild(li);
        });
    });
}

function del(id) {
    if(!confirm('Delete book?')) return;
    fetch('/api/delete?id=' + id, {method: 'POST'})
        .then(() => fetchList())
        .catch(e => alert('Error deleting'));
}

function upload() {
    const fileInput = document.getElementById('upfile');
    const file = fileInput.files[0];
    if(!file) return alert('Please select a file');
    
    const btn = document.getElementById('upbtn');
    const status = document.getElementById('status');
    const progress = document.getElementById('progress');
    const bar = document.getElementById('bar');
    
    btn.disabled = true;
    status.innerText = 'Uploading...';
    progress.style.display = 'block';
    
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload/' + encodeURIComponent(file.name), true);
    
    xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
            const percent = (e.loaded / e.total) * 100;
            bar.style.width = percent + '%';
        }
    };
    
    xhr.onload = () => {
        btn.disabled = false;
        progress.style.display = 'none';
        if (xhr.status === 200) {
            status.innerText = 'Upload complete!';
            fileInput.value = '';
            fetchList();
        } else {
            status.innerText = 'Upload failed: ' + xhr.statusText;
        }
    };
    
    xhr.onerror = () => {
        btn.disabled = false;
        status.innerText = 'Network error';
    };
    
    xhr.send(file);
}
fetchList();
fetchSettings();
</script>
</body>
</html>
)rawliteral";

// --- Helper Functions ---

static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

static void sanitize_filename(char* name) {
    // Truncate to 28 chars to be safe (SPIFFS limit is 32 bytes including null)
    if (strlen(name) > 28) {
        // Keep extension if possible
        char* ext = strrchr(name, '.');
        if (ext) {
            int ext_len = strlen(ext);
            if (ext_len < 28) {
                int base_len = 28 - ext_len;
                memmove(name + base_len, ext, ext_len + 1);
            } else {
                name[28] = 0;
            }
        } else {
            name[28] = 0;
        }
    }
    // Replace invalid chars and spaces
    for(int i=0; name[i]; i++) {
        if(name[i] == '/' || name[i] == '\\' || name[i] == ':' || name[i] == '"' || name[i] == ' ' || name[i] < 32 || name[i] > 126) {
            name[i] = '_';
        }
    }
}

/* Handler for upload */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filename[256];
    const char* uri = req->uri;
    const char* filename_start = strstr(uri, "/upload/");
    if (!filename_start) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    filename_start += 8; // Skip "/upload/"
    
    // Decode URL to get the real title
    url_decode(filename, filename_start);
    
    // Add to index and get the safe filesystem path
    std::string safePath = bookIndex.addBook(filename);
    ESP_LOGI(TAG, "Uploading '%s' to '%s'", filename, safePath.c_str());
    
    // Check if file exists and delete it first to ensure clean write
    struct stat st;
    if (stat(safePath.c_str(), &st) == 0) {
        unlink(safePath.c_str());
    }

    FILE *f = fopen(safePath.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s (errno: %d)", safePath.c_str(), errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    char *buf = (char*)malloc(4096);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received;
    int remaining = req->content_len;

    while (remaining > 0) {
        if ((received = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            fclose(f);
            unlink(safePath.c_str());
            ESP_LOGE(TAG, "File upload failed during receive");
            return ESP_FAIL;
        }
        size_t written = fwrite(buf, 1, received, f);
        if (written != received) {
            free(buf);
            fclose(f);
            ESP_LOGE(TAG, "File write failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= received;
    }
    free(buf);
    fclose(f);
    
    gui.refreshLibrary();
    
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* API to list files as JSON */
static esp_err_t api_list_handler(httpd_req_t *req)
{
    // Reload index to ensure we have latest
    bookIndex.init(); 
    auto books = bookIndex.getBooks();
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    
    bool first = true;
    for (const auto& book : books) {
        if (!first) httpd_resp_send_chunk(req, ",", 1);
        char buf[512];
        // Escape quotes in title if needed (simplified here)
        snprintf(buf, sizeof(buf), "{\"name\":\"%s\", \"id\":%d}", book.title.c_str(), book.id);
        httpd_resp_send_chunk(req, buf, strlen(buf));
        first = false;
    }
    
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* API to delete file */
static esp_err_t api_delete_handler(httpd_req_t *req)
{
    char buf[100];
    size_t buf_len = sizeof(buf);
    
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "id", param, sizeof(param)) == ESP_OK) {
            int id = atoi(param);
            
            // Get path before removing from index
            char filepath[32];
            snprintf(filepath, sizeof(filepath), "/spiffs/%d.epub", id);
            
            ESP_LOGI(TAG, "Deleting ID %d (%s)", id, filepath);
            
            unlink(filepath); // Delete actual file
            bookIndex.removeBook(id); // Update index
            
            gui.refreshLibrary();
            
            httpd_resp_send(req, "OK", 2);
            return ESP_OK;
        }
    }
    httpd_resp_send_500(req);
    return ESP_FAIL;
}

/* API to get/set settings */
static esp_err_t api_settings_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"fontSize\":%.1f, \"font\":\"%s\"}", 
            gui.getFontSize(), gui.getFont().c_str());
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, strlen(buf));
        return ESP_OK;
    } else if (req->method == HTTP_POST) {
        char buf[256];
        int ret, remaining = req->content_len;
        if (remaining >= sizeof(buf)) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        
        if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        
        // Simple JSON parsing
        char* pSize = strstr(buf, "\"fontSize\":");
        if (pSize) {
            float size = atof(pSize + 11);
            gui.setFontSize(size);
        }
        
        char* pFont = strstr(buf, "\"font\":\"");
        if (pFont) {
            char* fontStart = pFont + 8;
            char* fontEnd = strchr(fontStart, '"');
            if (fontEnd) {
                std::string fontName(fontStart, fontEnd - fontStart);
                gui.setFont(fontName);
            }
        }
        
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* Main page handler */
static esp_err_t index_handler(httpd_req_t *req)
{
    if (wifiManager.isConnected()) {
        httpd_resp_send(req, MANAGER_HTML, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, CAPTIVE_HTML, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* Handler for WiFi config */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char ssid[32] = {0};
    char pass[64] = {0};
    
    char* p_ssid = strstr(buf, "ssid=");
    char* p_pass = strstr(buf, "pass=");
    
    if (p_ssid && p_pass) {
        p_ssid += 5;
        char* end_ssid = strchr(p_ssid, '&');
        if (end_ssid) {
            int len = end_ssid - p_ssid;
            if(len > 31) len = 31;
            strncpy(ssid, p_ssid, len);
            ssid[len] = 0;
            
            p_pass += 5;
            // URL decode pass if needed, simplified here
            strcpy(pass, p_pass);
            
            wifiManager.saveCredentials(ssid, pass);
            httpd_resp_send(req, "Saved. Restarting...", HTTPD_RESP_USE_STRLEN);
            
            // Schedule restart
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }
    }
    
    return ESP_OK;
}

/* Handler for redirecting captive portal requests */
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void WebServer::init(const char* basePath) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 10;
    config.stack_size = 8192; // Increase stack for file ops

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t list_api_uri = {
            .uri       = "/api/list",
            .method    = HTTP_GET,
            .handler   = api_list_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &list_api_uri);
        
        httpd_uri_t del_api_uri = {
            .uri       = "/api/delete",
            .method    = HTTP_POST,
            .handler   = api_delete_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &del_api_uri);

        httpd_uri_t settings_api_uri = {
            .uri       = "/api/settings",
            .method    = HTTP_GET,
            .handler   = api_settings_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &settings_api_uri);
        
        httpd_uri_t settings_post_uri = {
            .uri       = "/api/settings",
            .method    = HTTP_POST,
            .handler   = api_settings_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &settings_post_uri);

        httpd_uri_t upload_uri = {
            .uri       = "/upload/*",
            .method    = HTTP_POST,
            .handler   = upload_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &upload_uri);
        
        httpd_uri_t wifi_uri = {
            .uri       = "/wifi",
            .method    = HTTP_POST,
            .handler   = wifi_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &wifi_uri);
        
        httpd_uri_t catch_all_uri = {
            .uri       = "/*",
            .method    = HTTP_GET,
            .handler   = captive_portal_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &catch_all_uri);
    }
}
