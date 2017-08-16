// Example to demonstrate write latency for preallocated exFAT files.
// I suggest you write a PC program to convert very large bin files.
//
// An exFAT SD is required. The ExFatFormatter example will format
// smaller cards with an exFAT file system.
//
// The maximum data rate will depend on the quality of your SD,
// the size of the FIFO, and using dedicated SPI.
#include "SdFs.h"
#include "FreeStack.h"
#include "ExFatLogger.h"

// Set USE_RTC nonzero for file timestamps.
// RAM use may be marginal on Uno with RTClib.
#define USE_RTC 0
#if USE_RTC
#include "RTClib.h"
#endif

// LED to light if overruns occur.
#define ERROR_LED_PIN -1

/*
  Change the value of SD_CS_PIN if you are using SPI and
  your hardware does not use the default value, SS.
  Common values are:
  Arduino Ethernet shield: pin 4
  Sparkfun SD shield: pin 8
  Adafruit SD shields and modules: pin 10
*/

// SDCARD_SS_PIN is defined for the built-in SD on some boards.
#ifndef SDCARD_SS_PIN
const uint8_t SD_CS_PIN = SS;
#else  // SDCARD_SS_PIN
// Assume built-in SD is used.
const uint8_t SD_CS_PIN = SDCARD_SS_PIN;
#endif  // SDCARD_SS_PIN

// Interval between data records in microseconds.
// Try 250 with Teensy 3.6, Due, or STM32.
// Try 2000 with AVR boards.
// Try 4000 with SAMD Zero boards.
const uint32_t LOG_INTERVAL_USEC = 2000;

// FIFO SIZE - 512 byte sectors.  Modify for your board.
#ifdef __AVR_ATmega328P__
// Use 512 bytes for 328 boards.
#define FIFO_SIZE_SECTORS 1
#elif defined(__AVR__)
// Use 2 KiB for other AVR boards.
#define FIFO_SIZE_SECTORS 4
#else  // __AVR_ATmega328P__
// Use 8 KiB for non-AVR boards.
#define FIFO_SIZE_SECTORS 16
#endif  // __AVR_ATmega328P__

// Preallocate 1GiB file.  Use uint64_t for 4GiB or larger.
const uint32_t PREALLOCATE_SIZE = 1024UL*1024*1024;

// Select the fastest interface. Assumes no other SPI devices.
#if ENABLE_DEDICATED_SPI
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI)
#else  // ENABLE_DEDICATED_SPI
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI)
#endif  // ENABLE_DEDICATED_SPI

// Save SRAM if 328.
#ifdef __AVR_ATmega328P__
#include "MinimumSerial.h"
MinimumSerial MinSerial;
#define Serial MinSerial
#endif  // __AVR_ATmega328P__
//==============================================================================
// Replace logRecord(), printRecord(), and ExFatLogger.h for your sensors.
void logRecord(data_t* data, uint16_t overrun) {
  if (overrun) {
    // Add one since this record has no adc data. Could add overrun field.
    overrun++;
    data->adc[0] = 0X8000 | overrun;
  } else {
    for (size_t i = 0; i < ADC_COUNT; i++) {
      data->adc[i] = analogRead(i);
    }
  }
}
//------------------------------------------------------------------------------
void printRecord(Print* pr, data_t* data) {
  static uint32_t nr = 0;
  if (!data) {
    pr->print(F("LOG_INTERVAL_USEC,"));
    pr->println(LOG_INTERVAL_USEC);
    pr->print(F("rec#"));
    for (size_t i = 0; i < ADC_COUNT; i++) {
      pr->print(F(",adc"));
      pr->print(i);
    }
    pr->println();
    nr = 0;
    return;
  }
  if (data->adc[0] & 0X8000) {
    uint16_t n = data->adc[0] & 0X7FFF;
    nr += n;
    pr->print(F("-1,"));
    pr->print(n);
    pr->println(F(",overuns"));
  } else {
    pr->print(nr++);
    for (size_t i = 0; i < ADC_COUNT; i++) {
      pr->write(',');
      pr->print(data->adc[i]);
    }
    pr->println();
  }
}
//==============================================================================
// Max length of file name including zero byte.
#define FILE_NAME_DIM 40
// Max number of records to buffer while SD is busy.
const size_t FIFO_DIM = 512*FIFO_SIZE_SECTORS/sizeof(data_t);
SdExFat sd;

ExFile binFile;

// You may modify the filename.  Digits before the dot are file versions.
char binName[] = "ExFatLogger00.bin";
//------------------------------------------------------------------------------
#if USE_RTC
RTC_DS1307 rtc;

// Call back for file timestamps.  Only called for file create and sync().
void dateTime(uint16_t* date, uint16_t* time) {
  DateTime now = rtc.now();

  // Return date using FS_DATE macro to format fields.
  *date = FS_DATE(now.year(), now.month(), now.day());

  // Return time using FS_TIME macro to format fields.
  *time = FS_TIME(now.hour(), now.minute(), now.second());
}
#endif
//------------------------------------------------------------------------------
void errorHalt() {
  binFile.close();
  sd.printSdError(&Serial);
  SysCall::halt();
}
//------------------------------------------------------------------------------
#define error(s) {Serial.print(F("Error: "));Serial.println(F(s));errorHalt();}
//-----------------------------------------------------------------------------
// Convert binary file to csv file.
void binaryToCsv() {
  uint8_t lastPct = 0;
  uint32_t t0 = millis();
  ExFile csvFile;
  data_t binData[FIFO_DIM];
  // Save stack space by recycling FIFO buffer - important for Uno.
  char* csvName = (char*)binData;

  if (!binFile.isOpen()) {
    Serial.println();
    Serial.println(F("No current binary file"));
    return;
  }
  Serial.println();
  Serial.print(F("FreeStack: "));
  Serial.println(FreeStack());

  // Create a new csvFile.
  binFile.getName(csvName, sizeof(binData));
  char* dot = strchr(csvName, '.');
  if (!dot) {
    error("no dot in filename");
  }
  strcpy(dot + 1, "csv");
  if (!csvFile.open(csvName, O_WRITE | O_CREAT | O_TRUNC)) {
    error("open csvFile failed");
  }
  binFile.rewind();
  serialClearInput();
  Serial.print(F("Writing: "));
  Serial.print(csvName);
  Serial.println(F(" - type any character to stop"));
  printRecord(&csvFile, nullptr);
  uint32_t tPct = millis();
  while (!Serial.available() && binFile.available()) {
    int nb = binFile.read(binData, sizeof(binData));
    if (nb <= 0 ) {
      error("read binFile failed");
    }
    size_t nr = nb/sizeof(data_t);
    for (size_t i = 0; i < nr; i++) {
      printRecord(&csvFile, &binData[i]);
    }

    if ((millis() - tPct) > 1000) {
      uint8_t pct = binFile.curPosition()/(binFile.fileSize()/100);
      if (pct != lastPct) {
        tPct = millis();
        lastPct = pct;
        Serial.print(pct, DEC);
        Serial.println('%');
        csvFile.sync();
      }
    }
    if (Serial.available()) {
      break;
    }
  }
  csvFile.close();
  Serial.print(F("Done: "));
  Serial.print(0.001*(millis() - t0));
  Serial.println(F(" Seconds"));
}
//-------------------------------------------------------------------------------
void createBinFile() {
  binFile.close();
  while (sd.exists(binName)) {
    char* p = strchr(binName, '.');
    if (!p) {
      error("no dot in filename");
    }
    while (true) {
      p--;
      if (p < binName || *p < '0' || *p > '9') {
        error("Can't create file name");
      }
      if (p[0] != '9') {
        p[0]++;
        break;
      }
      p[0] = '0';
    }
  }
  if (!binFile.open(binName, O_RDWR | O_CREAT)) {
    error("open binName failed");
  }
  if (!binFile.preAllocate(PREALLOCATE_SIZE)) {
    error("preAllocate failed");
  }
  Serial.println(binName);
  Serial.print(F("preAllocated: "));
  Serial.print(PREALLOCATE_SIZE);
  Serial.println(F(" bytes"));
}
//-------------------------------------------------------------------------------
void logData() {
  int32_t delta;  // Jitter in log time.
  int32_t maxDelta = 0;
  uint32_t maxLogMicros = 0;
  uint32_t maxWriteMicros = 0;
  size_t maxFifoCount = 0;
  size_t fifoCount = 0;
  size_t fifoHead = 0;
  size_t fifoTail = 0;
  uint16_t overrun = 0;
  uint16_t maxOverrun = 0;
  uint32_t totalOverrun = 0;
  data_t fifoData[FIFO_DIM];

  Serial.println();
  Serial.print(F("FreeStack: "));
  Serial.println(FreeStack());
  serialClearInput();
  Serial.println(F("Type any character to stop"));

  // Wait until SD is not busy.
  while (sd.card()->isBusy()) {}

  // Start time for log file.
  uint32_t m = millis();

  // Time to log next record.
  uint32_t logTime = micros();
  while (1) {
    // Time for next data record.
    logTime += LOG_INTERVAL_USEC;

    // Wait until time to log data.
    delta = micros() - logTime;
    if (delta > 0) {
      Serial.print(F("delta: "));
      Serial.println(delta);
      error("Rate too fast");
    }
    while (delta < 0) {
      delta = micros() - logTime;
    }

    if (fifoCount < FIFO_DIM) {
      uint32_t m = micros();
      logRecord(fifoData + fifoHead, overrun);
      m = micros() - m;
      if (m > maxLogMicros) {
        maxLogMicros = m;
      }
      fifoHead = fifoHead < (FIFO_DIM - 1) ? fifoHead + 1 : 0;
      fifoCount++;
      if (overrun) {
        if (overrun > maxOverrun) {
          maxOverrun = overrun;
        }
        overrun = 0;
      }
    } else {
      totalOverrun++;
      overrun++;
      if (overrun > 0XFFF) {
        error("too many overruns");
      }
      if (ERROR_LED_PIN >= 0) {
        digitalWrite(ERROR_LED_PIN, HIGH);
      }
    }

    // Save max jitter.
    if (delta > maxDelta) {
      maxDelta = delta;
    }
    // Write data if SD is not busy.
    if (!sd.card()->isBusy()) {
      size_t nw = fifoHead > fifoTail ? fifoCount : FIFO_DIM - fifoTail;
      // Limit write time by not writing more than 512 bytes.
      const size_t MAX_WRITE = 512/sizeof(data_t);
      if (nw > MAX_WRITE) nw = MAX_WRITE;
      size_t nb = nw*sizeof(data_t);
      uint32_t usec = micros();
      if (nb != binFile.write(&fifoData[fifoTail], nb)) {
        error("write binFile failed");
      }
      usec = micros() - usec;
      if (usec > maxWriteMicros) {
        maxWriteMicros = usec;
      }
      fifoTail = (fifoTail + nw) < FIFO_DIM ? fifoTail + nw : 0;
      if (fifoCount > maxFifoCount) {
        maxFifoCount = fifoCount;
      }
      fifoCount -= nw;
      if (Serial.available()) {
        break;
      }
    }
  }
  Serial.print(F("\nLog time: "));
  Serial.print(0.001*(millis() - m));
  Serial.println(F(" Seconds"));
  binFile.truncate();
  binFile.sync();
  Serial.print(("File size: "));
  // Warning cast used for print since fileSize is uint64_t.
  Serial.print((uint32_t)binFile.fileSize());
  Serial.println(F(" bytes"));
  Serial.print(F("totalOverrun: "));
  Serial.println(totalOverrun);
  Serial.print(F("maxFifoCount: "));
  Serial.println(maxFifoCount);
  Serial.print(F("maxLogMicros: "));
  Serial.println(maxLogMicros);
  Serial.print(F("maxWriteMicros: "));
  Serial.println(maxWriteMicros);
  Serial.print(F("Log interval: "));
  Serial.print(LOG_INTERVAL_USEC);
  Serial.print(F(" micros\nmaxDelta: "));
  Serial.print(maxDelta);
  Serial.println(F(" micros"));
}
//------------------------------------------------------------------------------
void openBinFile() {
  char name[FILE_NAME_DIM];
  serialClearInput();
  Serial.println(F("\nEnter file name"));
  if (!serialReadLine(name, sizeof(name))) {
    return;
  }
  if (!sd.exists(name)) {
    Serial.println(name);
    Serial.println(F("File does not exist"));
    return;
  }
  binFile.close();
  if (!binFile.open(name, O_READ)) {
    Serial.println(name);
    Serial.println(F("open failed"));
    return;
  }
  Serial.println(F("File opened"));
}
//-----------------------------------------------------------------------------
void printData() {
  if (!binFile.isOpen()) {
    Serial.println();
    Serial.println(F("No current binary file"));
    return;
  }
  binFile.rewind();
  serialClearInput();
  Serial.println(F("type any character to stop\n"));
  delay(1000);
  printRecord(&Serial, nullptr);
  while (binFile.available() && !Serial.available()) {
    data_t record;
    if (binFile.read(&record, sizeof(data_t)) != sizeof(data_t)) {
      error("read binFile failed");
    }
    printRecord(&Serial, &record);
  }
}
//------------------------------------------------------------------------------
void serialClearInput() {
  do {
    delay(10);
  } while (Serial.read() >= 0);
}
//------------------------------------------------------------------------------
bool serialReadLine(char* str, size_t size) {
  size_t n = 0;
  while(!Serial.available()) {
    yield();
  }
  while (true) {
    int c = Serial.read();
    if (c < ' ') break;
    str[n++] = c;
    if (n >= size) {
      Serial.println(F("input too long"));
      return false;
    }
    uint32_t m = millis();
    while (!Serial.available() && (millis() - m) < 100){}
    if (!Serial.available()) break;
  }
  str[n] = 0;
  return true;
}
//------------------------------------------------------------------------------
void testSensor() {
  const uint32_t interval = 200000;
  int32_t diff;
  data_t data;
  serialClearInput();
  Serial.println(F("\nTesting - type any character to stop\n"));
  delay(1000);
  printRecord(&Serial, nullptr);
  uint32_t m = micros();
  while (!Serial.available()) {
    m += interval;
    do {
      diff = m - micros();
    } while (diff > 0);
    logRecord(&data, 0);
    printRecord(&Serial, &data);
  }
}
//------------------------------------------------------------------------------
void setup() {
  if (ERROR_LED_PIN >= 0) {
    pinMode(ERROR_LED_PIN, OUTPUT);
    digitalWrite(ERROR_LED_PIN, HIGH);
  }
  Serial.begin(9600);

  // Wait for USB Serial
  while (!Serial) {
    SysCall::yield();
  }
  delay(1000);
  Serial.println(F("Type any character to begin"));
  while (!Serial.available()) {
    yield();
  }

#if !ENABLE_DEDICATED_SPI
  Serial.println(F(
    "\nFor best performance edit SdFsConfig.h\n"
    "and set ENABLE_DEDICATED_SPI nonzero"));
#endif  // !ENABLE_DEDICATED_SPI

  Serial.print(F("\nFreeStack: "));
  Serial.println(FreeStack());
  Serial.print(FIFO_DIM);
  Serial.println(F(" FIFO entries will be used."));

  // Initialize SD.
  if (!sd.begin(SD_CONFIG)) {
    error("begin failed");
  }
#if USE_RTC
  if (!rtc.begin()) {
    error("rtc.begin failed");
  }
  if (!rtc.isrunning()) {
    // Set RTC to sketch compile date & time.
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    error("RTC is NOT running!");
  }
  // Set callback
  FsDateTime::callback = dateTime;
#endif  // USE_RTC
}
//------------------------------------------------------------------------------
void loop() {

  // Read any Serial data.
  serialClearInput();

  if (ERROR_LED_PIN >= 0) {
    digitalWrite(ERROR_LED_PIN, LOW);
  }  
  Serial.println();
  Serial.println(F("type:"));
  Serial.println(F("b - open existing bin file"));
  Serial.println(F("c - convert file to csv"));
  Serial.println(F("l - list files"));
  Serial.println(F("p - print data to Serial"));
  Serial.println(F("r - record data"));
  Serial.println(F("t - test without logging"));
  while(!Serial.available()) {
    SysCall::yield();
  }
  char c = tolower(Serial.read());

  if (c == 'b') {
    openBinFile();
  } else if (c == 'c') {
    binaryToCsv();
  } else if (c == 'l') {
    Serial.println(F("\nls:"));
    sd.ls(&Serial, LS_DATE | LS_SIZE);
  } else if (c == 'p') {
    printData();
  } else if (c == 'r') {
    createBinFile();
    logData();
  } else if (c == 't') {
    testSensor();
  } else {
    Serial.println(F("Invalid entry"));
  }
}