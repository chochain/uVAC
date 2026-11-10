/// -*- mode: c++ -*-
///
/// @file
/// @brief Web Server class implementation (esp_http_server v2.0.16)
///
#include "xsvr_actor.h"
#include "xserver2.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include <ctype.h>

static const char HTML_INDEX[] PROGMEM = R"XX(<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <title>uVAC Panel</title>
  <script src="https://unpkg.com/htmx.org@2.0.4"></script>
  <style>
    body { font-family:'Courier New', monospace; font-size:14px; background:#111; color:#0f0; padding:10px; margin:0; }
    #container { display: flex; height: 95vh; }
    #control   { flex: 0 1 auto; flex-direction: column; background-color:#222; }
    .abort-btn { background:#555; color:#aaa; border:none; font-weight:bold; cursor:not-allowed; width:100%; transition: 0.2s; }
    .abort-btn[data-run="true"] { background:#f00; color:#fff; cursor:pointer; }
    #log { flex: 0 0 58%; background-color:#222; border: 1px solid #333; overflow-y:auto; padding:10px; box-sizing:border-box; white-space: pre-wrap; }
    #tib-form  { flex: 0 0 40%; display: flex; flex-direction: column; }
    #tib-form form { flex: 1; display: flex; flex-direction: column; margin: 0; }
    #tib { flex: 1; background:#000; color:#0f0; border:1px solid #333; resize:none; padding:10px; font-family:inherit; font-size:inherit; }
    .cmd-entry { color: #0cf; margin-top: 5px; }
  </style>
  <script>
    // Define a tiny global HTMX extension to map a textarea straight into a raw HTTP POST body payload
    htmx.defineExtension('raw-text-body', {
      onEvent: function (name, evt) {
        if (name === "htmx:configRequest") {
          // Set text/plain MIME type header context variables
          evt.detail.headers['Content-Type'] = "text/plain"; // 
        }
      },
      encodeParameters: function(xhr, parameters, elt) {
        // Bypass key/value parsing completely, return the exact string inside the textarea
        const tibField = document.getElementById('tib-hidden-copy');
        return tibField ? tibField.value : "";
      }
    });
    const TAB = sessionStorage.getItem('uvac-tab') || (() => {
      const a = new Uint8Array(8); crypto.getRandomValues(a); // works on plain http
      const t = [...a].map(b => b.toString(16).padStart(2, '0')).join('');
      sessionStorage.setItem('uvac-tab', t);                  // per tab, survives reload
      return t;
    })();
    document.addEventListener('htmx:configRequest', e => { 
      e.detail.headers['X-Tab'] = TAB; 
    });
  </script>
</head>
<body>
  <!-- Hidden memory element to hold onto the code line during the HTMX request loop cycle -->
  <input type="hidden" id="tib-hidden-copy" value="" />
  <div id="staging" style="display:none;"
    hx-on::after-swap="
      const log = document.getElementById('log');
      const raw = this.innerText; 
      if (raw.length > 0) {
        log.appendChild(document.createTextNode(raw));
        this.innerText = ''; 
        log.scrollTop = log.scrollHeight;
      }
    "></div>
  <div id='container'>
    <div id='control'>
      <button id='abort' class="abort-btn"
        data-run  ="false"
        hx-target ="#log"
        hx-swap   ="beforeend"
        hx-post   = "/abort"
        hx-on::before-request="if (this.getAttribute('data-run') == 'false') event.preventDefault();"
        hx-on::response-error="
          const log = document.getElementById('log');
          const err = event.detail.xhr.statusText;
          log.innerHTML += `<div style='color:red;'>[INTERRUPT] ${err}</div>`;
          log.scrollTop = log.scrollHeight;
        ">X
      </button>
    </div>
    <div id='log' 
      hx-on::after-swap="
        if (this.scrollHeight - this.scrollTop - this.clientHeight < 300) this.scrollTop = this.scrollHeight
      ">uVAC v1.0 Initialized...<br/></div>
    <div id='tib-form'>
      <!-- 2. Apply our custom extension directly to the HTML Form layout -->
      <form hx-post='/execute' hx-target='#staging' hx-swap='innerHTML' hx-ext='raw-text-body'
        hx-on::before-request="
          // Back up the textarea code to our hidden landing field before clearing the UI input
          const tibValue = document.getElementById('tib').value;
          document.getElementById('tib-hidden-copy').value = tibValue;
          document.getElementById('tib').value='';
          const sid = Math.floor(Date.now() % 10000);   
          const btn = document.getElementById('abort');
          btn.setAttribute('data-sid', sid);
          btn.setAttribute('data-run', 'true');
        "
        hx-on::after-request="
          const btn = document.getElementById('abort');
          btn.setAttribute('data-run', 'false');
          document.getElementById('tib-hidden-copy').value = '';
          this.reset();
        "
        onsubmit="
          const log=document.getElementById('log'); 
          log.innerHTML += 
            '<div class=\'cmd-entry\'>&gt; ' +
            document.getElementById('tib').value.replace(/\n/g,'<br/>') + 
            '</div>'; 
          log.scrollTop = log.scrollHeight">
        <textarea id='tib' placeholder='Type Forth code here...'
          onkeydown="
            if (event.keyCode===13 && !event.shiftKey) { 
              event.preventDefault(); 
              htmx.trigger(this.form, 'submit'); 
            }"></textarea>
      </form>
    </div>
  </div>
</body>
</html>
)XX";

struct HttpContext {
    httpd_handle_t hd;
    int            fd;
};

static bool ct_eq(const char *a, const char *b) {          ///< constant-time compare, 32 chars
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static bool cookie_token(httpd_req_t *req, char out[33]) {  ///< parse "uvac=<32 hex>"
    size_t n = httpd_req_get_hdr_value_len(req, "Cookie");
    if (n == 0 || n > 400) return false;
    char *c = (char*)malloc(n + 1);
    if (!c) return false;
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", c, n + 1) == ESP_OK) {
        for (char *p = c; (p = strstr(p, "uvac=")); p += 5) {
            if (p != c && p[-1] != ' ' && p[-1] != ';') continue;
            const char *v = p + 5;
            int i = 0;
            while (i < 32 && isxdigit((unsigned char)v[i])) { out[i] = tolower(v[i]); i++; }
            if (i == 32 && (v[32] == '\0' || v[32] == ';' || v[32] == ' ')) {
                out[32] = '\0'; ok = true; break;
            }
        }
    }
    free(c);
    return ok;
}

static bool tab_header(httpd_req_t *req, char out[17]) {        ///< "X-Tab: <16 hex>"
    char v[24];
    if (httpd_req_get_hdr_value_str(req, "X-Tab", v, sizeof(v)) != ESP_OK) return false;
    int i = 0;
    while (i < 16 && isxdigit((unsigned char)v[i])) { out[i] = tolower(v[i]); i++; }
    if (i != 16 || v[16] != '\0') return false;
    out[16] = '\0';
    return true;
}

static void handle_overflow(void* arg, const char* failed_line) {
    auto* ctx = static_cast<HttpContext*>(arg);
    if (!ctx) return;

    const char* err = "<div style='color:#ffaa00;'>\r\n[SYSTEM ERROR] Pipeline Saturated: Script truncated, engine busy.<div>";
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%X\r\n", strlen(err));
    
    httpd_handle_t hd = ctx->hd;
    int            fd = ctx->fd;
    
    httpd_socket_send(hd, fd, hdr, strlen(hdr), 0);
    httpd_socket_send(hd, fd, err, strlen(err), 0);
    httpd_socket_send(hd, fd, "\r\n", 2, 0);
}

static esp_err_t handle_abort(httpd_req_t *req) {
    XServer *svr = static_cast<XServer*>(req->user_ctx);

    char tok[33], tab[17];
    if (!cookie_token(req, tok) || !tab_header(req, tab)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Session");
        return ESP_FAIL;
    }
    uint32_t key = svr->tab_key(tok, tab, false);           /// abort
    
    ActorMsg x { MSG_FORTH_ABORT, SERVER_ACTOR_ID, key };   ///< sid field carries the key
    x.buf[0] = '\0';
    Sys.send(x, 0, true);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Execution Aborted");
    
    return ESP_OK;
}

static esp_err_t handle_execute(httpd_req_t *req) {
    XServer *svr = static_cast<XServer*>(req->user_ctx);
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;

    DEBUG("xserver2 on core[%u] handle_execute fd=%d\n", xPortGetCoreID(), fd);
    
    char tok[33], tab[17];
    if (!cookie_token(req, tok) || !tab_header(req, tab)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No session (reload page)");
        return ESP_FAIL;
    }
    uint32_t key = svr->tab_key(tok, tab, true);
    
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "sid", v, sizeof(v)) == ESP_OK) key = strtoul(v, nullptr, 10);
    if (!key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing tab id");
        return ESP_FAIL;
    }
    char* psram_buf = nullptr;
    if (!svr->read_form_psram(req, &psram_buf)) return ESP_FAIL;
    svr->conn(key, fd, req->handle, psram_buf);
    
    return ESP_OK;
}

// Explicitly updated to pull streaming data directly across the bus into PSRAM
bool XServer::read_form_psram(httpd_req_t *req, char **out_psram_ptr) {
    int total = req->content_len;
    DEBUG("xserver2 content len=%d\n", total);
    if (total <= 0 || total >= 8192) { 
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Payload Length");
        return false;
    }

    // 1. Allocate a flat character buffer directly on the 8 MB Octal PSRAM chip
    char* raw = (char*) heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == nullptr) {
        LOG("[ERROR] Failed to allocate memory capsule (%x) in PSRAM!\n", total+1);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of External Memory");
        return false;
    }

    // 2. Stream packets straight out of the LwIP network socket into the PSRAM destination pointer
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, raw + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            heap_caps_free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Network Stream Aborted");
            return false;
        }
        received += r;
    }
    
    // 3. Simple null-termination. Pristine RAW plain-text content ready for the Coordinator iterator!
    raw[total] = '\0';
    DEBUG("xserver2 raw = '%s'\n", raw);

    *out_psram_ptr = raw;
    return true;
}

void XServer::conn(uint32_t key, int sockfd, httpd_handle_t handle, char *buf) {
    uint32_t sid = Sys.alloc_id();            ///< per request
    _coor->register_new_conn(key, sid, sockfd, handle, buf, strlen(buf));
}

bool XServer::begin(ServerActor *actor, int priority) {
    if (!actor) return false;
    
    _coor = actor;
    if (!_httpd) setup();
    
    return true;
}

void XServer::setup() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _password);
    while (WiFi.status() != WL_CONNECTED) {
        LOG("%c", '.');
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = _port;
    config.core_id     = 0;
    config.stack_size  = 8192; 

    if (httpd_start(&_httpd, &config) != ESP_OK) return;

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) {
            char tok[33], ck[96];
            if (!cookie_token(req, tok)) {
                uint8_t r[16];
                esp_fill_random(r, sizeof(r));                    ///< hardware RNG, 128 bit
                for (int i = 0; i < 16; i++) snprintf(tok + 2*i, 3, "%02x", r[i]);
            }
            snprintf(ck, sizeof(ck), "uvac=%s; Path=/; HttpOnly; SameSite=Strict", tok);
            httpd_resp_set_hdr(req, "Set-Cookie", ck);
            httpd_resp_set_type(req, "text/html");
            
            return httpd_resp_send(req, (char*)HTML_INDEX, HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpd, &root_uri);

    httpd_uri_t exec_uri = {
        .uri = "/execute",
        .method = HTTP_POST,
        .handler = handle_execute,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpd, &exec_uri);

    httpd_uri_t abort_uri = {
        .uri = "/abort",
        .method = HTTP_POST,
        .handler = handle_abort,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpd, &abort_uri);
    
    LOG("\nxserver2> live at http://%s on core[%u]\n",
        WiFi.localIP().toString().c_str(), config.core_id);
}

uint32_t XServer::tab_key(const char *tok, const char *tab, bool create) {
    Tab *lru = &_tabs[0];
    for (auto &t : _tabs) {
        if (t.key && ct_eq(t.tok, tok) && !strcmp(t.tab, tab)) { t.last = ++_tick; return t.key; }
        if (t.last < lru->last) lru = &t;
    }
    if (!create) return 0;
    
    memcpy(lru->tok, tok, 33);
    memcpy(lru->tab, tab, 17);
    lru->key  = Sys.alloc_id();
    lru->last = ++_tick;
    
    return lru->key;
}
