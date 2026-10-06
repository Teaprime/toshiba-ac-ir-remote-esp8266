#include <ESP8266WiFi.h>
#include <ESP8266WebServerSecure.h>
#include <ESP8266mDNS.h>
#include <Crypto.h>

#include "config.h"
#include "tls_credentials.h"

namespace {

BearSSL::ESP8266WebServerSecure server(443);
BearSSL::ServerSessions serverSessions(2);
BearSSL::X509List* serverCertificate = nullptr;
BearSSL::PrivateKey* serverPrivateKey = nullptr;
char pendingCommand[96];
bool commandPending = false;
char activeNonce[33];
uint32_t challengeExpiresAt = 0;
bool challengePending = false;

const char controllerPage[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Toshiba AC controller</title>
  <style>
    :root { color-scheme: light dark; font: 16px system-ui, sans-serif; }
    body { max-width: 520px; margin: 0 auto; padding: 20px; }
    h1 { font-size: 1.5rem; }
    section { margin: 20px 0; }
    h2 { margin-bottom: 8px; font-size: 1rem; }
    .choices, .commands { display: flex; flex-wrap: wrap; gap: 8px; }
    button, input { border: 1px solid #888; border-radius: 8px; padding: 10px 14px; font: inherit; }
    button.active { background: #1769aa; color: white; border-color: #1769aa; }
    input { box-sizing: border-box; width: 100%; }
    #status { min-height: 1.5em; }
  </style>
</head>
<body>
  <h1>Toshiba AC controller</h1>
  <section>
    <h2>Shared command secret</h2>
    <input id="secret" type="password" autocomplete="off" spellcheck="false"
           placeholder="Enter the shared secret" aria-label="Shared command secret">
  </section>
  <section><h2>Temperature</h2><div class="choices" id="temperatures"></div></section>
  <section><h2>Mode</h2><div class="choices" id="modes"></div></section>
  <section><h2>Fan</h2><div class="choices" id="fans"></div></section>
  <section class="commands">
    <button id="on">Power on</button>
    <button data-command="off">Power off</button>
    <button data-command="swing">Swing</button>
    <button data-command="hipwr">High power</button>
    <button data-command="sleep">Sleep</button>
    <button data-command="vertical">Vertical swing</button>
  </section>
  <p id="status" role="status" aria-live="polite">Ready</p>
  <script>
    const selection = { temp: "23", mode: "auto", fan: "auto" };
    function makeChoices(containerId, key, values) {
      const container = document.getElementById(containerId);
      for (const value of values) {
        const button = document.createElement("button");
        button.textContent = value;
        button.classList.toggle("active", value === selection[key]);
        button.setAttribute("aria-pressed", String(value === selection[key]));
        button.addEventListener("click", () => {
          selection[key] = value;
          for (const choice of container.children) {
            const selected = choice === button;
            choice.classList.toggle("active", selected);
            choice.setAttribute("aria-pressed", String(selected));
          }
        });
        container.appendChild(button);
      }
    }
    function toHex(bytes) {
      return Array.from(new Uint8Array(bytes), byte => byte.toString(16).padStart(2, "0")).join("");
    }
    async function sendCommand(command) {
      const secret = document.getElementById("secret").value.trim();
      const status = document.getElementById("status");
      if (!secret) {
        status.textContent = "Enter the shared secret first.";
        return;
      }

      const fields = {
        cmd: command,
        temp: command === "on" ? selection.temp : "",
        mode: command === "on" ? selection.mode : "",
        fan: command === "on" ? selection.fan : ""
      };
      status.textContent = "Sending...";
      try {
        const challengeResponse = await fetch("/api/challenge", { cache: "no-store" });
        if (!challengeResponse.ok) throw new Error("Challenge request failed.");
        const { nonce } = await challengeResponse.json();
        const message = [nonce, fields.cmd, fields.temp, fields.mode, fields.fan].join("\n");
        const key = await crypto.subtle.importKey(
          "raw", new TextEncoder().encode(secret), { name: "HMAC", hash: "SHA-256" },
          false, ["sign"]);
        const signature = toHex(await crypto.subtle.sign(
          "HMAC", key, new TextEncoder().encode(message)));
        const body = new URLSearchParams({ ...fields, nonce, signature });
        const response = await fetch("/api/command", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body
        });
        const result = await response.json();
        status.textContent = response.ok ? result.message : result.error;
      } catch (_) {
        status.textContent = "Could not authenticate or reach the controller.";
      }
    }
    makeChoices("temperatures", "temp", ["17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30"]);
    makeChoices("modes", "mode", ["auto", "cool", "dry", "heat"]);
    makeChoices("fans", "fan", ["auto", "1", "2", "3", "4", "5"]);
    document.getElementById("on").addEventListener("click", () => sendCommand("on"));
    for (const button of document.querySelectorAll("[data-command]")) {
      button.addEventListener("click", () => sendCommand(button.dataset.command));
    }
  </script>
</body>
</html>
)rawliteral";

void respondJson(int statusCode, const char* json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json", json);
}

bool validTemperature(const String& value) {
  return value == "17" || value == "18" || value == "19" || value == "20" ||
         value == "21" || value == "22" || value == "23" || value == "24" ||
         value == "25" || value == "26" || value == "27" || value == "28" ||
         value == "29" || value == "30";
}

bool validMode(const String& value) {
  return value == "auto" || value == "cool" || value == "dry" || value == "heat";
}

bool validFan(const String& value) {
  return value == "auto" || value == "1" || value == "2" || value == "3" ||
         value == "4" || value == "5";
}

void issueChallenge() {
  const uint32_t randomWords[4] = {ESP.random(), ESP.random(), ESP.random(), ESP.random()};
  for (uint8_t i = 0; i < 4; i++) {
    snprintf(activeNonce + i * 8, sizeof(activeNonce) - i * 8, "%08lx",
             static_cast<unsigned long>(randomWords[i]));
  }
  challengeExpiresAt = millis() + 30000;
  challengePending = true;

  char response[64];
  snprintf(response, sizeof(response), "{\"nonce\":\"%s\"}", activeNonce);
  respondJson(200, response);
}

bool decodeSignature(const String& encoded, uint8_t signature[32]) {
  if (encoded.length() != 64) {
    return false;
  }
  const auto hexNibble = [](char value) -> int8_t {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (uint8_t i = 0; i < 32; i++) {
    const int8_t high = hexNibble(encoded[i * 2]);
    const int8_t low = hexNibble(encoded[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    signature[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool verifySignature(const String& nonce, const String& command, const String& temp,
                     const String& mode, const String& fan, const String& encodedSignature) {
  if (!challengePending || nonce != activeNonce ||
      static_cast<int32_t>(millis() - challengeExpiresAt) >= 0) {
    challengePending = false;
    return false;
  }
  challengePending = false;

  uint8_t received[32];
  uint8_t expected[32];
  if (!decodeSignature(encodedSignature, received)) {
    return false;
  }

  String message = nonce + "\n" + command + "\n" + temp + "\n" + mode + "\n" + fan;
  char secret[65];
  memcpy_P(secret, commandSecret, sizeof(secret));
  secret[sizeof(secret) - 1] = '\0';

  experimental::crypto::SHA256::hmac(
      message.c_str(), message.length(), secret, strlen(secret), expected,
      sizeof(expected));

  uint8_t difference = 0;
  for (uint8_t i = 0; i < sizeof(expected); i++) {
    difference |= expected[i] ^ received[i];
  }
  return difference == 0;
}

void handleCommand() {
  if (commandPending) {
    respondJson(503, "{\"error\":\"Controller is busy; try again.\"}");
    return;
  }
  if (!server.hasArg("cmd") || !server.hasArg("nonce") ||
      !server.hasArg("signature")) {
    respondJson(400, "{\"error\":\"Missing command authentication.\"}");
    return;
  }

  const String command = server.arg("cmd");
  String temp;
  String mode;
  String fan;
  if (command == "on") {
    if (!server.hasArg("temp") || !server.hasArg("mode") || !server.hasArg("fan")) {
      respondJson(400, "{\"error\":\"Missing setting.\"}");
      return;
    }
    temp = server.arg("temp");
    mode = server.arg("mode");
    fan = server.arg("fan");
    if (!validTemperature(temp) || !validMode(mode) || !validFan(fan)) {
      respondJson(400, "{\"error\":\"Invalid setting.\"}");
      return;
    }
  } else if (command != "off" && command != "swing" && command != "hipwr" &&
             command != "sleep" && command != "vertical") {
    respondJson(400, "{\"error\":\"Unsupported command.\"}");
    return;
  }

  if (!verifySignature(server.arg("nonce"), command, temp, mode, fan,
                       server.arg("signature"))) {
    respondJson(401, "{\"error\":\"Authentication failed; try again.\"}");
    return;
  }

  if (command == "on") {
    snprintf(pendingCommand, sizeof(pendingCommand),
             "{\"cmd\":\"on\",\"temp\":\"%s\",\"mode\":\"%s\",\"fan\":\"%s\"}",
             temp.c_str(), mode.c_str(), fan.c_str());
  } else {
    snprintf(pendingCommand, sizeof(pendingCommand), "{\"cmd\":\"%s\"}", command.c_str());
  }
  commandPending = true;
  respondJson(202, "{\"message\":\"Command queued.\"}");
}

void startWebServer() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (!MDNS.begin("toshiba-ac")) {
    Serial.println("Failed to start mDNS; the HTTPS hostname may not resolve.");
  }

  serverCertificate = new BearSSL::X509List(tlsServerCert);
  serverPrivateKey = new BearSSL::PrivateKey(tlsServerKey);
  if (serverCertificate->getCount() == 0 || !serverPrivateKey->isRSA()) {
    Serial.println("Invalid HTTPS certificate or RSA private key.");
    return;
  }

  server.getServer().setRSACert(serverCertificate, serverPrivateKey);
  server.getServer().setCache(&serverSessions);
  if (!server.getServer().setSSLVersion(BR_TLS12, BR_TLS12)) {
    Serial.println("Failed to configure TLS 1.2.");
    return;
  }
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", controllerPage);
  });
  server.on("/api/challenge", HTTP_GET, issueChallenge);
  server.on("/api/command", HTTP_POST, handleCommand);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();

  Serial.print("HTTPS controller: https://toshiba-ac.local (IP ");
  Serial.print(WiFi.localIP());
  Serial.println(":443)");
}

void serviceWebServer() {
  static uint32_t lastReconnectAttempt = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnectAttempt >= 5000) {
      WiFi.reconnect();
      lastReconnectAttempt = millis();
    }
    return;
  }

  server.handleClient();
  MDNS.update();
}

bool takeWebCommand(char* destination, size_t destinationSize) {
  if (!commandPending || destinationSize == 0) {
    return false;
  }
  snprintf(destination, destinationSize, "%s", pendingCommand);
  commandPending = false;
  return true;
}

}  // namespace

static void webServerSetup() {
  startWebServer();
}

static void webServerLoop() {
  serviceWebServer();
}

static bool webServerTakeCommand(char* destination, size_t destinationSize) {
  return takeWebCommand(destination, destinationSize);
}
