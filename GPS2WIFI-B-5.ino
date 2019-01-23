
// Cleaned up for final installation. 

#include <ESP8266WiFi.h> 
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>

// A new type for the "find" function's return-values:
enum find_status {
  FIND_JUNK,     // The first character of the buffer was not a start character. 
  FIND_START,   // A "start" character was found after a start, but before the next "end"
  FIND_END,     // an "end" was found (normal!)
  FIND_OVERRUN, // more than 128 characters passed an initial start-char without either a start or an end
  FIND_PARTIAL // following the initial start character, there are some (but not 128) other non-start, non-end characters; we need to wait for more. 
};

find_status find(int&); 

// Over the air (OTA) programming setup -----------------------
bool otaFlag = true;       // allow over-the-air-programming
#define OTA_INTERVAL 30    // allow this many seconds after startup for the OTA programming process to work. 

// WiFi setup -------------------------------
bool wifiFlag = false;                   // true if wifi's been set up. 
WiFiManager wifiManager;                 // A maanger to help set up the wifi connection. 

IPAddress ipBroadcast;                   // @@ My Wifi network is set up as a class-C network, with 
                                         // IP addresses ranging from 192.168.1.1 to 192.168.1.255, with 
                                         // 255 being the "broadcast" address. 
                                         
#define NMEA_PORT 10110 // The not-quite standardized port for transmitting NMEA data over IP.  
WiFiUDP Udp;            // An instance of the WiFi UDP class that can send and receive UDP messages. 

// Alarm LED and Reset pin setup -------------------------
// A pin to which to attach an "alarm" LED to indicate sentence or buffer over-runs
#define ALARM_LED_PIN  0    // hooked to red LED to convey alarms/problems; alarmStart turns on the LED, which gets turned off after 1 second. 
#define STATUS_LED_PIN 2    // hooked to green led to convey normal operation/status
#define RESET_NETWORK_PIN 1 // hooked to button so we can pull low during startup to reset the whoel wifi thing, 
#define RESET_PERIOD_SECS 5 // you get five seconds in which to reset the bridge before it goes to normal operation. 
// Alarm LED setup
unsigned long alarmStart = 0; // when did we turn on the alarm (red) LED
bool alarmOn = false;         // is the LED on at all? 

// NMEA setup ------------------------------
// Speed of data arriving on pin 07 (U0RXD, input for Serial)
// Must use same speed for monitor if you're using that for debugging. 
#define NMEA_BAUD_RATE 38400

// Buffer setup ---------------------------
// The minimum number of characters that should accumulate in the input buffer before we
// do anything about it (i.e., try to assemble characters into a sentence to send out over
// the Wifi network.)
#define BLOCK_SIZE 20

// The size of the buffer in which to accumulate data; typically only about 50 characters of this
// buffer seem to be used in practice
#define SERIAL_BUFFER_SIZE 1024

////////////////////////////////////////////// NMEA-data globals ///////////////////////////////
byte espBuffer[SERIAL_BUFFER_SIZE];      // The data we've read from the ESP's serial buffer.
int  bufferCount = 0;       // The count of the un-processed data: we add a  \0 byte at espABuffer[bufferAEnd], to make the buffer a printable string. 
                            // Note that if 0 == bufferACount, then the buffer contains no unprocessed data. 

void setup() 
{ 
  int attempt_counter = 0; 
  
  // pinMode(LED_BUILTIN, OUTPUT); // need to NOT do this, or it disables serial input!
  pinMode(STATUS_LED_PIN, OUTPUT); 
  pinMode(ALARM_LED_PIN, OUTPUT); 
  
  delay(500); 
  beep (250,100); beep (250,100); beep (250,100); // three green flashes to say you can try resetting now

  pinMode(RESET_NETWORK_PIN, INPUT); // and this sets pin 1 to input. 
  int t = millis(); 
  while (1){
    if ((millis() - t) > 1000 * RESET_PERIOD_SECS) break;
    else if (digitalRead(1) == LOW) {
      // 10 quick flashes
      beep (50,50); beep (50,50); beep (50,50); beep (50,50); beep (50,50);
      beep (50,50); beep (50,50); beep (50,50); beep (50,50); beep (50,50);
      wifiManager.startConfigPortal("NMEA Bridge");
    }
    delay(20); // to give other processes time to do whatever they need. 
  }
  pinMode(RESET_NETWORK_PIN, OUTPUT); // and this sets pin 1 to output again

  Serial.begin(NMEA_BAUD_RATE);
  otaSetup(); // OTA Stuff to make the chip re-programmable later
  // blink the lights to make it clear that startup is happening. 
  message("Checking LEDs");
  beep(1000,1000);                       // A 1-second green blink
  flashAlarm("Startup alarm test");      // turn on red
  beep(1000, 200);                       // and immediately turn on green for 1 sec

  while (!wifiFlag) {
    if (attempt_counter++ > 10) break; // try ten times to
    wifiFlag = setupWiFi();            // establish WiFi for this bridge
  }
  alarmCheck();      // and when that's done, turn off alarm light. 

  if (wifiFlag) {
    ipBroadcast = WiFi.localIP(); 
    ipBroadcast[3] = 255;
    message("We are connected!\n");
    beep(2000, 200);   // A 2-second green light to say we're on our way. 
  }
  //  run_tests(); // uncomment this to run extensive tests of individual functions. 
  message("End of setup!\n");
  delay(40);
  if (!wifiFlag) {
    flashAlarm("Wifi setup failed");
  }
}

// If early (before  OTA_INTERVAL seconds) check to see if there's an OTA program update arriving; after that interval, disallow updates. 
// 
// Check whether the alarm light's been on for a second or more, and if so, shut it off. 
// Finally, check whether new NMEA data has arrived, and if there's enough of it, forward packets to UDP. 
void loop()
{ 
  if (otaFlag) { // If withing the OTA programming interval, look for OTA arriving data
    ArduinoOTA.handle();
    delay(20);
    if (millis() > 1000 * OTA_INTERVAL) {
      otaFlag = false; // after interval, stop OTA
      Umessage("OTA programming off");
    }
  }
  if (!wifiFlag) flashAlarm("No wifi"); // if no wifi hookup, just keep the alarm light on. 
  
  alarmCheck(); // check to see whether it's time to reset the alarm LED
  // Check and handle data on serial line (if the wifi setup worked)
  //message("foo\n");
  if (wifiFlag) dataCheck();  
}


// ============= NMEA data reading and assembly =====================================
// If there are enough new characters available, read them into the buffer and then process them 
// by sending out any complete sentences as UDP packets, and leaving any incomplete sentences 
// in the buffer. Always add a \0 at the end to make the buffer a valid string. 
//
// If there are too many characters to fit in the buffer, read as many as the buffer can hold (overwriting 
//    anything that was in there), signal an error,
//    and throw away everything that's was in the buffer and that we just read. 
void dataCheck()
{
  int s = Serial.available(); // number of characters available.
  int bc = bufferCount; 
  if (s < BLOCK_SIZE) return; 
  int t = min(s, SERIAL_BUFFER_SIZE - bufferCount - 1); // the number of characters we're willing to read in. 
  if (t < s) {
    flashAlarm("Input buffer overfull"); // We're not keeping up!
    Serial.readBytes(espBuffer, t); // have to empty the serial buffer!
    bufferCount = 0; // and throw out everything that was in it. 
    espBuffer[bufferCount] = '\0';
    bc = 0;
  }
  else {
      // beep(100, 200);      // A quick green blink
      bufferCount += Serial.readBytes(espBuffer + bufferCount, s);
      espBuffer[bufferCount] = '\0';
  }
  // if there are characters to be processed, do so
  if (bufferCount > 0) {
    while (consume()); // as long as "consume" alters the buffer, try to consume more.    
  }
}

// try to extract stuff from a buffer. If empty, return 0; if not, 
// use "find" to locate either (a) a start, (b) an end, (c) neither, but fewer than 128 characters, or (d) neither, but at least 128 characters. 
//        either extract a complete sentence of fewer than 129 characters, send it out by UDP, and shift the remainder of the buffer, and return 1 OR
//        determine that there's just a partial sentence in the buffer (i.e., Sxxxx ) and return 0
//        determine that there's garbage at the start of the buffer, either
//                 SxxxxSxxx (i.e., another "start" before a "finish") OR
//                 Sxxx<lots>xx (i.e., more than 127 characters after the S, with no F in sight) OR
//                 xxxx (no start character at all) OR
//                 xxxS... (some junk before the first start character)
//        in which case we shift out the known garbage (the incomplete first sentence, 128 characters of junk, stuff before start char, or just junk) and return 1

bool consume()
{
  byte sentence[130];        // The sentence we're currently assembling, limited to 128 characters by the NMEA standard

  if (bufferCount == 0) return false; // nothing to consume
  
  int locn; 
  find_status s = find(locn); // figure our what the start of the buffer looks like,
                              // and if there's something interesting (like a start-char), 
                              // record its location. 
 
  switch (s) {
    case FIND_JUNK:         // first char wasn't a start char. 
      discardBadBytes(bufferCount);          
                          // we should shift things until a sentence-start char
                          // is at location 0 (or all data is gone)  
      flashAlarm("consume: start of buffer not a starting character"); 
      return true; 
      break;
      // In all subsequent cases, first character is a start-char.
    case FIND_START: 
      shiftBuffer(locn);
      flashAlarm("consume: repeated start with no matching end"); 
      return true; 
      break;
  
    case FIND_END:   // Ah...normal behavior! THere's a complete sentence!
     for (int k = 0; k < locn; k++) {
        sentence[k] = espBuffer[k];
      }
      sentence[locn] = '\r';
      sentence[locn + 1] = '\n';      
      sentence[locn + 2] = '\0';
      sendUDP(sentence, locn + 2);
      shiftBuffer(locn + 1);
      return true;
      break;

    case FIND_OVERRUN:
      espBuffer[0] = 'a'; // get rid of any "start" character...
      discardBadBytes(bufferCount);           
                          // this really should never happen, but if it does
                          // we should shift things until a sentence-start char
                          // is at location 0 (or all data is gone).  
      flashAlarm("consume: overrun"); 
      return true; 
      break;
    case FIND_PARTIAL:
       // we've got part of a sentence, but can't do anything with it yet.
       return false; 
      break;
  }
}

// Look at the espBuffer, whose first character is generally a start char (! or $)
// If the first char is not a start-char, report that there's junk at the start of the 
// buffer. Otherwise...
// Search for the first "start" or "end" (\n) character after the first, or if there is none,
// a sequence of 128 non-start-or-end characters, or (often best) a sequence of
// fewer than 128 non-start-or-end characters. In the first two cases, return 
// the position of the special character in "pos"
find_status find(int &pos)
{
  if ((espBuffer[0] != '$') && (espBuffer[0] != '!'))
  {
    flashAlarm("Find status: buffer doesn't start with $ or !"); 
    return FIND_JUNK; 
  }
  // For all other cases, the buffer starts with a start-character
  byte *p = espBuffer+1;

  int last = min(bufferCount, 128);
  while (p - espBuffer < last) {
    switch(*p) {
      case '$':
      case '!':
        pos = p - espBuffer;  
        return FIND_START; 
      case '\n': 
        pos = p - espBuffer;
        return FIND_END;
      default:
        p++;
    }
  }
  if (p - espBuffer == 128){
    pos = 128; 
    return FIND_OVERRUN;
  }
  else {
    pos = p - espBuffer;
    return FIND_PARTIAL; 
  }
}

// discardBadBytes: "eat" (i.e., examine and then throw away) at most n characters from the start of the espBuffer, 
// stopping at just before a sentence-start 
// character ($ or !) or at the end of the buffer, or after eating n characters. If any eating takes place,
// signal an error (because we're throwing away data!). Return the number of characters eaten (including 
// possibly zero). To "eat" 10 characters, we shift buf[10..end] to buf[0..(end-10)], updating bufferCount. 
int discardBadBytes(int n)
{
  if ((n == 0) || (bufferCount == 0) || (espBuffer[0] == '!') || (espBuffer[0] == '$')) {
    return 0;
  }
  int startIndex = 0; // where to start the data-copy
  while ((n > 0) && (startIndex < bufferCount) && (espBuffer[startIndex] != '!') && (espBuffer[startIndex] != '$')) {
    startIndex++;
    n--;
  }
  flashAlarm("discardBadBytes: discarding data"); 
  message("Alarm in discardBadBytes, startIndex = ");
  iMessage(startIndex); 
  message("\n");
  message((char *) espBuffer);
  shiftBuffer(startIndex); // move everything from startIndex to end back to [0..(end-startIndex)]
  return startIndex; 
}

// Take the contents of espBuffer between startIndex and the end of the buffer (bufferCount-1)
// and move them to lie between 0 and bufferCount-1-startIndex
void shiftBuffer(int startIndex){
  for (int i = startIndex; i < bufferCount+1; i++) { // +1 to capture the trailing \0
    espBuffer[i-startIndex] = espBuffer[i];
  }
  bufferCount = bufferCount - startIndex; 
}

// Testing=========================================
/*
// Fill the buffer with some known data
void setBuffer(char *s) {
  strcpy((char *)espBuffer, s);
  bufferCount = strlen(s); 
}

// show both the buffer and the bufferCount
void showBuffer() {
   message((char *)espBuffer); 
   message("");
   iMessage(bufferCount); 
   message("\n");
   delay(10); 
}
*/

// === WiFi setup ==============================================================
bool setupWiFi()
{
    bool flag = false; 
    
    //fetch ssid and pass from eeprom and tries to connect
    //if it does not connect it starts an access point named "NMEA Bridge AP"
    //and go into a blocking loop awaiting configuration
    Serial.println("Trying to get going...");
    flag = wifiManager.autoConnect("NMEA Bridge AP");
    
    if (flag) {    //if you get here you have connected to the WiFi
      Serial.print("Connected, with IP address: ");
      Serial.println(WiFi.localIP());
    }
    else{
      Serial.print("Could not connect, alas.");
      flashAlarm("Could not make wifi connection.");
    }
    Serial.flush();
    return flag;
}

//================ setup up for over-the-air programming. 
void otaSetup()
{
  ArduinoOTA.setPassword("bridge");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
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
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
}

// === UDP Handling (including informative messages) ============================
// Send out a single UDP packet on the connection that
// we've established
void sendUDP(const byte packet[],int packetSize)
{
  Udp.beginPacket(ipBroadcast, NMEA_PORT);
  Udp.write(packet, packetSize);
  Udp.endPacket(); 
  beep(1,0); // flash the green LED very briefly
}

// Send a debugging message as a UDP packet, with # prefix. Message must be null-terminated, no
// more than 198 characters. 
void Umessage(char* msg)
{
  byte sentence[200];
  int count = 1+strlen(msg); 

  sentence[0] = '#';
  byte* t = sentence+1; 
  for (char* s = msg; *s != 0; s++) {
    *t++ = *s; 
  }
  
  sendUDP(sentence, count);
}

// send a debugging message to Serial
void message(char* msg)
{
  Serial.print(msg);
  // Umessage(msg);  // change to this for debugging over UDP. 
}

// Send a debugging message -- a single int -- as a UDP packet
void iMessage(int n)
{
  Serial.print(n);
/*  byte buf[100]; // change to this for debugging over UDP
  memset(buf, 0, sizeof(buf));
  buf[0] = '#';
  itoa( (uint8_t)n , (char*) (buf + 1), 10 ); 
  buf[strlen((char *)buf)] = '\n';
  sendUDP(buf, strlen((char *) buf)); // cautious!
*/}

// === alarm-light handling procedures =======================================
// Turn on the red LED alarm light for 1 second (or more, if loop takes more 
// than one second to run (unlikely!). 
void flashAlarm(char *s)
{
  Serial.print("ALARM: "); 
  Serial.println(s);
  alarmLightOff(); // to make a blink if there's already an alarm registered!
  alarmOn = true; 
  alarmLightOn();
  alarmStart = millis();
}

// Check to see if the alarm LED is on, and if so, whether it should be 
// turned off because it's been on for at least a second. Must appear within main
// loop() or the alarm light will stay on forever once it's turned on
void alarmCheck()
{
  if (alarmOn) {
    unsigned int t = millis(); 
    if ((t < alarmStart)     // handles the case of wraparound
    || (t > (alarmStart + 1000))) // checks whether it's been a full second
    {
      alarmOn = false; 
      alarmStart = 0;
      alarmLightOff();
    }
  }
}

// === LED control procedures ==================================================
// Blink the green status light on for ms milliseconds, and then off, followed by doing nothing for delay_ms milliseconds
void beep(int ms, int delay_ms)
{
  statusLightOn();
  delay(ms);
  statusLightOff();
  delay(delay_ms);
}

// Turn off blue LED on the board (or external green LED, for the ESP-01)
// @@ NB: It's possible that on your board, the light is turned OFF by pulling this pin HIGH, and vice versa. 
// You'll have to test it to be sure. After startup, the blue LED (if your board has one at all!) should be off almost all the time. 
// NB: On the ESP-01, the blue LED is tied to Serial input. If you set it to be a digital output, then you can no longer
// read serial input (yow!). Hence we use an external LED (on pin 02, I think) to send basic signals to the user. 
void statusLightOff()
{
   digitalWrite(STATUS_LED_PIN, LOW);  
}

// Turn on blue LED on the board (or external LED, for the ESP-01)
void statusLightOn()
{
   digitalWrite(STATUS_LED_PIN, HIGH);  
}

// Turn off red LED alarm light
void alarmLightOff()
{
   digitalWrite(ALARM_LED_PIN, LOW);  
}

// Turn on red LED alarm light
void alarmLightOn()
{
   digitalWrite(ALARM_LED_PIN, HIGH);  
}

// Extensive testing of individual procs, commente out to save space in uploads. 
/*
void run_tests()
{  //setBuffer("abcasbd\n");
  //showBuffer();
  //int s = discardBadBytes(15);
  //showBuffer();
  //iMessage(s);
  //message("\n");

  int pos = 0; 
  
  find_status f; 
  setBuffer("$bca$bd\n");
  f = find(pos);
  show_status(f); 
  iMessage(pos); // Expect FIND_START, 4

  setBuffer("$bcadbd\n$abd");
  f = find(pos);
  show_status(f); 
  iMessage(pos); // Expect FIND_END, 7
  
  setBuffer("!bcadbdabd");
  f = find(pos);
  show_status(f); 
  iMessage(pos); // Expect FIND_PARTIAL, <anything>
  
  setBuffer("qbcadbdabd");
  f = find(pos);
  show_status(f); 
  iMessage(pos); // Expect FIND_JUNK, <anything>

  setBuffer("!1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890\n");
  f = find(pos);
  show_status(f); 
  iMessage(pos); // Expect FIND_OVERRUN, 128
  
  // Now test "consume". Cases:
  // empty buffer (expect 0, unchanged espBuf)
  setBuffer("");
  iMessage(consume());
  showBuffer();
  delay(100); 
  // Sxxx (expect false, unchanged buf)
  setBuffer("!xxx");
  iMessage(consume());
  showBuffer();
  delay(100); 
  // SxSxx (expect true, Sxx)
  setBuffer("$a$ab");
  iMessage(consume());
  showBuffer();
  delay(100); 
  // SSxxx (expect true, Sxxx)
  setBuffer("$$def");
  iMessage(consume());
  showBuffer();
  delay(100); 
  // SxxxES (expect true, S <and a UDP message sent>)
  setBuffer("$ghk\n$");
  iMessage(consume());
  showBuffer(); 
  delay(100); 
  // S xx<129 xs> ExxS (expect true, xxExxS)
  setBuffer("$123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789\nab$");
  iMessage(consume());
  showBuffer(); 
  delay(100); 
  // xxSxxESxxxE (expect true, SxxESxxxE)
  setBuffer("xx$pq\n$pqr\n");
  iMessage(consume());
  showBuffer(); 
  delay(100); 
  
}


void show_status(find_status f) {
  switch(f) {
    case FIND_START: 
      message("START ");
      break;
    case FIND_END: 
      message("END ");
      break;
    case FIND_OVERRUN: 
      message("OVERRUN ");
      break;
    case FIND_PARTIAL: 
      message("PARTIAL ");
      break;
    case FIND_JUNK: 
      message("ERROR ");
      break;
  }
}
*/
