//Ejemplo adaptado de https://www.teachmemicro.com/arduino-rfid-rc522-tutorial/
// pin NodeMCU  en https://github.com/miguelbalboa/rfid o  https://www.instructables.com/id/MFRC522-RFID-Reader-Interfaced-With-NodeMCU/

#include <SPI.h>
#include <MFRC522.h>

// includes comunicación libs
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h> // check if you are using versión 6 
#include <Keypad.h>


//***MODIFICAR PARA SU PROYECTO ***
//  configuración datos wifi 
// descomentar el define y poner los valores de su red y de su dispositivo
#define WIFI_AP "Casona PB"
#define WIFI_PASSWORD "12345"


//  configuración datos thingsboard
#define NODE_NAME "Papelera"   //nombre que le pusieron al dispositivo cuando lo crearon
#define NODE_TOKEN "PwDOMj5KDyfMNIM7bB28"   //Token que genera Thingboard para dispositivo cuando lo crearon


//***NO MODIFICAR ***
char thingsboardServer[] = "demo.thingsboard.io";

/*definir topicos.
 * telemetry - use to send telemetry
 * request -  use to receive and send requests to server
 * attributes - to receive commands from server to device
 */
char telemetryTopic[] = "v1/devices/me/telemetry";
char requestTopic[] = "v1/devices/me/rpc/request/+";  //RPC - El Servidor usa este topico para enviar rquests, cliente response
char responseTopic[] = "v1/devices/me/rpc/response/+"; // RPC responce
char attributesTopic[] = "v1/devices/me/attributes";  //El Servidor usa este topico para enviar atributos


// declarar cliente Wifi y PubSus
WiFiClient wifiClient;
PubSubClient client(wifiClient);



// Init array that will store new UID and String to store UID
String uidString; // 4 x 2 chars for the 4 bytes + trailing '\0'
int keyCount = 0;
char keysString[6];

//-------------------------------------------------------------
// Avoidance sensor
//-------------------------------------------------------------
int avoidancePin = D1;


//-------------------------------------------------------------
//Led colors
const int RED  = 0;
const int BLUE  = 1;
const int GREEN  = 2;

//-------------------------------------------------------------
const byte ROWS = 4; //four rows
const byte COLS = 3; //three columns
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};

byte rowPins[ROWS] = {D2, D3, D4, D5}; //connect to the row pinouts of the keypad
byte colPins[COLS] = {D6,D7,D8}; //connect to the column pinouts of the keypad

Keypad keypad = Keypad( makeKeymap(keys), rowPins, colPins, ROWS, COLS );

//-------------------------------------------------------------
// app logic state of userRead, userAuthenticated and bottleDetected
bool userAuthenticated = false;
bool userCredited = false;
int totalBottles = 0;
int userTotalBottles = 0;
enum cycleStatates {READ_FREE_BTLS, AUTH, WAIT_AUTH, READ_USER_BTLS, CREDITUSER, WAIT_CREDITUSER, INIT};
cycleStatates state = INIT;

//-------------------------------------------------------------
// THB request timer variables
//-------------------------------------------------------------
unsigned long lastSend;
int elapsedTime = 9000; // elapsed time request vs reply
int requestNumber =1;  
bool serverRequestInProgress = false;

// App Logic
void setup() { 
  Serial.begin(9600);

  // init wifi & pubsub callbacks
  Serial.println("init connection"),
  
  connectToWiFi();
  delay(10);
  
  client.setServer(thingsboardServer, 1883);
  client.setCallback(on_message);
   
  lastSend = 0; // variable to ctrl delays
      
  pinMode(avoidancePin, INPUT);

  Serial.print("STATE:");
  Serial.println(state);
  
}
 
void loop() 
{  
    if ( !client.connected() ) {
      reconnect();
      state = INIT;
      stopServerRequestTimer();
      }

    //Counting FREE Bottles, user not identified
    if (state == READ_FREE_BTLS) {
      
      if (checkBottleAndSend()) {
         requestToLedDevice(RED, "FLASH");
         state = READ_FREE_BTLS;
      }

      if (readUserCode(true) == 0) {
        Serial.println("Card detected");
        state = AUTH;
      }      
    }

    //Card read - check user and wait for server
    if (state == AUTH ) {
          // call server for authentication
          requestToLedDevice(BLUE, "ON");
          requestUserAuthentication(uidString);
          state = WAIT_AUTH;
      }

    //Waiting for server auth
    if (state == WAIT_AUTH && isServerRequestTimerInProgress() == false) {
          // note: async userAuthenticated variable is set by the server response on methods called by on_message()
          if(userAuthenticated) {
            Serial.println("user ok");
            requestToLedDevice(BLUE, "FLASH");
            state = READ_USER_BTLS;
          }  else {                
            Serial.println("user Not Auth");
            requestToLedDevice(GREEN, "FLASH");
            state = INIT;
            }
            
      }
      
    // User identified -> keep reading bottles until user passes card again
    if (state == READ_USER_BTLS) {
            // call server to set led

            if (checkBottleAndSend()) {
              Serial.println("bottle ok");
              
              userTotalBottles++;
              
              // flash to signal bottle ok
              requestToLedDevice(GREEN, "FLASH");
            }

            if (readUserCode(false) ==0) {
              Serial.println("Card detected");
              requestToLedDevice(BLUE, "ON");
              state = CREDITUSER;
            }      
   
    }

    // Credit user on server
    if (state == CREDITUSER && isServerRequestTimerInProgress() == false) {
              requestCreditToUser(uidString);
              state = WAIT_CREDITUSER;
              requestToLedDevice(BLUE, "FLASH");
    }

    // wait for server 
    if (state == WAIT_CREDITUSER && isServerRequestTimerInProgress() == false) {
              // note: async userCredited variable is set by the server response on methods called by on_message()
              if (userCredited) {
                Serial.println("user credited");
                 state = INIT;
              } else {
                Serial.println("user NOT credited");
                 state = INIT;
              }
    }

    if (state == INIT) {
       state = READ_FREE_BTLS;

       userTotalBottles = 0;
       userCredited = false;
       userAuthenticated = false;
       serverRequestInProgress = false;
       keyCount = 0;

       requestToLedDevice(GREEN, "FLASH");

       Serial.println("-------- INIT ------------");
    }
 
  // check for server timeout
  checkRequestInProgressTimeout();
 
  client.loop();

}

// server RPC Request only
void  requestToLedDevice(int ledColor, String action)
{      
      const int capacity = JSON_OBJECT_SIZE(10);
      StaticJsonDocument<capacity> doc;
      doc["method"] = "Led";
      JsonObject obj = doc.createNestedObject("params");
      obj["Action"] = action;
      obj["Color"] = ledColor;

       
      String output = "";
      serializeJson(doc, output);
      
      Serial.print("json to send:");
      Serial.println(output);
  
     char attributes[100];
     output.toCharArray( attributes, 100 );
     
     requestNumber++;
      
      String requestTopic = String("v1/devices/me/rpc/request/") + String(requestNumber);
  
      Serial.print("TOPIC:");
      Serial.print(requestTopic);
      Serial.print("  --- >json to send:");
      Serial.println(output);
      
     if (client.publish(requestTopic.c_str() , attributes ) == true) {
        Serial.println("publish resquest ok");
      } else {
         Serial.println("publish request ERROR");
         stopServerRequestTimer();
      }    
}

// server RPC Request only & Reply (see on_message)
void requestUserAuthentication(String uidString)
{    
  if (isServerRequestTimerInProgress() == false) {
    startServerRequestTimer();
    
    const int capacity = JSON_OBJECT_SIZE(3);
    StaticJsonDocument<capacity> doc;
    doc["method"] = "checkUserID";
    doc["params"] = uidString;
    
    String output = "";
    serializeJson(doc, output);
    
    char attributes[100];
    output.toCharArray( attributes, 100 );
    
    requestNumber++;
    
    String requestTopic = String("v1/devices/me/rpc/request/") + String(requestNumber);

    Serial.print("TOPIC:");
    Serial.print(requestTopic);
    Serial.print("  --- >json to send:");
    Serial.println(output);
    
   if (client.publish(requestTopic.c_str() , attributes ) == true) {
      Serial.println("publish resquest ok");
    } else {
       Serial.println("publish request ERROR");
       stopServerRequestTimer();
    }     
  }
}

// server RPC Request only & Reply (see on_message)
void requestCreditToUser(String uidString)
{
   if (isServerRequestTimerInProgress() == false) {
    startServerRequestTimer(); 
    
    const int capacity = JSON_OBJECT_SIZE(10);
    StaticJsonDocument<capacity> doc;
    doc["method"] = "creditPointsToUser";
    JsonObject obj = doc.createNestedObject("params");
    obj["user"] = uidString;
    obj["bottles"] = userTotalBottles;

    String output = "";
    serializeJson(doc, output);
    
    Serial.print("json to send:");
    Serial.println(output);

    char attributes[100];
    output.toCharArray( attributes, 100 );
    requestNumber++;
    
    String requestTopic = String("v1/devices/me/rpc/request/") + String(requestNumber);

    Serial.print("TOPIC:");
    Serial.print(requestTopic);
    Serial.print("  --- >json to send:");
    Serial.println(output);
    
   if (client.publish(requestTopic.c_str() , attributes ) == true) {
      Serial.println("publish resquest ok");
    } else {       
      Serial.println("publish request ERROR");
      stopServerRequestTimer();
    }     
   }
}



/*
 * Thingsboard methods
 */
void on_message(const char* topic, byte* payload, unsigned int length) 
{
    // print received message
  Serial.println("On message");
  char message[length + 1];
  strncpy ( message, (char*)payload, length);
  message[length] = '\0';
  
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Message: ");
  Serial.println( message);
  
  String topicStr = topic;
  String topicHead = topicStr.substring(0,strlen("v1/devices/me/rpc/response"));
  Serial.println(topicHead);
  
  // Check topic for attributes o request
  if (strcmp(topic, "v1/devices/me/attributes") ==0) { //es un cambio en atributos compartidos
    Serial.println("----> CAMBIO DE ATRIBUTOS");
    //processAttributeRequestCommand(message);
  } else if (strcmp(topicHead.c_str(), "v1/devices/me/rpc/response") == 0) {
    // request
    Serial.println("----> PROCESS REQUEST FROM SERVER");
    processRequest(message);
  }

  //message received stop timer
  stopServerRequestTimer();
}


// Process request
void processRequest(char *message)
{
  // Decode JSON request with ArduinoJson 6 https://arduinojson.org/v6/doc/deserialization/
  // Notar que a modo de ejemplo este mensaje se arma utilizando la librería ArduinoJson en lugar de desarmar el string a "mano"

  
  const int capacity = JSON_OBJECT_SIZE(4);
  StaticJsonDocument<capacity> doc;
  DeserializationError err = deserializeJson(doc, message);
  
  if (err) {
    Serial.print(("deserializeJson() failed with code "));
    Serial.println(err.c_str());
    
    return;
  }
    
  String methodName = doc["method"];
  if (methodName.equals("UserApr")) {
    bool authResponse = doc["params"];
    userAuthenticated = authResponse;

  } else if (methodName.equals("creditPointsToUser")) {
    bool creditResponse = doc["params"];
    userCredited = true;
  } else {
        userAuthenticated = true;
        userCredited = true;
  }
}

// start requestInProgress timer
void startServerRequestTimer()
{
    serverRequestInProgress = true;
    lastSend = millis();   //set timer
}

// stop requestInProgress timer
void stopServerRequestTimer()
{
    serverRequestInProgress = false;
}

// check requestInProgress timer status
bool isServerRequestTimerInProgress()
{
  return (serverRequestInProgress);
}

// check requestInProgress for timeout
void checkRequestInProgressTimeout()
{
  if ((millis() - lastSend > elapsedTime) && isServerRequestTimerInProgress() == true) { // Update and send only after 1 seconds
    Serial.println("-> Server RPC timeout");
    stopServerRequestTimer();
  }

}



/* 
 *  Solution Sensor management functions
 *
 */


bool checkBottleAndSend()
{
    bool returnValue = false;
    
    if (detectBottle()) {
      returnValue = true;
      totalBottles++;
      
      const int capacity = JSON_OBJECT_SIZE(3);
      StaticJsonDocument<capacity> doc;
      doc["bottle"] = 1;
      
      String output = "";
      serializeJson(doc, output);
      
      Serial.print("json to send telemetry:");
      Serial.println(output);
  
      char attributes[100];
      output.toCharArray( attributes, 100 );
      
      if (client.publish(telemetryTopic, attributes ) == true) {
          Serial.println("publish ok telemetry");
        } else {
           Serial.println("publish ERROR");
        }     
    }

    return returnValue;
}

//Check Avoidance sensor for bottle
bool detectBottle()
{
    return (digitalRead(avoidancePin) == LOW ? true: false);
}




int readUserCode(bool readId) 
{
  char key = keypad.getKey();
    // Look for new code
  if(readId == true){  
      if(key != NULL ){
        keysString[keyCount] = key;
        keyCount++;
     } 
  if (keyCount == 4){
    // if code is complete 4 digits
    Serial.println("AUTHORIZED");
    keysString[4] = '\0';
    uidString = String(keysString);
    keyCount = 0;
    Serial.println(uidString);
    return 0;
  } 
  }
   //code has already been authentificated: waiting for gameover
  else{
    if(key == '*')
        {
        keyCount = 0;
        return 0;
        }
    }
   return -1;
}




//***NO MODIFICAR - Conexion con Wifi y ThingsBoard ***
/*
 * THB connection and topics subscription
 */
void reconnect() {
  int statusWifi = WL_IDLE_STATUS;
  
  // Loop until we're reconnected
  while (!client.connected()) {
    statusWifi = WiFi.status();
    connectToWiFi();
    
    Serial.print("Connecting to ThingsBoard node ...");
    // Attempt to connect (clientId, username, password)
    if ( client.connect(NODE_NAME, NODE_TOKEN, NULL) ) {
      Serial.println( "[DONE]" );
      
      // Suscribir al Topico de request
      client.subscribe(requestTopic); 
      client.subscribe(attributesTopic); 
      client.subscribe(responseTopic);     
    } else {
      Serial.print( "[FAILED] [ rc = " );
      Serial.print( client.state() );
      Serial.println( " : retrying in 5 seconds]" );
      // Wait 5 seconds before retrying
      delay( 5000 );
    }
  }
}

 
/*
 * 
 *  wifi connection
 */
 
void connectToWiFi()
{
  Serial.println("Connecting to WiFi ...");
  // attempt to connect to WiFi network

  WiFi.begin(WIFI_AP, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to WiFi");
}
