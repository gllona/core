/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <svdata.hxx>
#include <tools/time.hxx>
#include <vcl/idle.hxx>
#include <saltimer.hxx>
#include <svdata.hxx>
#include <salinst.hxx>

namespace {

/**
 * clang won't compile this in the Timer.hxx header, even with a class Idle
 * forward definition, due to the incomplete Idle type in the template.
 * Currently the code is just used in the Scheduler, so we keep it local.
 *
 * @see http://clang.llvm.org/compatibility.html#undep_incomplete
 */
template< typename charT, typename traits >
inline std::basic_ostream<charT, traits> & operator <<(
    std::basic_ostream<charT, traits> & stream, const Timer& timer )
{
    bool bIsIdle = (dynamic_cast<const Idle*>( &timer ) != nullptr);
    stream << (bIsIdle ? "Idle " : "Timer")
           << " a: " << timer.IsActive() << " p: " << (int) timer.GetPriority();
    const sal_Char *name = timer.GetDebugName();
    if ( nullptr == name )
        stream << " (nullptr)";
    else
        stream << " " << name;
    if ( !bIsIdle )
        stream << " " << timer.GetTimeout() << "ms";
    stream << " (" << &timer << ")";
    return stream;
}

template< typename charT, typename traits >
inline std::basic_ostream<charT, traits> & operator <<(
    std::basic_ostream<charT, traits> & stream, const Idle& idle )
{
    return stream << static_cast<const Timer*>( &idle );
}

} // end anonymous namespace

void Scheduler::ImplDeInitScheduler()
{
    ImplSVData*     pSVData = ImplGetSVData();
    if (pSVData->mpSalTimer)
    {
        pSVData->mpSalTimer->Stop();
    }

    ImplSchedulerData* pSchedulerData = pSVData->mpFirstSchedulerData;
    while ( pSchedulerData )
    {
        if ( pSchedulerData->mpTask )
        {
            pSchedulerData->mpTask->mbActive = false;
            pSchedulerData->mpTask->mpSchedulerData = nullptr;
        }
        ImplSchedulerData* pTempSchedulerData = pSchedulerData;
        pSchedulerData = pSchedulerData->mpNext;
        delete pTempSchedulerData;
    }

    delete pSVData->mpSalTimer;
    pSVData->mpSalTimer = nullptr;

    pSVData->mpFirstSchedulerData = nullptr;
    pSVData->mpLastSchedulerData  = nullptr;
    pSVData->mnTimerPeriod        = 0;
}

/**
 * Start a new timer if we need to for nMS duration.
 *
 * if this is longer than the existing duration we're
 * waiting for, do nothing - unless bForce - which means
 * to reset the minimum period; used by the scheduled itself.
 */
void Scheduler::ImplStartTimer(sal_uInt64 nMS, bool bForce)
{
    ImplSVData* pSVData = ImplGetSVData();
    if (pSVData->mbDeInit)
    {
        // do not start new timers during shutdown - if that happens after
        // ImplSalStopTimer() on WNT the timer queue is restarted and never ends
        return;
    }

    DBG_TESTSOLARMUTEX();

    if (!pSVData->mpSalTimer)
    {
        pSVData->mnTimerPeriod = InfiniteTimeoutMs;
        pSVData->mpSalTimer = pSVData->mpDefInst->CreateSalTimer();
        pSVData->mpSalTimer->SetCallback(Scheduler::CallbackTaskScheduling);
    }

    if ( !nMS )
        nMS = 1;

    // Only if smaller timeout, to avoid skipping.
    if (bForce || nMS < pSVData->mnTimerPeriod)
    {
        pSVData->mnTimerPeriod = nMS;
        pSVData->mpSalTimer->Start(nMS);
    }
}

void Scheduler::CallbackTaskScheduling( bool bIdle )
{
    // this function is for the saltimer callback
    Scheduler::ProcessTaskScheduling( bIdle );
}

static bool g_bDeterministicMode = false;

void Scheduler::SetDeterministicMode(bool bDeterministic)
{
    g_bDeterministicMode = bDeterministic;
}

bool Scheduler::GetDeterministicMode()
{
    return g_bDeterministicMode;
}

inline bool Scheduler::HasPendingTasks( const ImplSVData* pSVData, const sal_uInt64 nTime )
{
    return ( pSVData->mbNeedsReschedule || ((pSVData->mnTimerPeriod != InfiniteTimeoutMs)
        && (nTime >= pSVData->mnLastProcessTime + pSVData->mnTimerPeriod )) );
}

bool Scheduler::HasPendingTasks()
{
    ImplSVData *pSVData = ImplGetSVData();
    sal_uInt64  nTime = tools::Time::GetSystemTicks();
    return HasPendingTasks( pSVData, nTime );
}

inline void Scheduler::UpdateMinPeriod( ImplSchedulerData *pSchedulerData,
                                        const sal_uInt64 nTime, sal_uInt64 &nMinPeriod )
{
    if ( nMinPeriod > ImmediateTimeoutMs )
    {
        sal_uInt64 nCurPeriod = nMinPeriod;
        nMinPeriod = pSchedulerData->mpTask->UpdateMinPeriod( nCurPeriod, nTime );
        assert( nMinPeriod <= nCurPeriod );
        if ( nCurPeriod < nMinPeriod )
            nMinPeriod = nCurPeriod;
    }
}

bool Scheduler::ProcessTaskScheduling( bool bIdle )
{
    ImplSVData *pSVData = ImplGetSVData();
    sal_uInt64  nTime = tools::Time::GetSystemTicks();
    if ( pSVData->mbDeInit || !HasPendingTasks( pSVData, nTime ) )
        return false;
    pSVData->mbNeedsReschedule = false;
    pSVData->mnLastProcessTime = nTime;

    ImplSchedulerData* pSchedulerData = nullptr;
    ImplSchedulerData* pPrevSchedulerData = nullptr;
    ImplSchedulerData *pMostUrgent = nullptr;
    sal_uInt64         nMinPeriod = InfiniteTimeoutMs;

    DBG_TESTSOLARMUTEX();

    pSchedulerData = pSVData->mpFirstSchedulerData;
    while ( pSchedulerData )
    {
        const Timer *timer = dynamic_cast<Timer*>( pSchedulerData->mpTask );
        if ( timer )
            SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
                << pSchedulerData << " " << *pSchedulerData << " " << *timer );
        else if ( pSchedulerData->mpTask )
            SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
                << pSchedulerData << " " << *pSchedulerData
                << " " << *pSchedulerData->mpTask );
        else
            SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
                << pSchedulerData << " " << *pSchedulerData << " (to be deleted)" );

        if ( pSchedulerData->mbInScheduler )
            goto next_entry;

        // Should Task be released from scheduling?
        if ( pSchedulerData->mbDelete || !pSchedulerData->mpTask )
        {
            if ( pPrevSchedulerData )
                pPrevSchedulerData->mpNext = pSchedulerData->mpNext;
            else
                pSVData->mpFirstSchedulerData = pSchedulerData->mpNext;
            if ( !pSchedulerData->mpNext )
                pSVData->mpLastSchedulerData = pPrevSchedulerData;
            if ( pSchedulerData->mpTask )
                pSchedulerData->mpTask->mpSchedulerData = nullptr;
            ImplSchedulerData *pDeleteItem = pSchedulerData;
            pSchedulerData = pSchedulerData->mpNext;
            delete pDeleteItem;
            continue;
        }

        assert( pSchedulerData->mpTask );
        if ( !pSchedulerData->mpTask->IsActive() )
            goto next_entry;

        UpdateMinPeriod( pSchedulerData, nTime, nMinPeriod );

        // skip ready tasks with lower priority than the most urgent (numerical lower is higher)
        if ( pSchedulerData->mpTask->ReadyForSchedule( bIdle, nTime ) &&
             (!pMostUrgent || (pSchedulerData->mpTask->GetPriority() < pMostUrgent->mpTask->GetPriority())) )
        {
            pMostUrgent = pSchedulerData;
        }

next_entry:
        pPrevSchedulerData = pSchedulerData;
        pSchedulerData = pSchedulerData->mpNext;
    }

    // delete clock if no more timers available,
    if ( !pSVData->mpFirstSchedulerData )
    {
        if ( pSVData->mpSalTimer )
            pSVData->mpSalTimer->Stop();
        if ( ImmediateTimeoutMs == nMinPeriod )
            SAL_INFO("vcl.schedule", "Unusual - no more tasks available - stop timer");
        else
            SAL_INFO("vcl.schedule", "Idles available - handle immediate");
    }
    else
    {
        Scheduler::ImplStartTimer( nMinPeriod, true );
        SAL_INFO("vcl.schedule", "Calculated minimum timeout as " << nMinPeriod );
    }

    pSVData->mnTimerPeriod = nMinPeriod;

    if ( pMostUrgent )
    {
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
                  << pMostUrgent << "  invoke     " << *pMostUrgent->mpTask );

        Task *pTask = pMostUrgent->mpTask;

        // prepare Scheduler Object for deletion after handling
        pTask->SetDeletionFlags();

        // invoke it
        pMostUrgent->mbInScheduler = true;
        pTask->Invoke();
        pMostUrgent->mbInScheduler = false;

        if ( pMostUrgent->mpTask && !pMostUrgent->mbDelete )
        {
            pMostUrgent->mnUpdateTime = tools::Time::GetSystemTicks();
            UpdateMinPeriod( pMostUrgent, nTime, nMinPeriod );
        }
    }

    return !!pMostUrgent;
}

const char *ImplSchedulerData::GetDebugName() const
{
    return mpTask && mpTask->GetDebugName() ?
        mpTask->GetDebugName() : "unknown";
}

void Task::StartTimer( sal_uInt64 nMS )
{
    Scheduler::ImplStartTimer( nMS, false );
}

void Task::SetDeletionFlags()
{
    mpSchedulerData->mbDelete = true;
    mbActive = false;
}

void Task::Start()
{
    ImplSVData *const pSVData = ImplGetSVData();
    if (pSVData->mbDeInit)
    {
        return;
    }

    DBG_TESTSOLARMUTEX();

    // Mark timer active
    mbActive = true;

    if ( !mpSchedulerData )
    {
        // insert Task
        mpSchedulerData                = new ImplSchedulerData;
        mpSchedulerData->mpTask        = this;
        mpSchedulerData->mbInScheduler = false;
        mpSchedulerData->mpNext        = nullptr;

        // insert last due to SFX!
        if ( !pSVData->mpLastSchedulerData )
        {
            pSVData->mpFirstSchedulerData = mpSchedulerData;
            pSVData->mpLastSchedulerData = mpSchedulerData;
        }
        else
        {
            pSVData->mpLastSchedulerData->mpNext = mpSchedulerData;
            pSVData->mpLastSchedulerData = mpSchedulerData;
        }
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
                  << " " << mpSchedulerData << "  added      " << *this );
    }
    else
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
                  << " " << mpSchedulerData << "  restarted  " << *this );

    mpSchedulerData->mbDelete      = false;
    mpSchedulerData->mnUpdateTime  = tools::Time::GetSystemTicks();
    pSVData->mbNeedsReschedule     = true;
}

void Task::Stop()
{
    SAL_INFO_IF( mbActive, "vcl.schedule", tools::Time::GetSystemTicks()
                  << " " << mpSchedulerData << "  stopped    " << *this );
    mbActive = false;
    if ( mpSchedulerData )
        mpSchedulerData->mbDelete = true;
}

Task& Task::operator=( const Task& rTask )
{
    if ( IsActive() )
        Stop();

    mbActive = false;
    mePriority = rTask.mePriority;

    if ( rTask.IsActive() )
        Start();

    return *this;
}

Task::Task( const sal_Char *pDebugName )
    : mpSchedulerData( nullptr )
    , mpDebugName( pDebugName )
    , mePriority( TaskPriority::HIGH )
    , mbActive( false )
{
}

Task::Task( const Task& rTask )
    : mpSchedulerData( nullptr )
    , mpDebugName( rTask.mpDebugName )
    , mePriority( rTask.mePriority )
    , mbActive( false )
{
    if ( rTask.IsActive() )
        Start();
}

Task::~Task()
{
    if ( mpSchedulerData )
    {
        mpSchedulerData->mbDelete = true;
        mpSchedulerData->mpTask = nullptr;
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
