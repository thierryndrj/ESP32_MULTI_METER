#include <Arduino.h>
#include <Wire.h>  
#include <FS.h>
#include <LittleFS.h>
#include "config.h"
#include "nv_data.h"
#include "wifi_cfg.h"
#include "ina226.h"

const char* FwRevision = "0.90";
static const char* TAG = "main";

volatile bool DataReadyFlag = false;
volatile bool GateOpenFlag = false;
volatile bool SocketConnectedFlag = false;
volatile bool CaptureFlag = false;
volatile bool TransmitOKFlag = false;
uint32_t ClientID;

#define WIFI_TASK_PRIORITY 		1
#define CAPTURE_TASK_PRIORITY 	(configMAX_PRIORITIES-1)

volatile MEASURE_t Measure;
volatile int16_t* Buffer = NULL; 
int MaxSamples;

#define ST_IDLE 		1
#define ST_TX			2
#define ST_TX_COMPLETE	3

static void wifi_task(void* pvParameter);
static void capture_task(void* pvParameter);

// create the desired tasks, and then delete arduino created loopTask that calls setup() and loop(). 
// Core 0 : wifi task with web server and websocket communication, and low level esp-idf wifi code
// Core 1 : capture task

void setup() {
	pinMode(pinAlert, INPUT); // external pullup, active low
	pinMode(pinGate, INPUT); // external pullup, active low
	pinMode(pinBtn1, INPUT_PULLUP);
	pinMode(pinBtn2, INPUT_PULLUP);
	pinMode(pinFET1, OUTPUT); // external pulldown
	pinMode(pinFET05, OUTPUT); // external pulldown

	Serial.begin(115200);
	ESP_LOGI(TAG,"ESP32_INA226 v%s compiled on %s at %s\n\n", FwRevision, __DATE__, __TIME__);
    ESP_LOGI(TAG, "Max task priority = %d", configMAX_PRIORITIES-1);
    ESP_LOGI(TAG, "arduino loopTask : setup() running on core %d with priority %d", xPortGetCoreID(), uxTaskPriorityGet(NULL));    

	nv_options_load(Options);

	// web server and web socket connection handler on core 0 along with low level wifi actions (ESP-IDF code)
    xTaskCreatePinnedToCore(&wifi_task, "wifi_task", 4096, NULL, WIFI_TASK_PRIORITY, NULL, CORE_0);
    // capture task on core 1, don't want i2c capture to be pre-empted as far as possible to maintain sampling rate. 
	xTaskCreatePinnedToCore(&capture_task, "capture_task", 4096, NULL, CAPTURE_TASK_PRIORITY, NULL, CORE_1);

	// destroy loopTask which called setup() from arduino:app_main()
    vTaskDelete(NULL);
    }


// never called as loopTask is deleted, but needs to be defined
void loop(){
	}


static void wifi_task(void* pVParameter) {
    ESP_LOGD(TAG, "wifi_task running on core %d with priority %d", xPortGetCoreID(), uxTaskPriorityGet(NULL));
	ESP_LOGI(TAG,"Starting web server");
    // do NOT format, partition is built and flashed using 
	// 1. PlatformIO Build FileSystem Image
	// 2. Upload FileSystem Image    
    if (!LittleFS.begin(false)) { 
		ESP_LOGE(TAG, "Cannot mount LittleFS, Rebooting");
		delay(1000);
		ESP.restart();
		}    
	// initialize web server and web socket interface	
	wifi_init();
	int state = ST_IDLE;
	int bufferOffset = 0;
	int txSamples = 0;
	int SamplesRemaining = 0;
	volatile int16_t* pb;
	int numBytes;
	int16_t msg;

	while (1) {
		vTaskDelay(1);
		ws.cleanupClients();
		if (SocketConnectedFlag == true) { 
			switch (state) {
				case ST_IDLE :
				default :
					if (GateOpenFlag){
						GateOpenFlag = false;
						ESP_LOGD(TAG,"Socket msg : Tx Gate Open");
						msg = MSG_GATE_OPEN;
						ws.binary(ClientID, (uint8_t*)&msg, 2); 
						}	
					else 
					if (DataReadyFlag == true) {
						DataReadyFlag = false;
						ESP_LOGD(TAG,"Socket msg : Tx Start");
						if (Measure.nSamples > MAX_TRANSMIT_SAMPLES) {
							numBytes = 6 + MAX_TRANSMIT_SAMPLES*4;
							ws.binary(ClientID, (uint8_t*)Buffer, numBytes); 
							bufferOffset += numBytes/2;
							txSamples += MAX_TRANSMIT_SAMPLES;
							state = ST_TX;
							}
						else {
							numBytes = 6 + Measure.nSamples*4;
							ws.binary(ClientID, (uint8_t*)Buffer, numBytes); 
							state = ST_TX_COMPLETE;
							}
						}
				break;

				case ST_TX :
					if (TransmitOKFlag == true) {
						TransmitOKFlag = false;
						ESP_LOGD(TAG,"Socket msg : Tx ...");
						SamplesRemaining = Measure.nSamples - txSamples;
						pb = Buffer + bufferOffset; 
						if (SamplesRemaining > MAX_TRANSMIT_SAMPLES) {
							numBytes = 2 + MAX_TRANSMIT_SAMPLES*4;
							ws.binary(ClientID, (uint8_t*)pb, numBytes); 
							bufferOffset += numBytes/2;
							txSamples += MAX_TRANSMIT_SAMPLES;
							state = ST_TX;
							}
						else {
							if (SamplesRemaining > 0 ) {
								numBytes = 2 + SamplesRemaining*4;
								ws.binary(ClientID, (uint8_t*)pb, numBytes); 
								}
							state = ST_TX_COMPLETE;
							}
						}						
				break;

				case ST_TX_COMPLETE :
					if (TransmitOKFlag == true) {
						TransmitOKFlag = false;
						ESP_LOGD(TAG,"Socket msg : Tx Complete");
						msg = MSG_TX_COMPLETE;
						ws.binary(ClientID, (uint8_t*)&msg, 2); 
						state = ST_IDLE;
						txSamples = 0;
						bufferOffset = 0;
						}
				break;
				}
			}	
		}
	}


static void capture_task(void* pvParameter)  {	
    esp_log_timestamp(TAG, "capture_task running on core %d with priority %d", xPortGetCoreID(), uxTaskPriorityGet(NULL));
	Wire.begin(pinSDA,pinSCL); 
	Wire.setClock(1000000);
	uint16_t val;
	ina226_read_reg(REG_ID, &val);
	if (val != 0x5449) {
		ESP_LOGE(TAG,"INA226 Manufacturer ID read = 0x%04X, expected 0x5449\n", val);
		ESP_LOGE(TAG,"Halting...");
		while (1){}
		}

	ina226_reset();
	// get largest malloc-able block of byte-addressable free memory
	int32_t maxBufferBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
	ESP_LOGI(TAG, "Free memory malloc-able for sample Buffer = %d bytes", maxBufferBytes);

	Buffer = (int16_t*)malloc(maxBufferBytes);
	if (Buffer == nullptr) {
		ESP_LOGE(TAG, "Could not allocate sample Buffer with %d bytes", maxBufferBytes);
		ESP_LOGE(TAG,"Halting...");
		while (1){}
		}

	MaxSamples = (maxBufferBytes - 4)/4;
	ESP_LOGI(TAG, "Max Buffer Samples = %d", MaxSamples);
	//nv_options_reset(Options);
	//nv_config_reset(ConfigTbl);
   	

	while (1){
			if (CaptureFlag == true) {
				CaptureFlag = false;
				if (Measure.nSamples == 0) {
					ESP_LOGD(TAG,"Capturing gated samples using cfg = 0x%04X, scale %d\n", Measure.cfg, Measure.scale );
					ina226_capture_gated(Measure, Buffer);
					}
				else {
					ESP_LOGD(TAG,"Capturing %d samples using cfg = 0x%04X, scale %d\n", Measure.nSamples, Measure.cfg, Measure.scale );
					ina226_capture_triggered(Measure, Buffer);
					}
				DataReadyFlag = true;
				}
			vTaskDelay(1);
			}
	}


