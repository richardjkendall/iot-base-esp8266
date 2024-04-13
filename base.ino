#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

#include <ArduinoJson.h>
#include <PubSubClient.h>

#define MAX_WIFI_WAIT  20
#define DEBUG          1
#define MAX_SUB_TOPICS 20
#define RELAY_TIME     3
#define STATUS_TIME    5
#define RELAY_PIN      2
#define STATUS_PIN     0
#define UPGRADE_ENABLE 1

// needs to be replaced with reading from a file
const char* wifiSsid     = "";              
const char* wifiPassword = ""; 
const char* apiEndpoint  = "";  

// these are populated from the config being pulled down from API server
char* mqttBrokerIp;
char* mqttBrokerPort;
char* mqttClientName;
char* mqttBrokerUsername;
char* mqttBrokerPassword;
char* mqttTopicPrefix;

// runtime variables
bool gotMqttDetails = false;
bool mqttLoginNeeded = false;
int mqttSubCount = 0;
char* mqttTopics[MAX_SUB_TOPICS];

int relayClosed = 0;
int relayClosedCount = 0;
int statusCount = 0;

// global client defs
WiFiClient wifiClient;
HTTPClient http;
PubSubClient mqttClient(wifiClient);

// these functions write status info to the serial port
void writeDebug(String module, String message) {
  Serial.print("$DEBUG,");
  Serial.print(module);
  Serial.print(",");
  Serial.println(message);
}

void writeStatus(String module, String message) {
  Serial.print("$INFO,");
  Serial.print(module);
  Serial.print(",");
  Serial.println(message);
}

void writeError(String module, String message) {
  Serial.print("$ERROR,");
  Serial.print(module);
  Serial.print(",");
  Serial.println(message);
}

// helper to copy strings
void copyString(char **dest, const char **src) {
  *dest = (char*) malloc((strlen(*src)+1) * sizeof(char));
  strcpy(*dest, *src);
}

// helper to compare string suffix
bool cmpSuffix(char *str, const char *suffix) {
  if (!str || !suffix)
    return false;
  
  size_t lenstr = strlen(str);
  size_t lensuffix = strlen(suffix);

  if(lensuffix > lenstr)
    return false;
  
  return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

// handle messages from broker
void pubSubCallback(char* topic, byte* payload, unsigned int length) {
  // need to get the payload, the pointer to the byte array may contain more data so we need to use the length
  char* message = (char*) malloc(length + 1);
  memcpy(message, payload, length*sizeof(char));
  message[length] = '\0';  // null terminate the string
    
  if(DEBUG) {
    writeDebug("topic-in", topic);
    writeDebug("message-in", message);
  }

  // check if this is the state topic
  if(cmpSuffix(topic, "state")) {
    writeStatus("message", "got state message");
    if(strcmp(message, "closed") == 0) {
      writeStatus("state", "got a state = closed message, pin going low");
      relayClosedCount = 0;
      relayClosed = 1;
      digitalWrite(RELAY_PIN, LOW);
    }
  }

  // check if it is the reset topic
  if(cmpSuffix(topic, "reset")) {
    writeStatus("reset", "got a reset request, restarting...");
    ESP.restart();
  }

  free(message);
}

// subscribes to an MQTT topic
bool subscribeToTopic(char* topic) {
  if(!mqttClient.connected()) {
    writeError("mqtt", "can't subscribe if not connected");
    return false;
  }
  if(mqttSubCount == MAX_SUB_TOPICS) {
    writeError("mqtt", "no spare subscriptions");
    return false;
  }
  if(_subscribeToTopic(topic)) {
    // save topic in list of topics
    const char* cTopic = (const char*)topic;
    copyString(&mqttTopics[mqttSubCount], &cTopic);
    mqttSubCount++;
    return true;
  } 

  // if we got here then we failed
  return false;
}

bool _subscribeToTopic(char* topic) {
  if(mqttClient.subscribe(topic)) {
    char message[100];
    sprintf(message, "subscribed to %s", topic);
    writeStatus("mqtt", message);
    return true;
  } else {
    char message[100];
    sprintf(message, "failed to subscribe to %s", topic);
    writeError("mqtt", message);
    return false;
  }
}

// publish messages to an MQTT topic
bool publishMessage(char* topic, char* message, bool retained) {
  if(WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
    mqttClient.publish(topic, message, retained);
  }
  return false;
}

// init MQTT client
void initMQTTClient() {
  // Connecting to MQTT server
  mqttClient.setServer(mqttBrokerIp, atoi(mqttBrokerPort));
  if(DEBUG) {
    writeDebug("mqtt", mqttBrokerIp);
    writeDebug("mqtt", mqttBrokerPort);
  }
  mqttClient.setCallback(pubSubCallback);
  if(mqttLoginNeeded) {
    writeStatus("mqtt", "attempting to connect with credentials");
    if(mqttClient.connect(mqttClientName, mqttBrokerUsername, mqttBrokerPassword)) {
      writeStatus("mqtt", "connected");
    } else {
      writeError("mqtt", "failed to connect");
    }
  } else {
    writeStatus("mqtt", "attempting to connect without credentials");
    if(mqttClient.connect(mqttClientName)) {
      writeStatus("mqtt", "connected");
    } else {
      writeError("mqtt", "failed to connect");
    }
  }
  // if we get here are are connected then we can subscribe to our list of topics, this is useful for reconnect scenarios
  if(mqttClient.connected()) {
    writeStatus("mqtt", "subscribing to any saved topics...");
    for(int i = 0; i < mqttSubCount; i++) {
      // need to call raw subscription method as we don't want to save this again
      _subscribeToTopic(mqttTopics[i]);
    }
  }
}

bool initWifiStation() {
  int loopCount = 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSsid, wifiPassword);    
  writeStatus("wifi", "connecting");
  while (WiFi.status() != WL_CONNECTED && loopCount < MAX_WIFI_WAIT) {
     delay(1000);
     writeStatus("wifi", "still connecting");
     loopCount++;
  }
  if(WiFi.status() == WL_CONNECTED) {
    writeStatus("wifi", "connected");
    return true;
  } else {
    writeError("wifi", "connection timed out");
    return false;
  }
}

bool getConfig() {
  // get my mac address
  String mac = WiFi.macAddress();
  mac.replace(":", "-");

  // request config from API
  char url[50];
  sprintf(url, "http://%s/", apiEndpoint);
  http.useHTTP10(true);
  http.begin(wifiClient, url);
  http.addHeader("x-client-mac", mac);
  http.GET();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  if (error) {
    writeError("config", error.c_str());
    return false;
  }

  writeStatus("config", "got response from API");
  const char* apiError = doc["error"] | "unknown";
  if(apiError != "unknown") {
    writeError("config", apiError);
    return false;
  }
  
  writeStatus("config", "no error in response");

  // attempt to get MQTT broker details
  const char* mqttHost = doc["broker"]["host"];
  if (mqttHost) {
    gotMqttDetails = true;
    copyString(&mqttBrokerIp, &mqttHost);

    // get the rest of the details, port and client name
    const char* mqttPort = doc["broker"]["port"];
    copyString(&mqttBrokerPort, &mqttPort);

    // client name
    const char* mqttClient = doc["broker"]["clientname"];
    copyString(&mqttClientName, &mqttClient);

    // check if username/password is specified
    const char* username = doc["broker"]["username"];
    if (username) {
      mqttLoginNeeded = true;
      copyString(&mqttBrokerUsername, &username);

      // password
      const char* password = doc["broker"]["password"];
      copyString(&mqttBrokerPassword, &password);
    }

    // get topic prefix
    const char* topicPrefix = doc["topicprefix"];
    copyString(&mqttTopicPrefix, &topicPrefix);
  }

  return true;
}

void checkForUpdates() {
  char url[50];
  sprintf(url, "http://%s/update", apiEndpoint);

  t_httpUpdate_return ret = ESPhttpUpdate.update(wifiClient, url, "");
  switch(ret) {
    case HTTP_UPDATE_FAILED:
      writeError("update", "Update failed");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      writeStatus("update", "No update available");
      break;
    case HTTP_UPDATE_OK:
      writeStatus("update", "Update okay.");
      break;
  }
}

bool bringBoardOnline() {
  // attempt to connect to the network
  if(initWifiStation()) {
    // we are connected
    writeStatus("ip", WiFi.localIP().toString());
    
    // check if updates are available
    if(UPGRADE_ENABLE == 1) {
      checkForUpdates();
    }
    
    // get config
    getConfig();

    // if we got MQTT broker details we need to connect
    if(gotMqttDetails) {
      initMQTTClient();

      // if we are connected, we need to subscribe to the reset topic
      char topic[100];
      sprintf(topic, "%s/reset", mqttTopicPrefix);
      subscribeToTopic(topic);
    }
    
  }

  return true;
}

// subscribe to the topics we need for our implementation
void subscribe() {
  char topic[100];
  sprintf(topic, "%s/state", mqttTopicPrefix);
  subscribeToTopic(topic);
}

void setup() {
  // set pin modes
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_PIN, OUTPUT);

  // set pin state, as the pins sink current they should be high
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(STATUS_PIN, HIGH);

  // setup serial
  Serial.begin(115200);
  Serial.println();

  // write hello
  writeStatus("hello", "world");

  // init the board
  bringBoardOnline();

  // implementation specific subscriptions
  subscribe();
}

void sendStatusUpdate() {
  JsonDocument doc;
  doc["code"] = ESP.getSketchMD5();
  doc["fh"] = ESP.getFreeHeap();
  doc["uptime"] = millis();
  char output[256];
  serializeJson(doc, output);

  char topic[100];
  sprintf(topic, "%s/status", mqttTopicPrefix);
  publishMessage(topic, output, true);
}

void sendRelayOpen() {
  char topic[100];
  sprintf(topic, "%s/state", mqttTopicPrefix);
  publishMessage(topic, "open", true);
}

void loop() {
  // check if Wifi disconnected, and attempt to connect if it has
  if(WiFi.status() != WL_CONNECTED) {
    digitalWrite(STATUS_PIN, HIGH);
    // we are not connected to WiFi
    writeStatus("wifi", "disconnected");
    if(initWifiStation()) {
      // we have reconnected
      writeStatus("ip", WiFi.localIP().toString());
    } else {
      // we failed to reconnect
      // next time the loop runs we'll try again
      writeError("wifi", "did not connect");
    }
  } else {
    // we were already connected
    // check MQTT broker is connected
    if(mqttClient.connected()) {
      // we are connected, so loop
      mqttClient.loop();
      digitalWrite(STATUS_PIN, LOW);
    } else {
      // we are not connected, so try again
      digitalWrite(STATUS_PIN, HIGH);
      writeStatus("mqtt", "disconnected");
      initMQTTClient();

      // try to subscribe if nothing is subscribed
      if(mqttSubCount < 2) {
        writeStatus("mqtt", "re-attempting subscriptions");
        subscribe();
      }
    }
  }

  // wait one second
  delay(1000);

  // check if we've reached the point where we need to print the status message
  // and send a status update to MQTT
  if(statusCount >= STATUS_TIME) {
    writeStatus("main", "in main loop");
    sendStatusUpdate();
    statusCount = 0;
  }

  // relay management
  if(relayClosed == 1) {
    if(relayClosedCount >= RELAY_TIME) {
      writeStatus("state", "changing pin low");
      digitalWrite(RELAY_PIN, HIGH);
      relayClosed = 0;
      sendRelayOpen();
    }
    relayClosedCount++;
  }

  // increment status count
  statusCount++;
}
