#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h" // Added
#include "nvs_flash.h"
#include "nvs.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "wifi_manager.h"
#include "book_index.h"
#include "gui.h"
#include "esp_timer.h"
#include "device_hal.h"
#include "sdkconfig.h"

static const char *TAG = "WEB";
extern WifiManager wifiManager;
extern BookIndex bookIndex;
extern GUI gui;
extern DeviceHAL& deviceHAL;
extern void syncRtcFromNtp();

// Static member initialization
uint32_t WebServer::lastHttpActivityTime = 0;

void WebServer::updateActivityTime() {
    lastHttpActivityTime = (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t WebServer::getLastActivityTime() {
    return lastHttpActivityTime;
}

#define MIN(a,b) ((a) < (b) ? (a) : (b))

// --- Helper for Timezone ---
static const char* resolve_timezone(const char* tz) {
    if (strcmp(tz, "Asia/Jerusalem") == 0) return "IST-2IDT,M3.4.4/26,M10.5.0";
    if (strcmp(tz, "Asia/Tel_Aviv") == 0) return "IST-2IDT,M3.4.4/26,M10.5.0";
    return tz;
}



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
<script src="https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"></script>
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
button.read { background: #34c759; padding: 6px 12px; font-size: 14px; margin-right: 8px; }
input { padding: 12px; border: 1px solid #d1d1d6; border-radius: 8px; width: 100%; box-sizing: border-box; margin-bottom: 15px; font-size: 16px; }
.progress { height: 4px; background: #eee; margin-top: 10px; border-radius: 2px; overflow: hidden; display: none; }
.bar { height: 100%; background: #34c759; width: 0%; transition: width 0.2s; }
.stat { font-size: 14px; color: #666; margin-bottom: 10px; }
#drop-zone.highlight { background: #f0f8ff; border-color: #007aff; }
</style>
</head>
<body>
<div class="card">
  <h2>Settings</h2>
  <div class="stat">Free Space: <span id="freeSpace">Loading...</span></div>
  <div style="margin-bottom: 15px;">
    <label>Font Size: <span id="sizeVal"></span></label>
    <div style="display: flex; gap: 10px; margin-top: 5px;">
        <button onclick="changeSize(-0.1)">-</button>
        <input type="number" id="customSize" step="0.1" style="width: 80px; margin:0;" onchange="setCustomSize()">
        <button onclick="changeSize(0.1)">+</button>
    </div>
  </div>
  <div style="margin-bottom: 15px;">
    <label>Line Spacing: <span id="lineSpacingVal"></span></label>
    <div style="display: flex; gap: 10px; margin-top: 5px;">
        <button onclick="changeLineSpacing(-0.1)">-</button>
        <input type="number" id="customLineSpacing" step="0.1" style="width: 80px; margin:0;" onchange="setCustomLineSpacing()">
        <button onclick="changeLineSpacing(0.1)">+</button>
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
  <div style="margin-top: 15px; border-top: 1px solid #eee; padding-top: 15px;">
    <label>Jump to:</label>
    <div style="display: flex; gap: 10px; margin-top: 5px;">
        <input type="number" id="jumpCh" placeholder="Chapter #" style="width: 100px; margin:0;">
        <input type="number" id="jumpPct" placeholder="%" style="width: 80px; margin:0;">
        <button onclick="jump()">Go</button>
    </div>
  </div>
    <div style="margin-top: 15px; border-top: 1px solid #eee; padding-top: 15px;">
    <label>Timezone (POSIX String):</label>
    <div style="display: flex; gap: 10px; margin-top: 5px;">
        <input id="tzStr" placeholder="e.g. EST5EDT,M3.2.0,M11.1.0" style="margin:0;">
        <button onclick="detectTZ()" style="width: auto;">Auto-Detect</button>
    </div>
    <div style="font-size: 12px; color: #666; margin-top: 5px;">
        Note: Auto-detect gives IANA name (e.g. Asia/Jerusalem). ESP32 prefers POSIX strings for correct DST. 
        <a href="https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv" target="_blank">Lookup POSIX TZ</a>
    </div>
    <button onclick="syncTime()" style="margin-top: 10px;">Sync Time (NTP)</button>
  </div>
</div>

<!-- M5PaperS3 Features Section (hidden on M5Paper) -->
<div class="card" id="s3-features" style="display: none;">
  <h2>M5PaperS3 Features</h2>
  <div class="stat">Device: <span id="deviceName">Loading...</span></div>
  
  <div style="margin-bottom: 15px;">
    <label style="display: flex; align-items: center; gap: 10px;">
      <input type="checkbox" id="buzzerEnabled" onchange="toggleBuzzer()" style="width: auto;">
      <span>Enable Touch Sound (Buzzer)</span>
    </label>
  </div>
  
  <div style="margin-bottom: 15px;">
    <label style="display: flex; align-items: center; gap: 10px;">
      <input type="checkbox" id="autoRotate" onchange="toggleAutoRotate()" style="width: auto;">
      <span>Auto-Rotate (Gyroscope)</span>
    </label>
  </div>
  
  <div style="margin-top: 15px; border-top: 1px solid #eee; padding-top: 15px;">
    <h3 style="margin-top: 0; font-size: 16px;">SD Card</h3>
    <div class="stat">SD Card Status: <span id="sdStatus">Checking...</span></div>
    <div class="stat" id="sdSizeInfo" style="display: none;">SD Card Size: <span id="sdSize">-</span></div>
    <div class="stat" id="sdFreeInfo" style="display: none;">SD Free Space: <span id="sdFree">-</span></div>
    <button onclick="formatSD()" class="del" style="margin-top: 10px;">Format SD Card</button>
    <p style="font-size: 12px; color: #666; margin-top: 5px;">Warning: This will erase all data on the SD card!</p>
  </div>
</div>

<div class="card">
  <h2>Library</h2>
  
  <div id="drop-zone" style="border: 2px dashed #ccc; border-radius: 8px; padding: 20px; text-align: center; margin-bottom: 20px; transition: all 0.2s;">
    <p style="margin: 0 0 10px 0; color: #666; font-weight: 500;">Drag & Drop EPUB files here</p>
    <p style="margin: 0 0 10px 0; color: #999; font-size: 12px;">or</p>
    <input type="file" id="upfile" accept=".epub" multiple style="display: none;">
    <button onclick="document.getElementById('upfile').click()" style="width: auto; margin-bottom: 15px;">Select Files</button>
    
    <div style="text-align: center;">
        <input type="checkbox" id="stripImg" checked style="width: auto; vertical-align: middle;"> 
        <label for="stripImg" style="vertical-align: middle;">Strip Images (Save Space)</label>
    </div>
    
    <div id="upload-queue" style="margin-top: 15px; text-align: left;"></div>
  </div>

  <ul id="list">Loading...</ul>
</div>

<script>
let currentSize = 1.0;
let currentLineSpacing = 1.4;
let isM5PaperS3 = false;

function fetchSettings() {
    fetch('/api/settings').then(r => r.json()).then(s => {
        currentSize = s.fontSize;
        document.getElementById('sizeVal').innerText = currentSize.toFixed(1);
        document.getElementById('customSize').value = currentSize.toFixed(1);
        document.getElementById('fontSel').value = s.font;
        if(s.lineSpacing) {
            currentLineSpacing = s.lineSpacing;
            document.getElementById('lineSpacingVal').innerText = currentLineSpacing.toFixed(1);
            document.getElementById('customLineSpacing').value = currentLineSpacing.toFixed(1);
        }
        if(s.freeSpace) {
            document.getElementById('freeSpace').innerText = (s.freeSpace / 1024 / 1024).toFixed(2) + ' MB';
        }
        if(s.timezone) {
            document.getElementById('tzStr').value = s.timezone;
        }
        
        // M5PaperS3 specific settings
        if(s.deviceName) {
            document.getElementById('deviceName').innerText = s.deviceName;
            isM5PaperS3 = s.deviceName === 'M5PaperS3';
            if(isM5PaperS3) {
                document.getElementById('s3-features').style.display = 'block';
                if(typeof s.buzzerEnabled !== 'undefined') {
                    document.getElementById('buzzerEnabled').checked = s.buzzerEnabled;
                }
                if(typeof s.autoRotate !== 'undefined') {
                    document.getElementById('autoRotate').checked = s.autoRotate;
                }
                fetchSDStatus();
            }
        }
    });
}

function fetchSDStatus() {
    fetch('/api/sd_status').then(r => r.json()).then(s => {
        if(s.mounted) {
            document.getElementById('sdStatus').innerText = 'Mounted';
            document.getElementById('sdSizeInfo').style.display = 'block';
            document.getElementById('sdFreeInfo').style.display = 'block';
            document.getElementById('sdSize').innerText = (s.totalSize / 1024 / 1024 / 1024).toFixed(2) + ' GB';
            document.getElementById('sdFree').innerText = (s.freeSize / 1024 / 1024 / 1024).toFixed(2) + ' GB';
        } else {
            document.getElementById('sdStatus').innerText = s.available ? 'Not Mounted' : 'Not Available';
            document.getElementById('sdSizeInfo').style.display = 'none';
            document.getElementById('sdFreeInfo').style.display = 'none';
        }
    }).catch(e => {
        document.getElementById('sdStatus').innerText = 'Error';
    });
}

function toggleBuzzer() {
    const enabled = document.getElementById('buzzerEnabled').checked;
    fetch('/api/s3_settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({buzzerEnabled: enabled})
    });
}

function toggleAutoRotate() {
    const enabled = document.getElementById('autoRotate').checked;
    fetch('/api/s3_settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({autoRotate: enabled})
    });
}

function formatSD() {
    if(!confirm('WARNING: This will ERASE ALL DATA on the SD card. Are you sure?')) return;
    if(!confirm('This cannot be undone. Confirm to proceed with formatting.')) return;
    
    document.getElementById('sdStatus').innerText = 'Formatting...';
    fetch('/api/format_sd', {method: 'POST'})
        .then(r => r.json())
        .then(result => {
            if(result.success) {
                alert('SD card formatted successfully!');
                fetchSDStatus();
            } else {
                alert('Format failed: ' + (result.error || 'Unknown error'));
            }
        })
        .catch(e => alert('Format error: ' + e.message));
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

function changeLineSpacing(delta) {
    currentLineSpacing += delta;
    if(currentLineSpacing < 1.0) currentLineSpacing = 1.0;
    if(currentLineSpacing > 3.0) currentLineSpacing = 3.0;
    updateSettings();
}

function setCustomLineSpacing() {
    const val = parseFloat(document.getElementById('customLineSpacing').value);
    if(!isNaN(val)) {
        currentLineSpacing = val;
        if(currentLineSpacing < 1.0) currentLineSpacing = 1.0;
        if(currentLineSpacing > 3.0) currentLineSpacing = 3.0;
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
    document.getElementById('lineSpacingVal').innerText = currentLineSpacing.toFixed(1);
    document.getElementById('customLineSpacing').value = currentLineSpacing.toFixed(1);
    
    fetch('/api/settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({fontSize: currentSize, font: font, lineSpacing: currentLineSpacing})
    });
}

function jump() {
  var ch = document.getElementById('jumpCh').value;
  var pct = document.getElementById('jumpPct').value;
  var url = '/jump?';
  if(ch) url += 'ch=' + ch + '&';
  if(pct) url += 'pct=' + pct;
  fetch(url).then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
  }).then(obj => {
      if (obj.applied) {
          if (obj.action === 'percent') {
              alert('Jump applied: ' + obj.percent + '% (offset ' + obj.offset + ' / ' + obj.chapterSize + ')');
          } else if (obj.action === 'chapter') {
              alert('Jumped to chapter ' + obj.chapter);
          } else {
              alert('Jump applied');
          }
      } else {
          alert('Jump not applied: ' + (obj.reason || 'unknown'));
      }
  }).catch(e => alert('Error: ' + e.message));
}

function detectTZ() {
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
    document.getElementById('tzStr').value = tz;
}

function syncTime() {
    const tz = document.getElementById('tzStr').value;
    if(!tz) { alert("Please set a timezone first"); return; }
    
    fetch('/set_time?tz=' + encodeURIComponent(tz))
        .then(r => r.text())
        .then(msg => alert(msg))
        .catch(e => alert("Error: " + e));
}

function fetchList() {
    fetch('/api/list').then(r => r.json()).then(files => {
        const list = document.getElementById('list');
        list.innerHTML = '';
        if(files.length === 0) list.innerHTML = '<li>No books found</li>';
        files.forEach(f => {
            const li = document.createElement('li');
            li.innerHTML = `<span>${f.name}</span><div><button class="read" onclick="readBook(${f.id})">Read</button><button class="del" onclick="del(${f.id})">Delete</button></div>`;
            list.appendChild(li);
        });
    });
}

function del(id) {
    if(!confirm('Delete book?')) return;
    fetch('/api/delete?id=' + id, {method: 'POST'})
        .then(() => {
            fetchList();
            fetchSettings(); // Update free space
        })
        .catch(e => alert('Error deleting'));
}

function readBook(id) {
    fetch('/api/open?id=' + id, {method: 'POST'})
        .then(r => {
            if (!r.ok) throw new Error('Failed to open');
            alert('Opening on device...');
        })
        .catch(e => alert(e.message));
}

// Drag & Drop Logic
const dropZone = document.getElementById('drop-zone');
const fileInput = document.getElementById('upfile');

['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
  dropZone.addEventListener(eventName, preventDefaults, false);
});

function preventDefaults(e) {
  e.preventDefault();
  e.stopPropagation();
}

['dragenter', 'dragover'].forEach(eventName => {
  dropZone.addEventListener(eventName, highlight, false);
});

['dragleave', 'drop'].forEach(eventName => {
  dropZone.addEventListener(eventName, unhighlight, false);
});

function highlight(e) {
  dropZone.classList.add('highlight');
  dropZone.style.borderColor = '#007aff';
  dropZone.style.background = '#f0f8ff';
}

function unhighlight(e) {
  dropZone.classList.remove('highlight');
  dropZone.style.borderColor = '#ccc';
  dropZone.style.background = 'transparent';
}

dropZone.addEventListener('drop', handleDrop, false);

function handleDrop(e) {
  const dt = e.dataTransfer;
  const files = dt.files;
  handleFiles(files);
}

fileInput.addEventListener('change', function() {
  handleFiles(this.files);
});

async function handleFiles(files) {
  const queue = document.getElementById('upload-queue');
  queue.innerHTML = ''; // Clear previous queue
  for (const file of files) {
      await uploadFile(file);
  }
}

async function uploadFile(file) {
  const queue = document.getElementById('upload-queue');
  const div = document.createElement('div');
  div.style.marginBottom = '10px';
  div.style.padding = '10px';
  div.style.background = '#f9f9f9';
  div.style.borderRadius = '6px';
  div.innerHTML = `<div style="font-size:14px; margin-bottom:4px; font-weight:500;">${file.name}</div>
                   <div class="progress" style="display:block; background:#ddd;"><div class="bar" style="width:0%"></div></div>
                   <div class="status" style="font-size:12px; color:#666; margin-top:4px;">Waiting...</div>`;
  queue.appendChild(div);
  
  const bar = div.querySelector('.bar');
  const status = div.querySelector('.status');
  const strip = document.getElementById('stripImg').checked;
  
  let fileToSend = file;
  
  if (strip) {
      try {
          status.innerText = 'Stripping images...';
          const zip = new JSZip();
          const content = await zip.loadAsync(file);
          
          const filesToRemove = [];
          zip.forEach((relativePath, zipEntry) => {
              const lower = relativePath.toLowerCase();
              if (lower.match(/\.(jpg|jpeg|png|gif|webp|bmp|tiff|ttf|otf|woff|woff2)$/i) || lower.includes('oebps/images/') || lower.includes('oebps/fonts/')) {
                  filesToRemove.push(relativePath);
              }
          });
          
          filesToRemove.forEach(f => zip.remove(f));
          
          if (filesToRemove.length > 0) {
              const blob = await zip.generateAsync({type:"blob", compression: "DEFLATE"});
              fileToSend = new File([blob], file.name, {type: "application/epub+zip"});
              status.innerText = `Stripped ${filesToRemove.length} files. Uploading...`;
          } else {
              status.innerText = 'No images to strip. Uploading...';
          }
      } catch (e) {
          status.innerText = 'Error processing ZIP: ' + e.message;
          status.style.color = 'red';
          return;
      }
  } else {
      status.innerText = 'Uploading...';
  }
  
  await new Promise((resolve) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload/' + encodeURIComponent(fileToSend.name), true);
      
      xhr.upload.onprogress = (e) => {
          if (e.lengthComputable) {
              const percent = (e.loaded / e.total) * 100;
              bar.style.width = percent + '%';
          }
      };
      
      xhr.onload = () => {
          if (xhr.status === 200) {
              status.innerText = 'Done';
              status.style.color = 'green';
              fetchList();
              fetchSettings();
          } else {
              status.innerText = 'Failed: ' + xhr.statusText;
              status.style.color = 'red';
          }
          resolve();
      };
      
      xhr.onerror = () => {
          status.innerText = 'Network error';
          status.style.color = 'red';
          resolve();
      };
      
      xhr.send(fileToSend);
  });
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

static size_t getFreeSpace() {
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        return total - used;
    }
    return 0;
}

static esp_err_t jump_handler(httpd_req_t *req) {
    WebServer::updateActivityTime();
    char buf[128];
    ESP_LOGI(TAG, "jump_handler called: uri=%s", req->uri);
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];

        // Support both 'percent' and 'pct' (web UI used pct) for percent jumps
        if (httpd_query_key_value(buf, "percent", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(buf, "pct", param, sizeof(param)) == ESP_OK) {
            float p = atof(param);
            ESP_LOGI(TAG, "jump_handler: percent param=%s -> %f", param, p);

            // Prepare JSON response
            httpd_resp_set_type(req, "application/json");
            char out[256];

            size_t chapterSize = gui.getCurrentChapterSize();
            size_t newOffset = (size_t)((p / 100.0f) * chapterSize);

            if (gui.canJump()) {
                gui.jumpTo(p);
                int len = snprintf(out, sizeof(out), "{\"result\":\"ok\",\"action\":\"percent\",\"percent\":%.2f,\"offset\":%u,\"chapterSize\":%u,\"applied\":true}", p, (unsigned)newOffset, (unsigned)chapterSize);
                httpd_resp_send(req, out, len);
            } else {
                int len = snprintf(out, sizeof(out), "{\"result\":\"error\",\"reason\":\"not in reader state\",\"applied\":false}");
                httpd_resp_send(req, out, len);
            }
            return ESP_OK;
        }

        // Support both 'chapter' and 'ch' (web UI used ch)
        if (httpd_query_key_value(buf, "chapter", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            int c = atoi(param);
            ESP_LOGI(TAG, "jump_handler: chapter param=%s -> %d", param, c);
            httpd_resp_set_type(req, "application/json");
            char out[256];

            if (gui.canJump()) {
                gui.jumpToChapter(c);
                int len = snprintf(out, sizeof(out), "{\"result\":\"ok\",\"action\":\"chapter\",\"chapter\":%d,\"applied\":true}", c);
                httpd_resp_send(req, out, len);
            } else {
                int len = snprintf(out, sizeof(out), "{\"result\":\"error\",\"reason\":\"not in reader state\",\"applied\":false}");
                httpd_resp_send(req, out, len);
            }
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "jump_handler: no query string provided");
    }

    httpd_resp_send(req, "Bad Request", HTTPD_400_BAD_REQUEST);
    return ESP_OK;
}

/* Handler for upload */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    // Update activity time at start of upload to prevent sleep
    WebServer::updateActivityTime();
    
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
    int64_t lastLogTime = 0;

    while (remaining > 0) {
        // Update activity time during upload to prevent sleep on large files
        WebServer::updateActivityTime();

        int64_t now = esp_timer_get_time();
        if (now - lastLogTime > 2000000) { // Log every 2 seconds
            lastLogTime = now;
            ESP_LOGI(TAG, "Upload progress: %d bytes remaining. Activity time updated.", remaining);
        }
        
        if ((received = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv failed with %d", received);
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
    WebServer::updateActivityTime();
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
    WebServer::updateActivityTime();
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

/* API to open a book on the device */
static esp_err_t api_open_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    char buf[100];
    size_t buf_len = sizeof(buf);

    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "id", param, sizeof(param)) == ESP_OK) {
            int id = atoi(param);
            if (gui.openBookById(id)) {
                httpd_resp_send(req, "OK", 2);
                return ESP_OK;
            }
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid book id");
    return ESP_FAIL;
}

/* API to get/set settings */
static esp_err_t api_settings_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    if (req->method == HTTP_GET) {
        char tz[64] = {0};
        // Load TZ from NVS
        nvs_handle_t my_handle;
        esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
        if (err == ESP_OK) {
            size_t required_size = sizeof(tz);
            nvs_get_str(my_handle, "timezone", tz, &required_size);
            nvs_close(my_handle);
        }

        char buf[512];
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
        snprintf(buf, sizeof(buf), 
            "{\"fontSize\":%.1f, \"font\":\"%s\", \"lineSpacing\":%.1f, \"freeSpace\":%u, \"timezone\":\"%s\", "
            "\"deviceName\":\"%s\", \"buzzerEnabled\":%s, \"autoRotate\":%s}", 
            gui.getFontSize(), gui.getFont().c_str(), gui.getLineSpacing(), 
            (unsigned int)getFreeSpace(), tz,
            deviceHAL.getDeviceName(),
            gui.isBuzzerEnabled() ? "true" : "false",
            gui.isAutoRotateEnabled() ? "true" : "false");
#else
        snprintf(buf, sizeof(buf), 
            "{\"fontSize\":%.1f, \"font\":\"%s\", \"lineSpacing\":%.1f, \"freeSpace\":%u, \"timezone\":\"%s\", \"deviceName\":\"%s\"}", 
            gui.getFontSize(), gui.getFont().c_str(), gui.getLineSpacing(), 
            (unsigned int)getFreeSpace(), tz,
            deviceHAL.getDeviceName());
#endif
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
        
        char* pLineSpacing = strstr(buf, "\"lineSpacing\":");
        if (pLineSpacing) {
            float spacing = atof(pLineSpacing + 14);
            gui.setLineSpacing(spacing);
        }
        
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* API for M5PaperS3 specific settings */
static esp_err_t api_s3_settings_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
    if (req->method == HTTP_POST) {
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
        
        // Parse buzzerEnabled
        char* pBuzzer = strstr(buf, "\"buzzerEnabled\":");
        if (pBuzzer) {
            bool enabled = (strstr(pBuzzer + 16, "true") != nullptr);
            gui.setBuzzerEnabled(enabled);
        }
        
        // Parse autoRotate
        char* pRotate = strstr(buf, "\"autoRotate\":");
        if (pRotate) {
            bool enabled = (strstr(pRotate + 13, "true") != nullptr);
            gui.setAutoRotateEnabled(enabled);
        }
        
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
#endif
    
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not supported");
    return ESP_FAIL;
}

/* API to get SD card status */
static esp_err_t api_sd_status_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    httpd_resp_set_type(req, "application/json");
    
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    char buf[256];
    bool mounted = deviceHAL.isSDCardMounted();
    bool available = deviceHAL.hasSDCardSlot();
    uint64_t totalSize = deviceHAL.getSDCardTotalSize();
    uint64_t freeSize = deviceHAL.getSDCardFreeSize();
    
    snprintf(buf, sizeof(buf), 
        "{\"available\":%s, \"mounted\":%s, \"totalSize\":%llu, \"freeSize\":%llu}",
        available ? "true" : "false",
        mounted ? "true" : "false",
        (unsigned long long)totalSize,
        (unsigned long long)freeSize);
    httpd_resp_send(req, buf, strlen(buf));
#else
    httpd_resp_send(req, "{\"available\":false, \"mounted\":false}", HTTPD_RESP_USE_STRLEN);
#endif
    
    return ESP_OK;
}

/* API to format SD card */
static esp_err_t api_format_sd_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    httpd_resp_set_type(req, "application/json");
    
#ifdef CONFIG_EBOOK_S3_ENABLE_SD_CARD
    ESP_LOGW(TAG, "Formatting SD card via web request");
    
    bool success = deviceHAL.formatSDCard([](int progress) {
        ESP_LOGI(TAG, "Format progress: %d%%", progress);
    });
    
    if (success) {
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"success\":false, \"error\":\"Format failed\"}", HTTPD_RESP_USE_STRLEN);
    }
#else
    httpd_resp_send(req, "{\"success\":false, \"error\":\"SD card not supported\"}", HTTPD_RESP_USE_STRLEN);
#endif
    
    return ESP_OK;
}

/* Main page handler */
static esp_err_t index_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
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
    WebServer::updateActivityTime();
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
    WebServer::updateActivityTime();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t set_time_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char tz_encoded[64] = {0};
        if (httpd_query_key_value(buf, "tz", tz_encoded, sizeof(tz_encoded)) == ESP_OK) {
            char tz[64] = {0};
            url_decode(tz, tz_encoded);
            
            const char* posix_tz = resolve_timezone(tz);
            
            ESP_LOGI(TAG, "Setting timezone to: %s (resolved from %s)", posix_tz, tz);
            
            // Save to NVS
            nvs_handle_t my_handle;
            esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
            if (err == ESP_OK) {
                nvs_set_str(my_handle, "timezone", posix_tz);
                nvs_commit(my_handle);
                nvs_close(my_handle);
            }
            
            // Set TZ environment variable
            setenv("TZ", posix_tz, 1);
            tzset();
            
            // Sync with NTP
            syncRtcFromNtp();
            
            httpd_resp_send(req, "Timezone set and time synced!", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_500(req);
    return ESP_FAIL;
}

void WebServer::init(const char* basePath) {
    if (server != NULL) return; // Already running

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16; // Increased for M5PaperS3 endpoints
    config.stack_size = 8192; // Increase stack for file ops
    config.recv_wait_timeout = 10; // Reduced to 10s to keep activity timer fresh
    config.send_wait_timeout = 10; // Reduced to 10s to keep activity timer fresh

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

        httpd_uri_t open_api_uri = {
            .uri       = "/api/open",
            .method    = HTTP_POST,
            .handler   = api_open_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &open_api_uri);

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

        httpd_uri_t jump_api_uri = {
            .uri       = "/api/jump",
            .method    = HTTP_GET,
            .handler   = jump_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &jump_api_uri);

        httpd_uri_t set_time_uri = {
            .uri       = "/set_time",
            .method    = HTTP_GET,
            .handler   = set_time_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &set_time_uri);

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
        
        httpd_uri_t jump_uri = {
            .uri       = "/jump",
            .method    = HTTP_GET,
            .handler   = jump_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &jump_uri);
        
        // M5PaperS3 specific endpoints
        httpd_uri_t s3_settings_uri = {
            .uri       = "/api/s3_settings",
            .method    = HTTP_POST,
            .handler   = api_s3_settings_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &s3_settings_uri);
        
        httpd_uri_t sd_status_uri = {
            .uri       = "/api/sd_status",
            .method    = HTTP_GET,
            .handler   = api_sd_status_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &sd_status_uri);
        
        httpd_uri_t format_sd_uri = {
            .uri       = "/api/format_sd",
            .method    = HTTP_POST,
            .handler   = api_format_sd_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &format_sd_uri);
        
        httpd_uri_t catch_all_uri = {
            .uri       = "/*",
            .method    = HTTP_GET,
            .handler   = captive_portal_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &catch_all_uri);
    }
}

void WebServer::stop() {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}
