#ifndef EGAUGE_PARSER_H
#define EGAUGE_PARSER_H

#include <tinyxml2.h>
using namespace tinyxml2;

/**
 * @brief EgaugeParser is a utility class for parsing XML data from eGauge devices.
 *
 * The XML structure is expected to have the format:
 * <data>
 *   <r did="0"><i>123.456</i></r>
 *   ...
 * </data>
 *
 * Each <r> element represents a data record with a device ID (did) and a numeric value.
 * The parser extracts up to 32 records, sorts them by `did`, and packs the values into a
 * byte buffer using a custom 4-byte fixed-point format:
 *
 * - Byte 0: MSB of integer part (bit 7 = sign flag, 1 if negative)
 * - Byte 1: LSB of integer part
 * - Byte 2: MSB of decimal part (multiplied by 1000)
 * - Byte 3: LSB of decimal part
 */
class EgaugeParser {
public:
    EgaugeParser() = delete; // Static-only utility class

    /**
     * @brief Parses a raw XML string from eGauge and converts the numeric records into a byte array.
     *
     * @param xmlRawStr Raw XML string from eGauge device.
     * @param count Output: number of records parsed.
     * @param data Output: pointer to byte buffer to store the encoded values (must be at least 4 * MAX_RECORDS bytes).
     * @return true if parsing succeeds and at least one record is extracted, false otherwise.
     */
    static bool Parse(const String& xmlRawStr, uint16_t& count, uint8_t* data) {
        struct Record {
            int did;
            float value;
        };

        // Clean and extract valid XML from HTTP response
        auto xmlStr = cleanXml(xmlRawStr);
        if(xmlStr.isEmpty())
            return false;

        const int MAX_RECORDS = 32;
        Record records[MAX_RECORDS];
        count = 0;

        // Parse XML
        XMLDocument doc;
        XMLError err = doc.Parse(xmlStr.c_str());
        if (err != XML_SUCCESS) return false;

        XMLElement* root = doc.FirstChildElement("data");
        if (!root) return false;

        // Extract and store records
        for (XMLElement* r = root->FirstChildElement("r"); r && count < MAX_RECORDS; r = r->NextSiblingElement("r")) {
            int did = r->IntAttribute("did", -1);
            if (did < 0) continue;

            XMLElement* iElem = r->FirstChildElement("i");
            if (!iElem || !iElem->GetText()) continue;

            float value = atof(iElem->GetText());
            records[count++] = { did, value };
        }

        // Sort records by device ID
        for (int i = 0; i < count - 1; ++i) {
            for (int j = i + 1; j < count; ++j) {
                if (records[j].did < records[i].did) {
                    Record tmp = records[i];
                    records[i] = records[j];
                    records[j] = tmp;
                }
            }
        }

        // Encode each record into 4 bytes
        for (int i = 0; i < count; ++i) {
            uint16_t integer = (uint16_t)(fabs(records[i].value));
            uint16_t decimal = (uint16_t)((fabs(records[i].value) - integer) * 1000);

            // Store integer part
            data[4*i]   = integer >> 8;
            data[4*i+1] = integer & 0xFF;

            // Store decimal part
            data[4*i+2] = decimal >> 8;
            data[4*i+3] = decimal & 0xFF;

            // Encode sign bit in MSB of first byte
            if (records[i].value < 0) {
                data[4*i] |= 0x80;
            }
        }

        return count > 0;
    }

    /**
     * @brief Extracts the valid XML content from a raw HTTP response.
     *
     * @param input HTTP response string containing embedded XML.
     * @return Clean XML string, or empty if markers not found.
     */
    static String cleanXml(const String& input) {
        int xmlStart = input.indexOf("<?xml");
        int xmlEnd = input.lastIndexOf("</data>");
        if (xmlStart == -1 || xmlEnd == -1) return "";
        return input.substring(xmlStart, xmlEnd + 7);
    }
};

#endif // EGAUGE_PARSER_H
