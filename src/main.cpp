#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Wire.h>

/* Place your settings in the settings.h file for ssid, password, and dweet post string
FILE: settings.h
// WiFi settings
const char* ssid = "MYSSID";
const char* password = "MyPassword";
const char* dweet = "/dweet/for/myservice?uv="
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
#define Standby 16
#define SPEED 500
#define PWMFREQ 15000

#define RolexTPD 650.0 // 650 TPD
#define RolexDir All
#define RolexPWM 5
#define RolexPINA 4
#define RolexPINB 2
#define RolexNAME "Rolex"

#define OmegaTPD 800.0
#define OmegaDir Clockwise
#define OmegaPWM 14
#define OmegaPINA 12
#define OmegaPINB 13
#define OmegaNAME "Omega"

#define RPM 8.0

// forwards
void connectToSerial(void);
void connectToWifi(void);
void sendToDweet(const char* log);
void setSM(StateMachine_t& sm, float tpd, WinderDirection_t direction,
    uint8_t pwm, uint8_t pina, uint8_t pinb, const char* name);
void checkSM(StateMachine_t& sm);
void startWinding(StateMachine_t& sm);
void stopWinding(StateMachine_t& sm);
void switchDirection(StateMachine_t& sm);

StateMachine_t RolexSM;
StateMachine_t OmegaSM;

void setup()
{
  connectToSerial();
  connectToWifi();

  //Setup outputs
  setSM(RolexSM, RolexTPD, All, RolexPWM, RolexPINA, RolexPINB, RolexNAME);
  setSM(OmegaSM, OmegaTPD, ClockWise, OmegaPWM, OmegaPINA, OmegaPINB, OmegaNAME);
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
  sm.idlelimit = 24*60*60 - sm.onlimit;

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


void connectToSerial() {
  Serial.begin(9600);
  while(!Serial); //wait for serial port to connect (needed for Leonardo only)
}

void connectToWifi() {
  // Connect to WiFi
   WiFi.begin(ssid, password);
   while (WiFi.status() != WL_CONNECTED) {
     delay(500);
     Serial.print(".");
   }
   Serial.println("");
   Serial.println("WiFi connected");

   // Print the IP address
   Serial.println(WiFi.localIP());
}


void loop()
{
  char log[256];

  Serial.println("In loop");

  // check state machine
  checkSM(OmegaSM);
  checkSM(RolexSM);
  // if nobody running, go to standby mode
  if((OmegaSM.mode == Idle) && (RolexSM.mode == Idle)) {
    Serial.println("Motor control to idle");
    digitalWrite(Standby, LOW);
  }

  delay(1000);
}

void checkSM(StateMachine_t& sm) {
    switch(sm.mode) {
      case NotRunning:
        // start it
        sm.mode = Winding;
        sm.count = sm.onlimit;
        startWinding(sm);
        Serial.printf("Motor %s switched from Not Running to Winding\n", sm.name);
        break;
      case Idle :
        // add second to idle time
        sm.count--;
        if(sm.count <= 0) {
          sm.mode = Winding;
          sm.count = sm.onlimit;
          startWinding(sm);
          Serial.printf("Motor %s switched from Idle to Winding\n", sm.name);
        }
        break;
      case Winding :
        sm.count--;
        if(sm.count <= 0) {
          sm.mode = Idle;
          sm.count = sm.idlelimit;
          stopWinding(sm);
          switchDirection(sm);
          Serial.printf("Motor %s switched from Winding to Idle\n", sm.name);
        }
    }
}


void sendToDweet(const char* log) {
  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(host, httpPort)) {
    Serial.println("connection failed");
    delay(1000);
    return;
  }

  // This will send the request to the server
  client.print(String("POST ") + dweet + log + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Connection: close\r\n\r\n");
  delay(10);

  // Read all the lines of the reply from server and print them to Serial
  while(client.available()){
    String line = client.readStringUntil('\r');
  }
}

void startWinding(StateMachine_t& sm) {
  char log[256];

  digitalWrite(Standby, HIGH);
  if(sm.current_direction == ClockWise) {
    digitalWrite(sm.pina, HIGH);
    digitalWrite(sm.pinb, LOW);
  } else {
    digitalWrite(sm.pina, LOW);
    digitalWrite(sm.pinb, HIGH);
  }
  analogWrite(sm.pwm, SPEED);

  snprintf(log, sizeof(log), "Started motor %s\n", sm.name);
  sendToDweet(log);
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
  char log[256];

  analogWrite(sm.pwm, 0);
  digitalWrite(sm.pina, HIGH);
  digitalWrite(sm.pinb, HIGH);

  snprintf(log, sizeof(log), "Started motor %s\n", sm.name);
  sendToDweet(log);
}
