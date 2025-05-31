#ifndef W25Q32_DRIVER_H
#define W25Q32_DRIVER_H

#include <Arduino.h>
#include <SPI.h>

class W25Q32Driver {
public:
    W25Q32Driver(SPIClass &spi, uint8_t cs) : _spi(spi), _cs(cs) {}

    void begin() {
        pinMode(_cs, OUTPUT);
        digitalWrite(_cs, HIGH);
        //_spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
        //digitalWrite(_cs, HIGH);
    }

    void end() {
        _spi.endTransaction();
    }

    void readJEDECID(uint8_t &manuf, uint8_t &memtype, uint8_t &capacity) {
        digitalWrite(_cs, LOW);
        _spi.transfer(0x9F);
        manuf = _spi.transfer(0x00);
        memtype = _spi.transfer(0x00);
        capacity = _spi.transfer(0x00);
        digitalWrite(_cs, HIGH);
    }

    void readUID(uint8_t *uid) {
        digitalWrite(_cs, LOW);
        _spi.transfer(0x4B);  // Read Unique ID
        for (int i = 0; i < 4; i++) _spi.transfer(0x00);  // Dummy
        for (int i = 0; i < 8; i++) uid[i] = _spi.transfer(0x00);
        digitalWrite(_cs, HIGH);
    }

    void readData(uint32_t addr, uint8_t *buf, size_t len) {
        digitalWrite(_cs, LOW);
        _spi.transfer(0x03);  // READ DATA
        _spi.transfer((addr >> 16) & 0xFF);
        _spi.transfer((addr >> 8) & 0xFF);
        _spi.transfer(addr & 0xFF);
        for (size_t i = 0; i < len; i++) {
            buf[i] = _spi.transfer(0x00);
        }
        digitalWrite(_cs, HIGH);
    }

    void writeEnable() {
        digitalWrite(_cs, LOW);
        _spi.transfer(0x06);
        digitalWrite(_cs, HIGH);
    }

    void pageProgram(uint32_t addr, const uint8_t *buf, size_t len) {
        if (len > 256) len = 256;
        writeEnable();
        digitalWrite(_cs, LOW);
        _spi.transfer(0x02);
        _spi.transfer((addr >> 16) & 0xFF);
        _spi.transfer((addr >> 8) & 0xFF);
        _spi.transfer(addr & 0xFF);
        for (size_t i = 0; i < len; i++) {
            _spi.transfer(buf[i]);
        }
        digitalWrite(_cs, HIGH);
        waitBusy();
    }

    void sectorErase(uint32_t addr) {
        writeEnable();
        digitalWrite(_cs, LOW);
        _spi.transfer(0x20);
        _spi.transfer((addr >> 16) & 0xFF);
        _spi.transfer((addr >> 8) & 0xFF);
        _spi.transfer(addr & 0xFF);
        digitalWrite(_cs, HIGH);
        waitBusy();
    }

    uint8_t readStatus() {
        digitalWrite(_cs, LOW);
        _spi.transfer(0x05);
        uint8_t status = _spi.transfer(0x00);
        digitalWrite(_cs, HIGH);
        return status;
    }

    void waitBusy() {
        while (readStatus() & 0x01) {
            delay(1);
        }
    }

private:
    SPIClass &_spi;
    uint8_t _cs;
};

#endif // W25Q32_DRIVER_H