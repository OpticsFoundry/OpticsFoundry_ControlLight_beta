// ControlLight.cpp : Defines the entry point for the application.
//

//#include "std.h"



#include "ControlAPI.h"
#include <iostream>


using namespace std;

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#define new DEBUG_NEW
#endif


void ControlMessageBox(const CString& lpszText, UINT nType = MB_OK, UINT nIDHelp = 0) {
#ifdef _AFXDLL
	if (DisplayErrors) AfxMessageBox(_T(lpszText), nType, nIDHelp);
#else
	if (DisplayErrors) MessageBox(NULL, lpszText, L"Control DLL", nType);
#endif
}


//if you use Namespace CLA, you need to put CLA:: in front of all DLL function names; you can then remove the CLA_ prefix from all functions

bool LoadControlHardwareInterface() {
	bool ControlHardwareInterfaceLoadedSuccessfully = false;
	try {
		bool success = CLA_LoadFromJSONFile("D:\\Florian\\Firefly\\FireflyControl\\ControlLight\\ControlHardwareConfig.json");
		CLA_Initialize();
		if (CLA_IsReady()) {
			if (!success) {
				ControlMessageBox("Warning: Loading of hardware configuration file worked only partially.");
			}
			ControlHardwareInterfaceLoadedSuccessfully = true;
			//AddErrorMessageCString("Hardware configuration file loaded");
		}
		else {
			if (success) {
				ControlMessageBox("Warning: Hardware configuration file did not contain a sequencer with ID=0.");
			}
		}
	}
	catch (...) {
		ControlMessageBox("Error loading hardware configuration file");
	}	
	if (!ControlHardwareInterfaceLoadedSuccessfully) {
		CLA_Cleanup();
		CLA_Create(/* InitializeAfx*/ false, /* InitializeAfxSocket*/ false);
	}
	return ControlHardwareInterfaceLoadedSuccessfully;
}


//Network settings of the sequencers (needs sequencer firmware from 2026-11-27 on).
//By default (DHCP) a sequencer gets its IP address from a DHCP server; without DHCP server (e.g. connected directly to the PC) it uses after 3 s
//a link-local address 169.254.x.y, as a PC does. A fixed IP address is useful if the PC's network adapter has a fixed IP address.
void ShowSequencerNetworkSettings() {
	//Discovery works without initialized system and finds sequencers also if their IP address is in another subnet than the PC
	char result[16384];
	size_t length = sizeof(result);
	if (CLA_DiscoverSequencers(result, length, /*timeout_in_seconds*/ 1.0)) cout << "Sequencers found:" << endl << result;
	if (!CLA_IsReady()) return;
	char info[1024];
	length = sizeof(info);
	if (CLA_GetSequencerNetworkSettings(/*SequencerNr*/ 0, info, length)) cout << "Network settings of sequencer 0: " << info << endl;
	//Changing the setting stores it in the sequencer and reboots it (about 10 s); afterwards enter its new IP address in ControlHardwareConfig.json:
	//CLA_SetSequencerFixedIP(/*SequencerNr*/ 0, "192.168.1.10", "255.255.255.0", /*Gateway, "" for none*/ "");
	//CLA_SetSequencerDHCP(/*SequencerNr*/ 0);
	//If a sequencer can't be reached with its IP address, set it by its MAC address (shown by the discovery above) with a UDP broadcast,
	//which works without initialized system and across subnets. Or hold the Z-turn USR button while switching it on, until it beeps twice: back to DHCP.
	//CLA_SetSequencerFixedIPByMAC("02:4F:46:4C:B5:52", "192.168.1.10", "255.255.255.0", "");
	//CLA_SetSequencerDHCPByMAC("02:4F:46:4C:B5:52");
}


int main() {
	if (!CLA_Create(/* InitializeAfx*/ true , /* InitializeAfxSocket*/ true)) {
		return 1; // Initialization failed
	}
	if (!LoadControlHardwareInterface()) {
		ControlMessageBox("Error loading hardware configuration file");
		
		CLA_AddDeviceSequencer(0, "OpticsFoundrySequencerV1", "192.168.0.109", 7, true, 0, 100, 10, false, true, true, true);
		CLA_AddDeviceAnalogOut16bit(0, 24, 4, true, -10, 10);
		CLA_AddDeviceAnalogOut16bit(0, 552, 4, true, -10, 10);
		CLA_AddDeviceDigitalOut(0, 1, 16);
		CLA_AddDeviceDigitalOut(0, 2, 16);
		CLA_AddDeviceAD9854(0, 232, 2, 300000000, 1, 1);
		CLA_AddDeviceAD9858(0, 652, 1200000000, 1);
		CLA_Initialize();
	}
	
	
	
	CLA_SwitchDebugMode(true, "TestSequencer");
	if (!CLA_IsReady()) {
		ControlMessageBox("Not all sequencers connected");
		ShowSequencerNetworkSettings();   //helps to find the right IP address for ControlHardwareConfig.json
		CLA_Cleanup();
		return -1;
	}
	ShowSequencerNetworkSettings();
	//test
	uint8_t* buffer = nullptr;
	for (int i = 0; i < 10; i++) {
		cout << "Iteration " << i << ": ";
		CLA_StartAssemblingSequence();

		//start data acquisition. This is an example for a command for which we didn't yet provide a convenience function in the DLL. 
		//In this somewhat convoluted manner one can achieve anything without API interface changes
		/*
		uint8_t ChannelNumber = 0;
		uint32_t NumberOfDataPoints = 1000;
		double DelayBetweenDataPoints_in_ms_in_ms = 0.02;
		CLA_SetValueSerialDevice(0, 0, 0, (uint8_t*)&ChannelNumber, 8);
		CLA_SetValueSerialDevice(0, 0, 1, (uint8_t*)&NumberOfDataPoints, 32);
		CLA_SetValueSerialDevice(0, 0, 2, (uint8_t*)&DelayBetweenDataPoints_in_ms_in_ms, 64);
		CLA_SetValueSerialDevice(0, 0, 3, (uint8_t*)&ChannelNumber, 8); //starts the acquisition
		*/

		//this is the same command using a convenience function
		CLA_SequencerStartAnalogInAcquisition(0, 0, 0, 0, 1000, 0.02);
		
		for (int j = 1; j < 100; j++) {
			CLA_SetVoltage(0, 24, 10.0*j/100.0);
			uint16_t data = 0xffff;
			CLA_SetValue(0, 1, 0, (uint8_t*)&data, 16);
			CLA_Wait_ms(0.1);
			data = 0;
			CLA_SetValue(0, 1, 0, (uint8_t*)&data, 16);
			CLA_Wait_ms(0.1);
			double Frequency = 1000.0 * j / 100.0;
			CLA_SetValue(0, 232, 0, (uint8_t*)&Frequency, 64);
		}
		CLA_Wait_ms(10);
		CLA_ExecuteSequence();
		bool running = false;
		unsigned long long DataPointsWritten = 0;
		CLA_GetSequenceExecutionStatus(running, DataPointsWritten);
		
		unsigned long buffer_length = 0;
		unsigned long EndTimeOfCycle = 0;
		CLA_WaitTillEndOfSequenceThenGetInputData(buffer, buffer_length, EndTimeOfCycle, 10);
		cout << "Buffer length: " << buffer_length << endl;

	}

	CLA_Cleanup();
	return 0;
}


//ToDo:
// Put test code into separate file to have clean header file to give to user
// Pull additional convenience commands for digital out and DDS devices from CDevice.h into the API
// Finish SetRegister and SetValue functions for DDS
//For Python and C++ APIs: throw exceptions instead of returning error codes
// For C: return error codes and messages correctly
//(optional, as we do take care of TimeDebt now): wait for startDelay tick counts to synchronize several sequencers

