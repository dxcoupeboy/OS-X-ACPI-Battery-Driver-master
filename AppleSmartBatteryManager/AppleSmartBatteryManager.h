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

#ifndef __AppleSmartBatteryManager__
#define __AppleSmartBatteryManager__

#define EXPORT __attribute__((visibility("default")))

#define AppleSmartBatteryManager rehab_ACPIBatteryManager
#define AppleSmartBattery rehab_ACPIBattery
#define ACPIACAdapter rehab_ACPIACAdapter
#define BatteryTracker rehab_BatteryTracker

#include <IOKit/IOService.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>

#include "AppleSmartBattery.h"
#include "BatteryTracker.h"

#ifdef DEBUG_MSG
#define DEBUG_LOG(args...)  IOLog(args)
#else
#define DEBUG_LOG(args...)
#endif

class AppleSmartBattery;
class BatteryTracker;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class EXPORT AppleSmartBatteryManager : public IOService
{
    typedef IOService super;
	OSDeclareDefaultStructors(rehab_ACPIBatteryManager)
    friend class BatteryTracker;

public:
#ifdef DEBUG
	virtual bool init(OSDictionary *dictionary = 0);
    virtual void free(void);
    virtual IOService *probe(IOService *provider, SInt32 *score);
#endif
    bool start(IOService *provider);
	void stop(IOService *provider);

    IOReturn setPowerState(unsigned long which, IOService *whom);
    IOReturn message(UInt32 type, IOService *provider, void *argument);

private:
	
    IOCommandGate           *fManagerGate;
    IOCommandGate           *fBatteryGate;
	IOACPIPlatformDevice    *fProvider;
	AppleSmartBattery       *fBattery;
    UInt32                  fBatterySTA;
    BatteryTracker          *fTracker;

	IOReturn setPollingInterval(int milliSeconds);

public:
	
    // Methods that return ACPI data into above structures
    
	IOReturn getBatterySTA(void);
	IOReturn getBatteryBIF(void);
	IOReturn getBatteryBIX(void);
	IOReturn getBatteryBBIX(void);
	IOReturn getBatteryBST(void);
    
    // Methods to test whether optional ACPI methods exist
    
    IOReturn validateBatteryBIX(void);
    IOReturn validateBatteryBBIX(void);
    
    // For AC adapter notification
    void notifyConnectedState(bool connected);
};

#endif
