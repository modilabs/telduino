/**
 *  \mainpage Telduino
 *
 *	\section Purpose
 *	This code runs on a custom avr board with hardware to control 20 relays and measurement circuits.
 *
 *	\section Implementation
 *  The code is a client to an embedded linux system that sends string commands over the serial port.
 *  These serial commands are executed by the telduino code and sent back to the linux box.
 */
 
#include <stdlib.h>
#include <errno.h>

#include <avr/io.h>
#include <avr/interrupt.h>

#include <string.h>
#include <stdint.h>

//Helper functions
#include "ReturnCode/returncode.h"
#include "prescaler.h"
#include "arduino/WProgram.h"

//Metering Hardware
#include "SPI/SPI.h"
#include "DbgTel/DbgTel.h"
#include "ADE7753/ADE7753.h"
#include "ShiftRegister/shiftregister.h"
#include "Demux/demux.h"
#include "Select/select.h"
#include "sd-reader/sd_raw.h"
#include "Switches/switches.h"

//Metering logic
#include "Circuit/circuit.h"
#include "Circuit/calibration.h"

#include "Strings/strings.h"

//definition of serial ports for debug, sheeva communication, and telit communication
#define debugPort Serial1
#define sheevaPort Serial2
#define telitPort Serial3

#define DEBUG_BAUD_RATE 9600
#define SHEEVA_BAUD_RATE 9600
#define TELIT_BAUD_RATE 115200
#define verbose 1
#define MAXLEN_PLUG_MESSAGE 127

boolean msgWaitLock = false;
Circuit ckts[NCIRCUITS];

/*  Disables the watchdog timer the first chance the AtMega gets as recommended
	by Atmel.
 */
#include <avr/wdt.h>
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void)
{
	MCUSR = 0;
	wdt_disable();
	return;     
}



int _testChannel = 20; //This is the input daughter board channel. The _ implies that it should only be changed by user input.


void setup();
void loop();
void softSetup();
void setupLVAMode(int icid, int32_t linecycVal);
void setupRVAMode(int icid);
void setupDefaultMode(int icid);
void jobReadLVA(int icid);
void jobReadRVA(int icid);
void displayChannelInfo(); 
void displayEnabled(const int8_t enabledC[WIDTH]);
int8_t getChannelID();
void testHardware();
void parseBerkeley();
void parseColumbia();
String getValueForKey(String key, String commandString);
void get_val(char *s, char *key, char *val);
String getSMSText(String commandString);
void meter(String commandString);
void meter_test(char *s);
void modem(String commandString);
void readSheevaPort();
void readTelitPort();
void chooseDestination(String destination, String commandString);
void turnOnTelit();

void setup()
{
	setClockPrescaler(CLOCK_PRESCALER_1);	//prescale of 2 after startup prescale of 8. This ensures that the arduino is running at 8 MHz.

	// start up serial ports
	debugPort.begin(DEBUG_BAUD_RATE);		//Debug serial
	telitPort.begin(TELIT_BAUD_RATE);		//Telit serial
	sheevaPort.begin(SHEEVA_BAUD_RATE);

	// write startup message to debug port
	debugPort.write("\r\n\r\ntelduino power up\r\n");
    debugPort.write("last compilation\r\n");
    debugPort.write(__DATE__);
    debugPort.write("\r\n");
    debugPort.write(__TIME__);
    debugPort.write("\r\n");

	turnOnTelit();				// set telit pin high

	pinMode(37, OUTPUT);		//Level shifters
	digitalWrite(37,HIGH);		//Level shifters
	initDbgTel();				//Blink leds
	SRinit();					//Shift registers
	initDemux();				//Muxers
	initSelect();				//Select Circuit
	sd_raw_init();				//SDCard
	SPI.begin();				//SPI

	SWallOff(); //switch all off.

	//Load circuit data from EEPROM
	uint8_t *addrEEPROM = 0;
	for (Circuit *c = ckts; c != &ckts[NCIRCUITS]+1; c++){
		Cload(c,addrEEPROM);
		addrEEPROM += sizeof(Circuit);
	}

} //end of setup section


void loop()
{	
	// parseBerkeley();
	parseColumbia();
} //end of main loop

/**
 *	single character serial interface for interaction with telduino
 */
void parseBerkeley() 
{
	setDbgLeds(GYRPAT);
	debugPort.print(_testChannel,DEC);
	debugPort.print(" $");
	while (debugPort.available() == 0);
	debugPort.println();
	/**
	if (_testChannel > 10) {
		debugPort.print("\b");
	}
	debugPort.print("\b\b");
	*/

	// Look for incoming single character command on debugPort line
	// Capital letters denote write operations and lower case letters are reads
	if (debugPort.available() > 0) {
		char incoming = debugPort.read(); 
		if (incoming == 'A') {			//Write to ADE Register
			char buff[16] = {0};
			debugPort.print("Register to write $");
			CLgetString(&debugPort,buff,sizeof(buff));
			debugPort.println();
			
			int32_t regData = 0;
			for (int i=0; i < regListSize/sizeof(regList[0]); i++) {
				if (strcmp(regList[i]->name,buff) == 0){
					CSselectDevice(_testChannel);
					debugPort.print("Current regData:");
					ADEgetRegister(*regList[i],&regData);
					debugPort.print(RCstr(_retCode));
					debugPort.print(":0x");
					debugPort.print(regData,HEX);
					debugPort.print(":");
					debugPort.println(regData,BIN);

					debugPort.print("Enter new regData:");
					if(CLgetInt(&debugPort,&regData) == CANCELED) break;	
					debugPort.println();
					ADEsetRegister(*regList[i],&regData);
					debugPort.print(RCstr(_retCode));
					debugPort.print(":0x");
					debugPort.print(regData,HEX);
					debugPort.print(":");
					debugPort.println(regData,DEC);
					CSselectDevice(DEVDISABLE);
					break;
				} 
			}
		} else if (incoming == 'a') {		//Read ADE reg
			char buff[16] = {0};
			debugPort.print("Enter name of register to read:");
			CLgetString(&debugPort,buff,sizeof(buff));
			debugPort.println();

			//debugPort.print("(int32_t)(&),HEX:");
			//debugPort.println((int32_t)(&WAVEFORM),HEX);
			int32_t regData = 0;
			for (int i=0; i < regListSize/sizeof(regList[0]); i++) {
				if (strcmp(regList[i]->name,buff) == 0){
					CSselectDevice(_testChannel);
					ADEgetRegister(*regList[i],&regData);
					debugPort.print("regData:");
					debugPort.print(RCstr(_retCode));
					debugPort.print(":0x");
					debugPort.print(regData,HEX);
					debugPort.print(":");
					debugPort.println(regData,DEC);
					CSselectDevice(DEVDISABLE);
					break;
				} 
			}
		} else if (incoming == 'x') {
			CSselectDevice(_testChannel);
			CLwaitForZX10VIRMS();
			CSselectDevice(DEVDISABLE);
		} else if (incoming == 'C') {		//Change active channel
			_testChannel = getChannelID();	
		} else if (incoming == 'S') {		//Toggle channel circuit
			int8_t ID = getChannelID();		
			SWset(ID,!SWisOn(ID));
		} else if (incoming == 's') {		//Display switch state
			displayEnabled(SWgetSwitchState());	
		} else if (incoming == 'T') {		//Test basic functionality
			testHardware();
		} else if (incoming == 'R') {		//Hard Reset using watchdog timer
			wdt_enable((WDTO_4S));			
			Serial1.println("resetting in 4s.");
		} else if (incoming == 'O') {		//soft Reset using the Setup routine
			softSetup();					//Set calibration values for ADE
		} else if (incoming == 'o') {		//Read channel using Achintya's code
			displayChannelInfo();
		} else if (incoming == 'P') {		//Program values in ckts[] to ADE
			for (int i = 0; i < NCIRCUITS; i++) {
				Circuit *c = &(ckts[i]);
				Cprogram(c);
				debugPort.println(RCstr(_retCode));
				debugPort.println("*****");
				ifnsuccess(_retCode) {
					break;
				}
			}
		} else if(incoming == 'p') {		//Measure circuit values and print
			Circuit *c = &(ckts[_testChannel]);
			Cmeasure(c);
			CprintMeas(&debugPort,c);
			debugPort.println(RCstr(_retCode));
			Cprint(&debugPort,c);
			debugPort.println();
		} else if (incoming == 'L') {		//Run calibration routine on channel
			Circuit *c = &(ckts[_testChannel]);
			calibrateCircuit(c);
			//debugPort.println(RCstr(_retCode));
		} else if (incoming == 'D') {		//Initialize ckts[] to safe defaults
			for (int i = 0; i < NCIRCUITS; i++) {
				Circuit *c = &(ckts[i]);
				CsetDefaults(c,i);
			}
			debugPort.println("Defaults set. Don't forget to program! ('P')");
		} else if (incoming == 'E') {		//Save data in ckts[] to EEPROM
			debugPort.println("Saving to EEPROM.");
			uint8_t *addrEEPROM = 0;
			for (Circuit *c = ckts; c != &ckts[NCIRCUITS]+1; c++){
				Csave(c,addrEEPROM);
				addrEEPROM += sizeof(Circuit);
			}
			debugPort.println(COMPLETESTR);
		} else if (incoming=='e'){			//Load circuit data from EEPROM
			uint8_t *addrEEPROM = 0;
			debugPort.println("Loading from EEPROM.");
			for (Circuit *c = ckts; c != &ckts[NCIRCUITS]+1; c++){
				Cload(c,addrEEPROM);
				addrEEPROM += sizeof(Circuit);
			}
			debugPort.println(COMPLETESTR);
		} else if (incoming=='w') {
			int32_t mask = 0;
			debugPort.print("Enter interrupt mask. Will wait for 4sec. $");
			CLgetInt(&debugPort,&mask);
			debugPort.println();
			//debugPort.print("(int32_t)(&),HEX:");
			//debugPort.println((int32_t)(&WAVEFORM),HEX);
			CSselectDevice(_testChannel);
			ADEwaitForInterrupt((int16_t)mask,4000);
			debugPort.println(RCstr(_retCode));
			CSselectDevice(DEVDISABLE);
		} else if (incoming == 'W')	 {
			CSselectDevice(_testChannel);
			int32_t regData;
			for (int i =0; i < 80; i++) {
				ADEgetRegister(WAVEFORM,&regData);
				debugPort.print(regData);
				debugPort.print(" ");
			}
			CSselectDevice(DEVDISABLE);
		}
		else {								//Indicate received character
			int waiting = 2048;				//Used to eat up junk that follows
			debugPort.print("\n\rNot_Recognized:");
			debugPort.print(incoming,BIN);
			debugPort.print(":'");
			debugPort.print(incoming);
			debugPort.println("'");
			while (debugPort.available() || waiting > 0) {
				if (debugPort.available()) {
					incoming = debugPort.read();
					debugPort.print("\n\rNot_Recognized:");
					debugPort.print(incoming,BIN);
					debugPort.print(":'");
					debugPort.print(incoming);
					debugPort.println("'");
				} else 	waiting--;
			}
		}
	}


	setDbgLeds(0);
}

void softSetup()
//resets the test channel (input daughter board) to default parameters and sets the linecycle count up.
{
	int32_t data = 0;

	debugPort.print("\n\n\rSetting Channel:");
	debugPort.println(_testChannel,DEC);
	
	CSselectDevice(_testChannel); //start SPI comm with the test device channel

	//Enable Digital Integrator for _testChannel
	int8_t ch1os=0,enableBit=1;
	debugPort.print("set CH1OS:");
	ADEsetCHXOS(1,&enableBit,&ch1os);
	debugPort.println(RCstr(_retCode));
	debugPort.print("get CH1OS:");
	ADEgetCHXOS(1,&enableBit,&ch1os);
	debugPort.println(RCstr(_retCode));
	debugPort.print("enabled: ");
	debugPort.println(enableBit,BIN);
	debugPort.print("offset: ");
	debugPort.println(ch1os);

	//set the gain to 16 for channel _testChannel since the sensitivity appears to be 0.02157 V/Amp
	int32_t gainVal = 0x4;
	debugPort.print("BIN GAIN (set,get):");
	ADEsetRegister(GAIN,&gainVal);
	debugPort.print(RCstr(_retCode));
	debugPort.print(",");
	ADEgetRegister(GAIN,&gainVal);
	debugPort.print(RCstr(_retCode));
	debugPort.print(":");
	debugPort.println(gainVal,BIN);
	
	//NOTE*****  I am using zeros right now because we are going to up the gain and see if this is the same
	//Set the IRMSOS to 0d444 or 0x01BC. This is the measured offset value.
	int32_t iRmsOsVal = 0x0;//0x01BC;
	ADEsetRegister(IRMSOS,&iRmsOsVal);
	ADEgetRegister(IRMSOS,&iRmsOsVal);
	debugPort.print("hex IRMSOS:");
	debugPort.println(iRmsOsVal, HEX);
	
	//Set the VRMSOS to -0d549. This is the measured offset value.
	int32_t vRmsOsVal = 0x0;//0x07FF;//F800
	ADEsetRegister(VRMSOS,&vRmsOsVal);
	ADEgetRegister(VRMSOS,&vRmsOsVal);
	debugPort.print("hex VRMSOS read from register:");
	debugPort.println(vRmsOsVal, HEX);
	
	//set the number of cycles to wait before taking a reading
	int32_t linecycVal = 200;
	ADEsetRegister(LINECYC,&linecycVal);
	ADEgetRegister(LINECYC,&linecycVal);
	debugPort.print("int linecycVal:");
	debugPort.println(linecycVal);
	
	//read and set the CYCMODE bit on the MODE register
	int32_t modeReg = 0;
	ADEgetRegister(MODE,&modeReg);
	debugPort.print("bin MODE register before setting CYCMODE:");
	debugPort.println(modeReg, BIN);
	modeReg |= CYCMODE;	 //set the line cycle accumulation mode bit
	ADEsetRegister(MODE,&modeReg);
	ADEgetRegister(MODE,&modeReg);
	debugPort.print("bin MODE register after setting CYCMODE:");
	debugPort.println(modeReg, BIN);
	
	//reset the Interrupt status register
	ADEgetRegister(RSTSTATUS, &data);
	debugPort.print("bin Interrupt Status Register:");
	debugPort.println(data, BIN);

	CSselectDevice(DEVDISABLE); //end SPI comm with the selected device	
}

void displayChannelInfo() {
	int32_t val;
	uint32_t iRMS = 0;
	uint32_t vRMS = 0;
	uint32_t lineAccAppEnergy = 0;
	uint32_t lineAccActiveEnergy = 0;
	int32_t interruptStatus = 0;
	uint32_t iRMSSlope = 164;
	uint32_t vRMSSlope = 4700;
	uint32_t appEnergyDiv = 5;
	uint32_t energyJoules = 0;

	//Select the Device
	CSselectDevice(_testChannel);
	
	//Read and clear the Interrupt Status Register
	ADEgetRegister(RSTSTATUS, &interruptStatus);
	
	if (0 /*loopCounter%4096*/ ){
		debugPort.print("bin Interrupt Status Register:");
		debugPort.println(interruptStatus, BIN);
	}	//endif
	
	//if the CYCEND bit of the Interrupt Status Registers is flagged
	debugPort.print("\n\n\r");
	debugPort.print("Waiting for next cycle: ");
	ADEwaitForInterrupt(CYCEND,4000);
	debugPort.println(RCstr(_retCode));

	ifsuccess(_retCode) {
		setDbgLeds(GYRPAT);

		debugPort.print("_testChannel:");
		debugPort.println(_testChannel,DEC);

		debugPort.print("bin Interrupt Status Register:");
		debugPort.println(interruptStatus, BIN);
		
		//IRMS SECTION
		debugPort.print("IRMS:");
		ADEgetRegister(IRMS,&val);
		debugPort.println( RCstr(_retCode) );
		debugPort.print("Counts:");
		debugPort.println(val);
		debugPort.print("mAmps:");
		iRMS = val/iRMSSlope;//data*1000/40172/4;
		debugPort.println(iRMS);
		
		//VRMS SECTION
		debugPort.print("VRMS:");
		ADEgetRegister(VRMS,&val);
		debugPort.println(RCstr(_retCode));
		debugPort.print("Counts:");
		debugPort.println(val);
		vRMS = val/vRMSSlope; //old value:9142
		debugPort.print("Volts:");
		debugPort.println(vRMS);

		
		//APPARENT ENERGY SECTION
		ADEgetRegister(LVAENERGY,&val);
		debugPort.print("int Line Cycle Apparent Energy after 200 half-cycles:");
		debugPort.println(val);
		energyJoules = val*2014/10000;
		debugPort.print("Apparent Energy in Joules over the past 2 seconds:");
		debugPort.println(energyJoules);
		debugPort.print("Calculated apparent power usage:");
		debugPort.println(energyJoules/2);
		
		//ACTIVE ENERGY SECTION
		ADEgetRegister(LAENERGY,&val);
		ifsuccess(_retCode) {
			debugPort.print("int Line Cycle Active Energy after 200 half-cycles:");
			debugPort.println(val);
		} else {
			debugPort.println("Line Cycle Active Energy read failed.");
		}// end ifsuccess
	} //end ifsuccess

	CSselectDevice(DEVDISABLE);
}

int8_t getChannelID() 
{
	int32_t ID = -1;
	while (ID == -1) {
		debugPort.print("Waiting for ID (0-20):");		
		ifnsuccess(CLgetInt(&debugPort,&ID)) ID = -1;
		debugPort.println();
		if (ID < 0 || 20 < ID ) {
			debugPort.print("Incorrect ID:");
			debugPort.println(ID,DEC);
			ID = -1;
		} else {
			debugPort.println((int8_t)ID,DEC);
		}
	}
	return (int8_t)ID;
}

void testHardware() {
	int8_t enabledC[WIDTH] = {0};
	int32_t val;

	debugPort.print("\n\rTest switches\n\r");
	//Shut off/on all circuits
	for (int i =0; i < 1; i++){
		SWallOn();
		delay(50);
		SWallOff();
		delay(50);
	}
	//Start turning each switch on with 1 second in between
	for (int i = 0; i < WIDTH; i++) {
		enabledC[i] = 1;
		delay(1000);
		SWsetSwitches(enabledC);
	}
	delay(1000);
	SWallOff();

	//Test communications with each ADE
	for (int i = 0; i < 21; i++) {
		CSselectDevice(i);
		
		debugPort.print("Can communicate with channel ");
		debugPort.print(i,DEC);
		debugPort.print(": ");

		ADEgetRegister(DIEREV,&val);
		ifnsuccess(_retCode) {
			debugPort.print("NO-");
			debugPort.println(RCstr(_retCode));
		} else {
			debugPort.print("YES-DIEREV:");
			debugPort.println(val,DEC);
		}
		CSselectDevice(DEVDISABLE);
	}
}
	
/** 
  Lists the state of the circuit switches.
  */
void displayEnabled(const int8_t enabledC[WIDTH])
{
	debugPort.println("Enabled Channels:");
	for (int i =0; i < WIDTH; i++) {
		debugPort.print(i);
		debugPort.print(":");
		debugPort.print(enabledC[i],DEC);
		if (i%4 == 3) {
			debugPort.println();
		} else {
			debugPort.print('\t');
		}
	}
	debugPort.println();
}

//JR needed to make compiler happy
extern "C" {
void __cxa_pure_virtual(void) 
{
	while(1) {
		setDbgLeds(RPAT);
		delay(332);
		setDbgLeds(YPAT);
		delay(332);
		setDbgLeds(GPAT);
		delay(332);
	}
}
}

/**
 *	key-value parsing interface for telduino-sheeva communications
 */
void parseColumbia() {
    if (verbose > 1) {
        debugPort.println("top of loop()");
        debugPort.println(millis());
    }
	
    readSheevaPort();
    readTelitPort();
}

void setupLVAMode(int icid, int32_t linecycVal) {
	/**
	 *	this function will setup the registers correctly for the LINECYC
	 *	mode reading of the LVAENERGY register.
	 *	The value for LINECYC is passed in to the function.
	 */
	
	int32_t data = 0;
	
	debugPort.print("Setting Channel for LVA Mode:");
	debugPort.println(icid, DEC);
	
	CSselectDevice(icid); //start SPI comm with the test device channel
								  //Enable Digital Integrator for _testChannel
	int8_t ch1os=0,enableBit=1;
	
	debugPort.print("set CH1OS:"); 
	ADEsetCHXOS(1,&enableBit,&ch1os);
	debugPort.println(RCstr(_retCode));
	debugPort.print("get CH1OS:"); 
	ADEgetCHXOS(1,&enableBit,&ch1os);
	debugPort.println(RCstr(_retCode)); 
	debugPort.print("enabled: ");
	debugPort.println(enableBit,BIN);
	debugPort.print("offset: ");
	debugPort.println(ch1os);
	
	//set the gain to 2 for channel _testChannel since the sensitivity appears to be 0.02157 V/Amp
	int32_t gainVal = 1;
	
	debugPort.print("BIN GAIN (set,get):"); 
	ADEsetRegister(GAIN,&gainVal);
	debugPort.println(RCstr(_retCode));
	debugPort.print(",");
	ADEgetRegister(GAIN,&gainVal);
	debugPort.print(RCstr(_retCode));
	debugPort.print(":");
	debugPort.println(gainVal,BIN);
	
	int32_t iRmsOsVal = 0x0000;
	ADEsetRegister(IRMSOS,&iRmsOsVal);
	ADEgetRegister(IRMSOS,&iRmsOsVal);
	debugPort.print("hex IRMSOS:");
	debugPort.println(iRmsOsVal, HEX);
	
	int32_t vRmsOsVal = 0x0000;
	ADEsetRegister(VRMSOS,&vRmsOsVal);
	ADEgetRegister(VRMSOS,&vRmsOsVal);
	debugPort.print("hex VRMSOS read from register:");
	debugPort.println(vRmsOsVal, HEX);
	
	//set the number of cycles to wait before taking a reading
	// int32_t linecycVal = 200;
	ADEsetRegister(LINECYC,&linecycVal);
	ADEgetRegister(LINECYC,&linecycVal);
	debugPort.print("int linecycVal:");
	debugPort.println(linecycVal);
	
	//read and set the CYCMODE bit on the MODE register
	int32_t modeReg = 0;
	ADEgetRegister(MODE,&modeReg);
	debugPort.print("bin MODE register before setting CYCMODE:");
	debugPort.println(modeReg, BIN);
	modeReg |= CYCMODE;	 //set the line cycle accumulation mode bit
	ADEsetRegister(MODE,&modeReg);
	ADEgetRegister(MODE,&modeReg);
	debugPort.print("bin MODE register after setting CYCMODE:");
	debugPort.println(modeReg, BIN);
	
	//reset the Interrupt status register
	ADEgetRegister(RSTSTATUS, &data);
	debugPort.print("bin Interrupt Status Register:");
	debugPort.println(data, BIN);
	
	CSselectDevice(DEVDISABLE); //end SPI comm with the selected device	
	
}

void setupDefaultMode(int icid) {
	int32_t modeReg = 0;
	CSselectDevice(icid); 
	
	// read bits before write
	ADEgetRegister(MODE,&modeReg);
	debugPort.print("MODE register before setting default:");
	debugPort.println(modeReg, BIN);
	
	// set all MODE bits to default
	modeReg &= ~DISHPF;
	modeReg &= ~DISHPF2; 
	modeReg |=  DISCF;
	modeReg |=  DISSAG; 
	modeReg &= ~ASUSPEND;
	modeReg &= ~TEMPSEL;
	modeReg &= ~SWRST;
	modeReg &= ~CYCMODE;	
	modeReg &= ~DISCH1;
	modeReg &= ~DISCH2;	
	modeReg &= ~SWAP;
	modeReg &= ~DTRT_0;	
	modeReg &= ~DTRT1_;	
	modeReg &= ~WAVESEL_0;
	modeReg &= ~WAVESEL1_;

	// verify bits
	ADEsetRegister(MODE,&modeReg);
	ADEgetRegister(MODE,&modeReg);
	debugPort.print("MODE register after setting default:");
	debugPort.println(modeReg, BIN);
	
	CSselectDevice(DEVDISABLE);
}

void setupRVAMode(int icid) {
	/**
	 *	this function aspires to setup the registers correctly for the accumulation
	 *	mode reading of the RVAENERGY register.
	 */
	int32_t data = 0;
	// debugging value
	
	debugPort.print("\n\n\rSetting Accumulation Mode for Channel:");
	debugPort.println(icid, DEC);
	
	CSselectDevice(icid); //start SPI comm with the test device channel
								  //Enable Digital Integrator for _testChannel
	int8_t ch1os=0,enableBit=1;
	
	debugPort.print("set CH1OS:");
	ADEsetCHXOS(1,&enableBit,&ch1os);
	debugPort.println(RCstr(_retCode));
	debugPort.print("get CH1OS:");
	ADEgetCHXOS(1,&enableBit,&ch1os);
	debugPort.println(RCstr(_retCode));
	debugPort.print("enabled: ");
	debugPort.println(enableBit,BIN);
	debugPort.print("offset: ");
	debugPort.println(ch1os);
	
	//set the gain to 2 for channel _testChannel since the sensitivity appears to be 0.02157 V/Amp
	int32_t gainVal = 0;
	
	debugPort.print("BIN GAIN (set,get):");
	ADEsetRegister(GAIN,&gainVal);
	debugPort.print(RCstr(_retCode));
	debugPort.print(",");
	ADEgetRegister(GAIN,&gainVal);
	debugPort.print(RCstr(_retCode));
	debugPort.print(":");
	debugPort.println(gainVal,BIN);
	
	int32_t iRmsOsVal = 0x0000;
	ADEsetRegister(IRMSOS,&iRmsOsVal);
	ADEgetRegister(IRMSOS,&iRmsOsVal);
	debugPort.print("hex IRMSOS:");
	debugPort.println(iRmsOsVal, HEX);
	
	int32_t vRmsOsVal = 0x0000;
	ADEsetRegister(VRMSOS,&vRmsOsVal);
	ADEgetRegister(VRMSOS,&vRmsOsVal);
	debugPort.print("hex VRMSOS read from register:");
	debugPort.println(vRmsOsVal, HEX);
	
	// set bits in interrupt register
	int32_t modeReg = 0;
	ADEgetRegister(IRQEN, &modeReg);
	modeReg |= WSMP;
	ADEsetRegister(IRQEN, &modeReg);
	debugPort.println("register read IRQEN");
	debugPort.println(modeReg, BIN);
	
	// set appropriate bits in MODE register
	// clear CYCMODE, WAVESEL_0, and WAVESEL1_
	modeReg = 0;
	ADEgetRegister(MODE, &modeReg);
	debugPort.print("register read MODE");
	debugPort.println(modeReg, BIN);
	debugPort.println("setting bits");
	modeReg &= ~CYCMODE;			//clear the line cycle accumulation mode bit
	modeReg &= ~WAVESEL_0;
	modeReg &= ~WAVESEL1_;
	ADEsetRegister(MODE, &modeReg);
	ADEgetRegister(MODE, &modeReg);
	debugPort.print("register read MODE");
	debugPort.println(modeReg, BIN);
	
	CSselectDevice(DEVDISABLE);		//end SPI comm with the selected device		
}

void jobReadLVA(int icid) {
	/**	dispatch job readLVA function
	 *  power should be returned in units of watt-hours
	 *	time will be returned in units of milliseconds
	 *	icid - circuit id
	 *
	 *	 input string: cmp=mtr&job=readLVA&cid=<cid>;
	 *	output string: cmp=mtr&job=readLVA&cid=<cid>&power=<power>&time=<time>;
	 */
	
	debugPort.println("reading circuit energy LVA");
	// actually do something here soon
	// read circuit energy or something using icid
	int32_t regVal = 0;
	
	// select SPI circuit
	CSselectDevice(icid);
	
	// read current
	int32_t irms = 0;
	ADEgetRegister(IRMS, &irms);
	debugPort.println("reg read IRMS");
	debugPort.println(irms, HEX);
	
	// read voltage
	int32_t vrms = 0;
	ADEgetRegister(VRMS, &vrms);
	debugPort.println("reg read VRMS");
	debugPort.println(vrms, HEX);
	
	//if the CYCEND bit of the Interrupt Status Registers is flagged
	debugPort.print("Waiting for next cycle: ");
	ADEwaitForInterrupt(CYCEND, 90000);
	debugPort.println(RCstr(_retCode));
	
	// test read of interrupt register
	ADEgetRegister(RSTSTATUS, &regVal);
	debugPort.println("reg read RSTSTATUS");
	debugPort.println(regVal, BIN);
	
	// read LVAENERGY value into power
	int32_t power = 0;
	ADEgetRegister(LVAENERGY, &power);
	debugPort.println("reg read LVAENERGY");
	debugPort.println(power, HEX);

	// test read of interrupt register
	ADEgetRegister(RSTSTATUS, &regVal);
	debugPort.println("reg read RSTSTATUS");
	debugPort.println(regVal, BIN);
	
	// deselect SPI circuit
	CSselectDevice(DEVDISABLE);
	
	// construct and send back response
	String responseString = "";
	responseString += "cmp=mtr&";
	responseString += "job=readLVA&";
	responseString += "cid=";
	responseString += icid;
	responseString += "&irms=";
	responseString += irms;
	responseString += "&vrms=";
	responseString += vrms;	
	responseString += "&power=";
	responseString += power;
	responseString += "&time=";
	responseString += millis();
	responseString += ";";
	sheevaPort.println(responseString);		
}

void jobReadRVA(int icid) {
	/**	dispatch job read function
	 *  power should be returned in units of watt-hours
	 *	time will be returned in units of milliseconds
	 *	icid - circuit id
	 *
	 *	 input string: cmp=mtr&job=read&cid=<cid>;
	 *	output string: cmp=mtr&job=read&cid=<cid>&power=<power>&time=<time>;
	 */
	
	debugPort.println("reading circuit energy");
	// actually do something here soon
	// read circuit energy or something using icid
	int32_t regVal = 0;
	
	// select SPI circuit
	CSselectDevice(icid);
	
	// read current
	int32_t irms = 0;
	ADEgetRegister(IRMS, &irms);
	debugPort.println("reg read IRMS");
	debugPort.println(irms, HEX);
	
	// read voltage
	int32_t vrms = 0;
	ADEgetRegister(VRMS, &vrms);
	debugPort.println("reg read VRMS");
	debugPort.println(vrms, HEX);
	
	//read AENERGY
	ADEgetRegister(AENERGY, &regVal);
	debugPort.println("reg read AENERGY");
	debugPort.println(regVal, HEX);
	
	// read VAENERGY
	ADEgetRegister(VAENERGY, &regVal);
	debugPort.println("reg read VAENERGY");
	debugPort.println(regVal, HEX);
	
	// read RVAENERGY value into power
	int32_t power = 0;
	ADEgetRegister(RVAENERGY, &power);
	debugPort.println("reg read RVAENERGY");
	debugPort.println(power, HEX);
		
	// deselect SPI circuit
	CSselectDevice(DEVDISABLE);
	
	// construct and send back response
	String responseString = "";
	responseString += "cmp=mtr&";
	responseString += "job=readRVA&";
	responseString += "cid=";
	responseString += icid;
	responseString += "&power=";
	responseString += power;
	responseString += "&irms=";
	responseString += irms;
	responseString += "&vrms=";
	responseString += vrms;
	responseString += "&time=";
	responseString += millis();
	responseString += ";";
	sheevaPort.println(responseString);	
}

String getValueForKey(String key, String commandString) {
	/**
	 *	given a String for the key value, this function returns the String corresponding
	 *	to the value for the key by reading until the next '&' or the end of the string.
	 */
    int keyIndex = commandString.indexOf(key);
    int valIndex = keyIndex + key.length() + 1;
    int ampersandIndex = commandString.indexOf("&",valIndex);
    // if ampersand not found, go until end of string
    if (ampersandIndex == -1) {
        ampersandIndex = commandString.length();
    }
    String val = commandString.substring(valIndex, ampersandIndex);
    return val;
}

String getSMSText(String commandString) {
	/**
	 *	this function is called when a cmp=mdm string is sent to the telduino.  the text 
	 *	surrounded by parenthesis is returned.  this message will be sent to the modem as
	 *	a raw string command.  
	 */
    int firstDelimiterIndex = commandString.indexOf('(');
    int secondDelimiterIndex = commandString.indexOf(')', firstDelimiterIndex + 1);
    String smsText = commandString.substring(firstDelimiterIndex + 1, secondDelimiterIndex);
    return smsText;
}

void meter(String commandString) {
	/**
	 *	this function takes care of parsing commands where cmp=mtr.
	 */
    String job = getValueForKey("job", commandString);
    String cid = getValueForKey("cid", commandString);
		
	// is there a better way to convert the cid string to int?
	char cidChar[3];
	cid.toCharArray(cidChar, 3);
	int icid = atoi(cidChar);
	
    if (verbose > 0) {
        debugPort.println();
        debugPort.println("entered void meter()");
        debugPort.print("executing job type - ");
        debugPort.println(job);
        debugPort.print("on circuit id - ");
        debugPort.println(cid);
        debugPort.println();
    }
    
	if (job == "con") {
		debugPort.println("execute con job");
		SWset(icid,1);
		debugPort.print("switch ");
		debugPort.print(icid, DEC);
		if (SWisOn(icid)) {
			debugPort.println(" is on");
		} else {
			debugPort.println(" is off");
		}
	}
	else if (job == "coff") {
		debugPort.println("execute coff job");
		SWset(icid,0);
		debugPort.print("switch ");
		debugPort.print(icid, DEC);
		if (SWisOn(icid)) {
			debugPort.println(" is on");
		} else {
			debugPort.println(" is off");
		}
	}
	else if (job == "readRVA") {
		jobReadRVA(icid);
	}
	else if (job == "readLVA") {
		jobReadLVA(icid);
	}
	else if (job == "modeRVA") {
		setupRVAMode(icid);
	}
	else if (job == "modeLVA") {
		int32_t linecycVal = 1000;
		String linecyc = getValueForKey("linecyc", commandString);
		char linecycChar[8];
		linecyc.toCharArray(linecycChar, 8);
		linecycVal = atoi(linecycChar);
		setupLVAMode(icid, linecycVal);
	}
	else if (job == "modeDefault") {
		setupDefaultMode(icid);
	}
	else if (job == "c") {
		_testChannel = icid;
		displayChannelInfo();		
	}
	else if (job == "T") {
		testHardware();
	}
	else if (job == "R") {
		wdt_enable((WDTO_4S));
		debugPort.println("resetting in 4s.");
	}
}

void modem(String commandString) {
	/**
	 *	this function takes care of parsing commands where cmp=mdm.
	 */
    String smsText = getSMSText(commandString);
    String job = getValueForKey("job", commandString);
	
	if (job == "ctrlz") {
		telitPort.print(26, BYTE);
		return;
	}

    if (verbose > 0) {
        debugPort.println();
        debugPort.println("entered void modem()");
        debugPort.print("sms text - ");
        debugPort.println(smsText);
        debugPort.println();
    }
	
	// send string to telit with a \r\n character appended to the string
	// todo - is it safer to send the char values for carriage return and linefeed?
    telitPort.print(smsText);
    telitPort.print("\r\n");
    
}

/**
 *	this function reads the sheevaPort (Serial2) for incoming commands
 *	and returns them as String objects.
 */
void readSheevaPort()
{
    int i;
    unsigned char c;
    boolean valid_message_streaming, valid_message_received;
    char s[MAXLEN_PLUG_MESSAGE];
    
    if (sheevaPort.available()) {
        debugPort.println("readSheevaPort():start");

        valid_message_streaming = false; 
        valid_message_received = false;

        i = 0;
        while (sheevaPort.available() && i < MAXLEN_PLUG_MESSAGE) {
            if ((c = sheevaPort.read()) != -1) {
                debugPort.print(c);
                if (valid_message_streaming) {
                    if (c == ')') {
                        valid_message_received =  true;
                    }
                    else if ((c != ' ') && (c != '\t')) { // skip whitespace
                        s[i++] = c;
                        if (c == '\n') { break; }
                    }
                }
                else if (c == '(') { 
                    valid_message_streaming = true; 
                }
                /* REMOVE WHEN DONE DEBUGGING */
                else if (c == 26) {
                    debugPort.println("got ctrl-z");
                    telitPort.println(c);
                }
            }
        }
        s[i] = '\0';

        if (i < 3) { // ()
            debugPort.println("received empty message.");
        }
        else if (valid_message_received) {
            if (msgWaitLock || \
                ((s[0] == 'a') || (s[0] == 'A')) && \
                    ((s[1] == 't') || (s[1] == 'T'))) { // modem job
                debugPort.println("received modem message");

                i = 0;
                do {
                    telitPort.print(s[i]);
                } while (s[++i] != '\0');

                if (msgWaitLock) { msgWaitLock = false; }
            }
            else { // meter job
                debugPort.println("received meter message:");
                meter_test(s);
            }
        }
        else {
            debugPort.println("received invalid message.");
        }

        debugPort.println("readSheevaPort():end");
    }
}

/* Parses &-delimited 'key=val' pairs and stores
 * the value for 'key' in 'val'
 */
void get_val(char *s, char *key, char *val)
{
    char *substr, *eq, *p, c;
    int i;

    substr = strstr(s, key);
    if (substr != NULL) {
        eq = strchr(substr, '=');
        if (eq != NULL) {
            p = eq;
            p++; // skip '='
            i = 0;
            c = *p;
            while ((c != NULL) && (c != '\0') && (c != '&') && \
                (c != '\n') && (c != '\r')) {
                val[i++] = *p++;
                c = *p;
            }
            val[i] = '\0';
        }
    }
}

void meter_test(char *s)
{
    char job[8], s_cid[8];
    int8_t cid;

    // get job
    get_val(s, "job", job);
    // get cid
    get_val(s, "cid", s_cid);
    cid = atoi(s_cid); // could use strtodyy

    if (verbose > 0) {
        debugPort.println();
        debugPort.println("entered void meter()");
        debugPort.print("executing job type:");
        debugPort.print(job);
        debugPort.print(", on circuit id:");
        debugPort.println(cid);
        debugPort.println();
    }
    
	if (!strncmp(job, "con", 3)) {
		debugPort.println("execute con job");
		SWset(cid,1);
		debugPort.print("switch ");
		debugPort.print(cid, DEC);
		if (SWisOn(cid)) {
			debugPort.println(" is on");
		} else {
			debugPort.println(" is off");
		}
	}
	else if (!strncmp(job, "coff", 4)) {
		debugPort.println("execute coff job");
		SWset(cid,0);
		debugPort.print("switch ");
		debugPort.print(cid, DEC);
		if (SWisOn(cid)) {
			debugPort.println(" is on");
		} else {
			debugPort.println(" is off");
		}
	}
	else if (!strncmp(job, "readRVA", 7)) {
		jobReadRVA(cid);
	}
	else if (!strncmp(job, "readLVA", 7)) {
		jobReadLVA(cid);
	}
	else if (!strncmp(job, "modeRVA", 7)) {
		setupRVAMode(cid);
	}
	else if (!strncmp(job, "modeLVA", 7)) {
		int32_t line_cycle = 1000;
        char s_line_cycle[8];
        get_val(s, "linecyc", s_line_cycle);
        line_cycle = atoi(s_line_cycle); // could use strtod
		setupLVAMode(cid, line_cycle);
	}
	else if (!strncmp(job, "modeDefault", 11)) {
		setupDefaultMode(cid);
	}
	else if (!strncmp(job, "c", 1)) {
		_testChannel = cid;
		displayChannelInfo();		
	}
	else if (!strncmp(job, "T", 1)) {
		testHardware();
	}
	else if (!strncmp(job, "R", 1)) {
		wdt_enable((WDTO_4S));
		debugPort.println("resetting in 4s.");
	}
}

/**
 *	this function reads the telitPort (Serial3) for incoming commands
 *	and returns them as String objects.
 */
void readTelitPort() {
	uint32_t startTime = millis();
    byte b;
    while (telitPort.available()) {
        if ((b = telitPort.read()) != -1) {
            debugPort.print(b);
            sheevaPort.print(b);
            if (b == '>') { // modem awaits the content of the sms 
                msgWaitLock = true;
                // delay(100);
            }
        } 
    }
}

void chooseDestination(String destination, String commandString) {
	/**
	 *	based on the value for the cmp key, this calls the function
	 *	meter if cmp=mtr
	 *	and
	 *  modem if cmp=mdm
	 */
    if (destination == "mtr") {
        meter(commandString);
    }
    else if (destination == "mdm") {
        modem(commandString);
    }
}

void turnOnTelit() {
	/**
	 *	Pull telit on/off pin high for 3 seconds to start up telit modem
	 */
	pinMode(22, OUTPUT);
	digitalWrite(22, HIGH);
	delay(3000);
	digitalWrite(22, LOW);
}
