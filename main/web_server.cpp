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
#include "esp_task_wdt.h"
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
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>OKeysh - E-Book Reader</title>
    <script src="https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"></script>
    <style>
        :root {
            --primary: #2563eb;
            --primary-hover: #1d4ed8;
            --bg: #f8fafc;
            --card-bg: #ffffff;
            --text: #1e293b;
            --text-secondary: #64748b;
            --border: #e2e8f0;
            --danger: #ef4444;
            --success: #22c55e;
            --warning: #f59e0b;
        }

        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background: var(--bg); color: var(--text); line-height: 1.5; }
        
        .container { max-width: 800px; margin: 0 auto; padding: 20px; }
        
        header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        h1 { font-size: 24px; font-weight: 700; color: var(--primary); }
        
        .tabs { display: flex; gap: 10px; margin-bottom: 20px; overflow-x: auto; padding-bottom: 5px; }
        .tab { padding: 8px 16px; background: var(--card-bg); border: 1px solid var(--border); border-radius: 20px; cursor: pointer; font-weight: 500; white-space: nowrap; transition: all 0.2s; }
        .tab.active { background: var(--primary); color: white; border-color: var(--primary); }
        
        .section { display: none; }
        .section.active { display: block; }
        
        .card { background: var(--card-bg); border-radius: 12px; padding: 20px; margin-bottom: 20px; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }
        .card h2 { font-size: 18px; margin-bottom: 15px; border-bottom: 1px solid var(--border); padding-bottom: 10px; }
        
        .btn { background: var(--primary); color: white; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-weight: 500; transition: background 0.2s; }
        .btn:hover { background: var(--primary-hover); }
        .btn:disabled { opacity: 0.5; cursor: not-allowed; }
        .btn.danger { background: var(--danger); }
        .btn.success { background: var(--success); }
        .btn.outline { background: transparent; border: 1px solid var(--border); color: var(--text); }
        .btn.sm { padding: 4px 8px; font-size: 14px; }
        
        .form-group { margin-bottom: 15px; }
        .form-group label { display: block; margin-bottom: 5px; font-weight: 500; color: var(--text-secondary); }
        .form-row { display: flex; gap: 10px; align-items: center; }
        
        input[type="text"], input[type="number"], input[type="password"], select { width: 100%; padding: 8px 12px; border: 1px solid var(--border); border-radius: 6px; font-size: 16px; }
        input[type="checkbox"] { width: auto; margin-right: 8px; }
        
        .book-list { list-style: none; }
        .book-item { display: flex; justify-content: space-between; align-items: center; padding: 12px 0; border-bottom: 1px solid var(--border); }
        .book-item:last-child { border-bottom: none; }
        .book-info { flex: 1; min-width: 0; padding-right: 10px; }
        .book-title { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        .book-author { font-size: 14px; color: var(--text-secondary); }
        .book-actions { display: flex; gap: 5px; }
        
        .drop-zone { border: 2px dashed var(--border); border-radius: 8px; padding: 30px; text-align: center; transition: all 0.2s; cursor: pointer; }
        .drop-zone.highlight { border-color: var(--primary); background: #eff6ff; }
        
        .progress-bar { height: 6px; background: var(--border); border-radius: 3px; overflow: hidden; margin-top: 5px; }
        .progress-fill { height: 100%; background: var(--success); width: 0%; transition: width 0.2s; }
        
        .stat-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 15px; }
        .stat-box { background: #f1f5f9; padding: 15px; border-radius: 8px; text-align: center; }
        .stat-val { font-size: 20px; font-weight: 700; color: var(--primary); }
        .stat-label { font-size: 12px; color: var(--text-secondary); margin-top: 5px; }

        .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); background: #333; color: white; padding: 10px 20px; border-radius: 20px; font-size: 14px; opacity: 0; transition: opacity 0.3s; pointer-events: none; z-index: 1000; }
        .toast.show { opacity: 1; }

        @media (max-width: 600px) {
            .book-item { flex-direction: column; align-items: flex-start; gap: 10px; }
            .book-actions { width: 100%; justify-content: flex-end; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>OKeysh</h1>
            <div id="device-name" style="font-size: 14px; color: var(--text-secondary);">Connecting...</div>
        </header>

        <div class="tabs">
            <div class="tab active" onclick="switchTab('library')">Library</div>
            <div class="tab" onclick="switchTab('settings')">Settings</div>
            <div class="tab" onclick="switchTab('system')">System</div>
        </div>

        <!-- Library Section -->
        <div id="library" class="section active">
            <div class="card">
                <div class="drop-zone" id="drop-zone" onclick="document.getElementById('upfile').click()">
                    <p style="font-weight: 500; margin-bottom: 5px;">Upload Books</p>
                    <p style="font-size: 12px; color: var(--text-secondary);">Drag & drop EPUBs here or click to select</p>
                    <input type="file" id="upfile" accept=".epub" multiple style="display: none;">
                </div>
                <div style="margin-top: 10px; display: flex; align-items: center; justify-content: center;">
                    <input type="checkbox" id="stripImg" checked>
                    <label for="stripImg" style="font-size: 14px;">Strip Images (Save Space)</label>
                </div>
                <div id="upload-queue" style="margin-top: 15px;"></div>
            </div>

            <div class="card">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px;">
                    <h2>My Books</h2>
                    <button class="btn sm outline" onclick="fetchList()">Refresh</button>
                </div>
                <ul id="book-list" class="book-list">
                    <li style="text-align: center; padding: 20px; color: var(--text-secondary);">Loading...</li>
                </ul>
            </div>
        </div>

        <!-- Settings Section -->
        <div id="settings" class="section">
            <div class="card">
                <h2>Reading Preferences</h2>
                <div class="form-group">
                    <label>Font Size: <span id="sizeVal">1.0</span></label>
                    <div class="form-row">
                        <button class="btn sm outline" onclick="changeSize(-0.1)">-</button>
                        <input type="number" id="customSize" step="0.1" onchange="setCustomSize()">
                        <button class="btn sm outline" onclick="changeSize(0.1)">+</button>
                    </div>
                </div>
                <div class="form-group">
                    <label>Line Spacing: <span id="lineSpacingVal">1.4</span></label>
                    <div class="form-row">
                        <button class="btn sm outline" onclick="changeLineSpacing(-0.1)">-</button>
                        <input type="number" id="customLineSpacing" step="0.1" onchange="setCustomLineSpacing()">
                        <button class="btn sm outline" onclick="changeLineSpacing(0.1)">+</button>
                    </div>
                </div>
                <div class="form-group">
                    <label>Font Family</label>
                    <select id="fontSel" onchange="changeFont()">
                        <option value="Default">Default</option>
                        <option value="Hebrew">Hebrew</option>
                        <option value="Roboto">Roboto</option>
                    </select>
                </div>
                <div class="form-group">
                    <label style="display: flex; align-items: center;">
                        <input type="checkbox" id="showImages" onchange="toggleShowImages()">
                        Show Images in Books
                    </label>
                </div>
            </div>

            <div class="card">
                <h2>Navigation</h2>
                <div class="form-group">
                    <label>Jump to Chapter / Percentage</label>
                    <div class="form-row">
                        <input type="number" id="jumpCh" placeholder="Chapter #">
                        <input type="number" id="jumpPct" placeholder="%">
                        <button class="btn" onclick="jump()">Go</button>
                    </div>
                </div>
            </div>

            <div class="card" id="s3-features" style="display: none;">
                <h2>Device Features (S3)</h2>
                <div class="form-group">
                    <label style="display: flex; align-items: center;">
                        <input type="checkbox" id="buzzerEnabled" onchange="toggleBuzzer()">
                        Touch Sound (Buzzer)
                    </label>
                </div>
                <div class="form-group">
                    <label style="display: flex; align-items: center;">
                        <input type="checkbox" id="autoRotate" onchange="toggleAutoRotate()">
                        Auto-Rotate Screen
                    </label>
                </div>
            </div>
        </div>

        <!-- System Section -->
        <div id="system" class="section">
            <div class="card">
                <h2>Storage</h2>
                <div class="stat-grid">
                    <div class="stat-box">
                        <div class="stat-val" id="freeSpace">-</div>
                        <div class="stat-label">Internal Free</div>
                    </div>
                    <div class="stat-box" id="sd-box" style="display: none;">
                        <div class="stat-val" id="sdFree">-</div>
                        <div class="stat-label">SD Free</div>
                    </div>
                </div>
                <div id="sd-controls" style="margin-top: 20px; display: none; border-top: 1px solid var(--border); padding-top: 15px;">
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <div>
                            <strong>SD Card Status:</strong> <span id="sdStatus">Checking...</span>
                        </div>
                        <button class="btn danger sm" onclick="formatSD()">Format SD Card</button>
                    </div>
                    <p style="font-size: 12px; color: var(--danger); margin-top: 5px;">Warning: Formatting erases all data!</p>
                </div>
            </div>

            <div class="card">
                <h2>Time & Date</h2>
                <div class="form-group">
                    <label>Timezone (POSIX)</label>
                    <div class="form-row">
                        <input id="tzStr" placeholder="e.g. IST-2IDT...">
                        <button class="btn outline" onclick="detectTZ()">Auto-Detect</button>
                    </div>
                    <div style="font-size: 12px; color: var(--text-secondary); margin-top: 5px;">
                        <a href="https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv" target="_blank" style="color: var(--primary);">Lookup POSIX TZ</a>
                    </div>
                </div>
                <button class="btn" onclick="syncTime()">Sync Time (NTP)</button>
            </div>

            <div class="card">
                <h2>System Update</h2>
                <div class="form-group">
                    <label>Update Web Interface (index.html)</label>
                    <div class="form-row">
                        <input type="file" id="uiFile" accept=".html">
                        <button class="btn" onclick="uploadUI()">Update UI</button>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <div id="toast" class="toast"></div>

    <script>
        // --- State ---
        let currentSize = 1.0;
        let currentLineSpacing = 1.4;
        let isM5PaperS3 = false;

        // --- UI Logic ---
        function switchTab(tabId) {
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
            
            document.querySelector(`.tab[onclick="switchTab('${tabId}')"]`).classList.add('active');
            document.getElementById(tabId).classList.add('active');
        }

        function showToast(msg, duration=3000) {
            const t = document.getElementById('toast');
            t.innerText = msg;
            t.classList.add('show');
            setTimeout(() => t.classList.remove('show'), duration);
        }

        // --- API Calls ---
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
                
                if(s.sdFreeSpace) {
                    document.getElementById('sd-box').style.display = 'block';
                    document.getElementById('sdFree').innerText = (s.sdFreeSpace / 1024 / 1024 / 1024).toFixed(2) + ' GB';
                }
                
                if(s.timezone) document.getElementById('tzStr').value = s.timezone;
                if(typeof s.showImages !== 'undefined') document.getElementById('showImages').checked = s.showImages;
                
                if(s.deviceName) {
                    document.getElementById('device-name').innerText = s.deviceName;
                    isM5PaperS3 = s.deviceName === 'M5PaperS3';
                    if(isM5PaperS3) {
                        document.getElementById('s3-features').style.display = 'block';
                        document.getElementById('sd-controls').style.display = 'block';
                        if(typeof s.buzzerEnabled !== 'undefined') document.getElementById('buzzerEnabled').checked = s.buzzerEnabled;
                        if(typeof s.autoRotate !== 'undefined') document.getElementById('autoRotate').checked = s.autoRotate;
                        fetchSDStatus();
                    }
                }
            }).catch(e => console.error("Settings error", e));
        }

        function fetchList() {
            fetch('/api/list').then(r => r.json()).then(files => {
                const list = document.getElementById('book-list');
                list.innerHTML = '';
                if(files.length === 0) {
                    list.innerHTML = '<li style="text-align: center; padding: 20px; color: var(--text-secondary);">No books found</li>';
                    return;
                }
                files.forEach(f => {
                    const li = document.createElement('li');
                    li.className = 'book-item';
                    const favClass = f.favorite ? 'warning' : 'outline';
                    const favText = f.favorite ? '★' : '☆';
                    
                    li.innerHTML = `
                        <div class="book-info">
                            <div class="book-title">${f.name}</div>
                            <div class="book-author">${f.author || 'Unknown Author'}</div>
                        </div>
                        <div class="book-actions">
                            <button class="btn sm ${favClass}" onclick="toggleFav(${f.id}, this)" title="Favorite">${favText}</button>
                            <button class="btn sm success" onclick="readBook(${f.id})">Read</button>
                            <button class="btn sm danger" onclick="del(${f.id})">Delete</button>
                        </div>
                    `;
                    list.appendChild(li);
                });
            }).catch(e => {
                document.getElementById('book-list').innerHTML = '<li style="color: var(--danger); text-align: center;">Error loading books</li>';
            });
        }

        function fetchSDStatus() {
            fetch('/api/sd_status').then(r => r.json()).then(s => {
                const statusEl = document.getElementById('sdStatus');
                if(s.mounted) {
                    statusEl.innerText = 'Mounted';
                    statusEl.style.color = 'var(--success)';
                    document.getElementById('sd-box').style.display = 'block';
                    document.getElementById('sdFree').innerText = (s.freeSize / 1024 / 1024 / 1024).toFixed(2) + ' GB';
                } else {
                    statusEl.innerText = s.available ? 'Not Mounted' : 'Not Available';
                    statusEl.style.color = 'var(--warning)';
                }
            });
        }

        // --- Actions ---
        function toggleFav(id, btn) {
            fetch('/api/favorite?id=' + id, {method: 'POST'})
                .then(r => r.json())
                .then(result => {
                    if(result.favorite) {
                        btn.innerText = '★';
                        btn.classList.remove('outline');
                        btn.classList.add('warning');
                    } else {
                        btn.innerText = '☆';
                        btn.classList.remove('warning');
                        btn.classList.add('outline');
                    }
                })
                .catch(() => showToast('Error toggling favorite'));
        }

        function del(id) {
            if(!confirm('Delete this book?')) return;
            fetch('/api/delete?id=' + id, {method: 'POST'})
                .then(() => {
                    showToast('Book deleted');
                    fetchList();
                    fetchSettings();
                })
                .catch(() => showToast('Error deleting book'));
        }

        function readBook(id) {
            fetch('/api/open?id=' + id, {method: 'POST'})
                .then(r => {
                    if (!r.ok) throw new Error('Failed');
                    showToast('Opening book on device...');
                })
                .catch(() => showToast('Failed to open book'));
        }

        function jump() {
            const ch = document.getElementById('jumpCh').value;
            const pct = document.getElementById('jumpPct').value;
            let url = '/jump?';
            if(ch) url += 'ch=' + ch + '&';
            if(pct) url += 'pct=' + pct;
            
            fetch(url).then(r => r.json()).then(obj => {
                if (obj.applied) {
                    showToast(obj.action === 'percent' ? `Jumped to ${obj.percent}%` : `Jumped to Chapter ${obj.chapter}`);
                } else {
                    showToast('Jump failed: ' + (obj.reason || 'unknown'));
                }
            }).catch(e => showToast('Error: ' + e.message));
        }

        // --- Settings Updates ---
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

        function changeSize(delta) {
            currentSize = Math.max(0.5, Math.min(5.0, currentSize + delta));
            updateSettings();
        }

        function setCustomSize() {
            const val = parseFloat(document.getElementById('customSize').value);
            if(!isNaN(val)) {
                currentSize = Math.max(0.5, Math.min(5.0, val));
                updateSettings();
            }
        }

        function changeLineSpacing(delta) {
            currentLineSpacing = Math.max(1.0, Math.min(3.0, currentLineSpacing + delta));
            updateSettings();
        }

        function setCustomLineSpacing() {
            const val = parseFloat(document.getElementById('customLineSpacing').value);
            if(!isNaN(val)) {
                currentLineSpacing = Math.max(1.0, Math.min(3.0, val));
                updateSettings();
            }
        }

        function changeFont() { updateSettings(); }

        function toggleShowImages() {
            const enabled = document.getElementById('showImages').checked;
            fetch('/api/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({showImages: enabled})
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

        function detectTZ() {
            const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
            document.getElementById('tzStr').value = tz;
        }

        function syncTime() {
            const tz = document.getElementById('tzStr').value;
            if(!tz) { showToast("Please set a timezone first"); return; }
            
            fetch('/set_time?tz=' + encodeURIComponent(tz))
                .then(r => r.text())
                .then(msg => showToast(msg))
                .catch(e => showToast("Error syncing time"));
        }

        function formatSD() {
            if(!confirm('WARNING: This will ERASE ALL DATA on the SD card. Are you sure?')) return;
            if(!confirm('This cannot be undone. Confirm to proceed with formatting.')) return;
            
            document.getElementById('sdStatus').innerText = 'Formatting...';
            fetch('/api/format_sd', {method: 'POST'})
                .then(r => r.json())
                .then(result => {
                    if(result.success) {
                        showToast('SD card formatted successfully!');
                        fetchSDStatus();
                    } else {
                        showToast('Format failed: ' + (result.error || 'Unknown error'));
                    }
                })
                .catch(e => showToast('Format error: ' + e.message));
        }

        // --- Upload Logic ---
        const dropZone = document.getElementById('drop-zone');
        const fileInput = document.getElementById('upfile');

        ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
            dropZone.addEventListener(eventName, e => { e.preventDefault(); e.stopPropagation(); }, false);
        });

        ['dragenter', 'dragover'].forEach(eventName => {
            dropZone.addEventListener(eventName, () => dropZone.classList.add('highlight'), false);
        });

        ['dragleave', 'drop'].forEach(eventName => {
            dropZone.addEventListener(eventName, () => dropZone.classList.remove('highlight'), false);
        });

        dropZone.addEventListener('drop', e => handleFiles(e.dataTransfer.files), false);
        fileInput.addEventListener('change', function() { handleFiles(this.files); });

        async function handleFiles(files) {
            const queue = document.getElementById('upload-queue');
            queue.innerHTML = '';
            for (const file of files) {
                await uploadFile(file);
            }
        }

        async function uploadFile(file) {
            const queue = document.getElementById('upload-queue');
            const div = document.createElement('div');
            div.style.marginBottom = '10px';
            div.innerHTML = `
                <div style="font-size:14px; margin-bottom:4px; font-weight:500;">${file.name}</div>
                <div class="progress-bar"><div class="progress-fill" style="width:0%"></div></div>
                <div class="status" style="font-size:12px; color:var(--text-secondary); margin-top:4px;">Waiting...</div>
            `;
            queue.appendChild(div);
            
            const bar = div.querySelector('.progress-fill');
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
                    status.style.color = 'var(--danger)';
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
                        status.style.color = 'var(--success)';
                        fetchList();
                        fetchSettings();
                    } else {
                        status.innerText = 'Failed: ' + xhr.statusText;
                        status.style.color = 'var(--danger)';
                    }
                    resolve();
                };
                
                xhr.onerror = () => {
                    status.innerText = 'Network error';
                    status.style.color = 'var(--danger)';
                    resolve();
                };
                
                xhr.send(fileToSend);
            });
        }

        // --- UI Update Logic ---
        function uploadUI() {
            const fileInput = document.getElementById('uiFile');
            if(fileInput.files.length === 0) {
                showToast('Please select a file first');
                return;
            }
            
            const file = fileInput.files[0];
            const formData = new FormData();
            formData.append("file", file);
            
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/update_ui', true);
            
            xhr.onload = () => {
                if (xhr.status === 200) {
                    showToast('UI Updated! Reloading...');
                    setTimeout(() => location.reload(), 1500);
                } else {
                    showToast('Update failed: ' + xhr.statusText);
                }
            };
            
            xhr.onerror = () => showToast('Network error');
            xhr.send(file); // Send raw file content or FormData depending on backend handler. 
                            // Since existing upload handler expects raw body for /upload/*, 
                            // I'll implement /update_ui to expect raw body too for simplicity.
        }

        // Init
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
    esp_task_wdt_add(NULL);
    // Update activity time at start of upload to prevent sleep
    WebServer::updateActivityTime();
    
    char filename[256];
    const char* uri = req->uri;
    const char* filename_start = strstr(uri, "/upload/");
    if (!filename_start) {
        httpd_resp_send_500(req);
        esp_task_wdt_delete(NULL);
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
        esp_task_wdt_delete(NULL);
        return ESP_FAIL;
    }

    char *buf = (char*)malloc(4096);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate buffer");
        httpd_resp_send_500(req);
        esp_task_wdt_delete(NULL);
        return ESP_FAIL;
    }

    int received;
    int remaining = req->content_len;
    int64_t lastLogTime = 0;

    while (remaining > 0) {
        esp_task_wdt_reset();
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
            esp_task_wdt_delete(NULL);
            return ESP_FAIL;
        }
        size_t written = fwrite(buf, 1, received, f);
        if (written != received) {
            free(buf);
            fclose(f);
            ESP_LOGE(TAG, "File write failed");
            httpd_resp_send_500(req);
            esp_task_wdt_delete(NULL);
            return ESP_FAIL;
        }
        remaining -= received;
    }
    free(buf);
    fclose(f);
    
    gui.refreshLibrary();
    
    httpd_resp_send_chunk(req, NULL, 0);
    esp_task_wdt_delete(NULL);
    return ESP_OK;
}

// Helper to escape JSON strings
static std::string escapeJsonString(const std::string& input) {
    std::string output;
    output.reserve(input.length() * 2);
    for (char c : input) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c >= 0 && c < 32) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)c);
                    output += hex;
                } else {
                    output += c;
                }
        }
    }
    return output;
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
        
        std::string escapedTitle = escapeJsonString(book.title);
        std::string escapedAuthor = escapeJsonString(book.author);
        
        char buf[1024];
        snprintf(buf, sizeof(buf), 
            "{\"name\":\"%s\",\"author\":\"%s\",\"id\":%d,\"favorite\":%s}", 
            escapedTitle.c_str(), 
            escapedAuthor.c_str(),
            book.id,
            book.isFavorite ? "true" : "false");
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
            
            BookEntry book = bookIndex.getBook(id);
            if (book.id != 0) {
                ESP_LOGI(TAG, "Deleting ID %d (%s)", id, book.path.c_str());
                unlink(book.path.c_str()); // Delete actual file
                bookIndex.removeBook(id); // Update index (also deletes metrics)
                
                gui.refreshLibrary();
                
                httpd_resp_send(req, "OK", 2);
                return ESP_OK;
            }
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

/* API to toggle book favorite status */
static esp_err_t api_favorite_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    char buf[100];
    size_t buf_len = sizeof(buf);

    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "id", param, sizeof(param)) == ESP_OK) {
            int id = atoi(param);
            bool currentFav = bookIndex.isFavorite(id);
            bookIndex.setFavorite(id, !currentFav);
            // Return new state
            char response[32];
            snprintf(response, sizeof(response), "{\"favorite\":%s}", !currentFav ? "true" : "false");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, response, strlen(response));
            return ESP_OK;
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
            "{\"fontSize\":%.1f, \"font\":\"%s\", \"lineSpacing\":%.1f, \"showImages\":%s, \"freeSpace\":%u, \"sdFreeSpace\":%llu, \"timezone\":\"%s\", "
            "\"deviceName\":\"%s\", \"buzzerEnabled\":%s, \"autoRotate\":%s}", 
            gui.getFontSize(), gui.getFont().c_str(), gui.getLineSpacing(), gui.isShowImages() ? "true" : "false",
            (unsigned int)getFreeSpace(), deviceHAL.getSDCardFreeSize(), tz,
            deviceHAL.getDeviceName(),
            gui.isBuzzerEnabled() ? "true" : "false",
            gui.isAutoRotateEnabled() ? "true" : "false");
#else
        snprintf(buf, sizeof(buf), 
            "{\"fontSize\":%.1f, \"font\":\"%s\", \"lineSpacing\":%.1f, \"showImages\":%s, \"freeSpace\":%u, \"timezone\":\"%s\", \"deviceName\":\"%s\"}", 
            gui.getFontSize(), gui.getFont().c_str(), gui.getLineSpacing(), gui.isShowImages() ? "true" : "false",
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

        char* pShowImages = strstr(buf, "\"showImages\":");
        if (pShowImages) {
            bool show = (strncmp(pShowImages + 13, "true", 4) == 0);
            gui.setShowImages(show);
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
    
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
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
    
#ifdef CONFIG_EBOOK_ENABLE_SD_CARD
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

/* Handler for updating the UI (index.html) */
static esp_err_t update_ui_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    
    // We expect the file content in the body
    FILE *f = fopen("/spiffs/index.html", "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open /spiffs/index.html for writing");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = (char*)malloc(4096);
    if (!buf) {
        fclose(f);
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
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, f);
        remaining -= received;
    }
    free(buf);
    fclose(f);
    
    httpd_resp_send(req, "UI Updated", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Main page handler */
static esp_err_t index_handler(httpd_req_t *req)
{
    WebServer::updateActivityTime();
    if (wifiManager.isConnected()) {
        // Check if custom index.html exists
        struct stat st;
        if (stat("/spiffs/index.html", &st) == 0) {
            FILE* f = fopen("/spiffs/index.html", "r");
            if (f) {
                char* buf = (char*)malloc(4096);
                if (buf) {
                    httpd_resp_set_type(req, "text/html");
                    size_t read;
                    while ((read = fread(buf, 1, 4096, f)) > 0) {
                        httpd_resp_send_chunk(req, buf, read);
                    }
                    httpd_resp_send_chunk(req, NULL, 0);
                    free(buf);
                    fclose(f);
                    return ESP_OK;
                }
                fclose(f);
            }
        }
        // Fallback to embedded HTML
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

        httpd_uri_t favorite_api_uri = {
            .uri       = "/api/favorite",
            .method    = HTTP_POST,
            .handler   = api_favorite_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &favorite_api_uri);

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
        
        httpd_uri_t update_ui_uri = {
            .uri       = "/update_ui",
            .method    = HTTP_POST,
            .handler   = update_ui_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &update_ui_uri);
        
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
