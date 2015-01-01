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

#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>

#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"

//REVIEW: avoids problem with Xcode 5.1.0 where -dead_strip eliminates these required symbols
#include <libkern/OSKextLib.h>
void* _org_rehabman_dontstrip_[] =
{
    (void*)&OSKextGetCurrentIdentifier,
    (void*)&OSKextGetCurrentLoadTag,
    (void*)&OSKextGetCurrentVersionString,
};

enum {
    kMyOnPowerState = 1
};

static IOPMPowerState myTwoStates[2] = {
    {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

OSDefineMetaClassAndStructors(rehab_ACPIBatteryManager, IOService)

#ifdef DEBUG

/******************************************************************************
 * AppleSmartBatteryManager::init
 *
 ******************************************************************************/

bool AppleSmartBatteryManager::init(OSDictionary *dict)
{
    bool result = super::init(dict);
    DEBUG_LOG("AppleSmartBatteryManager::init: Initializing\n");
    
    fTracker = NULL;
    
    return result;
}

/******************************************************************************
 * AppleSmartBatteryManager::free
 *
 ******************************************************************************/

void AppleSmartBatteryManager::free(void)
{
    DEBUG_LOG("AppleSmartBatteryManager::free: Freeing\n");
    super::free();
}

/******************************************************************************
 * AppleSmartBatteryManager::probe
 *
 ******************************************************************************/

IOService *AppleSmartBatteryManager::probe(IOService *provider,
                                                SInt32 *score)
{
    IOService *result = super::probe(provider, score);
    DEBUG_LOG("AppleSmartBatteryManager::probe: Probing\n");
    return result;
}

#endif // DEBUG

/******************************************************************************
 * AppleSmartBatteryManager::start
 *
 ******************************************************************************/

bool AppleSmartBatteryManager::start(IOService *provider)
{
    DEBUG_LOG("AppleSmartBatteryManager::start: called\n");
    
    fProvider = OSDynamicCast(IOACPIPlatformDevice, provider);

    if (!fProvider || !super::start(provider)) {
        return false;
    }

    IOWorkLoop *wl = getWorkLoop();
    if (!wl) {
        return false;
    }

    // Join power management so that we can get a notification early during
    // wakeup to re-sample our battery data. We don't actually power manage
    // any devices.
	
	PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);

    //rehabman: updated version
	IOLog("ACPIBatteryManager: Version 1.55 starting.\n");

#if 0
    //REVIEW_REHABMAN: I don't think this makes any sense...
	int value = getPlatform()->numBatteriesSupported();
	DEBUG_LOG("AppleSmartBatteryManager: Battery Supported Count(s) %d.\n", value);

    // TODO: Create battery array to hold battery objects if more than one battery in the system
	if (value > 1)
    { 
		if (kIOReturnSuccess == fProvider->evaluateInteger("_STA", &fBatterySTA)) {
			if (fBatterySTA & BATTERY_PRESENT) {
				goto populateBattery;
			} else {
				goto skipBattery;
			}
		}
	}
#endif

populateBattery:

	fBattery = AppleSmartBattery::smartBattery();

	if(!fBattery) 
		return false;

	fBattery->attach(this);

	fBattery->start(this);

    // Command gate for ACPIBatteryManager
    fManagerGate = IOCommandGate::commandGate(this);
    if (!fManagerGate) {
        return false;
    }
    wl->addEventSource(fManagerGate);

    // Command gate for ACPIBattery
    fBatteryGate = IOCommandGate::commandGate(fBattery);
    if (!fBatteryGate) {
        return false;
    }
    wl->addEventSource(fBatteryGate);

	fBattery->registerService(0);

skipBattery:

    this->setName("AppleSmartBatteryManager");
	this->registerService(0);
    
    // add to battery tracker
    fTracker = OSDynamicCast(BatteryTracker, waitForMatchingService(serviceMatching(kBatteryTrackerService)));
    if (NULL != fTracker) {
        fTracker->addBatteryManager(this);
    }

    return true;
}

/******************************************************************************
 * AppleSmartBatteryManager::stop
 ******************************************************************************/

void AppleSmartBatteryManager::stop(IOService *provider)
{
	DEBUG_LOG("AppleSmartBatteryManager::stop: called\n");

    // remove from battery tracker
    if (NULL != fTracker) {
        fTracker->removeBatteryManager(this);
        fTracker->release();
        fTracker = NULL;
    }
	
    fBattery->detach(this);
    
    fBattery->free();
    fBattery->stop(this);
    fBattery->terminate();
    fBattery = NULL;
    
    IOWorkLoop *wl = getWorkLoop();
    if (wl) {
        wl->removeEventSource(fBatteryGate);
    }

    fBatteryGate->free();
    fBatteryGate = NULL;
    
	PMstop();
    
    super::stop(provider);
}

/******************************************************************************
 * AppleSmartBatteryManager::setPollingInterval
 *
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::setPollingInterval(int milliSeconds)
{
    DEBUG_LOG("AppleSmartBatteryManager::setPollingInterval: interval = %d ms\n", milliSeconds);
    
    // Discard any negatize or zero arguments
    if (milliSeconds <= 0) return kIOReturnBadArgument;

    if (fBattery)
        fBattery->setPollingInterval(milliSeconds);

    setProperty("PollingInterval_msec", milliSeconds, 32);

    return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBatteryManager::setPowerState
 *
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::setPowerState(unsigned long which, IOService *whom)
{
	IOReturn ret = IOPMAckImplied;
	
	DEBUG_LOG("AppleSmartBatteryManager::setPowerState: which = 0x%lx\n", which);
	
    if(fBatteryGate)
	{
        // We are waking from sleep - kick off a battery read to make sure
        // our battery concept is in line with reality.
        ret = fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::handleSystemSleepWake), (void*)this, (void*)!which);
    }

    return ret;
}

/******************************************************************************
 * AppleSmartBatteryManager::message
 *
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::message(UInt32 type, IOService *provider, void *argument)
{
    UInt32 batterySTA;

	if( (kIOACPIMessageDeviceNotification == type)
        && (kIOReturnSuccess == fProvider->evaluateInteger("_STA", &batterySTA))
		&& fBatteryGate )
	{
		if (batterySTA ^ fBatterySTA) 
		{
			if (batterySTA & BATTERY_PRESENT) 
			{
				// Battery inserted
				DEBUG_LOG("AppleSmartBatteryManager: battery inserted\n");
				fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::handleBatteryInserted));
			}
			else 
			{
				// Battery removed
				DEBUG_LOG("AppleSmartBatteryManager: battery removed\n");
				fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::handleBatteryRemoved));
			}
		}
		else 
		{
            // Just an alarm; re-read battery state.
			DEBUG_LOG("AppleSmartBatteryManager: polling battery state\n");
            fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::pollBatteryState), (void*)kExistingBatteryPath);
		}
	}

    return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBatteryManager::validateBatteryBIX
 * Verify that DSDT _BIX method exists
 ******************************************************************************/
IOReturn AppleSmartBatteryManager::validateBatteryBIX(void)
{
    return fProvider->validateObject("_BIX");
}

/******************************************************************************
 * AppleSmartBatteryManager::validateBatteryBBIX
 * Verify that DSDT _BBIX method exists
 ******************************************************************************/
IOReturn AppleSmartBatteryManager::validateBatteryBBIX(void)
{
    return fProvider->validateObject("_BBIX");
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatterySTA
 * Call DSDT _STA method to return battery device status
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatterySTA(void)
{
    DEBUG_LOG("AppleSmartBatteryManager::getBatterySTA called\n");
    
    IOReturn evaluateStatus = fProvider->evaluateInteger("_STA", &fBatterySTA);
	if (evaluateStatus == kIOReturnSuccess)
    {
		return fBattery->setBatterySTA(fBatterySTA);
	}
    else 
    {
        DEBUG_LOG("AppleSmartBatteryManager::getBatterySTA: evaluateObject error 0x%x\n", evaluateStatus);
		return kIOReturnError;
	}
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBIF
 * Call DSDT _BIF method to return ACPI 3.x battery info
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBIF(void)
{
    DEBUG_LOG("AppleSmartBatteryManager::getBatteryBIF called\n");
    
    OSObject *fBatteryBIF = NULL;
    IOReturn evaluateStatus = fProvider->validateObject("_BIF");
    DEBUG_LOG("AppleSmartBatteryManager::getBatteryBIF: validateObject return 0x%x\n", evaluateStatus);
    
    evaluateStatus = fProvider->evaluateObject("_BIF", &fBatteryBIF);
	if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
        if (OSArray* acpibat_bif = OSDynamicCast(OSArray, fBatteryBIF))
        {
            setProperty("Battery Information", acpibat_bif);
            value = fBattery->setBatteryBIF(acpibat_bif);
        }
        OSSafeRelease(fBatteryBIF);
		return value;
	} 
    else 
    {
        DEBUG_LOG("AppleSmartBatteryManager::getBatteryBIF: evaluateObject error 0x%x\n", evaluateStatus);
		return kIOReturnError;
	}
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBIX
 * Call DSDT _BIX method to return ACPI 4.x battery info
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBIX(void)
{
    DEBUG_LOG("AppleSmartBatteryManager::getBatteryBIX called\n");
    
    OSObject *fBatteryBIX = NULL;
    IOReturn evaluateStatus = fProvider->evaluateObject("_BIX", &fBatteryBIX);
	if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
		if (OSArray* acpibat_bix = OSDynamicCast(OSArray, fBatteryBIX))
        {
            setProperty("Battery Extended Information", acpibat_bix);
            value = fBattery->setBatteryBIX(acpibat_bix);
        }
		OSSafeRelease(fBatteryBIX);
		return value;
	}
    else 
    {
        DEBUG_LOG("AppleSmartBatteryManager::getBatteryBIX: evaluateObject error 0x%x\n", evaluateStatus);
		return kIOReturnError;
	}
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBBIX
 * Call DSDT BBIX method to return all battery info (non-standard)
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBBIX(void)
{
    DEBUG_LOG("AppleSmartBatteryManager::getBatteryBBIX called\n");

	OSObject * fBatteryBBIX = NULL;
    IOReturn evaluateStatus = fProvider->evaluateObject("BBIX", &fBatteryBBIX);
	if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
		if (OSArray* acpibat_bbix = OSDynamicCast(OSArray, fBatteryBBIX))
        {
            setProperty("Battery Extra Information", acpibat_bbix);
            value = fBattery->setBatteryBBIX(acpibat_bbix);
        }
		OSSafeRelease(fBatteryBBIX);
		return value;
	} 
    else 
    {
        DEBUG_LOG("AppleSmartBatteryManager::getBatteryBBIX: evaluateObject error 0x%x\n", evaluateStatus);
		return kIOReturnError;
	}
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBST
 * Call DSDT _BST method to return the battery state
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBST(void)
{
    DEBUG_LOG("AppleSmartBatteryManager::getBatteryBST called\n");

	OSObject *fBatteryBST = NULL;
    IOReturn evaluateStatus = fProvider->evaluateObject("_BST", &fBatteryBST);
	if (evaluateStatus == kIOReturnSuccess)
	{
        IOReturn value = kIOReturnError;
		if (OSArray* acpibat_bst = OSDynamicCast(OSArray, fBatteryBST))
        {
            setProperty("Battery Status", acpibat_bst);
            value = fBattery->setBatteryBST(acpibat_bst);
        }
		OSSafeRelease(fBatteryBST);
		return value;
	}
	else
	{
        DEBUG_LOG("AppleSmartBatteryManager::getBatteryBST: evaluateObject error 0x%x\n", evaluateStatus);
		return kIOReturnError;
	}
}

/*****************************************************************************
 * AppleSmartBatteryManager::notifyConnectedState
 * Cause a fresh battery poll in workloop to check AC status
 ******************************************************************************/

void AppleSmartBatteryManager::notifyConnectedState(bool connected)
{
    if (NULL != fBatteryGate && NULL != fBattery)
    {
        fBattery->notifyConnectedState(connected);
    }
}

