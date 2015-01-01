//
//  ACAdapter.cpp
//  ACPIBatteryManager
//
//  Created by RehabMan on 11/16/13.
//
//  Code/ideas liberally borrowed from VoodooBattery...
//

#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include "IOPMPrivate.h"
#include "ACAdapter.h"

OSDefineMetaClassAndStructors(ACPIACAdapter, IOService)

bool ACPIACAdapter::init(OSDictionary* dict)
{
    if (!IOService::init(dict))
        return false;
    
    fTracker = NULL;
    fProvider = NULL;
    
    return true;
}

bool ACPIACAdapter::start(IOService* provider)
{
    if (!IOService::start(provider))
    {
        IOLog("ACPIACAdapter: IOService::start failed!\n");
        return false;
    }
    
    fProvider = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!fProvider)
    {
        IOLog("ACPIACAdapter: provider not IOACPIPlatformDevice\n");
        return false;
    }
    fProvider->retain();
    
	DEBUG_LOG("ACPIBatteryManager::start Version 1.55 starting ACPIACAdapter.\n");
    
    // get tracker for notifications
    fTracker = OSDynamicCast(BatteryTracker, waitForMatchingService(serviceMatching(kBatteryTrackerService)));
    
    return true;
}

void ACPIACAdapter::stop(IOService* provider)
{
    OSSafeReleaseNULL(fProvider);
    OSSafeReleaseNULL(fTracker);
    
    IOService::stop(provider);
}

IOReturn ACPIACAdapter::setPowerState(unsigned long state, IOService* device)
{
    DEBUG_LOG("ACPIACAdapter::setPowerState: state: %u, device: %s\n", (unsigned int) state, device->getName());
    
    return kIOPMAckImplied;
}

IOReturn ACPIACAdapter::message(UInt32 type, IOService* provider, void* argument)
{
    DEBUG_LOG("ACPIACAdapter::message: type: %08X provider: %s\n", (unsigned int)type, provider->getName());
    
    if (type == kIOACPIMessageDeviceNotification && fProvider)
    {
        UInt32 acpi = 0;
        if (kIOReturnSuccess == fProvider->evaluateInteger("_PSR", &acpi))
        {
            DEBUG_LOG("ACPIACAdapter::message setting AC %s\n", (acpi ? "connected" : "disconnected"));
            
            // notify system of change in AC state
            if (IOPMrootDomain* root = getPMRootDomain())
                root->receivePowerNotification(kIOPMSetACAdaptorConnected | acpi ? kIOPMSetValue : 0);
            else
                DEBUG_LOG("ACPIACAdapter::message could not notify OS about AC status\n");
            
            // notify battery managers of change in AC state
            if (NULL != fTracker)
                fTracker->notifyBatteryManagers(acpi);
        }
        else
        {
            IOLog("ACPIACAdapter: ACPI method _PSR failed\n");
        }
    }
    return kIOReturnSuccess;
}

