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
#include <osl/mutex.hxx>
#include <osl/conditn.h>

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

bool Scheduler::ImplInitScheduler()
{
    ImplSVData *pSVData = ImplGetSVData();
    assert( !pSVData->mbDeInit );
    ImplSchedulerContext &rSchedCtx = pSVData->maSchedCtx;

    rSchedCtx.mpInvokeMutex = new ::osl::Mutex();
    if ( nullptr == rSchedCtx.mpInvokeMutex )
        goto bailout_init;

    rSchedCtx.maInvokeCondition = osl_createCondition();
    if ( nullptr == rSchedCtx.maInvokeCondition )
        goto bailout_init;

    return true;

bailout_init:
    DELETEZ( rSchedCtx.mpInvokeMutex );

    return false;
}

void Scheduler::ImplDeInitScheduler()
{
    ImplSVData* pSVData = ImplGetSVData();
    assert( pSVData->mbDeInit );
    ImplSchedulerContext &rSchedCtx = pSVData->maSchedCtx;

    if (rSchedCtx.mpSalTimer) rSchedCtx.mpSalTimer->Stop();
    DELETEZ( rSchedCtx.mpSalTimer );

    if ( rSchedCtx.mpInvokeMutex ) rSchedCtx.mpInvokeMutex->acquire();
    DELETEZ( rSchedCtx.mpInvokeMutex );

    osl_destroyCondition( rSchedCtx.maInvokeCondition );
    rSchedCtx.maInvokeCondition = nullptr;

    // Free active tasks
    ImplSchedulerData* pSchedulerData = rSchedCtx.mpFirstSchedulerData;
    while ( pSchedulerData )
    {
        ImplSchedulerData* pTempSchedulerData = pSchedulerData;
        pSchedulerData = pSchedulerData->mpNext;
        delete pTempSchedulerData;
    }

    rSchedCtx.mnTimerPeriod = InfiniteTimeoutMs;

    rSchedCtx.mpFirstSchedulerData = nullptr;
    rSchedCtx.mpLastSchedulerData  = nullptr;
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

    ImplSchedulerContext &rSchedCtx = pSVData->maSchedCtx;
    if (!rSchedCtx.mpSalTimer)
    {
        rSchedCtx.mnTimerStart = 0;
        rSchedCtx.mnTimerPeriod = InfiniteTimeoutMs;
        rSchedCtx.mpSalTimer = pSVData->mpDefInst->CreateSalTimer();
        rSchedCtx.mpSalTimer->SetCallback(Scheduler::CallbackTaskScheduling);
    }

    assert(SAL_MAX_UINT64 - nMS >= nTime);

    sal_uInt64 nProposedTimeout = nTime + nMS;
    sal_uInt64 nCurTimeout = ( rSchedCtx.mnTimerPeriod == InfiniteTimeoutMs )
        ? SAL_MAX_UINT64 : rSchedCtx.mnTimerStart + rSchedCtx.mnTimerPeriod;

    // Only if smaller timeout, to avoid skipping.
    if (bForce || nProposedTimeout < nCurTimeout)
    {
        SAL_INFO( "vcl.schedule", "  Starting scheduler system timer (" << nMS << "ms)" );
        rSchedCtx.mnTimerStart = nTime;
        rSchedCtx.mnTimerPeriod = nMS;
        rSchedCtx.mpSalTimer->Start( nMS );
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

inline bool Scheduler::HasPendingTasks(
    const ImplSchedulerContext& rSchedCtx, const sal_uInt64 nTime )
{
    return ( rSchedCtx.mnTimerPeriod != InfiniteTimeoutMs
        && nTime >= rSchedCtx.mnTimerStart + rSchedCtx.mnTimerPeriod );
}

bool Scheduler::HasPendingTasks()
{
    ImplSchedulerContext &rSchedCtx = ImplGetSVData()->maSchedCtx;
    sal_uInt64  nTime = tools::Time::GetSystemTicks();
    return HasPendingTasks( rSchedCtx, nTime );
}

inline void Scheduler::UpdateSystemTimer( ImplSchedulerContext & rSchedCtx,
                                          const sal_uInt64 nMinPeriod,
                                          const bool bForce, const sal_uInt64 nTime )
{
    if ( InfiniteTimeoutMs == nMinPeriod )
    {
        if ( rSchedCtx.mpSalTimer )
            rSchedCtx.mpSalTimer->Stop();
        SAL_INFO("vcl.schedule", "  Stopping system timer");
        rSchedCtx.mnTimerPeriod = nMinPeriod;
    }
    else
        Scheduler::ImplStartTimer( nMinPeriod, bForce, nTime );
}

static inline void AppendSchedulerData( ImplSchedulerContext & rSchedCtx,
                                        ImplSchedulerData * const pSchedulerData )
{
    if ( !rSchedCtx.mpLastSchedulerData )
    {
        rSchedCtx.mpFirstSchedulerData = pSchedulerData;
        rSchedCtx.mpLastSchedulerData = pSchedulerData;
    }
    else
    {
        rSchedCtx.mpLastSchedulerData->mpNext = pSchedulerData;
        rSchedCtx.mpLastSchedulerData = pSchedulerData;
    }
    pSchedulerData->mpNext = nullptr;
}

static inline ImplSchedulerData* DropSchedulerData(
    ImplSchedulerContext & rSchedCtx,
    ImplSchedulerData * const pPrevSchedulerData,
    ImplSchedulerData * const pSchedulerData )
{
    assert( !pPrevSchedulerData || (pPrevSchedulerData->mpNext == pSchedulerData) );

    ImplSchedulerData * const pSchedulerDataNext = pSchedulerData->mpNext;
    if ( pPrevSchedulerData )
        pPrevSchedulerData->mpNext = pSchedulerDataNext;
    else
        rSchedCtx.mpFirstSchedulerData = pSchedulerDataNext;
    if ( !pSchedulerDataNext )
        rSchedCtx.mpLastSchedulerData = pPrevSchedulerData;
    return pSchedulerDataNext;
}

bool Scheduler::ProcessTaskScheduling()
{
    ImplSVData *pSVData = ImplGetSVData();
    ImplSchedulerContext &rSchedCtx = pSVData->maSchedCtx;
    sal_uInt64  nTime = tools::Time::GetSystemTicks();
    if ( pSVData->mbDeInit || !HasPendingTasks( rSchedCtx, nTime ) )
        return false;

    ImplSchedulerData* pSchedulerData = nullptr;
    ImplSchedulerData* pPrevSchedulerData = nullptr;
    ImplSchedulerData *pMostUrgent = nullptr;
    ImplSchedulerData *pPrevMostUrgent = nullptr;
    sal_uInt64         nMinPeriod = InfiniteTimeoutMs;
    sal_uInt64         nMostUrgentPeriod = InfiniteTimeoutMs;
    sal_uInt64         nReadyPeriod = InfiniteTimeoutMs;

    DBG_TESTSOLARMUTEX();

    pSchedulerData = rSchedCtx.mpFirstSchedulerData;
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
        if ( !pSchedulerData->mpTask || !pSchedulerData->mpTask->IsActive()
            || pSchedulerData->mbInScheduler )
        {
            ImplSchedulerData * const pSchedulerDataNext =
                DropSchedulerData( rSchedCtx, pPrevSchedulerData, pSchedulerData );
            if ( pSchedulerData->mbInScheduler )
            {
                pSchedulerData->mpNext = rSchedCtx.mpSchedulerStack;
                rSchedCtx.mpSchedulerStack = pSchedulerData;
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
        nReadyPeriod = pSchedulerData->mpTask->UpdateMinPeriod( nMinPeriod, nTime );
        if ( ImmediateTimeoutMs == nReadyPeriod &&
             (!pMostUrgent || (pSchedulerData->mpTask->GetPriority() < pMostUrgent->mpTask->GetPriority())) )
        {
            if ( pMostUrgent && nMinPeriod > nMostUrgentPeriod )
                nMinPeriod = nMostUrgentPeriod;
            pPrevMostUrgent = pPrevSchedulerData;
            pMostUrgent = pSchedulerData;
            nMostUrgentPeriod = nReadyPeriod;
        }
        else if ( nMinPeriod > nReadyPeriod )
            nMinPeriod = nReadyPeriod;

next_entry:
        pPrevSchedulerData = pSchedulerData;
        pSchedulerData = pSchedulerData->mpNext;
    }

    if ( InfiniteTimeoutMs != nMinPeriod )
        SAL_INFO("vcl.schedule", "Calculated minimum timeout as " << nMinPeriod );
    UpdateSystemTimer( rSchedCtx, nMinPeriod, true, nTime );

    if ( pMostUrgent )
    {
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
                  << pMostUrgent << "  invoke     " << *pMostUrgent->mpTask );

        Task *pTask = nullptr;
        {
            osl::MutexGuard aImplGuard( rSchedCtx.mpInvokeMutex );
            pMostUrgent->mbInScheduler = true;
            // SetDeletionFlags will set mpScheduler == nullptr for single-shot tasks
            pTask = pMostUrgent->mpTask;
        }

        if ( nullptr == pTask )
            goto skip_invoke;

        // prepare Scheduler object for deletion after handling
        pTask->SetDeletionFlags();

        // invoke the task
        // defer pushing the scheduler stack to next run, as most tasks will
        // not run a nested Scheduler loop and don't need a stack push!
        pTask->Invoke();
        // Invoke() can delete the sched(uler) object, so it might be invalid now,
        // e.g. see SfxItemDisruptor_Impl::Delete

        // eventually pop the scheduler stack
        // this just happens for nested calls, which renders all accounting
        // invalid, so we just enforce a rescheduling!
        if ( pMostUrgent == rSchedCtx.mpSchedulerStack )
        {
            pSchedulerData = rSchedCtx.mpSchedulerStack;
            rSchedCtx.mpSchedulerStack = pSchedulerData->mpNext;
            AppendSchedulerData( rSchedCtx, pSchedulerData );
            UpdateSystemTimer( rSchedCtx, ImmediateTimeoutMs, true,
                               tools::Time::GetSystemTicks() );
        }
        else
        {
            // Since we can restart tasks, round-robin all non-last tasks
            if ( pMostUrgent->mpNext )
            {
                DropSchedulerData( rSchedCtx, pPrevMostUrgent, pMostUrgent );
                AppendSchedulerData( rSchedCtx, pMostUrgent );
            }

            if ( pMostUrgent->mpTask && pMostUrgent->mpTask->IsActive() )
            {
                pMostUrgent->mnUpdateTime = nTime;
                if ( nMinPeriod > nMostUrgentPeriod )
                    nMinPeriod = nMostUrgentPeriod;
                UpdateSystemTimer( rSchedCtx, nMinPeriod, false, nTime );
            }
        }

skip_invoke:
        pMostUrgent->mbInScheduler = false;
        // notify all waiting / nested threads to check their mbInScheduler
        osl_setCondition( rSchedCtx.maInvokeCondition );
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
    if ( TaskStatus::DISPOSED != meStatus )
        meStatus = TaskStatus::STOPPED;
}

void Task::Start()
{
    ImplSVData *const pSVData = ImplGetSVData();
    if (pSVData->mbDeInit || TaskStatus::DISPOSED == meStatus)
        return;

    DBG_TESTSOLARMUTEX();

    // Mark timer active
    meStatus = TaskStatus::SCHEDULED;

    if ( !mpSchedulerData )
    {
        // insert Task
        mpSchedulerData                = new ImplSchedulerData;
        mpSchedulerData->mpTask        = this;
        mpSchedulerData->mbInScheduler = false;

        AppendSchedulerData( pSVData->maSchedCtx, mpSchedulerData );
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
                  << " " << mpSchedulerData << "  added      " << *this );
    }
    else
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
                  << " " << mpSchedulerData << "  restarted  " << *this );

    mpSchedulerData->mnUpdateTime = tools::Time::GetSystemTicks();
}

void Task::Stop()
{
    if ( TaskStatus::SCHEDULED != meStatus )
        return;
    SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
              << " " << mpSchedulerData << "  stopped    " << *this );
    Task::SetDeletionFlags();
}

void Task::Dispose( const DisposePolicy ePolicy )
{
    if ( TaskStatus::DISPOSED == meStatus )
        return;
    meStatus = TaskStatus::DISPOSED;
    if ( !mpSchedulerData )
        return;
    const ImplSVData *pSVData = ImplGetSVData();
    if ( pSVData->mbDeInit )
    {
        mpSchedulerData = nullptr;
        return;
    }

    const ImplSchedulerContext &rSchedCtx = pSVData->maSchedCtx;
    do {
        rSchedCtx.mpInvokeMutex->acquire();
        sal_Int32 nCount = ( mpSchedulerData->mbInScheduler
            && DisposePolicy::WAIT_INVOKE == ePolicy ) ? 1 : 0;
        osl_resetCondition( rSchedCtx.maInvokeCondition );
        rSchedCtx.mpInvokeMutex->release();
        if ( 0 == nCount )
            break;
        osl_waitCondition( rSchedCtx.maInvokeCondition, nullptr );
    }
    while ( true );

    SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
        << " " << mpSchedulerData << "  disposed   " << *this );
    if ( mpSchedulerData )
    {
        if ( !pSVData->mbDeInit )
            mpSchedulerData->mpTask = nullptr;
        mpSchedulerData = nullptr;
    }
}

Task& Task::operator=( const Task& rTask )
{
    if ( IsActive() )
        Stop();

    meStatus = TaskStatus::STOPPED;
    mePriority = rTask.mePriority;

    if ( rTask.IsActive() )
        Start();

    return *this;
}

bool Task::IsActive() const
{
    ImplSVData *pSVData = ImplGetSVData();
    assert( TaskStatus::SCHEDULED != meStatus ||
           (TaskStatus::SCHEDULED == meStatus && mpSchedulerData) );
    return ( !pSVData->mbDeInit
             && nullptr != mpSchedulerData
             && this == mpSchedulerData->mpTask
             && TaskStatus::SCHEDULED == meStatus );
}

Task::Task( const sal_Char *pDebugName )
    : mpSchedulerData( nullptr )
    , mpDebugName( pDebugName )
    , mePriority( TaskPriority::HIGH )
    , meStatus( TaskStatus::STOPPED )
{
}

Task::Task( const Task& rTask )
    : mpSchedulerData( nullptr )
    , mpDebugName( rTask.mpDebugName )
    , mePriority( rTask.mePriority )
    , meStatus( TaskStatus::STOPPED )
{
    if ( rTask.IsActive() )
        Start();
}

Task::~Task()
{
    Dispose();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
