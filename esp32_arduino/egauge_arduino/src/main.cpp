
#include <SPI.h>
#include <Ethernet.h>
#include <SD.h>
#include <esp_task_wdt.h>
#include <RTClib.h>
#include "sqlite3.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <EgaugeParse.h> // Custom eGauge XML parser

// --- Pin definitions for Ethernet (FSPI)
#define W5500_SCK 13
#define W5500_MISO 9
#define W5500_MOSI 14
#define W5500_CS 12

// --- Pin definitions for SD card (HSPI)
#define SD_SCK 5
#define SD_MISO 4
#define SD_MOSI 6
#define SD_CS 7

// --- I2C pins for DS3231 RTC
#define DS3231_SDA 18
#define DS3231_SCL 17

// --- UART communication with STM32
#define SERIAL_STM32_RX 19
#define SERIAL_STM32_TX 20

// --- Status LEDs
#define LED_GREEN_PIN 41     // Indicates successful LoRa or eGauge operation
#define LED_YELLOW_PIN 1     // Indicates SD card / DB access

// --- Config options
#define SAVE_TO_LOCAL 1      // Set to 1 to enable saving eGauge data to SD card
#define MAX_COUNT 20         // Maximum number of eGauge channels expected

// --- Struct to hold a full data row for SQLite logging
struct data_table_t {
	uint32_t time;
	float data[MAX_COUNT];
};

// --- eGauge configuration
//const char *egaugeHost = "192.168.1.88";
IPAddress egaugeIp(192, 168, 1, 88);
const int egaugePort = 80;

// --- Static IP configuration for Ethernet
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
IPAddress ip(192, 168, 1, 55);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// --- Hardware interfaces
RTC_DS3231 rtc;
SPIClass SPI_SD(1); // HSPI bus for SD

// --- SQLite configuration
sqlite3 *db;
char *sqlite_error_msg = 0;
int rc;
const char *dbFullPath = "/sdcard/data_logger.db";

// --- LoRa packet format
uint8_t lora_header[8];   // [0-3] = "coed", [4-5] = error_code, [6-7] = data count
uint8_t lora_body[100];   // Payload: [CRC, values..., 's','u','\n']
uint16_t lora_data_count; // Number of 4-byte float values in body
uint16_t error_code = 0;  // Bitwise error code (bit 0 = eGauge read fail)

// --- Queues for tasks
QueueHandle_t ledGreenFlashQueue;
QueueHandle_t ledYellowFlashQueue;
QueueHandle_t sdSaveQueue;

// --- Toggle functions for status LEDs
static bool ledGreenStatus = false;
void toggleGreenLed() {
	digitalWrite(LED_GREEN_PIN, ledGreenStatus);
	ledGreenStatus = !ledGreenStatus;
}

static bool ledYellowStatus = false;
void toggleYellowLed() {
	digitalWrite(LED_YELLOW_PIN, ledYellowStatus);
	ledYellowStatus = !ledYellowStatus;
}

// --- Flash green LED for specified count (triggered by queue)
void ledGreenFlashTask(void *parameter) {
	int flashCount;
	pinMode(LED_GREEN_PIN, OUTPUT);
	digitalWrite(LED_GREEN_PIN, 0);
	for (;;) {
		if (xQueueReceive(ledGreenFlashQueue, &flashCount, portMAX_DELAY) == pdTRUE) {
			for (int i = 0; i < flashCount; i++) {
				toggleGreenLed();
				vTaskDelay(pdMS_TO_TICKS(200));
				toggleGreenLed();
				vTaskDelay(pdMS_TO_TICKS(200));
			}
			digitalWrite(LED_GREEN_PIN, 0);
		}
	}
}

// --- Flash yellow LED for specified count (triggered by queue)
void ledYellowFlashTask(void *parameter) {
	int flashCount;
	pinMode(LED_YELLOW_PIN, OUTPUT);
	digitalWrite(LED_YELLOW_PIN, 0);
	for (;;) {
		if (xQueueReceive(ledYellowFlashQueue, &flashCount, portMAX_DELAY) == pdTRUE) {
			for (int i = 0; i < flashCount; i++) {
				toggleYellowLed();
				vTaskDelay(pdMS_TO_TICKS(200));
				toggleYellowLed();
				vTaskDelay(pdMS_TO_TICKS(200));
			}
			digitalWrite(LED_YELLOW_PIN, 0);
		}
	}
}

// --- Background task to initialize and write eGauge data to SQLite on SD
void sdCardTask(void *parameter) {
	data_table_t data;
	SPI_SD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

	// --- Mount SD card
	while (!SD.begin(SD_CS, SPI_SD, 8000000, "/sdcard")) {
		Serial.println("❌ SD init failed");
		toggleYellowLed();
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	Serial.println("✅ SD OK");

	// --- Open SQLite database
	rc = sqlite3_open(dbFullPath, &db);
	char sql[512];
	while (rc) {
		Serial.printf("❌ Can't open database: %s\n", sqlite3_errmsg(db));
		vTaskDelay(pdMS_TO_TICKS(1000));
		rc = sqlite3_open(dbFullPath, &db);
	}
	Serial.println("✅ Opened SQLite database");

	// --- Create table if not exists
	const char *createTableSQL =
		"CREATE TABLE IF NOT EXISTS egauge_log (timestamp INTEGER, did0 REAL, did1 REAL, did2 REAL, did3 REAL, did4 REAL, did5 REAL, did6 REAL, did7 REAL, did8 REAL, did9 REAL, "
		"did10 REAL, did11 REAL, did12 REAL, did13 REAL, did14 REAL, did15 REAL, did16 REAL, did17 REAL, did18 REAL, did19 REAL);";
	rc = sqlite3_exec(db, createTableSQL, NULL, 0, &sqlite_error_msg);
	while (rc != SQLITE_OK) {
		Serial.printf("SQL error: %s\n", sqlite_error_msg);
		sqlite3_free(sqlite_error_msg);
		vTaskDelay(pdMS_TO_TICKS(1000));
		rc = sqlite3_exec(db, createTableSQL, NULL, 0, &sqlite_error_msg);
	}
	Serial.println("✅ Table created or exists");

	// --- Write data to SQLite whenever new data is available
	for (;;) {
		if (xQueueReceive(sdSaveQueue, &data, portMAX_DELAY) == pdTRUE) {
			snprintf(sql, sizeof(sql),
				"INSERT INTO egauge_log (timestamp,did0,did1,did2,did3,did4,did5,did6,did7,did8,did9,"
				"did10,did11,did12,did13,did14,did15,did16,did17,did18,did19) "
				"VALUES (%d,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,"
				"%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f,%0.3f);",
				data.time, data.data[0], data.data[1], data.data[2], data.data[3], data.data[4],
				data.data[5], data.data[6], data.data[7], data.data[8], data.data[9],
				data.data[10], data.data[11], data.data[12], data.data[13], data.data[14],
				data.data[15], data.data[16], data.data[17], data.data[18], data.data[19]);

			Serial.printf("SQL: %s\n", sql);

			rc = sqlite3_exec(db, sql, NULL, NULL, &sqlite_error_msg);
			if (rc != SQLITE_OK) {
				Serial.printf("❌ Insert error: %s\n", sqlite_error_msg);
				sqlite3_free(sqlite_error_msg);
			} else {
				Serial.println("✅ Inserted log entry");
			}
		}
	}
}

// --- CRC-8/MAXIM algorithm for LoRa message integrity
uint8_t gencrc(uint8_t *data, size_t len) {
	uint8_t crc = 0xff;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (size_t j = 0; j < 8; j++) {
			crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
		}
	}
	return crc;
}

// --- Send data to LoRa module via Serial1 (not thread-safe)
void sendLoraData() {
	lora_header[4] = error_code >> 8;
	lora_header[5] = error_code & 0xFF;
	lora_header[6] = lora_data_count >> 8;
	lora_header[7] = lora_data_count & 0xFF;

	Serial1.write(lora_header, 8);

	if (error_code) return;

	vTaskDelay(pdMS_TO_TICKS(1)); // brief delay to ensure timing
	memcpy(lora_body + 4 * lora_data_count + 1, "su\n", 3);
	lora_body[4 * lora_data_count] = gencrc(lora_body, 4 * lora_data_count);
	Serial1.write(lora_body, 4 * lora_data_count + 4);
}

// --- Task for handling incoming commands from STM32 via Serial1
void loraTask(void *parameter) {
	String input = "";
	for (;;) {
		while (Serial1.available()) {
			char c = Serial1.read();
			Serial.print(c); // Debug
			input += c;

			if (input.endsWith("\r\n")) {
				input.trim();
				if (input.startsWith("OK")) {
					Serial.println("Send data to Lora module complete");
					int flashTimes = 6;
					xQueueSend(ledGreenFlashQueue, &flashTimes, 0);
				} else if (input.startsWith("GET")) {
					Serial.println("Receive Get");
					sendLoraData();
				}
				input = "";
			}
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

// --- Task to poll eGauge, parse data, and optionally log to SD
void egaugeTask(void *parameter) {
	EthernetClient client;
	data_table_t sql_data;

	for (;;) {
		if (client.connect(egaugeIp, egaugePort)) {
			client.print("GET /cgi-bin/egauge?inst HTTP/1.1\r\n");
			client.print("Host: 192.168.1.88\r\n");
			client.print("Connection: close\r\n");
			client.print("\r\n");

			String response = "";
			unsigned long timeout = millis();
			while (client.connected() && millis() - timeout < 3000) {
				while (client.available()) {
					char c = client.read();
					response += c;
				}
			}
			client.stop();

			// Parse and convert data
			//Serial.println(response);

			if (response.length() > 0 && EgaugeParser::Parse(response, lora_data_count, lora_body)) {
				Serial.println("Reading eGauge Data Success");
				int flashTimes = 6;
				xQueueSend(ledYellowFlashQueue, &flashTimes, 0);
				error_code &= 0xFE;

				if (SAVE_TO_LOCAL) {					
					if (rtc.begin()) {
						sql_data.time = rtc.now().unixtime();
					}
					for (int i = 0; i < lora_data_count; ++i) {
						sql_data.data[i] = (lora_body[2 * i] & 0x7F) + lora_body[2 * i + 1] / 1000.0f;
						if (lora_body[2 * i] & 0x80)
							sql_data.data[i] = -sql_data.data[i];
					}
					for (int i = lora_data_count; i < MAX_COUNT; ++i) {
						sql_data.data[i] = 0.0f;
					}
					xQueueSend(sdSaveQueue, &sql_data, 0);
				}
			} else {
				error_code |= 0x01;
				Serial.println("Reading eGauge Data Fail");
			}
		} else {
			error_code |= 0x01;
			Serial.println("Connecting to eGauge Fails, Please check the connection.");
		}
		vTaskDelay(pdMS_TO_TICKS(10000)); // 10s polling interval
	}
}

// --- System setup
void setup() {
	Serial.begin(115200);
	Serial1.begin(115200, SERIAL_8N1, SERIAL_STM32_RX, SERIAL_STM32_TX);
	memcpy(lora_header, "coed", 4);

	// --- Ethernet setup
	SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
	Ethernet.init(W5500_CS);
	Ethernet.begin(mac, ip, gateway, subnet);
	delay(500);
	Serial.print("Ethernet IP: ");
	Serial.println(Ethernet.localIP());

	// --- Queue creation
	ledGreenFlashQueue = xQueueCreate(5, sizeof(int));
	ledYellowFlashQueue = xQueueCreate(5, sizeof(int));
	if (SAVE_TO_LOCAL)
		sdSaveQueue = xQueueCreate(5, sizeof(data_table_t));
	if (!ledGreenFlashQueue || !ledYellowFlashQueue) {
		Serial.println("❌ Failed to create LED flash queue");
		while (1);
	}

	// --- Task creation
	xTaskCreatePinnedToCore(egaugeTask, "eGauge Reader", 8192, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(loraTask, "Serial STM", 4096, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(ledGreenFlashTask, "Green LED", 1024, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(ledYellowFlashTask, "Yellow LED", 1024, NULL, 1, NULL, 1);
	if (SAVE_TO_LOCAL)
		xTaskCreatePinnedToCore(sdCardTask, "save to sdcard", 8192, NULL, 1, NULL, 1);

	// --- RTC setup
	Wire.begin(DS3231_SDA, DS3231_SCL);
	if (!rtc.begin()) {
		Serial.println("❌ Couldn't find RTC");
	} else {
		Serial.println("✅ RTC found");
		if (rtc.lostPower()) {
			Serial.println("⚠️ RTC lost power, setting time to compile time");
			rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
		}
	}

	// --- Watchdog setup
	esp_task_wdt_init(5, true);
	esp_task_wdt_add(NULL);
}

// --- Background loop to feed watchdog
void loop() {
	delay(2000);
	esp_task_wdt_reset();
}
