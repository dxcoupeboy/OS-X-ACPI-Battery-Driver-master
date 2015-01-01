//
//  BatteryTracker.cpp
//  ACPIBatteryManager
//
//  Created by RehabMan on 12/3/13.
//
//

#include "BatteryTracker.h"
#include <IOKit/IOCommandGate.h>

OSDefineMetaClassAndStructors(BatteryTracker, IOService)

bool BatteryTracker::init(OSDictionary* dict)
{
    DEBUG_LOG("BatteryTracker::start: entering init\n");
    
    if (!IOService::init(dict))
    {
        IOLog("BatteryTracker: IOService::init failed!\n");
        return false;
    }
    
    m_pBatteryList = NULL;
    m_pLock = NULL;
    m_pCmdGate = NULL;
    
    return true;
}

bool BatteryTracker::start(IOService* provider)
{
    DEBUG_LOG("BatteryTracker::start: entering start\n");
    
    if (!IOService::start(provider))
    {
        IOLog("BatteryTracker: IOService::start failed!\n");
        return false;
    }
    
    IOWorkLoop* workLoop = getWorkLoop();
    if (!workLoop)
    {
        IOLog("BatteryTracker: getWorkLoop failed\n");
        return false;
    }
    m_pCmdGate = IOCommandGate::commandGate(this);
    if (!m_pCmdGate)
    {
        IOLog("BatteryTracker: IOCommandGate::commmandGate failed\n");
        return false;
    }
    workLoop->addEventSource(m_pCmdGate);
    
	DEBUG_LOG("ACPIBatteryManager: Version 1.55 starting BatteryTracker.\n");
    
    m_pBatteryList = OSArray::withCapacity(2);
    m_pLock = IORecursiveLockAlloc();
    
    registerService();
    
    return true;
}

void BatteryTracker::stop(IOService* provider)
{
    DEBUG_LOG("BatteryTracker::stop: entering stop\n");
    
    OSSafeReleaseNULL(m_pBatteryList);
    if (NULL != m_pLock)
    {
        IORecursiveLockFree(m_pLock);
        m_pLock = NULL;
    }
    if (m_pCmdGate)
    {
        IOWorkLoop* pWorkLoop = getWorkLoop();
        if (pWorkLoop)
            pWorkLoop->removeEventSource(m_pCmdGate);
        m_pCmdGate->release();
        m_pCmdGate = NULL;
    }
    
    IOService::stop(provider);
}

bool BatteryTracker::addBatteryManager(AppleSmartBatteryManager* pManager)
{
    DEBUG_LOG("BatteryTracker::addBatteryManager: entering addBatteryManager(%p)\n", pManager);
    
    bool result = false;
    
    IORecursiveLockLock(m_pLock);
    unsigned count = m_pBatteryList->getCount();
    unsigned i = 0;
    for (; i < count; ++i)
    {
        OSObject* pTemp = m_pBatteryList->getObject(i);
        if (pTemp == pManager)
            break;
        if (pTemp == NULL)
        {
            result = true;
            m_pBatteryList->setObject(i, pManager);
            break;
        }
    }
    if (i == count)
    {
        m_pBatteryList->setObject(pManager);
        result = true;
    }
    IORecursiveLockUnlock(m_pLock);
    
    return result;
}

bool BatteryTracker::removeBatteryManager(AppleSmartBatteryManager* pManager)
{
    DEBUG_LOG("BatteryTracker::removeBatteryManager: entering removeBatteryManager(%p)\n", pManager);
    
    bool result = false;
    
    IORecursiveLockLock(m_pLock);
    unsigned count = m_pBatteryList->getCount();
    for (unsigned i = 0; i < count; ++i)
    {
        if (m_pBatteryList->getObject(i) == pManager)
        {
            m_pBatteryList->setObject(i, NULL);
            result = true;
            break;
        }
    }
    IORecursiveLockUnlock(m_pLock);
    
    return result;
}

void BatteryTracker::notifyBatteryManagersGated(bool connected)
{
    IORecursiveLockLock(m_pLock);
    unsigned count = m_pBatteryList->getCount();
    for (unsigned i = 0; i < count; ++i)
    {
        AppleSmartBatteryManager* pManager = static_cast<AppleSmartBatteryManager*>(m_pBatteryList->getObject(i));
        if (NULL != pManager)
            pManager->notifyConnectedState(connected);
    }
    IORecursiveLockUnlock(m_pLock);
}

void BatteryTracker::notifyBatteryManagers(bool connected)
{
    DEBUG_LOG("BatteryTracker::notifyBatteryManagers: entering notifyBatteryManager(%s)\n", connected ? "connected" : "disconnected");
    
    if (m_pCmdGate)
        m_pCmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &BatteryTracker::notifyBatteryManagersGated), (void*)connected);
}

bool BatteryTracker::anyBatteriesDischarging(AppleSmartBattery* pExcept)
{
    bool result = false;
    
    IORecursiveLockLock(m_pLock);
    unsigned count = m_pBatteryList->getCount();
    for (unsigned i = 0; i < count; ++i)
    {
        AppleSmartBatteryManager* pManager = static_cast<AppleSmartBatteryManager*>(m_pBatteryList->getObject(i));
        if (pManager && pManager->fBattery && pExcept != pManager->fBattery && pManager->fBattery->fBatteryPresent && !pManager->fBattery->fACConnected)
        {
            result = true;
            break;
        }
    }
    IORecursiveLockUnlock(m_pLock);
    
    return result;
}

