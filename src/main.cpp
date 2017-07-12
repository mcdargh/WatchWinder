#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>


/* Place your settings in the settings.h file for ssid, password, and dweet post string
FILE: settings.h
// WiFi settings
const char* ssid = "MYSSID";
const char* password = "MyPassword";
const char* dweet = "/dweet/for/myservice?log=";
const char* otapwd = "supersecret";
NOTE:  Be sure to set this password also in the auth flag in platform.io
*/
#include "settings.h"

// Host
const char* host = "dweet.io";

// Direction Constants
enum WinderDirection_t {ClockWise, CounterClockWise, All } ;
enum StateMachineMode_t { NotRunning, Idle, Winding};

// state machines
struct StateMachine_t{
  uint16_t count, onlimit, idlelimit;
  StateMachineMode_t mode;
  WinderDirection_t direction, current_direction;
  uint8_t pwm, pina, pinb;
  float TPD;  // turns per day
  const char* name;
};

// pin and timer defines
#define Standby 14
#define SPEED 500
#define PWMFREQ 15000

// PWM A
#define RolexTPD 650.0 // 650 TPD
#define RolexDir All
#define RolexPWM 15
#define RolexPINA 12
#define RolexPINB 13
#define RolexNAME "Rolex"

// PWM B
#define OmegaTPD 800.0
#define OmegaDir Clockwise
#define OmegaPWM 5
#define OmegaPINA 2
#define OmegaPINB 4
#define OmegaNAME "Omega"

#define RPM 8.0

// forwards
void connectToSerial(void);
bool connectToWifi(void);
void sendToDweet(const char* log);
void setSM(StateMachine_t& sm, float tpd, WinderDirection_t direction,
    uint8_t pwm, uint8_t pina, uint8_t pinb, const char* name);
void checkSM(StateMachine_t& sm);
void startWinding(StateMachine_t& sm);
void stopWinding(StateMachine_t& sm);
void switchDirection(StateMachine_t& sm);
void setupOTA();
void log(const char* msg);

StateMachine_t RolexSM;
StateMachine_t OmegaSM;
bool wifiConnected = false;

void setup()
{
  connectToSerial();
  if(connectToWifi()) {
    setupOTA();
  }

  //Setup outputs
  setSM(RolexSM, RolexTPD, All, RolexPWM, RolexPINA, RolexPINB, RolexNAME);
  setSM(OmegaSM, OmegaTPD, ClockWise, OmegaPWM, OmegaPINA, OmegaPINB, OmegaNAME);
}

void connectToSerial() {
  Serial.begin(115200);
  while(!Serial); //wait for serial port to connect (needed for Leonardo only)
  Serial.println("Booting...");
}

bool connectToWifi() {
  int numTries = 10;
  // Connect to WiFi
   WiFi.mode(WIFI_STA);
   WiFi.begin(ssid, password);
   while ((WiFi.status() != WL_CONNECTED) && (numTries > 0)) {
     numTries--;
     Serial.print(".");
     delay(1000);
   }
   if(WiFi.status() != WL_CONNECTED) {
     Serial.println("WIFI not connected");
     return false;
   }

   wifiConnected = true;
   // Print the IP address
   log(WiFi.localIP());
   return true;
}


void setupOTA() {
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("watchwinder");

  // No authentication by default
  ArduinoOTA.setPassword(otapwd);

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());  
}

void setSM(StateMachine_t& sm, float tpd, WinderDirection_t direction,
    uint8_t pwm, uint8_t pina, uint8_t pinb, const char* name) {
  memset(&sm, 0x00, sizeof(StateMachine_t));
  sm.direction = direction;
  sm.TPD = tpd;
  sm.mode = NotRunning;
  sm.pwm = pwm;
  sm.pina = pina;
  sm.pinb = pinb;
  sm.name = name;

  // set times
  float onlimit = ((tpd / 24.0) / RPM) * 60.0;
  // how many seconds running per hour
  sm.onlimit = ceil(onlimit);
  // how many seconds idle per hour
  sm.idlelimit = 60*60 - sm.onlimit;

  // set start direction
  switch(direction) {
    case CounterClockWise:
      sm.current_direction = CounterClockWise;
      break;
    case All:         // all and clockwise are implied in all but for readability...
    case ClockWise:
    default :
      sm.current_direction = ClockWise;
  }

  // set up pins
  pinMode(sm.pina, OUTPUT);
  pinMode(sm.pinb, OUTPUT);
  pinMode(sm.pwm, OUTPUT);
  pinMode(Standby, OUTPUT);
  analogWriteFreq(PWMFREQ);
}

void loop()
{
  unsigned long start = millis();

  if(wifiConnected) {
    ArduinoOTA.handle();
  }

  Serial.println("In loop");

  // check state machine
  checkSM(OmegaSM);
  checkSM(RolexSM);
  // if nobody running, go to standby mode
  if((OmegaSM.mode == Idle) && (RolexSM.mode == Idle) && (digitalRead(Standby) != LOW)) {
    log("Motor control to idle");
    digitalWrite(Standby, LOW);
  }

  // try to keep the loop at one second in between passes
  unsigned long end = millis();
  unsigned long diff = 1000 - (end - start);
  // sanity check
  if(diff > 1000) {
    diff = 1000;
  } else if(diff < 0) {
    diff = 0;
  }
  delay(diff);
}

void checkSM(StateMachine_t& sm) {
    switch(sm.mode) {
      case NotRunning:
        // start it
        sm.mode = Winding;
        sm.count = sm.onlimit;
        startWinding(sm);
        break;
      case Idle :
        // add second to idle time
        sm.count--;
        if(sm.count <= 0) {
          sm.mode = Winding;
          sm.count = sm.onlimit;
          startWinding(sm);
        }
        break;
      case Winding :
        sm.count--;
        if(sm.count <= 0) {
          sm.mode = Idle;
          sm.count = sm.idlelimit;
          stopWinding(sm);
          switchDirection(sm);
        }
    }
}


void sendToDweet(const char* log) {
  char msg[512];

  if(!wifiConnected) {
    return;
  }
  
  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(host, httpPort)) {
    Serial.println("connection failed");
    delay(100);
    yield;
    return;
  }

  // make log entry websafe
  String slog = String(log);
  slog.replace(String(" "), String("%20"));
  
  snprintf(msg, 512, "GET %s%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", dweet, slog.c_str(), host);
  // This will send the request to the server
  client.print(String(msg));

  delay(100);
  yield;

  // Read all the lines of the reply from server and print them to Serial
  while(client.available()){
    String line = client.readStringUntil('\r');
    Serial.println(line);
  }
}

void startWinding(StateMachine_t& sm) {
  char slog[256];

  analogWrite(sm.pwm, SPEED);
  if(sm.current_direction == ClockWise) {
    digitalWrite(sm.pina, HIGH);
    digitalWrite(sm.pinb, LOW);
  } else {
    digitalWrite(sm.pina, LOW);
    digitalWrite(sm.pinb, HIGH);
  }
  digitalWrite(Standby, HIGH);

  snprintf(slog, sizeof(slog), "Started motor %s", sm.name);
  log(slog);
}

void switchDirection(StateMachine_t& sm) {
  switch(sm.direction) {
    case ClockWise :
    case CounterClockWise:
      return;
  }
  if(sm.current_direction == ClockWise) {
    sm.current_direction = CounterClockWise;
  } else {
    sm.current_direction = ClockWise;
  }
}

void stopWinding(StateMachine_t& sm) {
  char slog[256];

  analogWrite(sm.pwm, 0);
  digitalWrite(sm.pina, HIGH);
  digitalWrite(sm.pinb, HIGH);

  snprintf(slog, sizeof(slog), "Stopped motor %s", sm.name);
  log(slog);
}

void log(const char* msg) {
  Serial.print(msg);
  if(wifiConnected) {
    sendToDweet(msg);
  }
}