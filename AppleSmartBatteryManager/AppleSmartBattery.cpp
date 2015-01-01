/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/* Modification History
 *
 * Unknown date: Original mods credit to glsy/insanelymac.com
 *
 * 2012-08-31, RehabMan/tonymacx86.com.
 *   - Integrate zprood's hack to gather cycle count as extra field in _BIF.
 *   - Also fix bug where if boot with no batteries installed, incorrect
 *     "Power Soure: Battery" was displayed in Mac menu bar.
 *   - Also a few other changes to get battery status to show in System Report
 *
 * 2012-09-02, RehabMan/tonymacx86.com
 *   - Fix bug where code assumes that "not charging" means 100% charged.
 *   - Minor code cleanup (const issues)
 *   - Added code to set warning/critical levels
 *
 * 2012-09-10, RehabMan/tonymacx86.com
 *   - Added ability to get battery temperature thorugh _BIF method
 *
 * 2012-09-19, RehabMan/tonymacx86.com
 *   - Calculate for watts correctly in case ACPI is reporting watts instead 
 *     of amps.
 *
 * 2012-09-21, RehabMan/tonymacx86.com
 *   - In preparation to merge probook and master branches, validate
 *     _BIX, and _BBIX methods before attempting to use them.
 *
 */

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/RootDomain.h>
//#include <IOKit/pwr_mgt/IOPMPrivate.h>    //rehabman: I don't have this header in latest xcode
#include <libkern/c++/OSObject.h>

#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"

// Retry attempts on command failure

enum { 
    kRetryAttempts = 5,
    kInitialPollCountdown = 5,
    kIncompleteReadRetryMax = 10
};

enum 
{
    kSecondsUntilValidOnWake    = 30,
    kPostChargeWaitSeconds      = 120,
    kPostDischargeWaitSeconds   = 120
};

enum 
{
    kDefaultPollInterval = 0,
    kQuickPollInterval = 1
};

#define kErrorRetryAttemptsExceeded         "Read Retry Attempts Exceeded"
#define kErrorOverallTimeoutExpired         "Overall Read Timeout Expired"
#define kErrorZeroCapacity                  "Capacity Read Zero"
#define kErrorPermanentFailure              "Permanent Battery Failure"
#define kErrorNonRecoverableStatus          "Non-recoverable status failure"

// Polling intervals
// The battery kext switches between polling frequencies depending on
// battery load

static uint32_t milliSecPollingTable[2] =
{ 
	30000,    // 0 == Regular 30 second polling
	1000      // 1 == Quick 1 second polling
};

static const uint32_t kBatteryReadAllTimeout = 10000;       // 10 seconds

// Keys we use to publish battery state in our IOPMPowerSource::properties array
static const OSSymbol *_MaxErrSym =				OSSymbol::withCString(kIOPMPSMaxErrKey);
static const OSSymbol *_DeviceNameSym =			OSSymbol::withCString(kIOPMDeviceNameKey);
static const OSSymbol *_FullyChargedSym =		OSSymbol::withCString(kIOPMFullyChargedKey);
static const OSSymbol *_AvgTimeToEmptySym =		OSSymbol::withCString("AvgTimeToEmpty");
static const OSSymbol *_AvgTimeToFullSym =		OSSymbol::withCString("AvgTimeToFull");
static const OSSymbol *_InstantTimeToEmptySym = OSSymbol::withCString("InstantTimeToEmpty");
static const OSSymbol *_InstantTimeToFullSym =	OSSymbol::withCString("InstantTimeToFull");
static const OSSymbol *_InstantAmperageSym =	OSSymbol::withCString("InstantAmperage");
static const OSSymbol *_ManufactureDateSym =	OSSymbol::withCString(kIOPMPSManufactureDateKey);
static const OSSymbol *_DesignCapacitySym =		OSSymbol::withCString(kIOPMPSDesignCapacityKey);
static const OSSymbol *_QuickPollSym =			OSSymbol::withCString("Quick Poll");
static const OSSymbol *_TemperatureSym =		OSSymbol::withCString(kIOPMPSBatteryTemperatureKey);
static const OSSymbol *_CellVoltageSym =		OSSymbol::withCString("CellVoltage");
static const OSSymbol *_ManufacturerDataSym =	OSSymbol::withCString("ManufacturerData");
static const OSSymbol *_PFStatusSym =			OSSymbol::withCString("PermanentFailureStatus");
static const OSSymbol *_TypeSym =				OSSymbol::withCString("BatteryType");
static const OSSymbol *_ChargeStatusSym =		OSSymbol::withCString(kIOPMPSBatteryChargeStatusKey);

static const OSSymbol *_RunTimeToEmptySym =			OSSymbol::withCString("RunTimeToEmpty");
static const OSSymbol *_RelativeStateOfChargeSym =	OSSymbol::withCString("RelativeStateOfCharge");
static const OSSymbol *_AbsoluteStateOfChargeSym =	OSSymbol::withCString("AbsoluteStateOfCharge");
static const OSSymbol *_RemainingCapacitySym =		OSSymbol::withCString("RemainingCapacity");
static const OSSymbol *_AverageCurrentSym =			OSSymbol::withCString("AverageCurrent");
static const OSSymbol *_CurrentSym =				OSSymbol::withCString("Current");

/* _SerialNumberSym represents the manufacturer's 16-bit serial number in
 numeric format. 
 */
static const OSSymbol *_SerialNumberSym =		OSSymbol::withCString("FirmwareSerialNumber");

/* _SoftwareSerialSym == AppleSoftwareSerial
 represents the Apple-generated user readable serial number that will appear
 in the OS and is accessible to users.
 */
static const OSSymbol *_HardwareSerialSym =		OSSymbol::withCString("BatterySerialNumber");
static const OSSymbol *_DateOfManufacture =		OSSymbol::withCString("Date of Manufacture");

OSDefineMetaClassAndStructors(rehab_ACPIBattery, IOPMPowerSource)

/******************************************************************************
 * AppleSmartBattery::ACPIBattery
 *     
 ******************************************************************************/

AppleSmartBattery * AppleSmartBattery::smartBattery(void)
{
	AppleSmartBattery * me;
	me = new AppleSmartBattery;
	
	if (me && !me->init())
	{
		me->release();
		return NULL;
	}
	
	return me;
}

/******************************************************************************
 * AppleSmartBattery::init
 *
 ******************************************************************************/

bool AppleSmartBattery::init(void) 
{
    if (!super::init()) {
        return false;
    }
	
    fProvider = NULL;
    fWorkLoop = NULL;
    fPollTimer = NULL;
    
    fTracker = NULL;
	
    return true;
}

/******************************************************************************
 * AppleSmartBattery::free
 *
 ******************************************************************************/

void AppleSmartBattery::free(void) 
{
    fPollTimer->cancelTimeout();
#ifdef REVIEW
    fBatteryReadAllTimer->cancelTimeout();
#endif
    fWorkLoop->disableAllEventSources();
    
    clearBatteryState(true);
    
    super::free();
}

/******************************************************************************
 * AppleSmartBattery::start
 *
 ******************************************************************************/

bool AppleSmartBattery::start(IOService *provider)
{
    OSNumber        *debugPollingSetting;
	OSBoolean		*useExtendedInformation;
	OSBoolean		*useExtraInformation;
	
    fProvider = OSDynamicCast(AppleSmartBatteryManager, provider);
	
    if (!fProvider || !super::start(provider)) {
        return false;
    }
	
    debugPollingSetting = (OSNumber *)fProvider->getProperty(kBatteryPollingDebugKey);
    
	if( debugPollingSetting && OSDynamicCast(OSNumber, debugPollingSetting) )
    {
        /* We set our polling interval to the "BatteryPollingPeriodOverride" property's value,
		 in seconds.
		 Polling Period of 0 causes us to poll endlessly in a loop for testing.
         */
        fPollingInterval = debugPollingSetting->unsigned32BitValue();
        fPollingOverridden = true;
    }
	else 
	{
        fPollingInterval = kDefaultPollInterval;
        fPollingOverridden = false;
    }
	
	// Check if we should use extended information in _BIX (ACPI 4.0) or older _BIF
	
	useExtendedInformation = (OSBoolean *)fProvider->getProperty(kUseBatteryExtendedInfoKey);
    fUseBatteryExtendedInformation = false;
	if (useExtendedInformation && OSDynamicCast(OSBoolean, useExtendedInformation) )
	{
		fUseBatteryExtendedInformation = useExtendedInformation->isTrue();
        if (fUseBatteryExtendedInformation && kIOReturnSuccess != fProvider->validateBatteryBIX())
            fUseBatteryExtendedInformation = false;
	}
	
	if (fUseBatteryExtendedInformation)
	{
		IOLog("ACPIBatteryManager: Using ACPI extended battery information method _BIX\n");
	}
	else 
	{
		IOLog("ACPIBatteryManager: Using ACPI regular battery information method _BIF\n");
	}

	// Check if we should use extra information in BBIX
	
	useExtraInformation = (OSBoolean *)fProvider->getProperty(kUseBatteryExtraInfoKey);
    fUseBatteryExtraInformation = false;
	if (useExtraInformation && OSDynamicCast(OSBoolean, useExtraInformation) )
	{
		fUseBatteryExtraInformation = useExtraInformation->isTrue();
        if (fUseBatteryExtraInformation && kIOReturnSuccess != fProvider->validateBatteryBBIX())
            fUseBatteryExtraInformation = false;
	}
	
	if (fUseBatteryExtraInformation)
	{
		IOLog("ACPIBatteryManager: Using ACPI extra battery information method BBIX\n");
	}

    fEstimateCycleCountDivisor = 6;
    OSNumber* estimateCycleCountDivisor = (OSNumber*)fProvider->getProperty(kEstimateCycleCountDivisorInfoKey);
    if (estimateCycleCountDivisor && OSDynamicCast(OSNumber, estimateCycleCountDivisor))
    {
        fEstimateCycleCountDivisor = estimateCycleCountDivisor->unsigned32BitValue();
    }

    fBatteryPresent		= false;
    fACConnected		= false;
    fACChargeCapable	= false;
	fSystemSleeping     = false;
    fPowerServiceToAck  = NULL;
	fPollingNow         = false;
	
	// Make sure that we read battery state at least 5 times at 30 second intervals
    // after system boot.
    fInitialPollCountdown = kInitialPollCountdown;
	
    fWorkLoop = getWorkLoop();
	
    fPollTimer = IOTimerEventSource::timerEventSource( this, 
													  OSMemberFunctionCast( IOTimerEventSource::Action, 
																		   this, &AppleSmartBattery::pollingTimeOut) );
    if( !fWorkLoop || !fPollTimer
	   || (kIOReturnSuccess != fWorkLoop->addEventSource(fPollTimer)))
    {
        return false;
    }
    
#ifdef REVIEW
    fBatteryReadAllTimer = IOTimerEventSource::timerEventSource( this,
																OSMemberFunctionCast( IOTimerEventSource::Action,
																					 this, &AppleSmartBattery::incompleteReadTimeOut) );
    if ( !fBatteryReadAllTimer
        || (kIOReturnSuccess != fWorkLoop->addEventSource(fBatteryReadAllTimer)))
    {
        return false;
    }
#endif
    
    // get tracker for status of other batteries
    fTracker = OSDynamicCast(BatteryTracker, waitForMatchingService(serviceMatching(kBatteryTrackerService)));

    this->setName("AppleSmartBattery");
	
    // Publish the intended period in seconds that our "time remaining"
    // estimate is wildly inaccurate after wake from sleep.
    setProperty( kIOPMPSInvalidWakeSecondsKey,		kSecondsUntilValidOnWake, NUM_BITS);
	
    // Publish the necessary time period (in seconds) that a battery
    // calibrating tool must wait to allow the battery to settle after
    // charge and after discharge.
    setProperty( kIOPMPSPostChargeWaitSecondsKey,	kPostChargeWaitSeconds, NUM_BITS);
    setProperty( kIOPMPSPostDishargeWaitSecondsKey, kPostDischargeWaitSeconds, NUM_BITS);
	
    // zero out battery state with argument (do_set == true)
    clearBatteryState(false);

    // some DSDT implementations aren't ready to read the EC yet, so avoid false reading
    IOSleep(500);
	
    // Kick off the 30 second timer and do an initial poll
    pollBatteryState( kNewBatteryPath );
	
    return true;
}

#ifdef DEBUG

/******************************************************************************
 * AppleSmartBattery::stop
 *
 ******************************************************************************/

void AppleSmartBattery::stop(IOService *provider)
{
    OSSafeReleaseNULL(fTracker);
    
    super::stop(provider);
}

#endif

/******************************************************************************
 * AppleSmartBattery::logReadError
 *
 ******************************************************************************/

void AppleSmartBattery::logReadError(
										  const char *error_type, 
										  uint16_t additional_error,
										  void *t)
{
    if(!error_type) return;
	
    setProperty((const char *)"LatestErrorType", error_type);
	
    IOLog("ACPIBatteryManager: Error: %s (%d)\n", error_type, additional_error);  
	
    return;
}

/******************************************************************************
 * AppleSmartBattery::setPollingInterval
 *
 ******************************************************************************/

void AppleSmartBattery::setPollingInterval(
												int milliSeconds)
{
    DEBUG_LOG("AppleSmartBattery::setPollingInterval: New interval = %d ms\n", milliSeconds);
    
    if (!fPollingOverridden) {
        milliSecPollingTable[kDefaultPollInterval] = milliSeconds;
        fPollingInterval = kDefaultPollInterval;
    }
}

/******************************************************************************
 * AppleSmartBattery::pollBatteryState
 *
 * Asynchronously kicks off the register poll.
 ******************************************************************************/

bool AppleSmartBattery::pollBatteryState(int path)
{
    DEBUG_LOG("AppleSmartBattery::pollBatteryState: path = %d\n", path);

//REVIEW: this could be simplified kNewBatteryPath vs. kExistingBatteryPath means little/nothing...
    
    // This must be called under workloop synchronization
    if (kNewBatteryPath == path) 
	{
		/* Cancel polling timer in case this round of reads was initiated
		 by an alarm. We re-set the 30 second poll later. */
		fPollTimer->cancelTimeout();
		
#ifdef REVIEW
		/* Initialize battery read timeout to catch any longstanding stalls. */
		fBatteryReadAllTimer->cancelTimeout();
		fBatteryReadAllTimer->setTimeoutMS( kBatteryReadAllTimeout );
#endif
		
		pollBatteryState( kExistingBatteryPath );
	} 
	else 
	{
		fPollingNow = true;
		
        fProvider->getBatterySTA();
		
        if (fBatteryPresent) 
		{
			if(fUseBatteryExtendedInformation)
				fProvider->getBatteryBIX();
			else
				fProvider->getBatteryBIF();
			
			if(fUseBatteryExtraInformation)
				fProvider->getBatteryBBIX();
			
			fProvider->getBatteryBST();
        }
		else
		{
            //rehabman: added to correct power source Battery if boot w/ no batteries
            DEBUG_LOG("AppleSmartBattery: !fBatteryPresent\n");
            fACConnected = true;
            setExternalConnected(fACConnected);
            setFullyCharged(false);
            clearBatteryState(true);
        }
		
		fPollingNow = false;

        fPollTimer->cancelTimeout();
		if (!fPollingOverridden) 
		{
            if (-1 == fRealAC || fRealAC == fACConnected)
            {
                /* Restart timer with standard polling interval */
                fPollTimer->setTimeoutMS(milliSecPollingTable[fPollingInterval]);
            }
            else
            {
                /* Restart timer with quick polling interval */
                fPollTimer->setTimeoutMS(milliSecPollingTable[kQuickPollInterval]);
            }
		}
		else
		{
			/* restart timer with debug value */
			fPollTimer->setTimeoutMS(1000 * fPollingInterval);
		}
	}
	
    return true;
}

void AppleSmartBattery::handleBatteryInserted(void)
{
    DEBUG_LOG("AppleSmartBattery::handleBatteryInserted called\n");
    
    // This must be called under workloop synchronization
    pollBatteryState( kNewBatteryPath );
	
    return;
}

void AppleSmartBattery::handleBatteryRemoved(void)
{
    DEBUG_LOG("AppleSmartBattery::handleBatteryRemoved called\n");
    
	// Removed battery means cancel any ongoing polling session */
	if(fPollingNow)
	{
		fCancelPolling = true;
		fPollTimer->cancelTimeout();
#ifdef REVIEW
		fBatteryReadAllTimer->cancelTimeout();
#endif
	}
	
    // This must be called under workloop synchronization
    clearBatteryState(true);
	acknowledgeSystemSleepWake();
	
    return;
}

/*****************************************************************************
 * AppleSmartBatteryManager::notifyConnectedState
 * Cause a fresh battery poll in workloop to check AC status
 ******************************************************************************/

void AppleSmartBattery::notifyConnectedState(bool connected)
{
    fRealAC = connected;
    if (fBatteryPresent)
    {
        // on AC status change, poll right away (will set quick timer if AC is out-of-sync)
        pollBatteryState(kExistingBatteryPath);
    }
}

/******************************************************************************
 * AppleSmartBattery::handleSystemSleepWake
 *
 * Caller must hold the gate.
 ******************************************************************************/

IOReturn AppleSmartBattery::handleSystemSleepWake(IOService* powerService, bool isSystemSleep)
{
    IOReturn ret = kIOPMAckImplied;
	
	DEBUG_LOG("AppleSmartBattery::handleSystemSleepWake: isSystemSleep = %d\n", isSystemSleep);
	
    if (!powerService || (fSystemSleeping == isSystemSleep))
        return kIOPMAckImplied;
	
    if (fPowerServiceToAck)
    {
        fPowerServiceToAck->release();
        fPowerServiceToAck = 0;
    }
	
    fSystemSleeping = isSystemSleep;
	
    if (fSystemSleeping) // System Sleep
    {
        // Stall PM until battery poll in progress is cancelled.
        if (fPollingNow)
        {
            fPowerServiceToAck = powerService;
            fPowerServiceToAck->retain();
            fPollTimer->cancelTimeout();
#ifdef REVIEW
            fBatteryReadAllTimer->cancelTimeout();
            ret = (kBatteryReadAllTimeout * 1000);
#endif
        }
    }
    else // System Wake
    {
        fPowerServiceToAck = powerService;
        fPowerServiceToAck->retain();
        pollBatteryState(kExistingBatteryPath);
		
        if (fPollingNow)
        {
            // Transaction started, wait for completion.
            ret = (kBatteryReadAllTimeout * 1000);
        }
        else if (fPowerServiceToAck)
        {
            fPowerServiceToAck->release();
            fPowerServiceToAck = 0;
        }
    }
	
    DEBUG_LOG("AppleSmartBattery::handleSystemSleepWake: handleSystemSleepWake(%d) = %x\n",
			isSystemSleep, (unsigned)ret);
    return ret;
}

/******************************************************************************
 * AppleSmartBattery::acknowledgeSystemSleepWake
 *
 * Caller must hold the gate.
 ******************************************************************************/

void AppleSmartBattery::acknowledgeSystemSleepWake( void )
{
	DEBUG_LOG("AppleSmartBattery::acknowledgeSystemSleepWake called\n");
	
    if (fPowerServiceToAck)
    {
        fPowerServiceToAck->acknowledgeSetPowerState();
        fPowerServiceToAck->release();
        fPowerServiceToAck = 0;
    }
}

/******************************************************************************
 * pollingTimeOut
 *
 * Regular 30 second poll expiration handler.
 ******************************************************************************/

void AppleSmartBattery::pollingTimeOut(void)
{
    DEBUG_LOG("AppleSmartBattery::pollingTimeOut called\n");
    
	// Timer will be re-enabled from the battery polling routine.
    // Timer will not be kicked off again if battery is plugged in and
    // fully charged.
    if( fPollingNow ) 
        return;
    
    if (fInitialPollCountdown > 0) 
    {
        // At boot time we make sure to re-read everything kInitialPoltoCountdown times
        pollBatteryState( kNewBatteryPath );
        --fInitialPollCountdown;
    } else {
		pollBatteryState( kExistingBatteryPath );
	}
}

/******************************************************************************
 * incompleteReadTimeOut
 * 
 * The complete battery read has not completed in the allowed timeframe.
 * We assume this is for several reasons:
 *    - The EC has dropped an SMBus packet (probably recoverable)
 *    - The EC has stalled an SMBus request; IOSMBusController is hung (probably not recoverable)
 *
 * Start the battery read over from scratch.
 *****************************************************************************/

void AppleSmartBattery::incompleteReadTimeOut(void)
{
    DEBUG_LOG("AppleSmartBattery::incompleteReadTimeOut called\n");
    
    logReadError(kErrorOverallTimeoutExpired, 0, NULL);
	
	pollBatteryState( kExistingBatteryPath );
}

/******************************************************************************
 * AppleSmartBattery::clearBatteryState
 *
 ******************************************************************************/

void AppleSmartBattery::clearBatteryState(bool do_update)
{
    DEBUG_LOG("AppleSmartBattery::clearBatteryState: do_update = %s\n", do_update == true ? "true" : "false");
    
    // Only clear out battery state; don't clear manager state like AC Power.
    // We just zero out the int and bool values, but remove the OSType values.

    fBatteryPresent = false;
    fACConnected = false;
    fACChargeCapable = false;
	
    setBatteryInstalled(false);
    setIsCharging(false);
    setCurrentCapacity(0);
    setMaxCapacity(0);
    setTimeRemaining(0);
    setAmperage(0);
    setVoltage(0);
    setCycleCount(0);
	setMaxErr(0);
	
    properties->removeObject(manufacturerKey);
    removeProperty(manufacturerKey);
    properties->removeObject(serialKey);
    removeProperty(serialKey);
    properties->removeObject(batteryInfoKey);
    removeProperty(batteryInfoKey);
    properties->removeObject(errorConditionKey);
    removeProperty(errorConditionKey);
	
	// setBatteryBIF/setBatteryBIX
	properties->removeObject(_DesignCapacitySym);
    removeProperty(_DesignCapacitySym);
	properties->removeObject(_DeviceNameSym);
    removeProperty(_DeviceNameSym);
	properties->removeObject(_TypeSym);
    removeProperty(_TypeSym);
	properties->removeObject(_MaxErrSym);
    removeProperty(_MaxErrSym);
	properties->removeObject(_ManufactureDateSym);
    removeProperty(_ManufactureDateSym);
	properties->removeObject(_SerialNumberSym);
    removeProperty(_SerialNumberSym);
	properties->removeObject(_ManufacturerDataSym);
    removeProperty(_ManufacturerDataSym);
	properties->removeObject(_PFStatusSym);
	removeProperty(_PFStatusSym);
	properties->removeObject(_AbsoluteStateOfChargeSym);
	removeProperty(_AbsoluteStateOfChargeSym);
	properties->removeObject(_DateOfManufacture);
	removeProperty(_DateOfManufacture);
	properties->removeObject(_RelativeStateOfChargeSym);
	removeProperty(_RelativeStateOfChargeSym);
	properties->removeObject(_RemainingCapacitySym);
	removeProperty(_RemainingCapacitySym);
	properties->removeObject(_RunTimeToEmptySym);
	removeProperty(_RunTimeToEmptySym);

	// setBatteryBST
	properties->removeObject(_AvgTimeToEmptySym);
    removeProperty(_AvgTimeToEmptySym);
	properties->removeObject(_AvgTimeToFullSym);
    removeProperty(_AvgTimeToFullSym);
	properties->removeObject(_InstantTimeToEmptySym);
    removeProperty(_InstantTimeToEmptySym);
	properties->removeObject(_InstantTimeToFullSym);
    removeProperty(_InstantTimeToFullSym);
	properties->removeObject(_InstantAmperageSym);
    removeProperty(_InstantAmperageSym);
	properties->removeObject(_QuickPollSym);
    removeProperty(_QuickPollSym);
	properties->removeObject(_CellVoltageSym);
    removeProperty(_CellVoltageSym);
	properties->removeObject(_TemperatureSym);
    removeProperty(_TemperatureSym);
	properties->removeObject(_HardwareSerialSym);
    removeProperty(_HardwareSerialSym);
	
    rebuildLegacyIOBatteryInfo(do_update);

    fRealAC = -1;   // real AC status is unknown...
	
    if(do_update) {
        updateStatus();
    }
}

/******************************************************************************
 *  Package battery data in "legacy battery info" format, readable by
 *  any applications using the not-so-friendly IOPMCopyBatteryInfo()
 ******************************************************************************/

void AppleSmartBattery::rebuildLegacyIOBatteryInfo(bool do_update)
{
    OSDictionary        *legacyDict = OSDictionary::withCapacity(5);
    uint32_t            flags = 0;
    OSNumber            *flags_num = NULL;
	
    DEBUG_LOG("AppleSmartBattery::rebuildLegacyIOBatteryInfo called\n");
    
    if (externalConnected()) flags |= kIOPMACInstalled;
    if (batteryInstalled()) flags |= kIOPMBatteryInstalled;
    if (isCharging()) flags |= kIOPMBatteryCharging;
	
    if(do_update)
    {
        flags_num = OSNumber::withNumber((unsigned long long)flags, NUM_BITS);
        legacyDict->setObject(kIOBatteryFlagsKey, flags_num);
        flags_num->release();
	
        legacyDict->setObject(kIOBatteryCurrentChargeKey, properties->getObject(kIOPMPSCurrentCapacityKey));
        legacyDict->setObject(kIOBatteryCapacityKey, properties->getObject(kIOPMPSMaxCapacityKey));
        legacyDict->setObject(kIOBatteryVoltageKey, properties->getObject(kIOPMPSVoltageKey));
        legacyDict->setObject(kIOBatteryAmperageKey, properties->getObject(kIOPMPSAmperageKey));
        legacyDict->setObject(kIOBatteryCycleCountKey, properties->getObject(kIOPMPSCycleCountKey));
	
        setLegacyIOBatteryInfo(legacyDict);
	
        legacyDict->release();
    }
    else
    {
        properties->removeObject(kIOPMPSCurrentCapacityKey);
        properties->removeObject(kIOPMPSMaxCapacityKey);
        properties->removeObject(kIOPMPSVoltageKey);
        properties->removeObject(kIOPMPSAmperageKey);
        properties->removeObject(kIOPMPSCycleCountKey);
    }
}

/******************************************************************************
 *  Fabricate a serial number from our battery controller model and serial
 *  number.
 ******************************************************************************/

#define kMaxGeneratedSerialSize (64)

void AppleSmartBattery::constructAppleSerialNumber(void)
{
    OSSymbol        *device_string = fDeviceName;
    const char *    device_cstring_ptr;
    OSSymbol        *serial_string = fSerialNumber;
    const char *    serial_cstring_ptr;
	
    const OSSymbol  *printableSerial = NULL;
    char            serialBuf[kMaxGeneratedSerialSize];
	
    DEBUG_LOG("AppleSmartBattery::constructAppleSerialNumber called\n");
    
    if (device_string) {
        device_cstring_ptr = device_string->getCStringNoCopy();
    } else {
        device_cstring_ptr = "Unknown";
    }
	
    if (serial_string) {
        serial_cstring_ptr = serial_string->getCStringNoCopy();
    } else {
        serial_cstring_ptr = "Unknown";
    }
	
    bzero(serialBuf, kMaxGeneratedSerialSize);
    snprintf(serialBuf, kMaxGeneratedSerialSize, "%s-%s", device_cstring_ptr, serial_cstring_ptr);
	
    printableSerial = OSSymbol::withCString(serialBuf);
    if (printableSerial) {
		setPSProperty(_HardwareSerialSym, const_cast<OSSymbol*>(printableSerial));
        printableSerial->release();
    }
	
    return;
}

/******************************************************************************
 * Given a packed date from SBS, decode into a human readable date and return
 * an OSSymbol
 ******************************************************************************/

#define kMaxDateSize	12

const OSSymbol * AppleSmartBattery::unpackDate(UInt32 packedDate)
{
    DEBUG_LOG("AppleSmartBattery::unpackDate: packedDate = 0x%x\n", (unsigned int) packedDate);
    
	/* The date is packed in the following fashion: (year-1980) * 512 + month * 32 + day.
	 *
	 * Field	Bits Used	Format				Allowable Values
	 * Day		0...4		5 bit binary value	1 - 31 (corresponds to date)
	 * Month	5...8		4 bit binary value	1 - 12 (corresponds to month number)
	 * Year		9...15		7 bit binary value	0 - 127 (corresponds to year biased by 1980
	 */

	UInt32	yearBits	= packedDate >> 9;
	UInt32	monthBits	= (packedDate >> 5) & 0xF;
	UInt32	dayBits		= packedDate & 0x1F;

	char dateBuf[kMaxDateSize];
    bzero(dateBuf, kMaxDateSize);
	
	// TODO: Determine date format from OS and format according to that, but for now use YYYY-MM-DD
	
    snprintf(dateBuf, kMaxDateSize, "%4d-%02d-%02d%c", (unsigned int) yearBits + 1980, (unsigned int) monthBits, (unsigned int) dayBits, (char)0);
	
    const OSSymbol *printableDate = OSSymbol::withCString(dateBuf);
    if (printableDate) {
		return printableDate;
	}
	else {
		return NULL;
	}

}

/******************************************************************************
 *  Power Source value accessors
 *  These supplement the built-in accessors in IOPMPowerSource.h, and should 
 *  arguably be added back into the superclass IOPMPowerSource
 ******************************************************************************/

void AppleSmartBattery::setMaxErr(int error)
{
    OSNumber *n = OSNumber::withNumber(error, NUM_BITS);
    if (n) {
		setPSProperty(_MaxErrSym, n);
        n->release();
    }
}

int AppleSmartBattery::maxErr(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_MaxErrSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setDeviceName(const OSSymbol *sym)
{
    if (sym)
		setPSProperty(_DeviceNameSym, const_cast<OSSymbol*>(sym));
}

OSSymbol * AppleSmartBattery::deviceName(void)
{
    return OSDynamicCast(OSSymbol, properties->getObject(_DeviceNameSym));
}

void AppleSmartBattery::setFullyCharged(bool charged)
{
	setPSProperty(_FullyChargedSym, 
						  (charged ? kOSBooleanTrue:kOSBooleanFalse));
}

bool AppleSmartBattery::fullyCharged(void) 
{
    return (kOSBooleanTrue == properties->getObject(_FullyChargedSym));
}

void AppleSmartBattery::setInstantaneousTimeToEmpty(int seconds)
{
    OSNumber *n = OSNumber::withNumber(seconds, NUM_BITS);
    if (n) {
        setPSProperty(_InstantTimeToEmptySym, n);
		n->release();
    }
}

void AppleSmartBattery::setInstantaneousTimeToFull(int seconds)
{
    OSNumber *n = OSNumber::withNumber(seconds, NUM_BITS);
    if (n) {
        setPSProperty(_InstantTimeToFullSym, n);
		n->release();
    }
}

void AppleSmartBattery::setInstantAmperage(int mA)
{
    OSNumber *n = OSNumber::withNumber(mA, NUM_BITS);
    if (n) {
        setPSProperty(_InstantAmperageSym, n);
		n->release();
    }
}

void AppleSmartBattery::setAverageTimeToEmpty(int seconds)
{
	
    OSNumber *n = OSNumber::withNumber(seconds, NUM_BITS);
	if (n) {
		setPSProperty(_AvgTimeToEmptySym, n);
        n->release();
    }
}

int AppleSmartBattery::averageTimeToEmpty(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_AvgTimeToEmptySym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setAverageTimeToFull(int seconds)
{
    OSNumber *n = OSNumber::withNumber(seconds, NUM_BITS);
    if (n) {
        setPSProperty(_AvgTimeToFullSym, n);
		n->release();
    }
}

int AppleSmartBattery::averageTimeToFull(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_AvgTimeToFullSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setRunTimeToEmpty(int seconds)
{
    OSNumber *n = OSNumber::withNumber(seconds, NUM_BITS);
    if (n) {
        setPSProperty(_RunTimeToEmptySym, n);
		n->release();
    }
}

int AppleSmartBattery::runTimeToEmpty(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_RunTimeToEmptySym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setRelativeStateOfCharge(int percent)
{
    OSNumber *n = OSNumber::withNumber(percent, NUM_BITS);
    if (n) {
        setPSProperty(_RelativeStateOfChargeSym, n);
		n->release();
    }
}

int AppleSmartBattery::relativeStateOfCharge(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_RelativeStateOfChargeSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setAbsoluteStateOfCharge(int percent)
{
    OSNumber *n = OSNumber::withNumber(percent, NUM_BITS);
    if (n) {
        setPSProperty(_AbsoluteStateOfChargeSym, n);
		n->release();
    }
}

int AppleSmartBattery::absoluteStateOfCharge(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_AbsoluteStateOfChargeSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setRemainingCapacity(int mah)
{
    OSNumber *n = OSNumber::withNumber(mah, NUM_BITS);
    if (n) {
        setPSProperty(_RemainingCapacitySym, n);
		n->release();
    }
}

int AppleSmartBattery::remainingCapacity(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_RemainingCapacitySym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setAverageCurrent(int ma)
{
    OSNumber *n = OSNumber::withNumber(ma, NUM_BITS);
    if (n) {
        setPSProperty(_AverageCurrentSym, n);
		n->release();
    }
}

int AppleSmartBattery::averageCurrent(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_AverageCurrentSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setCurrent(int ma)
{
    OSNumber *n = OSNumber::withNumber(ma, NUM_BITS);
    if (n) {
        setPSProperty(_CurrentSym, n);
		n->release();
    }
}

int AppleSmartBattery::current(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_CurrentSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setTemperature(int temperature)
{
    OSNumber *n = OSNumber::withNumber(temperature, NUM_BITS);
    if (n) {
        setPSProperty(_TemperatureSym, n);
		n->release();
    }
}

int AppleSmartBattery::temperature(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_TemperatureSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setManufactureDate(int date)
{
    OSNumber *n = OSNumber::withNumber(date, NUM_BITS);
    if (n) {
		setPSProperty(_ManufactureDateSym, n);
        n->release();
    }
}

int AppleSmartBattery::manufactureDate(void)
{
    OSNumber *n = OSDynamicCast(OSNumber, properties->getObject(_ManufactureDateSym));
    if (n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void AppleSmartBattery::setSerialNumber(const OSSymbol *sym)
{
	// BatterySerialNumber
	
    if (sym) 
	{
		setPSProperty(_HardwareSerialSym, const_cast<OSSymbol*>(sym));
	
		// FirmwareSerialNumber - This is a number so we have to convert it from the zero padded
		//                        string returned by ACPI.
	
        long lSerialNumber = strtol(sym->getCStringNoCopy(), (char **)NULL, 16);
											 
		OSNumber *n = OSNumber::withNumber((unsigned long long) lSerialNumber, NUM_BITS);

		if(n) {
			setPSProperty(_SerialNumberSym, n);
			n->release();
		}
	}
}

OSSymbol *AppleSmartBattery::serialNumber(void)
{
	return OSDynamicCast(OSSymbol, properties->getObject(_SerialNumberSym));
}

void AppleSmartBattery::setManufacturerData(uint8_t *buffer, uint32_t bufferSize)
{
    OSData *newData = OSData::withBytes( buffer, bufferSize );
    if (newData) {
        setPSProperty(_ManufacturerDataSym, newData);
		newData->release();
    }
}

void AppleSmartBattery::setChargeStatus(const OSSymbol *sym)
{
	if (sym == NULL) {
		properties->removeObject(_ChargeStatusSym);
		removeProperty(_ChargeStatusSym);
	} else {
		setPSProperty(_ChargeStatusSym, const_cast<OSSymbol*>(sym));
	}
}

const OSSymbol *AppleSmartBattery::chargeStatus(void)
{
	return (const OSSymbol *)properties->getObject(_ChargeStatusSym);
}

void AppleSmartBattery::setDesignCapacity(unsigned int val)
{
    OSNumber *n = OSNumber::withNumber(val, NUM_BITS);
	if(n)
	{
		setPSProperty(_DesignCapacitySym, n);
		n->release();
	}
}

unsigned int AppleSmartBattery::designCapacity(void) 
{
    OSNumber *n;

    n = OSDynamicCast(OSNumber, properties->getObject(_DesignCapacitySym));
    if(!n) 
		return 0;
    else 
		return (unsigned int)n->unsigned32BitValue();
}

void AppleSmartBattery::setBatteryType(const OSSymbol *sym)
{
    if (sym)
		setPSProperty(_TypeSym, const_cast<OSSymbol*>(sym));
}

OSSymbol * AppleSmartBattery::batteryType(void)
{
    return OSDynamicCast(OSSymbol, properties->getObject(_TypeSym));
}

void AppleSmartBattery::setPermanentFailureStatus(unsigned int val)
{
	OSNumber *n = OSNumber::withNumber(val, NUM_BITS);
	
    if (n)
	{
		setPSProperty(_PFStatusSym, n);
		n->release();
	}
}

unsigned int AppleSmartBattery::permanentFailureStatus(void)
{
    OSNumber *n;
	
    n = OSDynamicCast(OSNumber, properties->getObject(_PFStatusSym));
    if(!n) 
		return 0;
    else 
		return (unsigned int)n->unsigned32BitValue();
}

/******************************************************************************
 * AppleSmartBattery::setBatterySTA
 *
 ******************************************************************************/

IOReturn AppleSmartBattery::setBatterySTA(UInt32 battery_status)
{
    DEBUG_LOG("AppleSmartBattery::setBatterySTA: battery_status = 0x%x\n", (unsigned int) battery_status);
    
	if (battery_status & BATTERY_PRESENT) 
	{
		fBatteryPresent = true;
		setBatteryInstalled(fBatteryPresent);
	}
	else 
	{
		fBatteryPresent = false;
		setBatteryInstalled(fBatteryPresent);
	}
	
	return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBattery::setBatteryBIF
 *
 ******************************************************************************/
/*
 * _BIF (Battery InFormation)
 * Arguments: none
 * Results  : package _BIF (Battery InFormation)
 * Package {
 * 	// ASCIIZ is ASCII character string terminated with a 0x00.
 * 	Power Unit						//DWORD     0x00
 * 	Design Capacity					//DWORD     0x01
 * 	Last Full Charge Capacity		//DWORD     0x02
 * 	Battery Technology				//DWORD     0x03
 * 	Design Voltage					//DWORD     0x04
 * 	Design Capacity of Warning		//DWORD     0x05
 * 	Design Capacity of Low			//DWORD     0x06
 * 	Battery Capacity Granularity 1	//DWORD     0x07
 * 	Battery Capacity Granularity 2	//DWORD     0x08
 * 	Model Number					//ASCIIZ    0x09
 * 	Serial Number					//ASCIIZ    0x0A
 * 	Battery Type					//ASCIIZ    0x0B
 * 	OEM Information					//ASCIIZ    0x0C
 *  Cycle Count                     //DWORD (//rehabman: this is zprood's extension!!)  0x0D
 *  Battery Temperatue              //DWORD (//rehabman: this is rehabman extension!!)  0x0E
 * }
 */

IOReturn AppleSmartBattery::setBatteryBIF(OSArray *acpibat_bif)
{
    DEBUG_LOG("AppleSmartBattery::setBatteryBIF: acpibat_bif size = %d\n", acpibat_bif->getCapacity());
    
	fPowerUnit			= GetValueFromArray (acpibat_bif, BIF_POWER_UNIT);
	fDesignCapacity		= GetValueFromArray (acpibat_bif, BIF_DESIGN_CAPACITY);
	fMaxCapacity		= GetValueFromArray (acpibat_bif, BIF_LAST_FULL_CAPACITY);
	fBatteryTechnology	= GetValueFromArray (acpibat_bif, BIF_TECHNOLOGY);
	fDesignVoltage		= GetValueFromArray (acpibat_bif, BIF_DESIGN_VOLTAGE);
    fCapacityWarning    = GetValueFromArray (acpibat_bif, BIF_CAPACITY_WARNING);
    fLowWarning         = GetValueFromArray (acpibat_bif, BIF_LOW_WARNING);
	fDeviceName			= GetSymbolFromArray(acpibat_bif, BIF_MODEL_NUMBER);
	fSerialNumber		= GetSymbolFromArray(acpibat_bif, BIF_SERIAL_NUMBER);					 
	fType				= GetSymbolFromArray(acpibat_bif, BIF_BATTERY_TYPE);
	fManufacturer		= GetSymbolFromArray(acpibat_bif, BIF_OEM);

	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fPowerUnit       = 0x%x\n", (unsigned)fPowerUnit);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fDesignCapacity  = %d\n", (int)fDesignCapacity);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fMaxCapacity     = %d\n", (int)fMaxCapacity);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fBatteryTech     = 0x%x\n", (unsigned)fBatteryTechnology);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fDesignVoltage   = %d\n", (int)fDesignVoltage);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fCapacityWarning = %d\n", (int)fCapacityWarning);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fLowWarning      = %d\n", (int)fLowWarning);
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fDeviceName      = '%s'\n", fDeviceName->getCStringNoCopy());
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fSerialNumber    = '%s'\n", fSerialNumber->getCStringNoCopy());
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fType            = '%s'\n", fType->getCStringNoCopy());
	DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fManufacturer    = '%s'\n", fManufacturer->getCStringNoCopy());
    
	if (WATTS == fPowerUnit && fDesignVoltage)
    {
        // Watts = Amps X Volts
        fDesignCapacity = (fDesignCapacity * 1000) / fDesignVoltage;
        fMaxCapacity = (fMaxCapacity * 1000) / fDesignVoltage;
        fCapacityWarning = (fCapacityWarning * 1000) / fDesignVoltage;
        fLowWarning = (fLowWarning * 1000) / fDesignVoltage;
	}
	
	if ((fDesignCapacity == 0) || (fMaxCapacity == 0))  {
		logReadError(kErrorZeroCapacity, 0, NULL);
	}
	
	setDesignCapacity(fDesignCapacity);
	setMaxCapacity(fMaxCapacity);
	setDeviceName(fDeviceName);
	setSerialNumber(fSerialNumber);
	setBatteryType(fType);
	setManufacturer(fManufacturer);
    
    //rehabman: added to get battery status to show
    setBatteryInstalled(true);
    setExternalChargeCapable(true);
    setSerial(fSerialNumber);
    
    //rehabman: zprood's technique of expanding the _BIF to include cycle count
    uint32_t cycleCnt = 0;
    if (acpibat_bif->getCount() > BIF_CYCLE_COUNT)
        cycleCnt = GetValueFromArray(acpibat_bif, BIF_CYCLE_COUNT);
    else if (fDesignCapacity > fMaxCapacity && fEstimateCycleCountDivisor)
        cycleCnt = (fDesignCapacity - fMaxCapacity) / fEstimateCycleCountDivisor;
    setCycleCount(cycleCnt);
    
    //rehabman: getting temperature from extended _BIF
    fTemperature = -1;
    if (acpibat_bif->getCount() > BIF_TEMPERATURE) {
        fTemperature = GetValueFromArray(acpibat_bif, BIF_TEMPERATURE);
        DEBUG_LOG("AppleSmartBattery::setBatteryBIF: fTemperature = %d (0.1K)\n", (unsigned)fTemperature);
    }
    if (-1 == fTemperature || 0 == fTemperature)
        fTemperature = 2731; // 2731(.1K) == 0 degrees C
    setTemperature((fTemperature - 2731) * 10);
    
	// ACPI _BIF doesn't provide these
	setMaxErr(0);
	setManufactureDate(0);
    
    //rehabman: removed this code to get battery status to show in System Report
/*
	fManufacturerData = OSData::withCapacity(10);
	setManufacturerData((uint8_t *)fManufacturerData, fManufacturerData->getLength());
*/
	setPermanentFailureStatus(0);
	
	return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBattery::setBatteryBIX
 *
 ******************************************************************************/
/*
 * _BIX (Battery InFormation eXtended)
 * Arguments: none
 * Results  : package _BIX (Battery InFormation Extended)
 * Package { 
 *   // ASCIIZ is ASCII character string terminated with a 0x00.
 *   Revision							// Integer
 *   Power Unit							// Integer (DWORD)
 *   Design Capacity					// Integer (DWORD)
 *   Last Full Charge Capacity			// Integer (DWORD)
 *   Battery Technology					// Integer (DWORD)
 *   Design Voltage						// Integer (DWORD)
 *   Design Capacity of Warning			// Integer (DWORD)
 *   Design Capacity of Low				// Integer (DWORD)
 *   Cycle Count						// Integer (DWORD)
 *   Measurement Accuracy				// Integer (DWORD)
 *   Max Sampling Time					// Integer (DWORD)
 *   Min Sampling Time					// Integer (DWORD)
 *   Max Averaging Interval				// Integer (DWORD)
 *   Min Averaging Interval				// Integer (DWORD)
 *   Battery Capacity Granularity 1		// Integer (DWORD)
 *   Battery Capacity Granularity 2		// Integer (DWORD)
 *   Model Number						// String (ASCIIZ)
 *   Serial Number						// String (ASCIIZ)
 *   Battery Type						// String (ASCIIZ)
 *   OEM Information					// String (ASCIIZ)
 * }
 */

IOReturn AppleSmartBattery::setBatteryBIX(OSArray *acpibat_bix)
{
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: acpibat_bix size = %d\n", acpibat_bix->getCapacity());
    
	fPowerUnit			= GetValueFromArray (acpibat_bix, BIX_POWER_UNIT);
	fDesignCapacity		= GetValueFromArray (acpibat_bix, BIX_DESIGN_CAPACITY);
	fMaxCapacity		= GetValueFromArray (acpibat_bix, BIX_LAST_FULL_CAPACITY);
	fBatteryTechnology	= GetValueFromArray (acpibat_bix, BIX_TECHNOLOGY);
	fDesignVoltage		= GetValueFromArray (acpibat_bix, BIX_DESIGN_VOLTAGE);
    fCapacityWarning    = GetValueFromArray (acpibat_bix, BIX_CAPACITY_WARNING);
    fLowWarning         = GetValueFromArray (acpibat_bix, BIX_LOW_WARNING);
	fCycleCount			= GetValueFromArray (acpibat_bix, BIX_CYCLE_COUNT);
	fMaxErr				= GetValueFromArray (acpibat_bix, BIX_ACCURACY);
	fDeviceName			= GetSymbolFromArray(acpibat_bix, BIX_MODEL_NUMBER);
	fSerialNumber		= GetSymbolFromArray(acpibat_bix, BIX_SERIAL_NUMBER);					 
	fType				= GetSymbolFromArray(acpibat_bix, BIX_BATTERY_TYPE);
	fManufacturer		= GetSymbolFromArray(acpibat_bix, BIX_OEM);
	
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fPowerUnit       = 0x%x\n", (unsigned)fPowerUnit);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fDesignCapacity  = %d\n", (int)fDesignCapacity);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fMaxCapacity     = %d\n", (int)fMaxCapacity);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fBatteryTech     = 0x%x\n", (unsigned)fBatteryTechnology);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fDesignVoltage   = %d\n", (int)fDesignVoltage);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fCapacityWarning = %d\n", (int)fCapacityWarning);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fLowWarning      = %d\n", (int)fLowWarning);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fCycleCount      = %d\n", (int)fCycleCount);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fMaxErr          = %d\n", (int)fMaxErr);
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fDeviceName      = '%s'\n", fDeviceName->getCStringNoCopy());
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fSerialNumber    = '%s'\n", fSerialNumber->getCStringNoCopy());
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fType            = '%s'\n", fType->getCStringNoCopy());
    DEBUG_LOG("AppleSmartBattery::setBatteryBIX: fManufacturer    = '%s'\n", fManufacturer->getCStringNoCopy());

    if (WATTS == fPowerUnit && fDesignVoltage)
    {
        // Watts = Amps X Volts
        fDesignCapacity = (fDesignCapacity * 1000) / fDesignVoltage;
        fMaxCapacity = (fMaxCapacity * 1000) / fDesignVoltage;
        fCapacityWarning = (fCapacityWarning * 1000) / fDesignVoltage;
        fLowWarning = (fLowWarning * 1000) / fDesignVoltage;
	}
	
	if ((fDesignCapacity == 0) || (fMaxCapacity == 0))  {
		logReadError(kErrorZeroCapacity, 0, NULL);
	}
	
	setDesignCapacity(fDesignCapacity);
	setMaxCapacity(fMaxCapacity);
	setDeviceName(fDeviceName);
	setSerialNumber(fSerialNumber);
	setBatteryType(fType);
	setManufacturer(fManufacturer);
	setCycleCount(fCycleCount);
    
    //REVIEW_REHABMAN: Not sure it makes sense to set MaxErr based on BIF_ACCURACY
	//setMaxErr(fMaxErr);
    setMaxErr(0);
	
	// ACPI _BIX doesn't provide these...
	
	setManufactureDate(0);

    //rehabman: removed this code to get battery status to show in System Report
/*
	fManufacturerData = OSData::withCapacity(10);
	setManufacturerData((uint8_t *)fManufacturerData, fManufacturerData->getLength());
*/
	setPermanentFailureStatus(0);
	
	return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBattery::setBatteryBBIX
 *
 ******************************************************************************/
/*
 * BBIX (Battery InFormation Extra)
 * Arguments: none
 * Results  : package BBIX (Battery InFormation Extra)
 Name (PBIG, Package ()
 {
 0x00000000, // 0x00, ManufacturerAccess() - WORD - ?
 0x00000000, // 0x01, BatteryMode() - WORD - unsigned int
 0xFFFFFFFF, // 0x02, AtRateTimeToFull() - WORD - unsigned int (min)
 0xFFFFFFFF, // 0x03, AtRateTimeToEmpty() - WORD - unsigned int (min)
 0x00000000, // 0x04, Temperature() - WORD - unsigned int (0.1K)
 0x00000000, // 0x05, Voltage() - WORD - unsigned int (mV)
 0x00000000, // 0x06, Current() - WORD - signed int (mA)
 0x00000000, // 0x07, AverageCurrent() - WORD - signed int (mA)
 0x00000000, // 0x08, RelativeStateOfCharge() - WORD - unsigned int (%)
 0x00000000, // 0x09, AbsoluteStateOfCharge() - WORD - unsigned int (%)
 0x00000000, // 0x0a, RemaingingCapacity() - WORD - unsigned int (mAh or 10mWh)
 0xFFFFFFFF, // 0x0b, RunTimeToEmpty() - WORD - unsigned int (min)
 0xFFFFFFFF, // 0x0c, AverageTimeToEmpty() - WORD - unsigned int (min)
 0xFFFFFFFF, // 0x0d, AverageTimeToFull() - WORD - unsigned int (min)
 0x00000000, // 0x0e, ManufactureDate() - WORD - unsigned int (packed date)
 " "         // 0x0f, ManufacturerData() - BLOCK - Unknown
 })
 */

IOReturn AppleSmartBattery::setBatteryBBIX(OSArray *acpibat_bbix)
{
    DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: acpibat_bbix size = %d\n", acpibat_bbix->getCapacity());
    
	fManufacturerAccess		= GetValueFromArray (acpibat_bbix, BBIX_MANUF_ACCESS);
	fBatteryMode			= GetValueFromArray (acpibat_bbix, BBIX_BATTERYMODE);
	fAtRateTimeToFull		= GetValueFromArray (acpibat_bbix, BBIX_ATRATETIMETOFULL);
	fAtRateTimeToEmpty		= GetValueFromArray (acpibat_bbix, BBIX_ATRATETIMETOEMPTY);
	fTemperature			= GetValueFromArray (acpibat_bbix, BBIX_TEMPERATURE);
	fVoltage				= GetValueFromArray (acpibat_bbix, BBIX_VOLTAGE);
	fCurrent				= GetValueFromArray (acpibat_bbix, BBIX_CURRENT);
	fAverageCurrent			= GetValueFromArray (acpibat_bbix, BBIX_AVG_CURRENT);
	fRelativeStateOfCharge	= GetValueFromArray (acpibat_bbix, BBIX_REL_STATE_CHARGE);					 
	fAbsoluteStateOfCharge	= GetValueFromArray (acpibat_bbix, BBIX_ABS_STATE_CHARGE);
	fRemainingCapacity		= GetValueFromArray (acpibat_bbix, BBIX_REMAIN_CAPACITY);
	fRunTimeToEmpty			= GetValueFromArray (acpibat_bbix, BBIX_RUNTIME_TO_EMPTY);
	fAverageTimeToEmpty		= GetValueFromArray (acpibat_bbix, BBIX_AVG_TIME_TO_EMPTY);
	fAverageTimeToFull		= GetValueFromArray (acpibat_bbix, BBIX_AVG_TIME_TO_FULL);
	fManufactureDate		= GetValueFromArray (acpibat_bbix, BBIX_MANUF_DATE);
	fManufacturerData		= GetDataFromArray  (acpibat_bbix, BBIX_MANUF_DATA);
	
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fManufacturerAccess    = 0x%x\n", (unsigned)fManufacturerAccess);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fBatteryMode           = 0x%x\n", (unsigned)fBatteryMode);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fAtRateTimeToFull      = %d (min)\n", (int)fAtRateTimeToFull);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fAtRateTimeToEmpty     = %d (min)\n", (int)fAtRateTimeToEmpty);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fTemperature           = %d (0.1K)\n", (int)fTemperature);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fVoltage               = %d (mV)\n", (int)fVoltage);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fCurrent               = %d (mA)\n", (int)fCurrent);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fAverageCurrent        = %d (mA)\n", (int)fAverageCurrent);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fRelativeStateOfCharge = %d (%%)\n", (int)fRelativeStateOfCharge);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fAbsoluteStateOfCharge = %d (%%)\n", (int)fAbsoluteStateOfCharge);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fRemainingCapacity     = %d (mAh)\n", (int)fRemainingCapacity);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fRunTimeToEmpty        = %d (min)\n", (int)fRunTimeToEmpty);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fAverageTimeToEmpty    = %d (min)\n", (int)fAverageTimeToEmpty);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fAverageTimeToFull     = %d (min)\n", (int)fAverageTimeToFull);
	DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fManufactureDate       = 0x%x\n", (unsigned) fManufactureDate);
    DEBUG_LOG("AppleSmartBattery::setBatteryBBIX: fManufacturerData size = 0x%x\n", (unsigned) (fManufacturerData ? fManufacturerData->getLength() : -1));
	
    // temperature must be converted from .1K to .01 degrees C
    if (-1 == fTemperature || 0 == fTemperature)
        fTemperature = 2731;
	setTemperature((fTemperature - 2731) * 10);
    
	setManufactureDate(fManufactureDate);
	
	const OSSymbol *manuDate = this->unpackDate(fManufactureDate);
	if (manuDate) {
		setPSProperty(_DateOfManufacture, const_cast<OSSymbol*>(manuDate));
		manuDate->release();
	}
	
	setRunTimeToEmpty(fRunTimeToEmpty);
	setRelativeStateOfCharge(fRelativeStateOfCharge);
	setAbsoluteStateOfCharge(fAbsoluteStateOfCharge);
	setRemainingCapacity(fRemainingCapacity);
	setAverageCurrent(fAverageCurrent);
	setCurrent(fCurrent);
    if (fManufacturerData)
        setManufacturerData((uint8_t *)fManufacturerData, fManufacturerData->getLength());
	
	return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBattery::setBatteryBST
 *
 ******************************************************************************/
/*
 * _BST (Battery STatus)
 * Arguments: none
 * Results  : package _BST (Battery STatus)
 * Package {
 * 	Battery State				//DWORD
 * 	Battery Present Rate		//DWORD
 * 	Battery Remaining Capacity	//DWORD
 * 	Battery Present Voltage		//DWORD
 * }
 *
 * Battery State:
 * --------------
 * Bit values. Notice that the Charging bit and the Discharging bit are mutually exclusive and must not both be set at the same time. 
 * Even in critical state, hardware should report the corresponding charging/discharging state.
 *
 * Bit0 – 1 indicates the battery is discharging. 
 * Bit1 – 1 indicates the battery is charging. 
 * Bit2 – 1 indicates the battery is in the critical energy state (see section 3.9.4, “Low Battery Levels”). This does not mean battery failure.
 *
 * Battery Present Rate:
 * ---------------------
 *
 * Returns the power or current being supplied or accepted through the battery’s terminals (direction depends on the Battery State value). 
 * The Battery Present Rate value is expressed as power [mWh] or current [mAh] depending on the Power Unit value.
 * Batteries that are rechargeable and are in the discharging state are required to return a valid Battery Present Rate value.
 * 
 * 0x00000000 – 0x7FFFFFFF in [mW] or [mA] 
 * 0xFFFFFFFF – Unknown rate
 *
 * Battery Remaining Capacity:
 * --------------------------
 *
 * Returns the estimated remaining battery capacity. The Battery Remaining Capacity value is expressed as power [mWh] or current [mAh] 
 * depending on the Power Unit value.
 * Batteries that are rechargeable are required to return a valid Battery Remaining Capacity value.
 * 
 * 0x00000000 – 0x7FFFFFFF in [mWh] or [mAh] 
 * 0xFFFFFFFF – Unknown capacity
 *
 * Battery Present Voltage:
 * -----------------------
 * 
 * Returns the voltage across the battery’s terminals. Batteries that are rechargeable must report Battery Present Voltage.
 * 
 * 0x000000000 – 0x7FFFFFFF in [mV] 
 * 0xFFFFFFFF – Unknown voltage
 * 
 * Note: Only a primary battery can report unknown voltage.
 */

IOReturn AppleSmartBattery::setBatteryBST(OSArray *acpibat_bst)
{
    DEBUG_LOG("AppleSmartBattery::setBatteryBST: acpibat_bst size = %d\n", acpibat_bst->getCapacity());
    
	// Get the values from the ACPI array
	
	UInt32 currentStatus = GetValueFromArray(acpibat_bst, BST_STATUS);
	fCurrentRate		 = GetValueFromArray(acpibat_bst, BST_RATE);
	fCurrentCapacity	 = GetValueFromArray(acpibat_bst, BST_CAPACITY);
	fCurrentVoltage		 = GetValueFromArray(acpibat_bst, BST_VOLTAGE);
	
	DEBUG_LOG("AppleSmartBattery::setBatteryBST: fPowerUnit       = 0x%x\n", (unsigned)fPowerUnit);
	DEBUG_LOG("AppleSmartBattery::setBatteryBST: currentStatus    = 0x%x\n", (unsigned)currentStatus);
	DEBUG_LOG("AppleSmartBattery::setBatteryBST: fCurrentRate     = %d\n", (int)fCurrentRate);
	DEBUG_LOG("AppleSmartBattery::setBatteryBST: fCurrentCapacity = %d\n", (int)fCurrentCapacity);
	DEBUG_LOG("AppleSmartBattery::setBatteryBST: fCurrentVoltage  = %d\n", (int)fCurrentVoltage);
    
	if (fCurrentRate == ACPI_UNKNOWN)
    {
		DEBUG_LOG("AppleSmartBattery::setBatteryBST: fCurrentRate is ACPI_UNKNOWN\n");
    }
    else if (WATTS == fPowerUnit && fDesignVoltage)
    {
        // Watts = Amps X Volts
        DEBUG_LOG("AppleSmartBattery::setBatteryBST: Calculating for WATTS\n");
        fCurrentRate = ((int)fCurrentRate * 1000) / fDesignVoltage;
        fCurrentCapacity = (fCurrentCapacity * 1000) / fDesignVoltage;
        DEBUG_LOG("AppleSmartBattery::setBatteryBST: fCurrentRate = %d\n", (int)fCurrentRate);
        DEBUG_LOG("AppleSmartBattery::setBatteryBST: fCurrentCapacity = %d\n",	(int)fCurrentCapacity);
    }

//REVIEW_REHABMAN: why only use lower 16-bits?
	if (fCurrentRate & 0x8000)
    {
		fCurrentRate = 0xFFFF - (fCurrentRate & 0xFFFF);
		DEBUG_LOG("AppleSmartBattery::setBatteryBST: adjusted fCurrentRate to %d\n", (int)fCurrentRate);
	}

    setCurrentCapacity(fCurrentCapacity);
    setVoltage(fCurrentVoltage);

	if (fAverageRate)
		fAverageRate = (fAverageRate + fCurrentRate) / 2;
	else
		fAverageRate = fCurrentRate;
	
	DEBUG_LOG("AppleSmartBattery::setBatteryBST: fAverageRate = %d\n", fAverageRate);
	
	if (currentStatus ^ fStatus) 
	{
		// The battery has changed states
		fStatus = currentStatus;
		fAverageRate = 0;
	}
	
	if ((currentStatus & BATTERY_DISCHARGING) && (currentStatus & BATTERY_CHARGING)) 
	{		
		// This should NEVER happen but...
		
		const OSSymbol *permanentFailureSym = OSSymbol::withCString(kErrorPermanentFailure);
		logReadError( kErrorPermanentFailure, 0, NULL);
		setErrorCondition( (OSSymbol *)permanentFailureSym );
		permanentFailureSym->release();
		
		/* We want to display the battery as present & completely discharged, not charging */
		setFullyCharged(false);
		setIsCharging(false);
		
		fACConnected = true;
		setExternalConnected(fACConnected);
		fACChargeCapable = false;
		setExternalChargeCapable(fACChargeCapable);
		
		setAmperage(0);
		setInstantAmperage(0);
		
		setTimeRemaining(0);
		setAverageTimeToEmpty(0);
		setAverageTimeToFull(0);
		setInstantaneousTimeToFull(0);
		setInstantaneousTimeToEmpty(0);
		
		DEBUG_LOG("AppleSmartBattery: Battery Charging and Discharging?\n");
	} 
	else if (currentStatus & BATTERY_DISCHARGING) 
	{
		setFullyCharged(false);
		setIsCharging(false);
		
		fACConnected = false;
		setExternalConnected(fACConnected);
		fACChargeCapable = false;
		setExternalChargeCapable(fACChargeCapable);
		
		setAmperage(fAverageRate * -1);
		setInstantAmperage(fCurrentRate * -1);
		
		if (fAverageRate)
            setTimeRemaining((60 * fCurrentCapacity) / fAverageRate);
		else
            setTimeRemaining(0xffff);
		
		if (fAverageRate)
            setAverageTimeToEmpty((60 * fCurrentCapacity) / fAverageRate);
		else
            setAverageTimeToEmpty(0xffff);
		
		if (fCurrentRate)
            setInstantaneousTimeToEmpty((60 * fCurrentCapacity) / fCurrentRate);
		else
            setInstantaneousTimeToEmpty(0xffff);

		setAverageTimeToFull(0xffff);
		setInstantaneousTimeToFull(0xffff);		
		
		DEBUG_LOG("AppleSmartBattery: Battery is discharging.\n");
	} 
	else if (currentStatus & BATTERY_CHARGING) 
	{
		setFullyCharged(false);
		setIsCharging(true);
		
		fACConnected = true;
		setExternalConnected(fACConnected);
		fACChargeCapable = true;
		setExternalChargeCapable(fACChargeCapable);
		
		setAmperage(fAverageRate);
		setInstantAmperage(fCurrentRate);
		
		if (fAverageRate)	
			setTimeRemaining((60 * (fMaxCapacity - fCurrentCapacity)) / fAverageRate);
		else
			setTimeRemaining(0xffff);
		
		if (fAverageRate)
			setAverageTimeToFull((60 * (fMaxCapacity - fCurrentCapacity)) / fAverageRate);
		else
			setAverageTimeToFull(0xffff);
		
		if (fCurrentRate)
			setInstantaneousTimeToFull((60 * (fMaxCapacity - fCurrentCapacity)) / fCurrentRate);
		else
			setInstantaneousTimeToFull(0xffff);
		
		setAverageTimeToEmpty(0xffff);
		setInstantaneousTimeToEmpty(0xffff);
		
		DEBUG_LOG("AppleSmartBattery: Battery is charging.\n");
	} 
	else 
	{	// BATTERY_CHARGED
		setFullyCharged(true);
		setIsCharging(false);
		
        bool batteriesDischarging = fTracker && fTracker->anyBatteriesDischarging(this);
		fACConnected = !batteriesDischarging;
		setExternalConnected(fACConnected);
		fACChargeCapable = !batteriesDischarging;
		setExternalChargeCapable(fACChargeCapable);
		
		setAmperage(0);
		setInstantAmperage(0);
		
		setTimeRemaining(0xffff);
		setAverageTimeToFull(0xffff);
		setAverageTimeToEmpty(0xffff);
		setInstantaneousTimeToFull(0xffff);
		setInstantaneousTimeToEmpty(0xffff);

        //rehabman: This code causes the battery to go to 100% even if it is not charged to 100%.
        //
        //  Not completely charged and not currently charging is perfectly normal:
        //    situation 1: battery is not depleted enough to cause a charge
        //       (ie. battery is 3% discharged, and you plug it back in... the battery will not
        //        charge to 100%, instead staying at 97% to keep cycles on the battery to a
        //        minimum)
        //    situation 2: battery might be getting hot, so the charger may stop charging it
        //    situation 3: the battery might be broken, so the charger stops charging it
        //    situation 4: broken DSDT code causing bad data to be returned
        
#if 0
		fCurrentCapacity = fMaxCapacity;
		setCurrentCapacity(fCurrentCapacity);
#endif
		
		DEBUG_LOG("AppleSmartBattery: Battery is charged.\n");
	}
	
	if (!fPollingOverridden && fMaxCapacity) {
		/*
		 * Conditionally set polling interval to 1 second if we're
		 *     discharging && below 5% && on AC power
		 * i.e. we're doing an Inflow Disabled discharge
		 */
		if ((((100*fCurrentCapacity) / fMaxCapacity) < 5) && fACConnected) {
			setProperty("Quick Poll", true);
			fPollingInterval = kQuickPollInterval;
		} else {
			setProperty("Quick Poll", false);
			fPollingInterval = kDefaultPollInterval;
		}
	}

    //rehabman: set warning/critical flags
    setAtWarnLevel(-1 != fCapacityWarning && fCurrentCapacity <= fCapacityWarning);
    setAtCriticalLevel(-1 != fLowWarning && fCurrentCapacity <= fLowWarning);
	
	// Assumes 4 cells but Smart Battery standard does not provide count to do this dynamically. 
	// Smart Battery can expose manufacturer specific functions, but they will be specific to the embedded battery controller
	
	fCellVoltages = OSArray::withCapacity(4); 
	
	fCellVoltage1 = fCurrentVoltage / 4;
    OSNumber* num = OSNumber::withNumber((unsigned long long)fCellVoltage1 , NUM_BITS);
    fCellVoltages->setObject(num);
	
	fCellVoltage2 = fCurrentVoltage / 4;
    num = OSNumber::withNumber((unsigned long long)fCellVoltage2 , NUM_BITS);
    fCellVoltages->setObject(num);
	
	fCellVoltage3 = fCurrentVoltage / 4;
    num = OSNumber::withNumber((unsigned long long)fCellVoltage3 , NUM_BITS);
    fCellVoltages->setObject(num);
	
	fCellVoltage4 = fCurrentVoltage - fCellVoltage1 - fCellVoltage2 - fCellVoltage3;
    num = OSNumber::withNumber((unsigned long long)fCellVoltage4 , NUM_BITS);
    fCellVoltages->setObject(num);
	
	setProperty("CellVoltage", fCellVoltages);

//REVIEW: no need to set this as it is never updated here...
//	setProperty("Temperature", (long long unsigned int)fTemperature, NUM_BITS);
	
	/* construct and publish our battery serial number here */
	constructAppleSerialNumber();
	
#ifdef REVIEW
	/* Cancel read-completion timeout; Successfully read battery state */
	fBatteryReadAllTimer->cancelTimeout();
#endif
	
	rebuildLegacyIOBatteryInfo(true);
	
	updateStatus();
	
	return kIOReturnSuccess;
}

IOReturn AppleSmartBattery::setPowerState(unsigned long which, IOService *whom)
{
	// 64-bit requires this method to be implemented but we can't actually set the power
	// state of the battery so we'll just return indicating we handled it...
	
	return IOPMAckImplied;
} 

UInt32 GetValueFromArray(OSArray * array, UInt8 index) 
{
	OSObject *object = array->getObject(index);
	
	if (object && (OSTypeIDInst(object) == OSTypeID(OSNumber))) 
	{
		OSNumber * number = OSDynamicCast(OSNumber, object);
		if (number) 
			return number->unsigned32BitValue();
	}
	return 0;
}

OSData *GetDataFromArray(OSArray *array, UInt8 index)
{
	OSObject *object = array->getObject(index);
	
	if(object && (OSTypeIDInst(object) == OSTypeID(OSString)))
	{
		OSString *oString = OSDynamicCast(OSString, object);
		if(oString)
		{
			OSData *osData = OSData::withBytes(oString->getCStringNoCopy(), oString->getLength());
			if(osData)
				return osData;
		}
	}
	
	if(object && (OSTypeIDInst(object) == OSTypeID(OSData)))
	{
		OSData *osData = OSDynamicCast(OSData, object);
		if(osData)
			return osData;
	}
	
	return (OSData *) NULL;
}

OSSymbol *GetSymbolFromArray(OSArray *array, UInt8 index) 
{
	const OSMetaClass *typeID;
    char stringBuf[255];
	
    typeID = OSTypeIDInst(array->getObject(index));
    
	if (typeID == OSTypeID(OSString)) 
	{
        OSString *osString = OSDynamicCast(OSString, array->getObject(index));
        
		return (OSSymbol *)OSSymbol::withString(osString);
	} 
	else if (typeID == OSTypeID(OSData)) 
	{
        OSData *osData = OSDynamicCast(OSData, array->getObject(index));

        bzero(stringBuf, sizeof(stringBuf));
		snprintf(stringBuf, sizeof(stringBuf), "%s", (char *)osData->getBytesNoCopy(0,osData->getLength()));
        
		return (OSSymbol *)OSSymbol::withCString(stringBuf);
	}

	return (OSSymbol *)unknownObjectKey;
}
