#include <avr/eeprom.h>

#include "ReturnCode/returncode.h"
#include "ADE7753/ADE7753.h"
#include "Select/select.h"
#include "Switches/switches.h"
#include "circuit.h"
#include "arduino/HardwareSerial.h"

#define dbg Serial
#define max(X,Y) ((X)>=(Y))?(X):(Y)
#define ERRCHECKRETURN(Cptr) if (_shouldReturn(Cptr)) return;

// TODO make switch wait time a parameter
// TODO Have fault detection code check interrupts for sag detection and frequency variation
// ipeak is RMS or what basically should I multiple by root2?

int8_t _shouldReturn(Circuit *c) 
{
    ifnsuccess(_retCode) {            
        if (_retCode == COMMERR) {        
            c->status |= COMM;            
            CSselectDevice(DEVDISABLE);    
            return true;                        
        } else if (_retCode == TIMEOUT) {
            //Do Nothing
        } else {                        
            CSselectDevice(DEVDISABLE);    
            return true;                        
        }                                
    }
    return false;
}

/**
 * Clears circuit interrupts
 * */
void Cclear(Circuit *c) 
{
    RCreset();
    int32_t regData;
    int8_t timeout = false;
    CSselectDevice(c->circuitID);                       ERRCHECKRETURN(c);

    //Check for presence and clears the interrupt register
    //Comm errors is stored in c->status
    ADEgetRegister(RSTSTATUS,&regData);                 ERRCHECKRETURN(c);
    c->status &= ~COMM;
    c->status &= 0xFFFF0000;
    c->status |= (0x0000FFFF&regData);
    CSselectDevice(DEVDISABLE);                       ERRCHECKRETURN(c);
}

/** 
  Updates circuit measured parameters
  @warning As a prerequisite Cclear should be called first.
  @warning a communications error may leave Circuit *c in an inconsisent state.
  @warning The completion time of this function is dependent on the frequency of the line as well as halfCyclesSample. At worst the function will take one minute to return if halfCyclesSample is 1400 and the frequency drops below 40hz.
  @return ARGVALUEERR if the circuitID is invalid
  @return COMMERR if there is a communications error or the ADE is not detected
*/
void Cmeasure(Circuit *c)
{
    int32_t regData;
    int8_t timeout = false;
    RCreset();
    CSselectDevice(c->circuitID);                       ERRCHECKRETURN(c);

    //Check for presence 
    ADEgetRegister(STATUS,&regData);                 ERRCHECKRETURN(c);
    c->status &= ~COMM;
    c->status &= 0xFFFF0000;
    c->status |= (0x0000FFFF&regData);

    //Start measuring
    ADEgetRegister(PERIOD,&regData);                    ERRCHECKRETURN(c);
    c->periodus = regData*22/10; //2.2us/bit

    dbg.println("Measuring Ckt");

    uint16_t waitTime = (uint16_t)((regData*22/100.0)*(c->halfCyclesSample/100));
    waitTime = waitTime + waitTime/2;//Wait at least 1.5 times the amount of time it takes for halfCycleSample halfCycles to occur
    //uint16_t waitTime = 2*1000*c->halfCyclesSample/max(c->frequency,40);

    ADEwaitForInterrupt(CYCEND,waitTime);               ERRCHECKRETURN(c);
    //The failure may have occured because there was no interrupt
    if (_retCode == TIMEOUT) {
        timeout = true;
    }

    if (!timeout) {
        //Apparent power or Volt Amps
        ADEgetRegister(LVAENERGY,&regData);             ERRCHECKRETURN(c);
        c->VA = regData*c->VAEslope/(c->halfCyclesSample/2.0*c->periodus/1000000);  //Watts

        //Active power or watts
        ADEgetRegister(LAENERGY,&regData);              ERRCHECKRETURN(c);
        c->W = regData*c->Wslope/(c->halfCyclesSample/2.0*c->periodus/1000000); //The denominator is the actual time in seconds

        //IRMS
        ADEgetRegister(IRMS,&regData);                  ERRCHECKRETURN(c);
        c->IRMS = regData*c->IRMSslope;

        //VRMS
        ADEgetRegister(VRMS,&regData);                  ERRCHECKRETURN(c);
        c->VRMS= regData*c->VRMSslope;

        //Apparent energy in joules accumulated since last query
        ADEgetRegister(RVAENERGY,&regData);             ERRCHECKRETURN(c);
        c->VAEnergy = regData*c->VAEslope/1000;

        //Actve energy accumulated since last query
        ADEgetRegister(RAENERGY,&regData);              ERRCHECKRETURN(c);
        c->WEnergy = regData*c->Wslope/1000;

        //Current and Voltage Peaks
        ADEgetRegister(RSTIPEAK,&regData);              ERRCHECKRETURN(c);
        c->ipeak = regData*c->IRMSslope;

        ADEgetRegister(RSTVPEAK,&regData);              ERRCHECKRETURN(c);
        c->vpeak = regData*c->VRMSslope;

        //Power Factor PF
        if (c->VAEnergy != 0){ 
            c->PF = (uint16_t)(((((uint64_t)c->WEnergy<<16)-c->WEnergy)-c->WEnergy)/c->VAEnergy);
        } else {
            c->PF = 65535;
        }
    } //end if (!timeout)

    CSselectDevice(DEVDISABLE);

    if (timeout) {
        _retCode = TIMEOUT;
    }
}

void Cprogram(Circuit *c)
{
    int32_t regData;
    _retCode = SUCCESS;
    CSselectDevice(c->circuitID);                       ERRCHECKRETURN(c);

    ADEreset();

    if (c->sagDurationCycles > 0) { 
        regData = c->sagDurationCycles + 1;
        ADEsetModeBit(DISSAG,false);                    ERRCHECKRETURN(c);
        ADEsetRegister(SAGCYC,&regData);                ERRCHECKRETURN(c);
    } else {
        ADEsetModeBit(DISSAG,true);                     ERRCHECKRETURN(c);
    }
    regData = c->phcal;                                 ERRCHECKRETURN(c);
    ADEsetRegister(PHCAL,&regData);

    ADEsetCHXOS(1,&c->chIint,&c->chIos);                ERRCHECKRETURN(c);
    regData = c->IRMSoffset;
    ADEsetRegister(IRMSOS, &regData);                   ERRCHECKRETURN(c);
    //since this is channel 2 c->chIint is ignored
    ADEsetCHXOS(2,&c->chIint,&c->chVos);                ERRCHECKRETURN(c);
    regData = c->VRMSoffset;
    ADEsetRegister(VRMSOS, &regData);                   ERRCHECKRETURN(c);
    if (c->halfCyclesSample> 0) {
        regData = c->halfCyclesSample;
        ADEsetRegister(LINECYC,&regData);               ERRCHECKRETURN(c);
        ADEsetModeBit(CYCMODE,true);                    ERRCHECKRETURN(c);
    } else {
        ADEsetModeBit(CYCMODE,false);                   ERRCHECKRETURN(c);
    }
    regData =  (c->chVgainExp<<5) | (c->chVscale<<3) | c->chIgainExp;
    ADEsetRegister(GAIN,&regData);                      ERRCHECKRETURN(c);
    
    CSselectDevice(DEVDISABLE);
}

void CsetOn(Circuit *c, int8_t on) 
{
    if (CisOn(c) != on) {
        ADEwaitForInterrupt(ZX0,10);
    }
    SWset(c->circuitID,on);
}

int8_t CisOn(Circuit *c) 
{
    return SWisOn(c->circuitID);
}

/**
 *  Loads circuit data from the EEPROM into memory. This data can now be used to program the registers.
 * */
void Cload(Circuit *c, Circuit* addrEEPROM)
{
    eeprom_read_block(c,(uint8_t*)addrEEPROM,sizeof(Circuit));
}

/**
 *  Save circuit data from the memory into EEPROM.
 * */
void Csave(Circuit *c, Circuit* addrEEPROM) 
{
    eeprom_update_block(c,(uint8_t*)addrEEPROM,sizeof(Circuit));
}

/** 
    Reasonable default values for Circuit.
    @warning Does not program the ADE7753s
  */
void CsetDefaults(Circuit *c, int8_t circuitID) 
{
    c->circuitID = circuitID;

    /** Measurement Configuration Parameters */
    c->halfCyclesSample = 120;
    c->phcal = 11; //Ox0B

    /** Current Calibration Parameters  */
    c->chIint = false;
    c->chIos = 0;
    c->chIgainExp = 4;
    c->IRMSoffset = -2048;//-2048;//0x01BC;
    c->IRMSslope = .00224;//.0010;//164; /** in mA/Counts */

    /** Voltage Calibration Parameters */
    c->chVos = 1;//15;
    c->chVgainExp = 1;
    c->chVscale = 0;
    c->VRMSoffset = -2048;//0x07FF;
    c->VRMSslope = .1068; /** in mV/Counts */

    /** Power Calibration Parameters */
    c->VAEslope = 37.5;//34.2760;//2014/10000.0; mJ/Counts
    c->VAoffset = 0;// TODO not used yet
    c->Wslope= 31.05; // TODO not used yet mJ/Counts for watts
    c->Woffset=0;// TODO not used yet

    /** Software Saftey Parameters */
    c->sagDurationCycles = 10;
    c->minVSag = 100;
    c->VAPowerMax = 2000;
    c->ipeakMax = 16000;
    c->vpeakMax = 400;

    // Current and Voltage
    // Measured
    c->IRMS = 0;
    c->VRMS = 0;
    c->periodus = 1024;
    c->VA = 0;
    c->W = 0;
    c->PF = 1234;// Is a value from 0 to 2^16-1
    c->VAEnergy = 0;
    c->WEnergy = 0;
    c->ipeak = 123;
    c->vpeak = 123;

}

/**
 * Prints all circuit values from memory and the current ADE settings for the circuit. 
 * @warning These values are not correlated until they have been programmed.
 * */
void Cprint(HardwareSerial *ser, Circuit *c) 
{

    ser->print("#Circuit");
    ser->print("circuitID&");
    ser->println(c->circuitID);
    ser->print("halfCyclesSample&");
    ser->println(c->halfCyclesSample);
    ser->print("phcal&");
    ser->println(c->phcal);
    ser->print("chIint&");
    ser->println(c->chIint);
    ser->print("chIOS&");
    ser->println(c->chIos);
    ser->print("chIgainExp&");
    ser->print(c->chIgainExp);
    ser->print("IRMSOS&");
    ser->println(c->IRMSoffset);
    ser->print("IRMS slope&");
    ser->println(c->IRMSslope);

    ser->print("chVOS&");
    ser->println(c->chVos);
    ser->print("chIgainExp&");
    ser->print(c->chVgainExp);
    ser->print("chVscale&");
    ser->println(c->chVscale);
    ser->print("VRMSOS&");
    ser->println(c->VRMSoffset);
    ser->print("VRMS slope&");
    ser->println(c->VRMSslope);

    ser->print("VAE slope&");
    ser->println(c->VAEslope);
    ser->print("VA OS&");
    ser->println(c->VAoffset);
    ser->print("W OS&");
    ser->println(c->VAoffset);
    ser->print("W slope&");
    ser->println(c->Wslope);

    ser->print("IRMS"); ser->println(c->IRMS);
    ser->print("VRMS"); ser->println(c->VRMS);
    ser->print("Period"); ser->println(c->periodus);
    ser->print("VA"); ser->println(c->VA);
    ser->print("W"); ser->println(c->W);
    ser->print("PF"); ser->println(c->PF);
    ser->print("VA Energy"); ser->println(c->VAEnergy);
    ser->print("W Energy"); ser->println(c->WEnergy);
    ser->print("ipeak"); ser->println(c->ipeak);
    ser->print("vpeak"); ser->println(c->vpeak);

    CSselectDevice(c->circuitID);
    ser->println("#ADE");
    for (const ADEReg** reg = &regList[0]; reg < &(regList[regListSize/sizeof(*reg)-1]);reg++) {
        int32_t regData = 0;
        ser->print((**reg).name); ser->print("& "); 
        ADEgetRegister(**reg,&regData);
        ifsuccess(_retCode) {
            ser->print(":0x");
            ser->print(regData,HEX);
            ser->print(":");
            ser->print(regData,DEC);
        } else {
            ser->println("FAILURE");
        }
        ser->println();
    }
    CSselectDevice(DEVDISABLE);
}

void CprintMeas(HardwareSerial *ser, Circuit *c)
{
    ser->print(c->circuitID,DEC);
    ser->print(",");
    ser->print(CisOn(c),DEC);
    ser->print(",");
    ser->print(c->VRMS,DEC);
    ser->print(",");
    ser->print(c->IRMS,DEC);
    ser->print(",");
    ser->print(c->vpeak,DEC);
    ser->print(",");
    ser->print(c->ipeak,DEC);
    ser->print(",");
    ser->print(c->periodus,DEC);
    ser->print(",");
    ser->print(c->VA,DEC);
    ser->print(",");
    ser->print(c->W,DEC);
    ser->print(",");
    ser->print(c->VAEnergy,DEC);
    ser->print(",");
    ser->print(c->WEnergy,DEC);
    ser->print(",");
    ser->print(c->PF,DEC);
    ser->print(",");
    // TODO VA ACCUM
    ser->print(0);
    ser->print(",");
    // TODO W ACCUM
    ser->print(0);
}

/** 
    Attempts to reestablish communications with the ADE.
    1) The SS pin is strobed. 
    2) TODO Then the ADE is reset. 
    3) TODO Finally the ADE is reprogrammed.
    @returns true if a communication was successful.
*/
int8_t CrestoreCommunications(Circuit *c)
{
    RCreset();
    CSselectDevice(c->circuitID);
    CSstrobe();
    
    //Get DIEREV guaranteed not to be zero
    int32_t regData = 0;
    ADEgetRegister(DIEREV, &regData);
    CSselectDevice(DEVDISABLE);

    ifsuccess(_retCode) {
        if (regData != 0) return true;
    }
    return false;

}

