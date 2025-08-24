#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
static const char *apPWD     = 0;       // No password
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients

static uint16_t ajaxInterval = 2500;

static bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static uint32_t connectTime = millis();

// Settings
String loginUsername = "";
String loginPassword = "";

// AsyncWebServer object on port 80
AsyncWebServer server(80);

// NTP Client to get time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
static void webInit();

static void webSetConfig(AsyncWebServerRequest *request);

static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webRadioPage();
static const String webMemoryPage();
static const String webConfigPage();
static const String radioControl();

//
// Delayed WiFi connection
//
void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx);
    connectTime = millis();
    itIsTimeToWiFi = false;
  }
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected, 2 - connected to network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WiFi.getMode();

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_AP:
      return(WiFi.softAPgetStationNum()? 1 : -1);
    case WIFI_STA:
      return(WiFi.status()==WL_CONNECTED? 2 : -1);
    case WIFI_AP_STA:
      return((WiFi.status()==WL_CONNECTED)? 2 : WiFi.softAPgetStationNum()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  return strcpy(ip, WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
}

//
// Stop WiFi hardware
//
void netStop()
{
  wifi_mode_t mode = WiFi.getMode();

  MDNS.end();

  // If network connection up, shut it down
  if((mode==WIFI_STA) || (mode==WIFI_AP_STA))
    WiFi.disconnect(true);

  // If access point up, shut it down
  if((mode==WIFI_AP) || (mode==WIFI_AP_STA))
    WiFi.softAPdisconnect(true);

  WiFi.mode(WIFI_MODE_NULL);
}

//
// Initialize WiFi network and services
//
void netInit(uint8_t netMode, bool showStatus)
{
  // Always disable WiFi first
  netStop();

  switch(netMode)
  {
    case NET_OFF:
      // Do not initialize WiFi if disabled
      return;
    case NET_AP_ONLY:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    case NET_AP_CONNECT:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP_STA);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    default:
      // No access point
      WiFi.mode(WIFI_STA);
      break;
  }

  // Initialize WiFi and try connecting to a network
  if(netMode>NET_AP_ONLY && wifiConnect())
  {
    // Let user see connection status if successful
    if(netMode!=NET_SYNC && showStatus) delay(2000);

    // NTP time updates will happen every 5 minutes
    ntpClient.setUpdateInterval(5*60*1000);

    // Get NTP time from the network
    clockReset();
    for(int j=0 ; j<10 ; j++)
      if(ntpSyncTime()) break; else delay(500);
  }

  // If only connected to sync...
  if(netMode==NET_SYNC)
  {
    // Drop network connection
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
  else
  {
    // Initialize web server for remote configuration
    webInit();

    // Initialize mDNS
    MDNS.begin("atsmini"); // Set the hostname to "atsmini.local"
    MDNS.addService("http", "tcp", 80);
  }
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpClient.isTimeSet());
}

//
// Update NTP time and synchronize clock with NTP time
//
bool ntpSyncTime()
{
  if(WiFi.status()==WL_CONNECTED)
  {
    ntpClient.update();

    if(ntpClient.isTimeSet())
      return(clockSet(
        ntpClient.getHours(),
        ntpClient.getMinutes(),
        ntpClient.getSeconds()
      ));
  }
  return(false);
}

//
// Initialize WiFi access point (AP)
//
static bool wifiInitAP()
{
  // These are our own access point (AP) addresses
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Start as access point (AP)
  WiFi.softAP(apSSID, apPWD, apChannel, apHideMe, apClients);
  WiFi.softAPConfig(ip, gateway, subnet);

  drawScreen(
    ("Use Access Point " + String(apSSID)).c_str(),
    ("IP : " + WiFi.softAPIP().toString() + " or atsmini.local").c_str()
  );

  ajaxInterval = 2500;
  return(true);
}

//
// Connect to a WiFi network
//
static bool wifiConnect()
{
  String status = "Connecting to WiFi network..";

  // Get the preferences
  prefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = prefs.getString("loginusername", "");
  loginPassword = prefs.getString("loginpassword", "");

  // Try connecting to known WiFi networks
  for(int j=0 ; (j<3) && (WiFi.status()!=WL_CONNECTED) ; j++)
  {
    char nameSSID[16], namePASS[16];
    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    String ssid = prefs.getString(nameSSID, "");
    String password = prefs.getString(namePASS, "");

    if(ssid != "")
    {
      WiFi.begin(ssid, password);
      for(int j=0 ; (WiFi.status()!=WL_CONNECTED) && (j<24) ; j++)
      {
        if(!(j&7))
        {
          status += ".";
          drawScreen(status.c_str());
        }
        delay(500);
        if(digitalRead(ENCODER_PUSH_BUTTON)==LOW)
        {
          WiFi.disconnect();
          break;
        }
      }
    }
  }

  // Done with preferences
  prefs.end();

  // If failed connecting to WiFi network...
  if(WiFi.status()!=WL_CONNECTED)
  {
    // WiFi connection failed
    drawScreen(status.c_str(), "No WiFi connection");
    // Done
    return(false);
  }
  else
  {
    // WiFi connection succeeded
    drawScreen(
      ("Connected to WiFi network (" + WiFi.SSID() + ")").c_str(),
      ("IP : " + WiFi.localIP().toString() + " or atsmini.local").c_str()
    );
    // Done
    ajaxInterval = 1000;
    return(true);
  }
}

static const int getbandId(String bandName) {
  for (int i = 0; i < getTotalBands(); i++) {
    if (String(bands[i].bandName) == bandName) return i;
  }

  return -1;
}

//
// Initialize internal web server
//
static void webInit()
{
  server.on("/", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webRadioPage());
  });

  server.on("/memory", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webMemoryPage());
  });

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    request->send(200, "text/html", webConfigPage());
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_ANY, webSetConfig);

  // This controller is to set frequency directly
  server.on("/rx", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("f")) {
      String fValue = request->getParam("f")->value();
      int f = fValue.toInt();
      updateFrequency(f, true);
      request->send(200, "text/plain", String(currentFrequency));
    } else {
      request->send(400, "text/plain", "Missing 'f' parameter");
    }
  });

  // This controller is to set band. Either HF or VHF
  server.on("/band", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("bandId")) {
    String value = request->getParam("bandId")->value();
    selectBand(value.toInt());

    request->send(200, "text/plain", String(getbandId(getCurrentBand()->bandName)));
  } else {
    request->send(400, "text/plain", "Missing 'vhf'");
  }
  });

  server.on("/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("modeId")) {
    String value = request->getParam("modeId")->value();
    setMode(value.toInt());
    request->send(200, "text/plain", String(currentMode));
  } else {
    request->send(400, "text/plain", "Missing 'vhf'");
  }
  });

  server.on("/sound", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("vol")) {
    String value = request->getParam("vol")->value();
    setVolume(value.toInt());
    request->send(200, "text/plain", String(volume));
  } else {
    request->send(400, "text/plain", "Missing 'vhf'");
  }
  });

  // Start web server
  server.begin();
}

void webSetConfig(AsyncWebServerRequest *request)
{
  uint32_t prefsSave = 0;

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    String utcOffset = request->getParam("utcoffset", true)->value();
    utcOffsetIdx = utcOffset.toInt();
    clockRefreshTime();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction, tuning hold off, and menu zoom
  scrollDirection = request->hasParam("scroll", true)? -1 : 1;
  tuneHoldOff     = request->getParam("holdoff", true)->value().toInt();
  zoomMenu        = request->hasParam("zoom", true);
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);

  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static const String webStyleSheet() {
  return
"body {"
  "margin: 0;"
  "padding: 0;"
  "font-family: sans-serif;"
  "background-color: #f8f8f8;"
"}"

"h1 {"
  "text-align: center;"
"}"

"table {"
  "width: 100%;"
  "max-width: 768px;"
  "margin: 0 auto;"
  "border: 0;"
"}"

"th, td {"
  "padding: 0.5em;"
"}"

"th.heading {"
  "background-color: #80A0FF;"
  "text-align: center;"
"}"

"td.label {"
  "text-align: right;"
"}"

/* === Control Panel === */
".control-panel {"
  "max-width: 768px;"
  "margin: 1em auto;"
  "padding: 1em;"
  "box-sizing: border-box;"
"}"

".frequency-block {"
  "text-align: center;"
  "margin-bottom: 1em;"
"}"

".freq-label {"
  "display: block;"
  "font-size: 1.2em;"
  "margin-bottom: 0.5em;"
"}"

".freq-row {"
  "display: flex;"
  "justify-content: center;"
  "align-items: center;"
  "gap: 10px;"
"}"

".freq-input {"
  "width: 120px;"
  "padding: 0.5em;"
  "font-size: 1.5em;"
  "text-align: center;"
  "border: 1px solid #ccc;"
  "border-radius: 4px;"
"}"

".step-btn {"
  "padding: 0.6em 1em;"
  "font-size: 1.5em;"
  "background-color: #e0e0e0;"
  "border: none;"
  "border-radius: 4px;"
  "cursor: pointer;"
"}"

".step-btn:active {"
  "background-color: #ccc;"
"}"

".selectors {"
  "display: flex;"
  "justify-content: center;"
  "flex-wrap: wrap;"
  "gap: 10px;"
"}"

".selector {"
  "min-width: 100px;"
  "text-align: center;"
"}"

".selector select, .selector input {"
  "width: 100%;"
  "padding: 0.5em;"
  "font-size: 1em;"
  "border: 1px solid #ccc;"
  "border-radius: 4px;"
"}"

".selector label {"
  "display: block;"
  "margin-bottom: 0.2em;"
  "font-size: 0.9em;"
"}"
/* Volume + other inline controls */
".control-block {"
  "display: flex;"
  "flex-direction: column;"
  "align-items: center;"
  "margin: 1em auto;"
  "max-width: 200px;"
"}"

/* Reuse label style */
".control-label {"
  "font-weight: bold;"
  "margin-bottom: 0.3em;"
  "text-align: center;"
"}"

/* Volume slider */
".control-slider {"
  "width: 100%;"
  "appearance: none;"
  "height: 6px;"
  "background: #ccc;"
  "border-radius: 4px;"
  "outline: none;"
"}"

".control-slider::-webkit-slider-thumb {"
  "appearance: none;"
  "width: 16px;"
  "height: 16px;"
  "background: #333;"
  "border-radius: 50%;"
  "cursor: pointer;"
"}"

".control-slider::-moz-range-thumb {"
  "width: 16px;"
  "height: 16px;"
  "background: #333;"
  "border-radius: 50%;"
  "cursor: pointer;"
"}"
".status-indicator {"
  "font-weight: bold;"
  "font-size: 0.6em;"
  "vertical-align: super;"
  "margin-left: 0.25em;"
"}"
;
}


static const String getControlScript() {
  return
    "const RFendpoint = '/rx?f=';"
    "const bandsEndpoint = '/band?bandId=';"
    "const modesEndpoint = '/mode?modeId=';"
    "const volumeEndpoint = '/sound?vol=';"

    "let step = 1;"

    "function handleChange(listenee, endpoint, viewProcessor = console.log, failsafe = console.error) {"
      "const currentValue = parseInt(document.getElementById(listenee).value);"

      "const indicator = document.getElementById('statusIndicator');"

      "if (indicator) {"
        "indicator.textContent = '...';"
      "}"

      "fetch(endpoint + currentValue)"
        ".then(r => r.text())"
        ".then(result => {"
          "if (indicator) indicator.textContent = 'OK';"
          "viewProcessor(result);"
        "})"
        ".catch(error => {"
          "if (indicator) indicator.textContent = '!!';"
          "failsafe(error);"
        "})"
        ".finally(() => {"
          "setTimeout(() => {"
            "if (indicator) indicator.textContent = '⯿';"
          "}, 1500);"
        "});"
    "}"

    "function refreshField(fieldName, value) {"
      "const element = document.getElementById(fieldName);"
      "if (element) element.value = value;"
    "}"

    "const updateBand = newValue => {"
      "refreshField('bandsSelector', newValue);"
    "};"

    "function handleBandsChange() {"
      "return handleChange('bandsSelector', bandsEndpoint, updateBand);"
    "}"

    "const updateMode = newValue => {"
      "refreshField('modesSelector', newValue);"
    "};"

    "function handleModesChange() {"
      "return handleChange('modesSelector', modesEndpoint, updateMode);"
    "}"

    "const updateFrequencyView = newValue => {"
      "refreshField('numInput', newValue);"
      "refreshField('freq', newValue);"
    "};"

    "function updateFrequency() {"
      "return handleChange('numInput', RFendpoint, updateFrequencyView);"
    "}"

    "const updateVolumeView = newValue => {"
      "refreshField('soundVolume', newValue);"
    "};"

    "function handleVolumeChange() {"
      "return handleChange('volumeDial', volumeEndpoint, updateVolumeView);"
    "}"

    "function changeFrequency(delta) {"
      "const input = document.getElementById('numInput');"
      "let current = parseInt(input.value, 10);"
      "current += delta;"
      "input.value = current;"
      "updateFrequency();"
    "}"

    "function handleStepChange() {"
      "step = parseInt(document.getElementById('stepInput').value) || 1;"
    "}"

;
}

static const String webPage(const String &body)
{
  return
    "<!DOCTYPE HTML>"
      "<HTML>"
        "<HEAD>"
          "<META CHARSET='UTF-8'>"
          "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
          "<TITLE>ATS-Mini Config</TITLE>"
          "<STYLE>" + webStyleSheet() + "</STYLE>"
          "<SCRIPT>" + getControlScript() + "</SCRIPT>"
        "</HEAD>"
        "<BODY STYLE='font-family: sans-serif;'>"
          + body +
        "</BODY>"
      "</HTML>"
;
}

static const String webUtcOffsetSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s (%s)</OPTION>",
      i, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].city, utcOffsets[i].desc
    );

    result += text;
  }

  return(result);
}

static const String webThemeSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

    result += text;
  }

  return(result);
}

static const String bandsOptions() {
  String options = "";

  for (int i = 0; i < getTotalBands(); i++) {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
      i,
      (i == bandIdx) ? " SELECTED" : "",
      String(bands[i].bandName)
      );

    options += text;
  }

  return(options);
}

String modesOptions() {
  String options = "";

  for (int i = 0; i < getTotalModes(); i++) {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
      i,
      (i == currentMode) ? " SELECTED" : "",
      bandModeDesc[i]
    );

    options += text;
  }

  return options;
}

static const String radioControl() {
  return
    "<div class='control-panel'>"
      "<div class='frequency-block'>"
        "<label for='numInput' class='freq-label'>Frequency (kHz) "
          "<span class='status-indicator' id='statusIndicator'>⯿</span>"
        "</label>"
        "<div class='freq-row'>"
          "<button onclick='changeFrequency(-step)' class='step-btn'>&laquo;</button>"
             "<input id='numInput' type='number' value='" + String(currentFrequency) + "' step='1' min='0' max='40000' onchange='updateFrequency()' placeholder='Enter Frequency' class='freq-input'>"
          "<button onclick='changeFrequency(step)' class='step-btn'>&raquo;</button>"
        "</div>"
      "</div>"

      "<div class='selectors'>"
        "<div class='selector'>"
          "<label for='bandsSelector'>Band</label><br>"
          "<select id='bandsSelector' onchange='handleBandsChange()'>" + bandsOptions() + "</select>"
        "</div>"

        "<div class='selector'>"
          "<label for='modesSelector'>Mode</label><br>"
          "<select id='modesSelector' onchange='handleModesChange()'>" + modesOptions() + "</select>"
        "</div>"

        "<div class='selector'>"
          "<label for='stepInput'>Step</label><br>"
          "<input id='stepInput' type='number' value='1' min='1' max='10000' onchange='handleStepChange()'>"
        "</div>"
      "</div>"
      "<div class='control-block'>"
        "<input id='soundVolume' type='text' value='" + volume + "' disabled class='volume-display' />"
        "<label for='volumeDial' class='control-label'>Volume</label>"
        "<input id='volumeDial' type='range' value='" + volume + "' min='0' max='63' onchange='handleVolumeChange()' class='control-slider' />"
      "</div>"
    "</div>";
}


static const String getFreq() {
  return currentMode == FM?
    String(currentFrequency / 100.0) + "MHz "
  : String(currentFrequency + currentBFO / 1000.0) + "kHz ";
}

static const String webRadioPage()
{
  String ip = "";
  String ssid = "";
  String freq = getFreq();

  if(WiFi.status()==WL_CONNECTED)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = String(apSSID);
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/memory'>Memory</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
"<TABLE COLUMNS=2>"
"<TR>"
  "<TD COLSPAN=2>" + String(radioControl()) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>IP Address</TD>"
  "<TD><A HREF='http://" + ip + "'>" + ip + "</A> (" + ssid + ")</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>MAC Address</TD>"
  "<TD>" + String(getMACAddress()) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Firmware</TD>"
  "<TD>" + String(getVersion(true)) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Band</TD>"
  "<TD>" + String(getCurrentBand()->bandName) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Frequency</TD>"
"<TD>" + "<SPAN ID='freq'>" + freq + "</SPAN>" + String(bandModeDesc[currentMode]) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal Strength</TD>"
  "<TD>" + String(rssi) + "dBuV</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal to Noise</TD>"
  "<TD>" + String(snr) + "dB</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Battery Voltage</TD>"
  "<TD>" + String(batteryMonitor()) + "V</TD>"
"</TR>"
"</TABLE>"
);
}

static const String webMemoryPage()
{
  String items = "";

  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    char text[64];
    sprintf(text, "<TR><TD CLASS='LABEL' WIDTH='10%%'>%02d</TD><TD>", j+1);
    items += text;

    if(!memories[j].freq)
      items += "&nbsp;---&nbsp;</TD></TR>";
    else
    {
      String freq = memories[j].mode == FM?
        String(memories[j].freq / 1000000.0) + "MHz "
      : String(memories[j].freq / 1000.0) + "kHz ";
      items += freq + bandModeDesc[memories[j].mode] + "</TD></TR>";
    }
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver Memory</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
"<TABLE COLUMNS=2>" + items + "</TABLE>"
);
}

const String webConfigPage()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = prefs.getString("wifissid1", "");
  String pass1 = prefs.getString("wifipass1", "");
  String ssid2 = prefs.getString("wifissid2", "");
  String pass2 = prefs.getString("wifipass2", "");
  String ssid3 = prefs.getString("wifissid3", "");
  String pass3 = prefs.getString("wifipass3", "");
  prefs.end();

  return webPage(
"<H1>ATS-Mini Config</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>"
  "&nbsp;|&nbsp;<A HREF='/memory'>Memory</A>"
"</P>"
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Login Credentials</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Username</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("password", loginPassword, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 1</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid1", ssid1) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass1", pass1, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 2</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid2", ssid2) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass2", pass2, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 3</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid3", ssid3) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass3", pass3, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Settings</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Time Zone</TD>"
    "<TD>"
      "<SELECT NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Theme</TD>"
    "<TD>"
      "<SELECT NAME='theme'>" + webThemeSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Reverse Scrolling</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (scrollDirection<0? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Tuning Display Delay</TD>"
    "<TD><INPUT TYPE='NUMBER' NAME='holdoff' VALUE='" +
tuneHoldOff + "' MIN='0' MAX='255'></TD>"
  "</TR>"
   "<TR>"
    "<TD CLASS='LABEL'>Zoomed Menu</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TH></TR>"
  "</TABLE>"
"</FORM>"
);
}
