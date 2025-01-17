#include <vector>
#include "Pylon.h"
#include "Log.h"

namespace PylonToMQTT
{
CommandInformation _infoCommands[] = { CommandInformation::GetVersionInfo, CommandInformation::GetBarCode, CommandInformation::None };
CommandInformation _readingsCommands[] = { CommandInformation::AnalogValueFixedPoint, CommandInformation::AlarmInfo, CommandInformation::None };

Pylon::Pylon()
{
	_asyncSerial = new AsyncSerial();
	_TempKeys = { "CellTemp1_4", "CellTemp5_8", "CellTemp9_12", "CellTemp13_16", "MOS_T", "ENV_T"};
}

Pylon::~Pylon()
{
	delete _asyncSerial;
}

bool Pylon::Transmit() {
	bool sequenceComplete = false;
    if (_numberOfPacks == 0){
		// _root.clear();
	    // send_cmd(0xFF, CommandInformation::GetPackCount);
		
		_numberOfPacks = 4;
		logi("Set Pack Count: %d", _numberOfPacks);
		for (int i = 0; i < _numberOfPacks; i++) {
			char packName[STR_LEN];
			sprintf(packName, "Pack%d", i+1);
			_Packs.push_back(Pack(packName, &_TempKeys, _pcb));
		}
    }
    else {
		if (_currentPack < _Packs.size()) {
			if (_Packs[_currentPack].InfoPublished() == false) {
				if (_infoCommands[_infoCommandIndex] != CommandInformation::None){
					send_cmd(_currentPack+1, _infoCommands[_infoCommandIndex]);
				}
				_infoCommandIndex++;
				if (_infoCommandIndex == sizeof(_infoCommands)){
					_infoCommandIndex = 0;
					_currentPack++;
					if (_currentPack == _numberOfPacks){
						_currentPack = 0;
						sequenceComplete = true;
					}
				}
			}
			else {
				if (_readingsCommands[_readingsCommandIndex] != CommandInformation::None){
					send_cmd(_currentPack+1, _readingsCommands[_readingsCommandIndex]);
				}
				_readingsCommandIndex++;
				if (_readingsCommandIndex == sizeof(_readingsCommands)){
					_readingsCommandIndex = 0;
					_currentPack++;
					if (_currentPack == _numberOfPacks){
						_currentPack = 0;
						sequenceComplete = true;
					}
				}
			}
		}
    }
	return sequenceComplete;
}

uint16_t Pylon::get_frame_checksum(char* frame){
	uint16_t sum = 0;
	uint16_t len = strlen(frame);
	for (int i = 0; i < len; i++) {
		sum += frame[i];
	}
	sum = ~sum;
	sum %= 0x10000;
	sum += 1;
	return sum;
}

int Pylon::get_info_length(const char* info) {
	size_t lenid = strlen(info);
	if (lenid == 0)
		return 0;
	int lenid_sum = (lenid & 0xf) + ((lenid >> 4) & 0xf) + ((lenid >> 8) & 0xf);
	int lenid_modulo = lenid_sum % 16;
	int lenid_invert_plus_one = 0b1111 - lenid_modulo + 1;
	return (lenid_invert_plus_one << 12) + lenid;
}

void Pylon::encode_cmd(char* frame, uint8_t address, uint8_t cid2, const char* info) {
	char sub_frame[64];
	uint8_t cid1 = 0x46;
	sprintf(sub_frame, "%02X%02X%02X%02X%04X", 0x25, address, cid1, cid2, get_info_length(info));
	strcat(sub_frame, info);
	sprintf(frame, "~%s%04X\r", sub_frame, get_frame_checksum(sub_frame));
	return;
}

void Pylon::send_cmd(uint8_t address, CommandInformation cmd) {
	_currentCommand = cmd;
	char raw_frame[64];
	memset(raw_frame, 0, 64);
    char bdevid[4];
    sprintf(bdevid, "%02X", address);
	encode_cmd(raw_frame, address, cmd, bdevid);
	logd("send_cmd: %s", raw_frame);
	JsonObject debug = _root.createNestedObject("debug");
	debug["send_cmd"] = cmd;
	debug["frame"] = raw_frame;
	publish("debug/send/Pack%d", address, false);
    _asyncSerial->Send(cmd, (byte*)raw_frame, strlen(raw_frame));
}

String Pylon::convert_ASCII(char* p){
   String ascii = "";
   String hex = p;
   for (size_t i = 0; i < strlen(p); i += 2){
      String part = hex.substring(i, 2);
      ascii += strtol(part.c_str(), nullptr, 16);
   }
   return ascii;
}


unsigned char parse_hex(char c)
{
    if ('0' <= c && c <= '9') return c - '0';
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    return 0;
}

std::vector<unsigned char> parse_string(const std::string & s)
{
    size_t size = s.size();
    if (size % 2 != 0) size++;
    std::vector<unsigned char> result(size / 2);
    for (std::size_t i = 0; i != size / 2; ++i)
        result[i] = 16 * parse_hex(s[2 * i]) + parse_hex(s[2 * i + 1]);
    return result;
}

void Pylon::publish(String topic, uint16_t packNumber, bool retained)
{				
	String s;
	serializeJson(_root, s);
	_root.clear();
	char buf[64];
	sprintf(buf, topic.c_str(), packNumber); 
	_pcb->Publish(buf, s.c_str(), retained);
}

int Pylon::ParseResponse(char *szResponse, size_t readNow, CommandInformation cmd)
{
	if(readNow > 0 && szResponse[0] != '\0')
	{
		_root.clear();
		JsonObject debug = _root.createNestedObject("debug");
		debug["data"] = szResponse;
		debug["received_size"] = readNow;
		logd("received_size: %d", readNow);
		logd("data: %s", szResponse);
        std::string chksum; 
        chksum.assign(&szResponse[readNow-4]);
        std::vector<unsigned char> cs = parse_string(chksum);
        int i = 0;
        uint16_t CHKSUM = toShort(i, cs);
        uint16_t sum = 0;
		for (int i = 1; i < readNow-4; i++) {
			sum += szResponse[i];
		}
		bool checksumValid = true;
		if (((CHKSUM+sum) & 0xFFFF) != 0) { 
			uint16_t c = ~sum + 1;
			loge("Checksum failed: %04x, should be: %04X", sum, c); 
			debug["Checksum failed"] = sum;
			debug["Checksum should be"] = c;
			// publish("debug", 0, false);
			// return -1;
			checksumValid = false;
		} 
        std::string frame;
        frame.assign(&szResponse[1]); // skip SOI (~)
        std::vector<unsigned char> v = parse_string(frame);
        int index = 0;
		uint16_t VER = v[index++];
		uint16_t ADR = v[index++];
		uint16_t CID1 = v[index++];
		uint16_t CID2 = v[index++];
		uint16_t LENGTH = toShort(index, v);
		uint16_t LCHKSUM = LENGTH & 0xF000;
		uint16_t LENID = LENGTH & 0x0FFF;
		_root["ADR"] = ADR;	// used as a tag in db
		// debug["ADR"] = ADR;
		debug["CID1"] = CID1;
		debug["CID2"] = CID2;
		debug["LENID"] = LENID;
		if (readNow < (LENID + 17)) {  
			loge("Data length error LENGTH: %04X LENID: %04X, Received: %d", LENGTH, LENID, (readNow-17));
			debug["Data length error"] = true;
			publish("debug/Pack%d", ADR, false);
			return -1;
		}
		logd("VER: %02X, ADR: %02X, CID1: %02X, CID2: %02X, LENID: %02X (%d), CHKSUM: %02X", VER, ADR, CID1, CID2, LENID, LENID, CHKSUM);
		if (CID2 != ResponseCode::Normal) {
			loge("CID2 error code: %02X", CID2);
			debug["CID2 error code"] = CID2;
			publish("debug/Pack%d", ADR, false);
			return -1;
		}
		if (readNow == 139) {
			if (!checksumValid) {
				publish("debug", 0, false);
				return -1;
			}
			cmd = CommandInformation::AnalogValueFixedPoint;
			_root.remove("debug");
		}
		else if (readNow == 93) {
			if (!checksumValid) {
				publish("debug", 0, false);
				return -1;
			}
			cmd = CommandInformation::AlarmInfo;
			_root.remove("debug");
		}
		else if (readNow == 57) {
			cmd = CommandInformation::GetVersionInfo;
			debug.remove("data");
		}
		else if (readNow == 97) {
			cmd = CommandInformation::GetBarCode;
			debug.remove("data");
		}
		else {
			debug["unrecognized message"] = readNow;
			publish("debug/Pack%d", ADR, false);
			cmd = CommandInformation::None;
		}
		switch (cmd) {
			case CommandInformation::AnalogValueFixedPoint:
			{
				uint16_t INFO = toShort(index, v);
				uint16_t packNumber = INFO & 0x00FF;
				logi("AnalogValueFixedPoint: INFO: %04X, Pack: %d", INFO, packNumber);
				JsonArray cells = _root.createNestedArray("Cells");
				// char key[16];
				uint16_t numberOfCells = v[index++];
				for (int i = 0; i < numberOfCells; i++) {
					// sprintf(key, "Cell_%d", i+1);
					JsonObject cell = cells.createNestedObject();
					cell["Reading"] = (toShort(index, v))/1000.0;
					// cell["State"] = 0xF0;
					cell["Num"] = i+1;	// used as a tag
				}
				JsonArray temps = _root.createNestedArray("Temps");
				uint16_t numberOfTemps = v[index++];
				for (int i = 0; i < numberOfTemps; i++) {
					if ( i < _TempKeys.size()) {
						JsonObject entry = temps.createNestedObject();
						entry["Name"] = _TempKeys[i];
						float kelvin = (toShort(index, v)) - 2730.0;// use 273.0 instead of 273.15 to match jakiper app
						entry["Reading"] = round(kelvin)/10.0; // limit to one decimal place
						// entry["State"] = 0;  // default to ok
						// entry["Num"] = i+1;	// used as a tag
					}
				}
				int packIndex = ADR - 1;
				if (packIndex < _Packs.size()) {
					_Packs[packIndex].setNumberOfCells(numberOfCells);
					_Packs[packIndex].setNumberOfTemps(numberOfTemps);
					_Packs[packIndex].PublishDiscovery(); // PublishDiscovery if ready and not already published
				}
				JsonObject entry = _root.createNestedObject("PackCurrent");
				float current = ((int16_t)toShort(index, v))/100.0;
				entry["Reading"] = current;
				// entry["State"] = 0;  // default to ok
				entry = _root.createNestedObject("PackVoltage");
				float voltage = (toShort(index, v))/1000.0;
				entry["Reading"] = voltage;
				// entry["State"] = 0;  // default to ok
				int remain = toShort(index, v);
				_root["RemainingCapacity"] = (remain/100.0);
				index++; //skip user def code	
				int total = toShort(index, v);
				_root["FullCapacity"] = (total/100.0);
				_root["CycleCount"] = ((v[index++]<<8) | v[index++]);
				_root["SOC"] = (remain * 100) / total;	
				_root["Power"] = round(voltage * current);
				// module["LAST"] = ((v[index++]<<8) | (v[index++]<<8) | v[index++]); 
				publish("readings/Pack%d", ADR, false);
			}
			break;
			case CommandInformation::GetVersionInfo: {
				logi("GetVersionInfo");
				std::string ver;
				std::string s(v.begin(), v.end());
				ver = s.substr(index);
				_root["Version"] = ver.substr(0, 19);
				publish("info/Pack%d", ADR, true);
				int packIndex = ADR - 1;
				if (packIndex < _Packs.size()) {
					_Packs[packIndex].setVersionInfo(ver);
					_Packs[packIndex].SetInfoPublished();
				}
			}
			break;
			case CommandInformation::AlarmInfo: {
				try {
					uint16_t INFO = toShort(index, v);
					uint16_t packNumber = INFO & 0x00FF;
					
					JsonArray cells = _root.createNestedArray("Cells");
					logi("GetAlarm: Pack: %d", packNumber);
					// char key[16];
					uint16_t numberOfCells = v[index++];
					logd("number of cells: %d", numberOfCells );
					for (int i = 0; i < numberOfCells; i++) {
						// sprintf(key, "Cell_%d", i+1);
						JsonObject cell = cells.createNestedObject();
						cell["State"] = v[index++];
						cell["Num"] = i+1;	// used as a tag
					}
					JsonArray temps = _root.createNestedArray("Temps");
					uint16_t numberOfTemps = v[index++];
					for (int i = 0; i < numberOfTemps; i++) {
						if ( i < _TempKeys.size()) {
							JsonObject entry = temps.createNestedObject();
							entry["Name"] = _TempKeys[i];	// used as a tag
							entry["State"] = v[index++]; 
						}
					}
					index++; //skip 65
					JsonObject entry = _root.createNestedObject("PackCurrent");
					entry["State"] = v[index++];
					entry = _root.createNestedObject("PackVoltage");
					entry["State"] = v[index++];
					uint8_t ProtectSts1 = v[index++];
					uint8_t ProtectSts2 = v[index++];
					uint8_t SystemSts = v[index++];
					uint8_t FaultSts = v[index++];
					index++; //skip 81
					index++; //skip 83
					uint8_t AlarmSts1 = v[index++];
					uint8_t AlarmSts2 = v[index++];

					JsonObject pso = _root.createNestedObject("Protect_Status");
					pso["Charger_OVP"] = CheckBit(ProtectSts1, 7);
					pso["SCP"] = CheckBit(ProtectSts1, 6);
					pso["DSG_OCP"] = CheckBit(ProtectSts1, 5);
					pso["CHG_OCP"] = CheckBit(ProtectSts1, 4);
					pso["Pack_UVP"] = CheckBit(ProtectSts1, 3);
					pso["Pack_OVP"] = CheckBit(ProtectSts1, 2);
					pso["Cell_UVP"] = CheckBit(ProtectSts1, 1);
					pso["Cell_OVP"] = CheckBit(ProtectSts1, 0);
					pso["ENV_UTP"] = CheckBit(ProtectSts2, 6);
					pso["ENV_OTP"] = CheckBit(ProtectSts2, 5);
					pso["MOS_OTP"] = CheckBit(ProtectSts2, 4);
					pso["DSG_UTP"] = CheckBit(ProtectSts2, 3);
					pso["CHG_UTP"] = CheckBit(ProtectSts2, 2);
					pso["DSG_OTP"] = CheckBit(ProtectSts2, 1);
					pso["CHG_OTP"] = CheckBit(ProtectSts2, 0);

					JsonObject sso = _root.createNestedObject("System_Status");
					sso["Fully_Charged"] = CheckBit(ProtectSts2, 7);
					sso["Heater"] = CheckBit(SystemSts, 7);
					sso["AC_in"] = CheckBit(SystemSts, 5);
					sso["Discharge_MOS"] = CheckBit(SystemSts, 2);
					sso["Charge_MOS"] = CheckBit(SystemSts, 1);
					sso["Charge_Limit"] = CheckBit(SystemSts, 0);
					
					JsonObject fso = _root.createNestedObject("Fault_Status");
					fso["Heater_Fault"] = CheckBit(FaultSts, 7);
					fso["CCB_Fault"] = CheckBit(FaultSts, 6);
					fso["Sampling_Fault"] = CheckBit(FaultSts, 5);
					fso["Cell_Fault"] = CheckBit(FaultSts, 4);
					fso["NTC_Fault"] = CheckBit(FaultSts, 2);
					fso["DSG_MOS_Fault"] = CheckBit(FaultSts, 1);
					fso["CHG_MOS_Fault"] = CheckBit(FaultSts, 0);

					JsonObject aso = _root.createNestedObject("Alarm_Status");
					aso["DSG_OC"] = CheckBit(AlarmSts1, 5);
					aso["CHG_OC"] = CheckBit(AlarmSts1, 4);
					aso["Pack_UV"] = CheckBit(AlarmSts1, 3);
					aso["Pack_OV"] = CheckBit(AlarmSts1, 2);
					aso["Cell_UV"] = CheckBit(AlarmSts1, 1);
					aso["Cell_OV"] = CheckBit(AlarmSts1, 0);

					aso["SOC_Low"] = CheckBit(AlarmSts2, 7);
					aso["MOS_OT"] = CheckBit(AlarmSts2, 6);
					aso["ENV_UT"] = CheckBit(AlarmSts2, 5);
					aso["ENV_OT"] = CheckBit(AlarmSts2, 4);
					aso["DSG_UT"] = CheckBit(AlarmSts2, 3);
					aso["CHG_UT"] = CheckBit(AlarmSts2, 2);
					aso["DSG_OT"] = CheckBit(AlarmSts2, 1);
					aso["CHG_OT"] = CheckBit(AlarmSts2, 0);

					publish("alarms/Pack%d", ADR, false);
				} catch (const std::exception& ex) {
					_root["exception"] = ex.what();
					publish("exception/Pack%d", ADR, false);
				}
			}
			break;
			case CommandInformation::GetBarCode: {
				std::string bc;
				std::string s(v.begin(), v.end()-2);
				bc = s.substr(index);
				logi("GetBarCode for %d bc: %s", ADR, bc.c_str());
				_root["BarCode"] = bc.substr(0, 15);
				publish("info/Pack%d", ADR, true);
				int packIndex = ADR - 1;
				if (packIndex < _Packs.size()) {
					_Packs[packIndex].setBarcode(bc);
				}
			}
			break;
			case CommandInformation::GetPackCount:
			{
                _numberOfPacks = v[index];
                _numberOfPacks > 8 ? 1 : _numberOfPacks; // max 8, default to 1
				_root.clear();
				logi("GetPackCount: %d", _numberOfPacks);
				for (int i = 0; i < _numberOfPacks; i++) {
					char packName[STR_LEN];
					sprintf(packName, "Pack%d", i+1);
					_Packs.push_back(Pack(packName, &_TempKeys, _pcb));
				}
			}
			break;
		}
	}
	return 0;
}

} // namespace PylonToMQTT