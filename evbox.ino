/* Arduino IDE settings
 * Board:      Generic ESP8266
 * Flash size: 1M (no SPIFFS)
 * Port:       
 */
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TimeLib.h>
#include <PubSubClient.h>

// ******** Definitions General ********
const bool debug = false;
bool first_loop = true;

// ******** Definitions Wifi ********
const char* hostName = "ev-box";
const char* WiFi_ssid = "??????????";
const char* WiFi_password = "??????????";

// ******** MQTT ********
const char*  MQTT_server = "??????????";
unsigned int MQTT_serverport = 1883;
const char*  MQTT_ClientID = hostName;
WiFiClient WiFi_Client;
PubSubClient MQTT_CLIENT(WiFi_Client);

// ******** Definitions NTP (Network Time Protocol) ********
unsigned int ntpLocalPort = 2390;
const char* ntpServerName = "nl.pool.ntp.org";  // time.nist.gov
const int NTP_PACKET_SIZE = 48;  // NTP timestamp is in the first 48 bytes of the message
byte ntpPacketBuffer[NTP_PACKET_SIZE];  // buffer to hold incoming and outgoing packets
WiFiUDP udp;

// ******** Definitions Cron (Run a job once every interval) ********
// Cron1
const int cron1_interval = 300;    // [seconds] 5 minutes
const int cron1_offset = 0;
unsigned long cron1_previous_timestamp;
// Cron2
const int cron2_interval = 86400;  // [seconds] 1 day
const int cron2_offset = 4*3600 + 0*60 + 10;  // [seconds] at 04:00:10
unsigned long cron2_previous_timestamp;

// ******** Definitions Serial data ********
#define MAX_MESSAGE_SIZE 512
char evbox_data1[MAX_MESSAGE_SIZE];
unsigned int evbox_data1_length = 0;
byte evbox_data1_stage = 0;  // (0=empty, 1=after2, 2=messageComplete)
char evbox_data2[MAX_MESSAGE_SIZE];
unsigned int evbox_data2_length = 0;
byte evbox_data2_stage = 0;  // (0=empty, 1=after2, 2=messageComplete)
byte evbox_data_active = 1;  // (1 or 2)
unsigned long last_evbox_data_received = 0;

// ********************************
void setup() {
  // ******** Setup Console ********
  if (debug) Serial.begin(115200);   // Serial0 connected to USB

  // ******** Setup Wifi ********
  WiFi.hostname(hostName);
  WiFi.mode(WIFI_STA);
  connectWiFi();

  // ******** Setup OTA (Over the air firmware updates) ********
  // Port defaults to 8266
  //ArduinoOTA.setPort(8266);
  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(hostName);
  // Authentication. Default = no password
  //ArduinoOTA.setPassword("admin");
  // Password can be set with it's md5 hash value as well
  //ArduinoOTA.setPasswordHash("c30ab0d97f98a27b96a91e4ddb683f11");
  ArduinoOTA.onStart([]() {
    if (debug) Serial.println("Start updating.");
  });
  ArduinoOTA.onEnd([]() {
    if (debug) Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (debug) Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    if (debug) Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) if (debug) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) if (debug) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) if (debug) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) if (debug) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  // ******** Setup NTP ********
  if (setCurrentDateTime()) {
    if (debug) {
      Serial.print("Current date/time set: ");
      char strDateTime[20];
      formatDateTime(strDateTime,now());
      Serial.println(strDateTime);
    }
  }
  else {
    if (debug) Serial.println("Failed to get current DateTime from NTP servers");
  }
  //setSyncProvider(getTimeFunction);// Set the external time provider
  //setSyncInterval(interval); // Set the number of seconds between re-syncs

  // ******** Setup Cron ********
  cron1_previous_timestamp = (now() - cron1_offset) / cron1_interval * cron1_interval + cron1_offset;
  cron2_previous_timestamp = (now() - cron2_offset) / cron2_interval * cron2_interval + cron2_offset;

  // ******** Setup Serial ********
  Serial.begin(38400);

  // ******** Setup MQTT ********
  MQTT_CLIENT.setBufferSize(1024);  // Override MQTT_MAX_PACKET_SIZE 256 in PubSubClient.h
  MQTT_CLIENT.setServer(MQTT_server, MQTT_serverport);  // Set MQTT broker address and port
  reconnectMQTT();
  MQTT_CLIENT.setCallback(MQTT_callback);
  MQTT_CLIENT.publish("tele/ev-box/lwt", "Online", true);
  
  // ****************
  if (debug) Serial.println("Ready.");
}


void connectWiFi() {
  if (debug) Serial.print("\r\nConnecting to Wifi");
  WiFi.begin(WiFi_ssid, WiFi_password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (debug) Serial.print(".");
  }
  if (debug) Serial.print("Connected.\r\nIP address: ");
  if (debug) Serial.println(WiFi.localIP());
  if (debug) Serial.print("RSSI: ");
  if (debug) Serial.println(WiFi.RSSI());
}

unsigned long sendNTPpacket() {
  if (debug) Serial.println("Sending NTP packet");
  memset(ntpPacketBuffer, 0, NTP_PACKET_SIZE);  // set all bytes in the buffer to 0
  // Create NTP request
  ntpPacketBuffer[0] = 0b11100011;   // LI, Version, Mode
  ntpPacketBuffer[1] = 0;     // Stratum, or type of clock
  ntpPacketBuffer[2] = 6;     // Polling Interval
  ntpPacketBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  ntpPacketBuffer[12] = 49;
  ntpPacketBuffer[13] = 0x4E;
  ntpPacketBuffer[14] = 49;
  ntpPacketBuffer[15] = 52;
  
  IPAddress timeServerIP;
  WiFi.hostByName(ntpServerName, timeServerIP);  //get a random server from the pool
  if (debug) Serial.print("TimeServer IP: ");
  if (debug) Serial.println(timeServerIP);
  
  udp.beginPacket(timeServerIP, 123);  // use port 123
  udp.write(ntpPacketBuffer, NTP_PACKET_SIZE);  // Send ntpPacket
  udp.endPacket();
}

bool setCurrentDateTime() {
  unsigned long currentDateTime = 0;
  
  udp.begin(ntpLocalPort);
  
  int retry_counter = 10;
  while (!currentDateTime && retry_counter>0) {
    sendNTPpacket(); // Request time
    
    if (debug) Serial.print("Waiting for NTP response");
    int wait_counter = 6;
    int packetSize = udp.parsePacket();
    while (!packetSize && wait_counter>0) {
      if (debug) Serial.print(".");
      delay(1000);
      packetSize = udp.parsePacket();
      wait_counter--;
    }
    if (debug) Serial.println();
    
    if (packetSize) {
      // Packet received.
      if (debug) Serial.print("NTP Packet received with size: ");
      if (debug) Serial.println(packetSize);
      
      udp.read(ntpPacketBuffer, NTP_PACKET_SIZE);  // read the packet into the buffer
    
      // The timestamp starts at byte 40 of the received packet and is 4 bytes,
      // or 2 words, long. First, extract the two words:
      unsigned long highWord = word(ntpPacketBuffer[40], ntpPacketBuffer[41]);
      unsigned long lowWord = word(ntpPacketBuffer[42], ntpPacketBuffer[43]);
      // Combine the 4 bytes (2 words)
      unsigned long secsSince1900 = highWord << 16 | lowWord;  // seconds since 1-JAN-1900 UTC
      currentDateTime = secsSince1900 - 2208988800UL;  // subtract 70 years (Unix time starts on 01-JAN-1970)
      
      setTime(currentDateTime);  // Set current DateTime
    }
    retry_counter--;
  }
  return (currentDateTime != 0);
}

void formatDateTime(char* destination,time_t t) {
  int yyyy = year(t);
  int mm = month(t);
  int dd = day(t);
  int h = hour(t);
  int m = minute(t);
  int s = second(t);
  snprintf(destination,20,"%04d-%02d-%02d %02d:%02d:%02d",yyyy,mm,dd,h,m,s);
}

void reconnectMQTT() {
  while (!MQTT_CLIENT.connected()) {
    MQTT_CLIENT.connect(MQTT_ClientID,"tele/ev-box/lwt",1,true,"Offline");
    byte retry_counter = 12;
    while (!MQTT_CLIENT.connected() && retry_counter-- > 0) delay(250);
  }
  MQTT_CLIENT.subscribe("cmnd/ev-box");
  if (!first_loop) MQTT_CLIENT.publish("tele/ev-box/lwt", "Reconnected", true);
}

int MQTT_publish(const char* topic, const char* payload) {
  if (!MQTT_CLIENT.connected()) reconnectMQTT();
  return MQTT_CLIENT.publish(topic, payload);
}

void MQTT_callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic,"cmnd/ev-box")==0) {
    char message[length+1];
    memcpy(message,payload,length);
    message[length]=0;
    write_evbox(message);
  }
  
}

void cron1_job(unsigned long timestamp) {  // Every 5 minutes.
  if (debug) Serial.println("5 min.");

}

void cron2_job(unsigned long timestamp) {  // Every day at 04:00:00 UTC
  if (debug) Serial.print("Old time:");
  char strDateTime[20];
  formatDateTime(strDateTime,now());
  if (debug) Serial.println(strDateTime);
  setCurrentDateTime();
  if (debug) Serial.println("Time synced.");
  if (debug) Serial.print("New time:");
  formatDateTime(strDateTime,now());
  if (debug) Serial.println(strDateTime);
}

bool isChecksumOk(char* message) {
  if (strlen(message) < 5) return false;
  byte CheckSumModulo256 = message[0];
  byte CheckSumXOR = message[0];
  int i;
  for(i=1; i<strlen(message)-4; i++) {
    CheckSumModulo256 += message[i];
    CheckSumXOR ^= message[i];
  }
  char checksum[5];
  snprintf(checksum,5,"%02X%02X",CheckSumModulo256,CheckSumXOR);
  return memcmp(&message[strlen(message)-4], checksum, 4) == 0;
}

void createChecksum(char* message, char* checksum) {
  byte CheckSumModulo256 = message[0];
  byte CheckSumXOR = message[0];
  int i;
  for(i=1; i<strlen(message); i++) {
    CheckSumModulo256 += message[i];
    CheckSumXOR ^= message[i];
  }
  snprintf(checksum,5,"%02X%02X",CheckSumModulo256,CheckSumXOR);
}

void read_evbox() {
  if (Serial.available()) {
    last_evbox_data_received = millis();
    byte input = Serial.read();  // Read 1 byte
    if (evbox_data_active == 1) {
      if (evbox_data1_length < MAX_MESSAGE_SIZE) {
        if (evbox_data1_stage == 0 && input == 2) evbox_data1_stage = 1;
        else if (evbox_data1_stage == 1) {
          if (input==2) evbox_data1_length = 0;  // Another Start of message detected
          else if (input==3) {
            evbox_data1[evbox_data1_length] = 0;
            evbox_data1_stage = 2;
            evbox_data_active = 2;
          }
          else
            evbox_data1[evbox_data1_length++] = input;
        }
      }
      else
        evbox_data1_length = 0;  // If the message is too big, we skip it.
    }
    else {
      if (evbox_data2_length < MAX_MESSAGE_SIZE) {
        if (evbox_data2_stage == 0 && input == 2) evbox_data2_stage = 1;
        else if (evbox_data2_stage == 1) {
          if (input==2) evbox_data2_length = 0;  // Another Start of message detected
          else if (input==3) {
            evbox_data2[evbox_data2_length] = 0;
            evbox_data2_stage = 2;
            evbox_data_active = 1;
          }
          else
            evbox_data2[evbox_data2_length++] = input;
        }
      }
      else
        evbox_data2_length = 0;  // If the message is too big, we delete it.
    }
  }
  else {
    unsigned long ms = millis();
    // If it takes more than 2 seconds to receive a message, we delete it.
    if (evbox_data_active == 1) {
      if (evbox_data1_length > 0 && ms - last_evbox_data_received > 2000) evbox_data1_length = 0;
    }
    else {
      if (evbox_data2_length > 0 && ms - last_evbox_data_received > 2000) evbox_data2_length = 0;
    }
  }
}

void write_evbox(char* command) {
  unsigned int message_length = strlen(command) + 7;  // Extra length for start/stop byte, checksum and nul terminator.
  char message[message_length];
  char checksum[5];
  createChecksum(command,checksum);
  snprintf(message,message_length,"%c%s%s%c",2,command,checksum,3);
  Serial.print(message);
}

void send_evbox_message_as_MQTT() {
  char payload[1000];

  if (evbox_data1_stage == 2){
    if (isChecksumOk(evbox_data1)) {
      evbox_data1[evbox_data1_length - 4] = 0;  // trim checksum from message
      snprintf(payload,1000,"{ \"epoch\": %lu, \"data\": \"%s\" }", now(), evbox_data1 );
      MQTT_publish("stat/ev-box", payload);
    }
    else {
      snprintf(payload,1000,"{ \"epoch\": %lu, \"checksumError\": true, \"data\": \"%s\" }", now(), evbox_data1 );
      MQTT_publish("dbug/ev-box", payload);
    }
    evbox_data1_length = 0;
    evbox_data1_stage = 0;
  }
  if (evbox_data2_stage == 2){
    if (isChecksumOk(evbox_data2)) {
      evbox_data2[evbox_data2_length - 4] = 0;  // trim checksum from message
      snprintf(payload,1000,"{ \"epoch\": %lu, \"data\": \"%s\" }", now(), evbox_data2 );
      MQTT_publish("stat/ev-box", payload);
    }
    else {
      snprintf(payload,1000,"{ \"epoch\": %lu, \"checksumError\": true, \"data\": \"%s\" }", now(), evbox_data2 );
      MQTT_publish("dbug/ev-box", payload);
    }
    evbox_data2_length = 0;
    evbox_data2_stage = 0;
  }
}

// ********************************
void loop() {
  // OTA
  ArduinoOTA.handle();

  // WiFi
  if (WiFi.status() != WL_CONNECTED) connectWiFi();  // If connection has dropped, reconnect Wifi
  yield();
  
  // MQTT
  if (!MQTT_CLIENT.connected()) reconnectMQTT();
  yield();
  
  // Cron1
  time_t t = now();
  unsigned long cron_timestamp = (t - cron1_offset) / cron1_interval * cron1_interval + cron1_offset;
  if (cron_timestamp > cron1_previous_timestamp) {
    cron1_job(cron_timestamp);  // Run the cron1_job
    cron1_previous_timestamp = t;
  }
  yield();
  // Cron2
  t = now();
  cron_timestamp = (t - cron2_offset) / cron2_interval * cron2_interval + cron2_offset;
  if (cron_timestamp > cron2_previous_timestamp) {
    cron2_job(cron_timestamp);  // Run the cron2_job
    cron2_previous_timestamp = t;
  }
  yield();
  
  // Read ev-box data
  read_evbox();
  yield();
  
  // Send complete ev-box-message as MQTT
  send_evbox_message_as_MQTT();
  yield();

  // MQTT
  MQTT_CLIENT.loop();
  
  
  if (first_loop) {
    //MQTT_publish("stat/ev-box", "");
  }
  first_loop = false;
}
