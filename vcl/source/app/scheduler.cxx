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
    pSVData->mnTimerPeriod = InfiniteTimeoutMs;

    pSVData->mpFirstSchedulerData = nullptr;
    pSVData->mpLastSchedulerData  = nullptr;
}

/**
 * Start a new timer if we need to for nMS duration.
 *
 * if this is longer than the existing duration we're
 * waiting for, do nothing - unless bForce - which means
 * to reset the minimum period; used by the scheduled itself.
 */
void Scheduler::ImplStartTimer(sal_uInt64 nMS, bool bForce, sal_uInt64 nTime)
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
        pSVData->mnTimerStart = 0;
        pSVData->mnTimerPeriod = InfiniteTimeoutMs;
        pSVData->mpSalTimer = pSVData->mpDefInst->CreateSalTimer();
        pSVData->mpSalTimer->SetCallback(Scheduler::CallbackTaskScheduling);
    }

    assert(SAL_MAX_UINT64 - nMS >= nTime);

    sal_uInt64 nProposedTimeout = nTime + nMS;
    sal_uInt64 nCurTimeout = ( pSVData->mnTimerPeriod == InfiniteTimeoutMs )
        ? SAL_MAX_UINT64 : pSVData->mnTimerStart + pSVData->mnTimerPeriod;

    // Only if smaller timeout, to avoid skipping.
    if (bForce || nProposedTimeout < nCurTimeout)
    {
        SAL_INFO( "vcl.schedule", "  Starting scheduler system timer (" << nMS << "ms)" );
        pSVData->mnTimerStart = nTime;
        pSVData->mnTimerPeriod = nMS;
        pSVData->mpSalTimer->Start( nMS );
    }
}

void Scheduler::CallbackTaskScheduling()
{
    // this function is for the saltimer callback
    Scheduler::ProcessTaskScheduling();
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
        && (nTime >= pSVData->mnTimerStart + pSVData->mnTimerPeriod )) );
}

bool Scheduler::HasPendingTasks()
{
    ImplSVData *pSVData = ImplGetSVData();
    sal_uInt64  nTime = tools::Time::GetSystemTicks();
    return HasPendingTasks( pSVData, nTime );
}

inline void Scheduler::UpdateMinPeriod( ImplSchedulerData * const pSchedulerData,
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

inline void Scheduler::UpdateSystemTimer( ImplSVData * const pSVData,
                                          const sal_uInt64 nMinPeriod,
                                          const bool bForce, const sal_uInt64 nTime )
{
    if ( InfiniteTimeoutMs == nMinPeriod )
    {
        if ( pSVData->mpSalTimer )
            pSVData->mpSalTimer->Stop();
        SAL_INFO("vcl.schedule", "  Stopping system timer");
        pSVData->mnTimerPeriod = nMinPeriod;
    }
    else
        Scheduler::ImplStartTimer( nMinPeriod, bForce, nTime );
}

static inline void AppendSchedulerData( ImplSVData * const pSVData,
                                        ImplSchedulerData * const pSchedulerData )
{
    if ( !pSVData->mpLastSchedulerData )
    {
        pSVData->mpFirstSchedulerData = pSchedulerData;
        pSVData->mpLastSchedulerData = pSchedulerData;
    }
    else
    {
        pSVData->mpLastSchedulerData->mpNext = pSchedulerData;
        pSVData->mpLastSchedulerData = pSchedulerData;
    }
    pSchedulerData->mpNext = nullptr;
}

static inline ImplSchedulerData* DropSchedulerData(
    ImplSVData * const pSVData, ImplSchedulerData * const pPrevSchedulerData,
                                ImplSchedulerData * const pSchedulerData )
{
    assert( !pPrevSchedulerData || (pPrevSchedulerData->mpNext == pSchedulerData) );

    ImplSchedulerData * const pSchedulerDataNext = pSchedulerData->mpNext;
    if ( pPrevSchedulerData )
        pPrevSchedulerData->mpNext = pSchedulerDataNext;
    else
        pSVData->mpFirstSchedulerData = pSchedulerDataNext;
    if ( !pSchedulerDataNext )
        pSVData->mpLastSchedulerData = pPrevSchedulerData;
    return pSchedulerDataNext;
}

bool Scheduler::ProcessTaskScheduling()
{
    ImplSVData *pSVData = ImplGetSVData();
    sal_uInt64  nTime = tools::Time::GetSystemTicks();
    if ( pSVData->mbDeInit || !HasPendingTasks( pSVData, nTime ) )
        return false;
    pSVData->mbNeedsReschedule = false;

    ImplSchedulerData* pSchedulerData = nullptr;
    ImplSchedulerData* pPrevSchedulerData = nullptr;
    ImplSchedulerData *pMostUrgent = nullptr;
    ImplSchedulerData *pPrevMostUrgent = nullptr;
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

        // Should the Task be released from scheduling or stacked?
        if ( pSchedulerData->mbDelete || !pSchedulerData->mpTask || pSchedulerData->mbInScheduler )
        {
            ImplSchedulerData * const pSchedulerDataNext =
                DropSchedulerData( pSVData, pPrevSchedulerData, pSchedulerData );
            if ( pSchedulerData->mbInScheduler )
            {
                pSchedulerData->mpNext = pSVData->mpSchedulerStack;
                pSVData->mpSchedulerStack = pSchedulerData;
            }
            else
            {
                if ( pSchedulerData->mpTask )
                    pSchedulerData->mpTask->mpSchedulerData = nullptr;
                delete pSchedulerData;
            }
            pSchedulerData = pSchedulerDataNext;
            continue;
        }

        assert( pSchedulerData->mpTask );
        if ( !pSchedulerData->mpTask->IsActive() )
            goto next_entry;

        // skip ready tasks with lower priority than the most urgent (numerical lower is higher)
        if ( pSchedulerData->mpTask->ReadyForSchedule( nTime ) &&
             (!pMostUrgent || (pSchedulerData->mpTask->GetPriority() < pMostUrgent->mpTask->GetPriority())) )
        {
            if ( pMostUrgent )
                UpdateMinPeriod( pMostUrgent, nTime, nMinPeriod );
            pPrevMostUrgent = pPrevSchedulerData;
            pMostUrgent = pSchedulerData;
        }
        else
            UpdateMinPeriod( pSchedulerData, nTime, nMinPeriod );

next_entry:
        pPrevSchedulerData = pSchedulerData;
        pSchedulerData = pSchedulerData->mpNext;
    }

    if ( InfiniteTimeoutMs != nMinPeriod )
        SAL_INFO("vcl.schedule", "Calculated minimum timeout as " << nMinPeriod );
    UpdateSystemTimer( pSVData, nMinPeriod, true, nTime );

    if ( pMostUrgent )
    {
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
                  << pMostUrgent << "  invoke     " << *pMostUrgent->mpTask );

        Task *pTask = pMostUrgent->mpTask;

        // prepare Scheduler object for deletion after handling
        pTask->SetDeletionFlags();

        // invoke the task
        // defer pushing the scheduler stack to next run, as most tasks will
        // not run a nested Scheduler loop and don't need a stack push!
        pMostUrgent->mbInScheduler = true;
        pTask->Invoke();
        pMostUrgent->mbInScheduler = false;

        // eventually pop the scheduler stack
        // this just happens for nested calls, which renders all accounting
        // invalid, so we just enforce a rescheduling!
        if ( pMostUrgent == pSVData->mpSchedulerStack )
        {
            pSchedulerData = pSVData->mpSchedulerStack;
            pSVData->mpSchedulerStack = pSchedulerData->mpNext;
            AppendSchedulerData( pSVData, pSchedulerData );
            UpdateSystemTimer( pSVData, ImmediateTimeoutMs, true,
                               tools::Time::GetSystemTicks() );
        }
        else
        {
            // Since we can restart tasks, round-robin all non-last tasks
            if ( pMostUrgent->mpNext )
            {
                DropSchedulerData( pSVData, pPrevMostUrgent, pMostUrgent );
                AppendSchedulerData( pSVData, pMostUrgent );
            }

            if ( pMostUrgent->mpTask && !pMostUrgent->mbDelete )
            {
                pMostUrgent->mnUpdateTime = nTime;
                UpdateMinPeriod( pMostUrgent, nTime, nMinPeriod );
                UpdateSystemTimer( pSVData, nMinPeriod, false, nTime );
            }
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
    Scheduler::ImplStartTimer( nMS, false, tools::Time::GetSystemTicks() );
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

        AppendSchedulerData( pSVData, mpSchedulerData );
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
