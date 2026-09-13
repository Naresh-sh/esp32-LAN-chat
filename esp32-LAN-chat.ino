b #include <WiFi.h>
#include <WebServer.h>

// IMPORTANT (traffic capacity): this project uses the Links2004/arduinoWebSockets
// library. Its default max simultaneous clients is small (often 4). Since we're
// moving to ESP32 specifically to handle more clients, raise the ceiling BEFORE
// the include below by adding this to your project's build flags, or edit
// WebSocketsServer.h in the library and bump WEBSOCKETS_SERVER_CLIENT_MAX.
// Example (platformio.ini):
//   build_flags = -D WEBSOCKETS_SERVER_CLIENT_MAX=20
#ifndef WEBSOCKETS_SERVER_CLIENT_MAX
#define WEBSOCKETS_SERVER_CLIENT_MAX 20
#endif
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// ── AP Config ──
const char* AP_SSID = "FreeLANChat";
const char* AP_PASS = "password";      // change this — min 8 chars for WPA2

// ── Chat login password (change here) ──
const char* CHAT_LOGIN_PASS = "112211";

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// ── Users ──
// ESP32 has far more RAM/heap than ESP8266, so we can safely track more
// simultaneous clients. Keep this in sync with WEBSOCKETS_SERVER_CLIENT_MAX above.
#define MAX_CLIENTS 20
struct User { String name; String ip; };
User activeUsers[MAX_CLIENTS];

// ── Admin ──
String adminIP = "";
bool   adminSet = false;

// ── Router / STA state ──
bool   staConnecting   = false;
bool   staConnected    = false;
String staSSID         = "";
unsigned long staConnectStart = 0;
#define STA_TIMEOUT 15000UL

// ── Message History ──
// Bumped up since ESP32 has the RAM headroom for it.
#define MAX_HISTORY 150
struct StoredMsg { String json; unsigned long ts; };
StoredMsg msgHistory[MAX_HISTORY];
int msgHistoryCount = 0;

// ── File transfer (send/receive files in chat) ──
// Files are streamed straight to flash (LittleFS) in small chunks, so RAM
// use stays tiny no matter the file size. MAX_FILE_SIZE is enforced on the
// device, independent of whatever the browser checks — never trust the client.
#define MAX_FILE_SIZE (10UL * 1024UL * 1024UL)   // 10 MB hard cap per file
#define FILES_DIR "/files"

File   uploadFile;
String uploadStoredPath;   // path saved on flash, e.g. /files/171234_5566.jpg
String uploadOrigName;     // original filename as sent by the browser
size_t uploadBytes    = 0;
bool   uploadOverLimit = false;
bool   uploadOk        = false;

String guessContentType(const String& path){
  if(path.endsWith(".png"))  return "image/png";
  if(path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if(path.endsWith(".gif"))  return "image/gif";
  if(path.endsWith(".webp")) return "image/webp";
  if(path.endsWith(".pdf"))  return "application/pdf";
  if(path.endsWith(".txt"))  return "text/plain";
  if(path.endsWith(".mp3"))  return "audio/mpeg";
  if(path.endsWith(".mp4"))  return "video/mp4";
  if(path.endsWith(".zip"))  return "application/zip";
  return "application/octet-stream";
}

void storeMessage(const String& json){
  if(msgHistoryCount < MAX_HISTORY){
    msgHistory[msgHistoryCount].json = json;
    msgHistory[msgHistoryCount].ts   = millis();
    msgHistoryCount++;
  } else {
    for(int i=0;i<MAX_HISTORY-1;i++) msgHistory[i]=msgHistory[i+1];
    msgHistory[MAX_HISTORY-1].json = json;
    msgHistory[MAX_HISTORY-1].ts   = millis();
  }
}

void sendHistoryToClient(uint8_t num){
  unsigned long now = millis();
  String out = "{\"type\":\"history\",\"messages\":[";
  bool first = true;
  for(int i=0;i<msgHistoryCount;i++){
    if(now - msgHistory[i].ts <= 60000UL){
      if(!first) out+=",";
      out += msgHistory[i].json;
      first = false;
    }
  }
  out += "]}";
  webSocket.sendTXT(num, out);
}

// ── Broadcast helpers ──
void broadcastUserList(){
  JsonDocument doc;
  doc["type"] = "userList";
  JsonArray users = doc["users"].to<JsonArray>();
  for(int i=0;i<MAX_CLIENTS;i++){
    if(activeUsers[i].name!=""){
      JsonObject u = users.add<JsonObject>();
      u["name"] = activeUsers[i].name;
      u["ip"]   = activeUsers[i].ip;
    }
  }
  String out; serializeJson(doc,out);
  webSocket.broadcastTXT(out);
}

void broadcastRouterStatus(){
  JsonDocument doc;
  doc["type"]      = "routerStatus";
  doc["connected"] = staConnected;
  doc["ssid"]      = staConnected ? staSSID : "";
  doc["ip"]        = staConnected ? WiFi.localIP().toString() : "";
  String out; serializeJson(doc,out);
  webSocket.broadcastTXT(out);
}

void sendAdminFlag(uint8_t num, bool isAdmin){
  String msg = "{\"type\":\"adminFlag\",\"isAdmin\":";
  msg += isAdmin ? "true" : "false";
  msg += "}";
  webSocket.sendTXT(num, msg);
}

// ════════════════════════════════════════════════════════
//  HTTP login verify endpoint
//  Frontend POST /auth  { "user":"...", "pass":"..." }
//  Returns { "ok": true/false }
// ════════════════════════════════════════════════════════
void handleAuth(){
  if(!server.hasArg("plain")){
    server.send(400,"application/json","{\"ok\":false}");
    return;
  }
  JsonDocument doc;
  if(deserializeJson(doc, server.arg("plain"))){
    server.send(400,"application/json","{\"ok\":false}");
    return;
  }
  String pass = doc["pass"].as<String>();
  bool ok = (pass == String(CHAT_LOGIN_PASS));
  server.send(200,"application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ── HTTP: scan networks (admin only) ──
void handleScan(){
  String callerIP = server.client().remoteIP().toString();
  if(callerIP != adminIP){ server.send(403,"application/json","{\"error\":\"Not admin\"}"); return; }
  int n = WiFi.scanNetworks(false, false);
  String json = "[";
  for(int i=0;i<n;i++){
    if(i>0) json+=",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + WiFi.RSSI(i)
          + ",\"enc\":" + (WiFi.encryptionType(i)==WIFI_AUTH_OPEN?"false":"true") + "}";
  }
  json += "]";
  server.send(200,"application/json",json);
  WiFi.scanDelete();
}

// ── HTTP: connect to router (admin only) ──
void handleConnect(){
  String callerIP = server.client().remoteIP().toString();
  if(callerIP != adminIP){ server.send(403,"application/json","{\"error\":\"Not admin\"}"); return; }
  if(!server.hasArg("plain")){ server.send(400,"application/json","{\"error\":\"No body\"}"); return; }
  JsonDocument doc;
  if(deserializeJson(doc, server.arg("plain"))){ server.send(400,"application/json","{\"error\":\"Bad JSON\"}"); return; }
  String ssid = doc["ssid"].as<String>();
  String pass = doc["pass"].as<String>();
  server.send(200,"application/json","{\"status\":\"connecting\"}");

  // Clear any half-open STA session left over from a scan or a previous
  // attempt before starting a fresh one — beginning on top of a stuck STA
  // state is the other common reason connect never finishes.
  WiFi.disconnect(true, false);
  delay(100);
  if(pass.length() == 0){
    WiFi.begin(ssid.c_str());              // open network: no password arg at all
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  staSSID         = ssid;
  staConnecting   = true;
  staConnected    = false;
  staConnectStart = millis();
  String notif = "{\"type\":\"routerStatus\",\"connected\":false,\"ssid\":\""+ssid+"\",\"connecting\":true,\"ip\":\"\"}";
  webSocket.broadcastTXT(notif);
}

// ── HTTP: disconnect from router (admin only) ──
void handleDisconnect(){
  String callerIP = server.client().remoteIP().toString();
  if(callerIP != adminIP){ server.send(403,"application/json","{\"error\":\"Not admin\"}"); return; }
  WiFi.disconnect(true, false);   // drop STA link only — stay in AP_STA mode
  staConnected  = false;
  staConnecting = false;
  staSSID       = "";
  broadcastRouterStatus();
  server.send(200,"application/json","{\"status\":\"disconnected\"}");
}

// ════════════════════════════════════════════════════════
//  HTTP file upload — POST /upload?from=..&to=..&targetIP=..&time=..
//  (multipart/form-data, field name "file")
//  Two callbacks: handleFileUpload runs per-chunk WHILE the body is
//  streaming in; handleUploadComplete runs once at the end to send the
//  HTTP response and broadcast the file to chat over the websocket.
// ════════════════════════════════════════════════════════
void handleFileUpload(){
  HTTPUpload& upload = server.upload();

  if(upload.status == UPLOAD_FILE_START){
    uploadOverLimit = false;
    uploadOk        = false;
    uploadBytes     = 0;
    uploadOrigName  = upload.filename;
    if(uploadOrigName.length() == 0) uploadOrigName = "file";

    String ext = "";
    int dot = uploadOrigName.lastIndexOf('.');
    if(dot >= 0) ext = uploadOrigName.substring(dot);
    // Unique name on flash so two uploads never collide.
    uploadStoredPath = String(FILES_DIR) + "/" + String(millis()) + "_" + String(random(1000,9999)) + ext;

    uploadFile = LittleFS.open(uploadStoredPath, "w");
    uploadOk   = (bool)uploadFile;
    if(!uploadOk) Serial.println("upload: failed to open file on flash");
  }
  else if(upload.status == UPLOAD_FILE_WRITE){
    uploadBytes += upload.currentSize;
    if(uploadBytes > MAX_FILE_SIZE){
      // Over the cap — stop writing further bytes but keep consuming the
      // request body so the connection stays valid until it ends.
      uploadOverLimit = true;
    } else if(uploadOk){
      uploadFile.write(upload.buf, upload.currentSize);
    }
  }
  else if(upload.status == UPLOAD_FILE_END){
    if(uploadOk) uploadFile.close();
    if(uploadOverLimit && uploadOk){
      LittleFS.remove(uploadStoredPath);   // discard oversized file, don't keep partial junk
    }
  }
}

void handleUploadComplete(){
  if(uploadOverLimit){
    server.send(413, "application/json", "{\"ok\":false,\"error\":\"File is over the 10MB limit\"}");
    return;
  }
  if(!uploadOk){
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"Upload failed\"}");
    return;
  }

  String from     = server.hasArg("from")     ? server.arg("from")     : "Unknown";
  String to       = server.hasArg("to")       ? server.arg("to")       : "all";
  String targetIP = server.hasArg("targetIP") ? server.arg("targetIP") : "";
  String timeStr  = server.hasArg("time")     ? server.arg("time")     : "";
  String fromIP   = server.client().remoteIP().toString();

  JsonDocument doc;
  doc["type"]     = "file";
  doc["from"]     = from;
  doc["fromIP"]   = fromIP;
  doc["to"]       = to;
  doc["targetIP"] = targetIP;
  doc["time"]     = timeStr;
  doc["fileName"] = uploadOrigName;
  doc["fileSize"] = (uint32_t)uploadBytes;
  doc["url"]      = uploadStoredPath;
  doc["msgID"]    = "file_" + String(millis()) + "_" + String(random(1000,9999));

  String out; serializeJson(doc, out);
  storeMessage(out);              // so late joiners in the 60s history window see it too
  webSocket.broadcastTXT(out);

  server.send(200, "application/json", "{\"ok\":true}");
}

// ── Serve uploaded files back for download: GET /files/<name> ──
void handleNotFound(){
  String uri = server.uri();
  if(uri.startsWith(String(FILES_DIR) + "/") && LittleFS.exists(uri)){
    File f = LittleFS.open(uri, "r");
    server.streamFile(f, guessContentType(uri));
    f.close();
    return;
  }
  server.send(404, "text/plain", "Not found");
}

// ── WebSocket event ──
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length){
  if(num >= MAX_CLIENTS) return; // safety: don't write past the array

  if(type == WStype_CONNECTED){
    String ip = webSocket.remoteIP(num).toString();
    if(!adminSet){ adminIP = ip; adminSet = true; }
  }
  else if(type == WStype_TEXT){
    JsonDocument doc;
    if(deserializeJson(doc,payload)) return;
    String t = doc["type"];
    if(t=="login"){
      String ip = webSocket.remoteIP(num).toString();
      activeUsers[num].name = doc["user"].as<String>();
      activeUsers[num].ip   = ip;
      sendHistoryToClient(num);
      broadcastUserList();
      sendAdminFlag(num, (ip == adminIP));
      broadcastRouterStatus();
    }
    else if(t=="chat"){
      doc["fromIP"] = webSocket.remoteIP(num).toString();
      String out; serializeJson(doc,out);
      storeMessage(out);
      webSocket.broadcastTXT(out);
    }
    else if(t=="delete"||t=="reaction"){
      doc["fromIP"] = webSocket.remoteIP(num).toString();
      String out; serializeJson(doc,out);
      storeMessage(out);
      webSocket.broadcastTXT(out);
    }
    else {
      doc["fromIP"] = webSocket.remoteIP(num).toString();
      String out; serializeJson(doc,out);
      webSocket.broadcastTXT(out);
    }
  }
  else if(type == WStype_DISCONNECTED){
    activeUsers[num].name = "";
    broadcastUserList();
  }
}

// ════════════════════════════════════════════════════════
//  HTML PAGE — clean, simple UI (password check via /auth)
// ════════════════════════════════════════════════════════
const char chatPage[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Free LAN Chat</title>
<style>
:root{
  --bg:#f4f5f7; --panel:#ffffff; --border:#e3e5e9; --text:#1f2430; --muted:#6b7280;
  --accent:#2563eb; --accent-dark:#1d4ed8; --accent-soft:#e8f0fe;
  --sent:#e8f0fe; --received:#f1f2f4; --surface:#fafafa; --hover:#f1f2f4;
  --danger:#dc2626; --radius:12px; --vh:1vh;
}
html.dark{
  --bg:#0f1115; --panel:#181b21; --border:#2a2e37; --text:#e5e7eb; --muted:#9199a8;
  --accent:#3b82f6; --accent-dark:#60a5fa; --accent-soft:#1e3a5f;
  --sent:#1e3a5f; --received:#232730; --surface:#1f232b; --hover:#242832;
  --danger:#f87171;
}
*{margin:0;padding:0;box-sizing:border-box}
body{
  background:var(--bg);font-family:'Segoe UI',system-ui,-apple-system,sans-serif;
  height:100vh;height:calc(var(--vh) * 100);display:flex;align-items:center;justify-content:center;
  color:var(--text);transition:background 0.2s,color 0.2s;
}
#login-screen{
  position:fixed;width:100%;height:100%;background:var(--bg);
  z-index:1000;display:flex;align-items:center;justify-content:center;
}
.login-box{
  background:var(--panel);padding:36px 32px;border-radius:var(--radius);
  border:1px solid var(--border);text-align:center;width:320px;max-width:90vw;
  box-shadow:0 4px 20px rgba(0,0,0,0.06);
}
.login-box input{
  width:100%;margin-top:12px;padding:11px 14px;border-radius:8px;
  border:1px solid var(--border);background:var(--surface);color:var(--text);outline:none;font-size:14px;
}
.login-box input:focus{border-color:var(--accent);background:var(--panel)}
.login-btn{
  margin-top:16px;width:100%;padding:11px;border-radius:8px;
  background:var(--accent);color:white;border:none;
  font-weight:600;font-size:14px;cursor:pointer;transition:background 0.15s;
}
.login-btn:hover{background:var(--accent-dark)}
.login-btn:disabled{opacity:0.5;cursor:not-allowed}
#loginErr{
  display:none;color:var(--danger);font-size:12px;margin-top:10px;
  background:#fef2f2;border:1px solid #fecaca;border-radius:6px;padding:7px 10px;
}
#main-ui{
  display:none;width:98%;max-width:1200px;height:92vh;
  background:var(--panel);border-radius:var(--radius);overflow:hidden;
  flex-direction:column;border:1px solid var(--border);box-shadow:0 4px 24px rgba(0,0,0,0.06);
}
.main-body{display:flex;flex:1;overflow:hidden;min-height:0}
#topBar{
  display:flex;align-items:center;justify-content:space-between;
  background:var(--panel);border-bottom:1px solid var(--border);
  padding:10px 16px;flex-shrink:0;
}
#topBarLeft{color:var(--text);font-weight:700;font-size:14px}
#topBarRight{display:flex;align-items:center;gap:8px}
#adminScanBtn{
  display:none;background:var(--panel);border:1px solid var(--accent);color:var(--accent);
  border-radius:20px;padding:5px 12px;font-size:11px;cursor:pointer;
  font-weight:600;white-space:nowrap;transition:all 0.15s;
}
#adminScanBtn:hover{background:var(--accent);color:#fff}
#themeToggle{
  background:#f1f2f4;border:1px solid var(--border);color:var(--text);
  width:30px;height:30px;border-radius:50%;cursor:pointer;font-size:14px;
  display:flex;align-items:center;justify-content:center;transition:all .15s;flex-shrink:0;
}
#themeToggle:hover{background:#e7e9ec}
html.dark #themeToggle{background:#232730}
html.dark #themeToggle:hover{background:#2a2e37}
#routerPill{
  display:flex;align-items:center;gap:5px;background:var(--surface);
  border:1px solid var(--border);border-radius:20px;padding:5px 12px;
  font-size:11px;color:var(--muted);transition:all 0.3s;white-space:nowrap;
}
#routerPill.connected{border-color:#16a34a;color:#16a34a}
#routerPill.connecting{border-color:#d97706;color:#d97706}
#routerDot{width:6px;height:6px;border-radius:50%;background:#c1c5cc;flex-shrink:0}
#routerPill.connected #routerDot{background:#16a34a}
#routerPill.connecting #routerDot{background:#d97706}
#scanModal{
  display:none;position:fixed;inset:0;background:rgba(0,0,0,0.35);
  z-index:2000;align-items:center;justify-content:center;
}
#scanModal.open{display:flex}
.scan-box{
  background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  width:340px;max-width:90vw;max-height:78vh;display:flex;flex-direction:column;
  box-shadow:0 10px 30px rgba(0,0,0,0.15);overflow:hidden;
}
.scan-header{
  padding:14px 16px;background:var(--surface);color:var(--text);font-weight:700;
  font-size:13px;display:flex;align-items:center;justify-content:space-between;
  flex-shrink:0;border-bottom:1px solid var(--border);
}
.scan-close{cursor:pointer;font-size:17px;color:var(--muted)}
.scan-close:hover{color:var(--text)}
#scanList{flex:1;overflow-y:auto;padding:8px}
.scan-item{
  padding:10px 12px;border-radius:8px;cursor:pointer;transition:0.15s;
  display:flex;align-items:center;justify-content:space-between;
  border:1px solid transparent;margin-bottom:4px;color:var(--text);
}
.scan-item:hover{background:var(--hover);border-color:var(--border)}
.scan-ssid{font-size:13px;font-weight:600;display:flex;align-items:center;gap:5px}
.scan-rssi{font-size:10px;color:var(--muted);margin-top:2px}
.scan-loading{padding:30px;text-align:center;color:var(--muted);font-size:13px}
#disconnectBtn{
  display:none;margin:8px;padding:9px;background:#fef2f2;border:1px solid #fecaca;
  color:var(--danger);border-radius:8px;cursor:pointer;font-size:12px;font-weight:600;
  text-align:center;transition:all 0.15s;flex-shrink:0;
}
#disconnectBtn:hover{background:#fee2e2}
#passModal{
  display:none;position:fixed;inset:0;background:rgba(0,0,0,0.35);
  z-index:2100;align-items:center;justify-content:center;
}
#passModal.open{display:flex}
.pass-box{
  background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  width:300px;max-width:90vw;padding:24px;box-shadow:0 10px 30px rgba(0,0,0,0.15);text-align:center;
}
.pass-box h3{color:var(--text);margin-bottom:14px;font-size:13px;line-height:1.4}
.pass-box input{
  width:100%;padding:9px 12px;border-radius:8px;border:1px solid var(--border);
  background:var(--surface);color:var(--text);outline:none;font-size:14px;margin-bottom:12px;
}
.pass-box input:focus{border-color:var(--accent)}
.pbtn-row{display:flex;gap:8px}
.pbtn{flex:1;padding:9px;border-radius:8px;border:none;cursor:pointer;font-weight:600;font-size:13px;transition:all 0.15s}
.pbtn.ok{background:var(--accent);color:white}
.pbtn.ok:hover{background:var(--accent-dark)}
.pbtn.cancel{background:var(--hover);color:var(--muted);border:1px solid var(--border)}
.pbtn.cancel:hover{background:var(--border)}
.sidebar{width:28%;min-width:220px;background:var(--surface);border-right:1px solid var(--border);display:flex;flex-direction:column;flex-shrink:0}
.sidebar-header{
  padding:14px 18px;background:var(--panel);color:var(--text);font-weight:700;
  border-bottom:1px solid var(--border);font-size:13px;display:flex;align-items:center;gap:8px;
}
#userList{flex:1;overflow-y:auto}
.contact{
  padding:12px 14px;border-bottom:1px solid #eee;cursor:pointer;transition:0.15s;
  display:flex;align-items:center;justify-content:space-between;color:var(--text);
}
.contact:hover{background:var(--hover)}
.contact.active{background:var(--accent-soft);border-left:3px solid var(--accent)}
.contact-name{display:flex;align-items:center;gap:8px;font-size:13px}
.online-dot{width:7px;height:7px;border-radius:50%;background:#16a34a;flex-shrink:0}
.badge{background:var(--accent);color:white;border-radius:50%;padding:2px 6px;font-size:10px;font-weight:700;display:none;min-width:18px;text-align:center}
.chat-container{flex:1;display:flex;flex-direction:column;background:var(--panel);min-width:0}
#chat-title{
  padding:14px 18px;background:var(--panel);border-bottom:1px solid var(--border);
  color:var(--text);font-weight:700;font-size:15px;display:flex;align-items:center;gap:10px;
  flex-shrink:0;
}
#backToContacts{display:none;cursor:pointer;font-size:20px;margin-right:6px;color:var(--muted)}
#typing-indicator{font-size:11px;color:var(--muted);font-weight:400;font-style:italic;margin-left:auto}
#messages{flex:1;padding:16px;overflow-y:auto;display:flex;flex-direction:column;gap:8px;min-height:0}
.msg{
  padding:9px 13px;border-radius:14px;max-width:72%;font-size:13.5px;
  color:var(--text);position:relative;word-break:break-word;line-height:1.5;
  animation:popIn 0.15s ease;
}
@keyframes popIn{from{transform:scale(0.96);opacity:0}to{transform:scale(1);opacity:1}}
@keyframes delAnim{from{transform:scale(1);opacity:1}to{transform:scale(0.7);opacity:0}}
.msg.deleting{animation:delAnim 0.25s ease forwards}
.sent{align-self:flex-end;background:var(--sent);border-bottom-right-radius:3px}
.received{align-self:flex-start;background:var(--received);border-bottom-left-radius:3px}
.msg-sender{font-size:10px;color:var(--accent);font-weight:700;margin-bottom:2px}
.msg-meta{font-size:9px;color:var(--muted);margin-top:4px;display:flex;align-items:center;justify-content:flex-end;gap:6px}
.msg-text{font-size:14px}
.deleted-msg{font-style:italic;color:#9ca3af;font-size:12px}
.msg:hover .msg-actions{opacity:1}
.msg-actions{position:absolute;top:-18px;right:4px;display:flex;gap:4px;opacity:0;transition:opacity 0.15s}
.action-btn{
  background:var(--panel);border:1px solid var(--border);border-radius:8px;
  padding:2px 7px;font-size:11px;cursor:pointer;color:var(--muted);white-space:nowrap;transition:all 0.15s;
  box-shadow:0 1px 3px rgba(0,0,0,0.08);
}
.action-btn:hover{background:var(--hover);color:var(--text)}
.action-btn.del{color:var(--danger)}
.action-btn.del:hover{background:#fef2f2}
.reactions{display:flex;flex-wrap:wrap;gap:3px;margin-top:5px}
.reaction-badge{
  background:var(--panel);border:1px solid var(--border);border-radius:12px;
  padding:2px 7px;font-size:12px;cursor:pointer;transition:background 0.15s;
}
.reaction-badge:hover{background:var(--hover)}
.input-area{
  padding:12px 14px;background:var(--panel);display:flex;
  align-items:center;gap:8px;position:relative;border-top:1px solid var(--border);flex-shrink:0;
}
#emojiBtn{
  width:35px;height:35px;flex-shrink:0;background:var(--hover);border:1px solid var(--border);
  border-radius:50%;font-size:18px;cursor:pointer;display:flex;align-items:center;
  justify-content:center;transition:all 0.15s;user-select:none;
}
#emojiBtn:hover{background:var(--border)}
#attachBtn{
  width:35px;height:35px;flex-shrink:0;background:var(--hover);border:1px solid var(--border);
  border-radius:50%;font-size:16px;cursor:pointer;display:flex;align-items:center;
  justify-content:center;transition:all 0.15s;user-select:none;
}
#attachBtn:hover{background:var(--border)}
.file-chip{display:flex;align-items:center;gap:9px;min-width:180px}
.file-icon{font-size:22px;flex-shrink:0}
.file-info{flex:1;min-width:0}
.file-name{font-size:13px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file-size{font-size:10px;color:var(--muted);margin-top:1px}
.file-dl{
  flex-shrink:0;width:28px;height:28px;border-radius:50%;background:var(--accent);
  color:#fff;display:flex;align-items:center;justify-content:center;text-decoration:none;
  font-size:14px;transition:background 0.15s;
}
.file-dl:hover{background:var(--accent-dark)}
#emojiPicker{
  display:none;position:absolute;bottom:60px;left:12px;width:305px;max-width:90vw;
  background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:10px;
  z-index:999;box-shadow:0 6px 25px rgba(0,0,0,0.15);
}
.emoji-tabs{display:flex;gap:4px;margin-bottom:8px;flex-wrap:wrap}
.etab{padding:3px 7px;border-radius:8px;cursor:pointer;font-size:15px;background:var(--hover);border:1px solid var(--border);transition:background 0.15s}
.etab:hover,.etab.active{background:var(--accent-soft);border-color:var(--accent)}
.emoji-search{width:100%;padding:5px 10px;border-radius:8px;border:1px solid var(--border);background:var(--surface);color:var(--text);outline:none;margin-bottom:8px;font-size:13px}
.emoji-search:focus{border-color:var(--accent)}
.emoji-grid{display:flex;flex-wrap:wrap;gap:3px;max-height:155px;overflow-y:auto}
.emoji-item{font-size:21px;cursor:pointer;padding:4px;border-radius:6px;transition:all 0.15s;user-select:none}
.emoji-item:hover{background:var(--hover);transform:scale(1.15)}
#msgInput{
  flex:1;padding:10px 15px;border-radius:25px;border:1px solid var(--border);
  background:var(--surface);color:var(--text);outline:none;font-size:14px;transition:border 0.15s;
}
#msgInput:focus{border-color:var(--accent);background:var(--panel)}
#sendBtn{
  padding:10px 18px;background:var(--accent);color:white;
  border:none;border-radius:25px;cursor:pointer;font-weight:600;flex-shrink:0;
  transition:background 0.15s;font-size:13px;
}
#sendBtn:hover{background:var(--accent-dark)}
#histBanner{
  display:none;background:#fffbeb;border-bottom:1px solid #fde68a;
  padding:6px 16px;font-size:11px;color:#92400e;text-align:center;flex-shrink:0;
}
::-webkit-scrollbar{width:5px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:#d1d5db;border-radius:10px}

/* ── Mobile layout ──
   Desktop keeps sidebar + chat side-by-side. On narrow screens the sidebar
   becomes the default full-width view; tapping a contact slides the chat in
   full-width instead, with a back arrow to return. This is what was missing
   before — the old CSS never adapted below the desktop breakpoint. */
@media (max-width:768px){
  body{padding:0}
  #main-ui{
    width:100%;height:calc(var(--vh) * 100);max-width:none;
    border-radius:0;border:none;
  }
  #topBarLeft{font-size:13px}
  #adminScanBtn{font-size:10px;padding:4px 9px}
  .main-body{position:relative}
  .sidebar{
    position:absolute;inset:0;width:100%;min-width:0;z-index:5;
    transition:transform .25s ease;
  }
  .chat-container{
    position:absolute;inset:0;width:100%;z-index:4;
    transform:translateX(100%);transition:transform .25s ease;
  }
  .main-body.show-chat .sidebar{transform:translateX(-100%)}
  .main-body.show-chat .chat-container{transform:translateX(0)}
  #backToContacts{display:inline-block}
  .msg{max-width:85%}
  #emojiPicker{left:8px;bottom:64px}
  /* prevent iOS auto-zoom on focus by keeping inputs at 16px+ */
  #msgInput,.login-box input,.pass-box input,.emoji-search{font-size:16px}
}
</style>
</head>
<body>

<div id="login-screen">
  <div class="login-box">
    <div style="font-size:34px;margin-bottom:8px">💬</div>
    <h2 style="color:var(--text);margin-bottom:5px;font-size:19px">Free LAN Chat</h2>
    <p style="color:var(--muted);font-size:12px;margin-bottom:14px">Local network messenger</p>
    <input type="text" id="userName" placeholder="Enter your name"
           onkeydown="if(event.key==='Enter')document.getElementById('passCode').focus()">
    <input type="password" id="passCode" placeholder="Enter password"
           onkeydown="if(event.key==='Enter')login()">
    <div id="loginErr"></div>
    <button class="login-btn" id="loginBtn" onclick="login()">Join Chat</button>
  </div>
</div>

<div id="main-ui">
  <div id="topBar">
    <div id="topBarLeft">Free LAN Chat</div>
    <div id="topBarRight">
      <button id="adminScanBtn" onclick="openScanModal()">Scan & Connect Router</button>
      <div id="routerPill">
        <div id="routerDot"></div>
        <span id="routerLabel">No Router</span>
      </div>
      <div id="themeToggle" onclick="toggleTheme()" title="Toggle dark/light">🌙</div>
    </div>
  </div>
  <div id="histBanner">Message history restored (last 60 seconds)</div>
  <div class="main-body">
    <div class="sidebar">
      <div class="sidebar-header">Chats</div>
      <div id="userList"></div>
    </div>
    <div class="chat-container">
      <div id="chat-title">
        <span id="backToContacts" onclick="closeChatMobile()">‹</span>
        <span id="chatTitleText">Group Chat</span>
        <span id="typing-indicator"></span>
      </div>
      <div id="messages"></div>
      <div class="input-area">
        <div id="attachBtn" onclick="document.getElementById('fileInput').click()" title="Send a file (max 10MB)">📎</div>
        <input type="file" id="fileInput" style="display:none" onchange="handleFileSelect(this.files)">
        <div id="emojiBtn" onclick="toggleEmojiPicker()" title="Emojis">🙂</div>
        <div id="emojiPicker">
          <div class="emoji-tabs" id="emojiTabs"></div>
          <input class="emoji-search" type="text" id="emojiSearch"
                 placeholder="Search emoji..." oninput="filterEmojis(this.value)">
          <div class="emoji-grid" id="emojiGrid"></div>
        </div>
        <input type="text" id="msgInput"
               placeholder="Write a message (Enter to send)" autocomplete="off">
        <button id="sendBtn" onclick="sendMsg()">Send</button>
      </div>
    </div>
  </div>
</div>

<div id="scanModal">
  <div class="scan-box">
    <div class="scan-header">
      <span>Nearby Networks</span>
      <span class="scan-close" onclick="closeScanModal()">✕</span>
    </div>
    <div id="scanList"><div class="scan-loading">Scanning...</div></div>
    <div id="disconnectBtn" onclick="doDisconnect()">Disconnect from Router</div>
  </div>
</div>

<div id="passModal">
  <div class="pass-box">
    <h3 id="passModalTitle">Enter WiFi Password</h3>
    <input type="password" id="routerPassInput" placeholder="WiFi password"
           onkeydown="if(event.key==='Enter')doConnect()">
    <div class="pbtn-row">
      <button class="pbtn cancel" onclick="closePassModal()">Cancel</button>
      <button class="pbtn ok" onclick="doConnect()">Connect</button>
    </div>
  </div>
</div>

<script>
var AudioCtx=window.AudioContext||window.webkitAudioContext,actx=null;
function getACtx(){if(!actx)actx=new AudioCtx();return actx;}
function soundSent(){try{var a=getACtx(),o=a.createOscillator(),g=a.createGain();o.connect(g);g.connect(a.destination);o.type="sine";o.frequency.setValueAtTime(600,a.currentTime);o.frequency.exponentialRampToValueAtTime(1100,a.currentTime+0.12);g.gain.setValueAtTime(0.2,a.currentTime);g.gain.exponentialRampToValueAtTime(0.001,a.currentTime+0.18);o.start(a.currentTime);o.stop(a.currentTime+0.18);}catch(e){}}
function soundReceived(){try{var a=getACtx();[0,0.1].forEach(function(d,i){var o=a.createOscillator(),g=a.createGain();o.connect(g);g.connect(a.destination);o.type="triangle";o.frequency.setValueAtTime(i===0?880:1100,a.currentTime+d);g.gain.setValueAtTime(0.15,a.currentTime+d);g.gain.exponentialRampToValueAtTime(0.001,a.currentTime+d+0.15);o.start(a.currentTime+d);o.stop(a.currentTime+d+0.15);});}catch(e){}}
function soundJoin(){try{var a=getACtx(),o=a.createOscillator(),g=a.createGain();o.connect(g);g.connect(a.destination);o.type="sine";o.frequency.setValueAtTime(300,a.currentTime);o.frequency.exponentialRampToValueAtTime(900,a.currentTime+0.25);g.gain.setValueAtTime(0.12,a.currentTime);g.gain.exponentialRampToValueAtTime(0.001,a.currentTime+0.3);o.start(a.currentTime);o.stop(a.currentTime+0.3);}catch(e){}}
function soundDelete(){try{var a=getACtx(),o=a.createOscillator(),g=a.createGain();o.connect(g);g.connect(a.destination);o.type="sawtooth";o.frequency.setValueAtTime(400,a.currentTime);o.frequency.exponentialRampToValueAtTime(80,a.currentTime+0.2);g.gain.setValueAtTime(0.15,a.currentTime);g.gain.exponentialRampToValueAtTime(0.001,a.currentTime+0.22);o.start(a.currentTime);o.stop(a.currentTime+0.22);}catch(e){}}
document.addEventListener("click",function(){getACtx();},{once:true});

// ── Theme toggle (dark/light) ──
function applyTheme(t){
document.documentElement.classList.toggle("dark",t==="dark");
var btn=document.getElementById("themeToggle");
if(btn)btn.textContent=(t==="dark")?"☀️":"🌙";
try{localStorage.setItem("lanchat_theme",t);}catch(e){}
}
function toggleTheme(){
var cur=document.documentElement.classList.contains("dark")?"dark":"light";
applyTheme(cur==="dark"?"light":"dark");
}
(function initTheme(){
var saved=null;
try{saved=localStorage.getItem("lanchat_theme");}catch(e){}
var theme=saved||((window.matchMedia&&window.matchMedia("(prefers-color-scheme: dark)").matches)?"dark":"light");
applyTheme(theme);
})();

// ── Mobile viewport height fix ──
// Mobile browsers (esp. iOS Safari / Chrome with address bar) don't size
// 100vh consistently. --vh is recalculated from the real innerHeight instead.
function setVH(){
document.documentElement.style.setProperty("--vh",(window.innerHeight*0.01)+"px");
}
setVH();
window.addEventListener("resize",setVH);
window.addEventListener("orientationchange",setVH);

var EMOJIS={
"😀":"smiling grinning","😁":"grin happy","😂":"joy laugh cry","🤣":"rofl laugh",
"😃":"smile happy","😄":"smile grin","😅":"sweat smile","😆":"laugh satisfied",
"😇":"innocent angel","😉":"wink","😊":"blush smile","😋":"yum tongue",
"😍":"heart eyes love","🥰":"smiling hearts","😘":"kiss love","🤩":"star struck wow",
"😏":"smirk","😒":"unamused","😞":"disappointed sad","😢":"cry sad tear",
"😭":"sob cry","😤":"triumph huff","😠":"angry mad","😡":"rage angry",
"🤬":"cursing mad","🤯":"mind blown wow","😱":"scream shocked","😨":"fearful scared",
"😰":"anxious nervous","😓":"downcast sweat","🤔":"thinking hmm","😴":"sleep zzz",
"🤒":"sick ill","🤑":"money rich","🤠":"cowboy","🥳":"party celebrate",
"😎":"cool sunglasses","🤓":"nerd glasses","🥺":"pleading sad cute",
"😲":"astonished shocked","🥵":"hot fire","🥶":"cold freeze","😵":"dizzy faint",
"👋":"wave hi hello","👌":"ok perfect","✌":"peace victory","👍":"thumbs up good",
"👎":"thumbs down bad","👊":"fist punch","✊":"raised fist","👏":"clap applause",
"🙌":"raised hands praise","🙏":"pray thanks","💪":"muscle strong","👀":"eyes look",
"🧠":"brain smart","💀":"skull dead","👻":"ghost boo","🤖":"robot ai",
"💩":"poop","❤":"heart love red","🧡":"orange heart",
"💛":"yellow heart","💚":"green heart","💙":"blue heart","💜":"purple heart",
"🖤":"black heart","💔":"broken heart","💕":"two hearts","💯":"100 perfect",
"🔥":"fire hot lit","💥":"boom explosion","✨":"sparkles magic","⚡":"lightning bolt",
"🌈":"rainbow","⭐":"star","🌟":"glowing star","🌙":"moon night","☀":"sun bright",
"❄":"snowflake cold","🌊":"wave water ocean","🎉":"party popper","🎊":"confetti celebrate",
"🏆":"trophy winner","🎯":"target bullseye","🎮":"game controller",
"🎵":"music note","🎶":"music notes song","🎸":"guitar music",
"🚀":"rocket space launch","🌍":"earth world globe",
"💻":"laptop computer","📱":"phone mobile","🔑":"key","🔒":"locked secure",
"🛡":"shield protect","🍕":"pizza food",
"🍔":"burger food","🍟":"fries food","🌮":"taco food","🍜":"ramen noodles",
"🎂":"cake birthday","☕":"coffee drink","🍺":"beer drink","⚠":"warning danger",
"✅":"check done","❌":"cross wrong no","💤":"zzz sleep"
};
var TABS=[
{icon:"😊",label:"Faces",keys:["smil","grin","joy","wink","blush","cry","angry","cool","nerd","love","kiss","yum","sad","scared","think"]},
{icon:"👋",label:"Hands",keys:["wave","ok","peace","fist","clap","pray","muscle","thumbs","finger"]},
{icon:"❤",label:"Hearts",keys:["heart","love","broken","two hearts","100"]},
{icon:"🔥",label:"Symbols",keys:["fire","boom","sparkle","lightning","star","rainbow","party","trophy","100","warning","check","cross"]},
{icon:"🎮",label:"Fun",keys:["game","rocket","earth","magic","guitar","music","pizza","burger","cake","coffee","beer"]},
{icon:"💻",label:"Tech",keys:["laptop","phone","key","lock","shield","robot","brain"]}
];
var emojiAllList=[];
Object.keys(EMOJIS).forEach(function(e){emojiAllList.push({e:e,n:EMOJIS[e]});});
(function buildTabs(){
var td=document.getElementById("emojiTabs");
var all=document.createElement("span");all.className="etab active";all.textContent="All";
all.dataset.tab="all";all.onclick=function(){setTab(this);};td.appendChild(all);
TABS.forEach(function(t){
var s=document.createElement("span");s.className="etab";s.textContent=t.icon;s.title=t.label;
s.dataset.tab=t.label;s.onclick=function(){setTab(this);};td.appendChild(s);
});
buildEmojiGrid(emojiAllList);
})();
function setTab(el){
document.querySelectorAll(".etab").forEach(function(e){e.classList.remove("active");});el.classList.add("active");
document.getElementById("emojiSearch").value="";
if(el.dataset.tab==="all"){buildEmojiGrid(emojiAllList);return;}
var tab=TABS.find(function(t){return t.label===el.dataset.tab;});
buildEmojiGrid(tab?emojiAllList.filter(function(o){return tab.keys.some(function(k){return o.n.indexOf(k)!==-1;});}):emojiAllList);
}
function buildEmojiGrid(list){
var grid=document.getElementById("emojiGrid");grid.innerHTML="";
list.forEach(function(obj){
var s=document.createElement("span");s.className="emoji-item";s.textContent=obj.e;s.title=obj.n.split(" ")[0];
s.onclick=function(){insertEmoji(obj.e);};grid.appendChild(s);
});
}
function filterEmojis(q){q=q.toLowerCase().trim();
buildEmojiGrid(q?emojiAllList.filter(function(o){return o.n.indexOf(q)!==-1;}):emojiAllList);}
function toggleEmojiPicker(){
var p=document.getElementById("emojiPicker");p.style.display=(p.style.display==="block")?"none":"block";
if(p.style.display==="block")document.getElementById("emojiSearch").focus();
}
function insertEmoji(e){
var inp=document.getElementById("msgInput"),s=inp.selectionStart,end=inp.selectionEnd;
inp.value=inp.value.substring(0,s)+e+inp.value.substring(end);
inp.selectionStart=inp.selectionEnd=s+e.length;inp.focus();
}
document.addEventListener("click",function(ev){
var p=document.getElementById("emojiPicker"),b=document.getElementById("emojiBtn");
if(p.style.display==="block"&&!p.contains(ev.target)&&ev.target!==b)p.style.display="none";
});

var routerConnected=false;
function updateRouterPill(data){
var pill=document.getElementById("routerPill"),label=document.getElementById("routerLabel");
var discBtn=document.getElementById("disconnectBtn");
pill.classList.remove("connected","connecting");
if(data.connecting){
pill.classList.add("connecting");label.textContent="Connecting to "+data.ssid;
if(discBtn)discBtn.style.display="none";routerConnected=false;
}else if(data.connected){
pill.classList.add("connected");label.textContent=data.ssid;
if(discBtn)discBtn.style.display="block";routerConnected=true;
showToast("Connected to "+data.ssid+" - "+data.ip);
}else{
label.textContent="No Router";if(discBtn)discBtn.style.display="none";routerConnected=false;
if(data.error==="timeout")showToast("Connection timeout. Wrong password?");
}
}

var pendingSsid="",pendingEnc=false;
function openScanModal(){
var modal=document.getElementById("scanModal");modal.classList.add("open");
var sl=document.getElementById("scanList");sl.innerHTML='<div class="scan-loading">Scanning networks...</div>';
document.getElementById("disconnectBtn").style.display=routerConnected?"block":"none";
fetch("/scan").then(function(r){return r.json();}).then(function(nets){
if(!nets||nets.length===0){sl.innerHTML='<div class="scan-loading">No networks found.</div>';return;}
nets.sort(function(a,b){return b.rssi-a.rssi;});
sl.innerHTML="";
nets.forEach(function(n){
var sig=n.rssi>-55?"||||":n.rssi>-70?"|||.":n.rssi>-80?"||..":"| ..";
var div=document.createElement("div");div.className="scan-item";
var nameSpan=document.createElement("div");nameSpan.className="scan-ssid";
nameSpan.textContent=n.ssid+(n.enc?" 🔒":"");
var rssiSpan=document.createElement("div");rssiSpan.className="scan-rssi";
rssiSpan.textContent=sig+" "+n.rssi+" dBm";
var left=document.createElement("div");left.appendChild(nameSpan);left.appendChild(rssiSpan);
div.appendChild(left);
div.onclick=function(){selectNetwork(n.ssid,n.enc);};
sl.appendChild(div);
});
}).catch(function(){sl.innerHTML='<div class="scan-loading">Scan failed. Are you the admin?</div>';});
}
function closeScanModal(){document.getElementById("scanModal").classList.remove("open");}
function selectNetwork(ssid,enc){
closeScanModal();pendingSsid=ssid;pendingEnc=enc;
if(!enc){connectToRouter(ssid,"");return;}
document.getElementById("passModalTitle").textContent="Password for: "+ssid;
document.getElementById("routerPassInput").value="";
document.getElementById("passModal").classList.add("open");
setTimeout(function(){document.getElementById("routerPassInput").focus();},80);
}
function closePassModal(){document.getElementById("passModal").classList.remove("open");pendingSsid="";}
function doConnect(){var pass=document.getElementById("routerPassInput").value;closePassModal();connectToRouter(pendingSsid,pass);}
function connectToRouter(ssid,pass){
fetch("/connect",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({ssid:ssid,pass:pass})}).catch(function(){});
showToast("Connecting to "+ssid);
}
function doDisconnect(){
closeScanModal();
fetch("/disconnect",{method:"POST"}).catch(function(){});
showToast("Disconnecting from router...");
}

var socket,myName="",currentChat="all",allMessages=[],unreadCounts={};
var deletedMsgs={},reactStore={},isAdmin=false;

function login(){
var u=document.getElementById("userName").value.trim();
var p=document.getElementById("passCode").value;
var errDiv=document.getElementById("loginErr");
var btn=document.getElementById("loginBtn");
if(u===""){
errDiv.textContent="Name can't be empty.";
errDiv.style.display="block";
setTimeout(function(){errDiv.style.display="none";},3000);
return;
}
if(p===""){
errDiv.textContent="Enter the password.";
errDiv.style.display="block";
setTimeout(function(){errDiv.style.display="none";},3000);
return;
}
btn.disabled=true;
btn.textContent="Verifying...";
fetch("/auth",{
method:"POST",
headers:{"Content-Type":"application/json"},
body:JSON.stringify({user:u,pass:p})
}).then(function(r){return r.json();}).then(function(data){
if(data.ok){
myName=u;
getACtx();
document.getElementById("login-screen").style.display="none";
document.getElementById("main-ui").style.display="flex";
initSocket();
}else{
errDiv.textContent="Wrong password. Try again.";
errDiv.style.display="block";
btn.disabled=false;
btn.textContent="Join Chat";
document.getElementById("passCode").value="";
document.getElementById("passCode").focus();
setTimeout(function(){errDiv.style.display="none";},3500);
}
}).catch(function(){
errDiv.textContent="Can't reach the server. Refresh and try again.";
errDiv.style.display="block";
btn.disabled=false;
btn.textContent="Join Chat";
setTimeout(function(){errDiv.style.display="none";},3500);
});
}

var prevUserCount=0;
function initSocket(){
socket=new WebSocket("ws://"+window.location.hostname+":81/");
socket.onopen=function(){socket.send(JSON.stringify({type:"login",user:myName}));focusInput();};
socket.onmessage=function(ev){
var data=JSON.parse(ev.data);
if(data.type==="userList"){
if(data.users.length>prevUserCount)soundJoin();
prevUserCount=data.users.length;updateSidebar(data.users);
}else if(data.type==="adminFlag"){
isAdmin=data.isAdmin;
if(isAdmin){document.getElementById("adminScanBtn").style.display="inline-block";showToast("You are the Admin");}
}else if(data.type==="routerStatus"){updateRouterPill(data);
}else if(data.type==="history"){
if(data.messages&&data.messages.length>0){
data.messages.forEach(function(m){applyHistoryMsg(m);});
renderMessages();
var b=document.getElementById("histBanner");b.style.display="block";
setTimeout(function(){b.style.display="none";},4000);
}
}else if(data.type==="chat"||data.type==="file"){allMessages.push(data);handleNewMsg(data);
}else if(data.type==="typing"){
if(data.from!==myName){
document.getElementById("typing-indicator").textContent=data.from+" is typing...";
clearTimeout(window._typClear);
window._typClear=setTimeout(function(){document.getElementById("typing-indicator").textContent="";},2000);
}
}else if(data.type==="reaction"){applyReaction(data.msgID,data.emoji,data.from);
}else if(data.type==="delete"){deletedMsgs[data.msgID]=true;renderMessages();}
};
socket.onerror=function(){showToast("Connection error!");};
socket.onclose=function(){showToast("Disconnected — trying to reconnect...");setTimeout(initSocket,2000);};
}

function applyHistoryMsg(m){
if(m.type==="delete"){deletedMsgs[m.msgID]=true;return;}
if(m.type==="reaction"){applyReaction(m.msgID,m.emoji,m.from);return;}
if(m.type==="chat"||m.type==="file")allMessages.push(m);
}
function handleNewMsg(data){
var sid=(data.to==="all")?"all":data.fromIP;
var isForMe=(data.to==="all")||(data.to==="private"&&(data.fromIP===currentChat||data.targetIP===currentChat));
if(currentChat!==sid&&data.from!==myName){unreadCounts[sid]=(unreadCounts[sid]||0)+1;updateSidebarListOnly();}
if(data.from!==myName&&isForMe)soundReceived();
renderMessages();
}
function focusInput(){var i=document.getElementById("msgInput");if(i)i.focus();}

document.addEventListener("DOMContentLoaded",function(){
buildEmojiGrid(emojiAllList);
document.getElementById("msgInput").addEventListener("keydown",function(e){
if(e.key==="Enter"&&!e.shiftKey){e.preventDefault();sendMsg();return;}
if(socket&&socket.readyState===1)socket.send(JSON.stringify({type:"typing",from:myName,to:currentChat}));
});
});

function updateSidebar(u){window.cachedUsers=u;updateSidebarListOnly();}
function updateSidebarListOnly(){
var list=document.getElementById("userList");
var gc=unreadCounts["all"]||0;
var gB=gc>0?'<span class="badge" style="display:inline-block">'+gc+'</span>':'<span class="badge"></span>';
list.innerHTML='<div class="contact '+(currentChat==="all"?"active":"")+'" onclick="setChat(\'all\',\'Group Chat\')">'
+'<div class="contact-name"><span class="online-dot"></span>Group Chat</div>'+gB+'</div>';
if(window.cachedUsers)window.cachedUsers.forEach(function(u){
if(u.name!==myName){
var c=unreadCounts[u.ip]||0;
var bH=c>0?'<span class="badge" style="display:inline-block">'+c+'</span>':'<span class="badge"></span>';
list.innerHTML+='<div class="contact '+(currentChat===u.ip?"active":"")+'" onclick="setChat(\''+u.ip+'\',\''+u.name+'\')">'
+'<div class="contact-name"><span class="online-dot"></span>'+u.name+'</div>'+bH+'</div>';
}
});
}
function setChat(id,name){
currentChat=id;unreadCounts[id]=0;
document.getElementById("chatTitleText").textContent=name;
updateSidebarListOnly();renderMessages();focusInput();
document.querySelector(".main-body").classList.add("show-chat");
}
function closeChatMobile(){
document.querySelector(".main-body").classList.remove("show-chat");
}

function renderMessages(){
var mDiv=document.getElementById("messages");mDiv.innerHTML="";
allMessages.forEach(function(m,idx){
var isG=(currentChat==="all"&&m.to==="all");
var isP=(currentChat!=="all"&&((m.fromIP===currentChat&&m.to==="private")||(m.from===myName&&m.targetIP===currentChat)));
if(!isG&&!isP)return;
var type=(m.from===myName)?"sent":"received",mID="msg_"+idx;
var isMine=(m.from===myName);
if(deletedMsgs[mID]){
mDiv.innerHTML+='<div class="msg '+type+'" id="'+mID+'"><span class="deleted-msg">Message deleted</span><span class="msg-meta">'+m.time+'</span></div>';
return;
}
var rHtml="";
if(reactStore[mID]){rHtml='<div class="reactions">';Object.keys(reactStore[mID]).forEach(function(em){rHtml+='<span class="reaction-badge" onclick="addReaction(\''+mID+'\',\''+em+'\','+idx+')">'+em+' '+reactStore[mID][em]+'</span>';});rHtml+="</div>";}
var actHtml='<div class="msg-actions"><span class="action-btn" onclick="showReactPicker(\''+mID+'\','+idx+')">+Reaction</span>';
if(isMine)actHtml+='<span class="action-btn del" onclick="deleteMsg(\''+mID+'\','+idx+')">Delete</span>';
actHtml+="</div>";
var bodyHtml;
if(m.type==="file"){
bodyHtml='<div class="file-chip"><span class="file-icon">📎</span>'
+'<div class="file-info"><div class="file-name">'+escapeHtml(m.fileName)+'</div>'
+'<div class="file-size">'+formatFileSize(m.fileSize)+'</div></div>'
+'<a class="file-dl" href="'+m.url+'" download="'+escapeHtml(m.fileName)+'" target="_blank" title="Download">⬇</a></div>';
}else{
bodyHtml='<div class="msg-text">'+m.text+'</div>';
}
mDiv.innerHTML+='<div class="msg '+type+'" id="'+mID+'">'+(type==="received"?'<div class="msg-sender">'+m.from+'</div>':"")
+bodyHtml+'<span class="msg-meta">'+m.time+'</span>'+rHtml+actHtml+'</div>';
});
mDiv.scrollTop=mDiv.scrollHeight;
}

function deleteMsg(mID,idx){
if(!confirm("Delete this message?"))return;
deletedMsgs[mID]=true;soundDelete();
if(socket&&socket.readyState===1)socket.send(JSON.stringify({type:"delete",msgID:mID,from:myName}));
var el=document.getElementById(mID);
if(el){el.classList.add("deleting");setTimeout(function(){renderMessages();},300);}else renderMessages();
}

var quickReacts=["👍","❤","😂","😮","😢","🔥","💯"];
function showReactPicker(mID,idx){
var old=document.getElementById("rp_"+mID);if(old){old.remove();return;}
var msgEl=document.getElementById(mID);if(!msgEl)return;
var div=document.createElement("div");div.id="rp_"+mID;
div.style.cssText="position:absolute;top:-50px;left:0;background:var(--panel);border:1px solid var(--border);border-radius:20px;padding:5px 8px;display:flex;gap:6px;z-index:100;box-shadow:0 3px 12px rgba(0,0,0,0.15);";
quickReacts.forEach(function(em){
var s=document.createElement("span");s.textContent=em;
s.style.cssText="font-size:20px;cursor:pointer;transition:transform 0.15s;";
s.onmouseover=function(){s.style.transform="scale(1.3)";};
s.onmouseout=function(){s.style.transform="scale(1)";};
s.onclick=function(){addReaction(mID,em,idx);div.remove();};div.appendChild(s);
});
msgEl.style.position="relative";msgEl.appendChild(div);
setTimeout(function(){if(div.parentNode)div.remove();},3500);
}
function addReaction(mID,emoji,idx){
if(!reactStore[mID])reactStore[mID]={};
reactStore[mID][emoji]=(reactStore[mID][emoji]||0)+1;
if(socket&&socket.readyState===1)socket.send(JSON.stringify({type:"reaction",msgID:mID,emoji:emoji,from:myName}));
renderMessages();
}
function applyReaction(mID,emoji,from){
if(!reactStore[mID])reactStore[mID]={};
reactStore[mID][emoji]=(reactStore[mID][emoji]||0)+1;renderMessages();
}

function sendMsg(){
var inp=document.getElementById("msgInput"),txt=inp.value.trim();
if(txt!==""&&socket&&socket.readyState===WebSocket.OPEN){
var timeNow=new Date().toLocaleTimeString([],{hour:"2-digit",minute:"2-digit"});
socket.send(JSON.stringify({type:"chat",from:myName,text:txt,
to:(currentChat==="all"?"all":"private"),targetIP:currentChat,time:timeNow}));
inp.value="";soundSent();document.getElementById("emojiPicker").style.display="none";focusInput();
}
}

// ── File sharing ──
var MAX_FILE_SIZE = 10*1024*1024; // 10 MB — keep in sync with the firmware's MAX_FILE_SIZE
function formatFileSize(bytes){
if(bytes<1024) return bytes+" B";
if(bytes<1024*1024) return (bytes/1024).toFixed(1)+" KB";
return (bytes/(1024*1024)).toFixed(2)+" MB";
}
function escapeHtml(s){
var d=document.createElement("div");d.textContent=(s==null?"":s);return d.innerHTML;
}
function handleFileSelect(files){
if(!files||files.length===0)return;
var file=files[0];
var fileInput=document.getElementById("fileInput");
if(file.size>MAX_FILE_SIZE){
showToast("File too big — max "+formatFileSize(MAX_FILE_SIZE));
fileInput.value="";
return;
}
var timeNow=new Date().toLocaleTimeString([],{hour:"2-digit",minute:"2-digit"});
var fd=new FormData();
fd.append("file",file,file.name);
var qs="from="+encodeURIComponent(myName)
+"&to="+encodeURIComponent(currentChat==="all"?"all":"private")
+"&targetIP="+encodeURIComponent(currentChat==="all"?"":currentChat)
+"&time="+encodeURIComponent(timeNow);
showToast("Uploading "+file.name+"...");
fetch("/upload?"+qs,{method:"POST",body:fd})
.then(function(r){return r.json();})
.then(function(res){
if(!res.ok)showToast(res.error||"Upload failed");
}).catch(function(){showToast("Upload failed — try again");});
fileInput.value="";
}

function showToast(msg){
var t=document.createElement("div");
t.style.cssText="position:fixed;top:18px;left:50%;transform:translateX(-50%);background:#1f2430;color:#fff;padding:9px 18px;border-radius:10px;z-index:9999;font-size:13px;font-weight:600;box-shadow:0 4px 15px rgba(0,0,0,0.2);";
t.textContent=msg;document.body.appendChild(t);setTimeout(function(){t.remove();},3000);
}
</script>
</body>
</html>
)=====";

void setup(){
  Serial.begin(115200);

  // ── Fix: this is the actual reason "scan works but connect doesn't" ──
  // The old code booted in WIFI_AP only, then flipped to WIFI_AP_STA inside
  // handleConnect(). On ESP32, switching AP -> AP_STA on the fly (with modem
  // sleep still on) very often leaves the STA radio in a state where
  // WiFi.begin() silently never reaches WL_CONNECTED — it just sits there
  // until STA_TIMEOUT. Fix is to bring both interfaces up once, at boot, and
  // disable modem sleep (the other classic cause of AP+STA flakiness on ESP32).
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);   // clear any stale/auto-reconnect STA state
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  // Flash filesystem for uploaded files. true = format if mount fails
  // (first boot / corrupted FS) so this never gets permanently stuck.
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS mount failed — file sharing will not work");
  } else if(!LittleFS.exists(FILES_DIR)){
    LittleFS.mkdir(FILES_DIR);
  }

  server.on("/",[](){
    server.send(200,"text/html; charset=utf-8",
      String(reinterpret_cast<const char*>(chatPage)));
  });
  server.on("/auth",       HTTP_POST, handleAuth);
  server.on("/scan",       HTTP_GET,  handleScan);
  server.on("/connect",    HTTP_POST, handleConnect);
  server.on("/disconnect", HTTP_POST, handleDisconnect);
  server.on("/upload",     HTTP_POST, handleUploadComplete, handleFileUpload);
  server.onNotFound(handleNotFound);   // serves /files/<name> downloads

  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("Server started!");
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
}

void loop(){
  webSocket.loop();
  server.handleClient();

  if(staConnecting){
    if(WiFi.status()==WL_CONNECTED){
      staConnecting=false; staConnected=true;
      Serial.print("Router IP: "); Serial.println(WiFi.localIP());
      // ESP32 has one radio, so the softAP and the STA link must share a
      // channel. Re-pin the AP onto whatever channel we just connected the
      // router on, or the two links fight each other and things get flaky.
      WiFi.softAP(AP_SSID, AP_PASS, WiFi.channel());
      broadcastRouterStatus();
    } else if(millis()-staConnectStart > STA_TIMEOUT){
      staConnecting=false; staConnected=false;
      WiFi.disconnect(true, false);   // drop STA link only, keep AP_STA mode
      Serial.println("STA timeout");
      String fail="{\"type\":\"routerStatus\",\"connected\":false,\"ssid\":\"\",\"connecting\":false,\"ip\":\"\",\"error\":\"timeout\"}";
      webSocket.broadcastTXT(fail);
    }
  }
}