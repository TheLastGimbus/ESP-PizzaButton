#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include "FS.h"

// this is pretty much abandoned, but i'll keep it in case of emergency
String softwareVersion = "1.0";


#define PIN_MAIN 13
#define PIN_LED_GREEN 14
#define PIN_LED_RED 12
#define PIN_STANDBY 5
#define PIN_FACTORY_RESET 4

ADC_MODE(ADC_VCC);

bool setupMode = 0;

String wifiName = "";
String wifiPass = "";

bool tryingToConnect = 0;
unsigned long timerConnectingToWifi = 0;
bool wifiFailedFlag = 0;

bool weNeedToSend = 0;
String messageToSend = "";
unsigned long sendingLastTry = 0;
unsigned long sendingBegin = 0;

unsigned long timerNotDoingAnything = 0;

unsigned long timersButton[16]; // timers


#define LOG_LEVEL 1 // all tags below this will not be printed

#define TAG_DATA 1
#define TAG_EVENT 2
#define TAG_ERROR 3
#define TAG_IMPORTANT 4
void Log(int tag, String message){
	if(tag < LOG_LEVEL){
		return;
	}
	String head = "";
	switch (tag) {
		case 1: head = "DATA"; break;
		case 2: head = "EVENT"; break;
		case 3: head = "ERROR"; break;
		case 4: head = "IMPORTANT"; break;
		default: head = "UKNOWN"; break;
	}
	Serial.println(head + ": " + message);
}

void goToSleep(){
	Log(TAG_EVENT, "Going to sleep...");
	delay(10);
	digitalWrite(PIN_STANDBY, 0);
	delay(15000);
}

void setLed(bool green, bool red, bool blue = false){
	digitalWrite(PIN_LED_GREEN, green);
	if(red){
		analogWrite(PIN_LED_RED, 180);
	}
	else{
		analogWrite(PIN_LED_RED, 0);
	}

	digitalWrite(2, !blue);
}

void resetConfigFile(String ssid = "", String pass = ""){
	if(SPIFFS.begin()){
		SPIFFS.remove("config.json");
		File config = SPIFFS.open("config.json", "w");
		config.println("{");
		config.println("    \"ssid\": \""+ssid+"\",");
		config.println("    \"password\": \""+pass+"\"");
		config.println("}");
		config.flush();
		config.close();
		Log(TAG_EVENT, "New config file created");
	}
	else{
		Log(TAG_ERROR, "SPIFFS begin fail");
	}
	SPIFFS.end();
}

void loadDataFromFS(){
	Log(TAG_EVENT, "Loading DATA from FS...");
	if(SPIFFS.begin()){
		if(SPIFFS.exists("config.json")){
			File file = SPIFFS.open("config.json", "r");
			if(file && file.size() > 0){
				StaticJsonBuffer<600> jsonBuffer;
				JsonObject& json = jsonBuffer.parseObject(file.readString().c_str());
				if(json.success()){
					const char* ssid = json["ssid"];
					const char* password = json["password"];
					Log(TAG_DATA, "SSID from JSON: " + String(ssid));
					Log(TAG_DATA, "Password from JSON: " + String(password));
					wifiName = ssid;
					wifiPass = password;
					Log(TAG_EVENT, "Loaded DATA from JSON");
				}
				else{
					Log(TAG_ERROR, "JSON parsing fail! Creating new one...");
					file.close();
					resetConfigFile();
				}
			}
			else{
				Log(TAG_ERROR, "Filed opening file, creating new one...");
				file.close();
				resetConfigFile();
			}
			file.close();
		}
		else{
			Log(TAG_ERROR, "config file doesn't exist. Creating new one...");
			resetConfigFile();
		}
	}
	else{
		Log(TAG_ERROR, "SPIFFS begin fail");
	}
	SPIFFS.end();
}

float getVcc(){
	float vccVolt = ((float)ESP.getVcc()) / 1024;
	if(vccVolt > (float)4.2){
		vccVolt = (float)4.2;
	}
	if(vccVolt < (float)3){
		vccVolt = 3;
	}
	Log(TAG_DATA, "Vcc: " + String((float)vccVolt));
	return (float)vccVolt;
}

void saveToSendState(bool main, bool left, bool right){
	if(setupMode){
		setLed(1, 1, 1);
	}
	else{
		setLed(1, 1);
	}
	StaticJsonBuffer<300> buff;
	JsonObject& json = buff.createObject();
	json["main"] = main;
	json["left"] = left; // TODO
	json["right"] = right; // TODO
	json["voltage"] = getVcc();
	json["button-software-version-device"] = softwareVersion;
	json.printTo(messageToSend);
	weNeedToSend = 1;
	wifiFailedFlag = 0;
	Log(TAG_EVENT, "Saved data to send and checked weNeedToSend flag to true");
}

void factoryReset(){
	Log(TAG_IMPORTANT, "Performing factory reset...");
	resetConfigFile("", "");
	goToSleep();
}

void factoryResetInterrupt(){
	Log(TAG_EVENT, "Factory reset button pressed!");
	unsigned long x = millis();
	unsigned long ledTimer = millis();
	bool led = 0;
	while(!digitalRead(PIN_FACTORY_RESET)){
		if((millis() - x) > 5000){
			factoryReset();
		}
		if((millis() - ledTimer) > 100){
			ledTimer = millis();
			setLed(led, !led);
		 	led = !led;
		}
	}
	WiFi.disconnect();
	tryingToConnect = 0;
	weNeedToSend = 0;
	setLed(0, 0);
}

void setup() {
	pinMode(PIN_STANDBY, OUTPUT);
	digitalWrite(PIN_STANDBY, 1);

	pinMode(PIN_MAIN, INPUT_PULLUP);
	pinMode(PIN_LED_GREEN, OUTPUT);
	pinMode(PIN_LED_RED, OUTPUT);
	Serial.begin(115200);
	if(digitalRead(PIN_MAIN)){
		saveToSendState(true, false, false); // TODO
	}

	pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);
	pinMode(2, OUTPUT);
    digitalWrite(2, 1);
	Log(TAG_DATA, "Last reset: " + ESP.getResetReason());
	loadDataFromFS();
	WiFi.disconnect();
	WiFi.softAPdisconnect();

	Log(TAG_DATA, "Freq: " + String(ESP.getCpuFreqMHz()));

	// config mode setup
	if(wifiName.length() <= 0) {
		Log(TAG_IMPORTANT, "We are in setup mode");
		setupMode = 1;
		WiFi.mode(WIFI_AP_STA);
		delay(100);

		WiFi.softAP("PIZZA BUTTON WIFI");

		ArduinoOTA.setHostname("pizza-sms-button");
        ArduinoOTA.begin();

		saveToSendState(false, false, false);
	}
	else{
		Log(TAG_IMPORTANT, "We are in normal mode");
	}

	attachInterrupt(digitalPinToInterrupt(PIN_FACTORY_RESET), factoryResetInterrupt, FALLING);

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
        Serial.printf("ERROR[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
}

bool asyncButtonCheck(byte pin, bool wantedState = 0, unsigned long ignoreTime = 1000){
    if(
		digitalRead(pin) == wantedState &&
		(millis() - timersButton[pin]) > ignoreTime
	) {
        timersButton[pin] = millis();
        return 1;
    }
    else{
        return 0;
    }
}

void parseReceivedData(String data){
	if(data.length() > 0){
		Log(TAG_DATA, "Received data: " + data);
		StaticJsonBuffer<300> buff;
		JsonObject& json = buff.parseObject(data.c_str());
		String newSsid = json["ssid"];
		String newPass = json["password"];
		if(newSsid != wifiName || newPass != wifiPass){
			Log(TAG_IMPORTANT, "Wifi data received is not the same! Changing config file...");
			resetConfigFile(newSsid, newPass);
		}
	}
}

void wifiFailed(){
	Log(TAG_ERROR, "Connecting to wifi failed!");
	WiFi.disconnect();
	wifiFailedFlag = 1;
	tryingToConnect = 0;
	weNeedToSend = 0;
	setLed(0, 1, 10);
}

void sendingFailed(){
	Log(TAG_ERROR, "Sending request failed!");
	WiFi.disconnect();
	tryingToConnect = 0;
	weNeedToSend = 0;
	setLed(0, 1);
	if(setupMode){
		setLed(1, 0);
		delay(5000);
		goToSleep();
	}
}

void sendingSucces(String received){
	messageToSend = "";
	weNeedToSend = 0;
	sendingBegin = 0;
	Log(TAG_EVENT, "Message send succes!");
	if(setupMode){
		parseReceivedData(received);
	}
	// you can't order pizza for next 30 seconds, for safety ;)
	setLed(1, 0);
	delay(5000);
	if(setupMode){ goToSleep(); } // unless you were just setting it up
	delay(55000);
	goToSleep();
}

void asyncWifiConnectHandle(){
    if(!tryingToConnect && !WiFi.isConnected()){
		Log(TAG_DATA, "WiFi begin, SSID: " + wifiName + ", PASS: " + wifiPass);

		WiFi.disconnect();
		WiFi.softAPdisconnect();
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifiName.c_str(), wifiPass.c_str());

		timerConnectingToWifi = millis();
        tryingToConnect = 1;
    }
	if(tryingToConnect && !WiFi.isConnected()){
		if((millis() - timerConnectingToWifi) > 60 * 1000){
			wifiFailed();
		}
	}
    if(WiFi.isConnected() && tryingToConnect){
		Log(TAG_EVENT, "Connected to wifi '" + wifiName + "' !");
		Log(TAG_DATA, "IP: " + WiFi.localIP().toString());
		ArduinoOTA.setHostname("pizza-sms-button");
        ArduinoOTA.begin();

		timerConnectingToWifi = millis();
        tryingToConnect = 0;
    }
}

void asyncDataSending(){
	if((weNeedToSend && WiFi.isConnected()) || setupMode){
		if((millis() - sendingLastTry) > 2000){
			Log(TAG_DATA, "Sending time: " + String(millis() - sendingBegin));
			Log(TAG_EVENT, "Trying to find mDNS...");
			int a = MDNS.queryService("pizza-app", "tcp");
			Log(TAG_DATA, String(a) + " host(s) found");
			for(int x = 0; x < a; x++){
				Log(TAG_DATA, "Host number " + String(x) + ": \n" +
							MDNS.IP(x).toString() + " \n" +
							String(MDNS.port(x)) + " \n" +
							MDNS.hostname(x));
				HTTPClient http;
				http.begin(MDNS.IP(x).toString(), MDNS.port(x));
				int code = http.POST(messageToSend);
				Log(TAG_DATA, "Http code: " + String(code));
				String received = http.getString();
				// succes
				if(code >= 200 && code < 300){
					sendingSucces(received);
					return;
				}
			}
			sendingLastTry = millis();
		}

		if((millis() - sendingBegin) > 120 * 1000){
			sendingFailed();
		}
	}
	else{
		sendingBegin = millis();
	}
}

void loop() {
	// setting up the button's settings
	if(setupMode) {
		asyncDataSending();
	}

	// normal workflow
	else{
		if(!wifiFailedFlag){
			asyncWifiConnectHandle();
		}
		asyncDataSending();
		if(asyncButtonCheck(PIN_MAIN, 1)){
			saveToSendState(true, false, false); // TODO
		}

		if(weNeedToSend == false && tryingToConnect == false){
			if((millis() - timerNotDoingAnything) > 180 * 1000){
				Log(TAG_EVENT, "Not pretty much doing anything so...");
				goToSleep();
			}
		}
		else{
			timerNotDoingAnything = millis();
		}
	}
	ArduinoOTA.handle();

	// safety first :)
	if(millis() > 10 * 60 * 1000){
		Log(TAG_IMPORTANT, "SAFETY SLEEP! THIS SHOULD NEVER HAPPEN!");
		goToSleep();
	}
}
