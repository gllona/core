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
    return stream;
}

template< typename charT, typename traits >
inline std::basic_ostream<charT, traits> & operator <<(
    std::basic_ostream<charT, traits> & stream, const Idle& idle )
{
    return stream << static_cast<const Timer*>( &idle );
}

void ImplSchedulerData::Invoke()
{
    DBG_TESTSOLARMUTEX();

    if ( mbInScheduler )
    {
        assert( mbInScheduler && !mpScheduler );
        return;
    }

    ImplSVData *pSVData = ImplGetSVData();
    Scheduler *sched;
    {
        osl::MutexGuard aImplGuard( pSVData->mpInvokeMutex );
        if ( !mpScheduler )
            return;
        mbInScheduler = true;
        // SetDeletionFlags will set mpScheduler == nullptr for single-shot tasks
        sched = mpScheduler;
    }

    sched->SetDeletionFlags();

    sched->Invoke();
    // Invoke() can delete the sched(uler) object, so it might be invalid now;
    // e.g. see SfxItemDisruptor_Impl::Delete

    mbInScheduler = false;

    // notify all waiting / nested threads to check their mbInScheduler
    osl_setCondition( pSVData->maInvokeCondition );
}

bool Scheduler::IsActive() const
{
    ImplSVData *pSVData = ImplGetSVData();
    return ( !pSVData->mbDeInit && nullptr != mpSchedulerData &&
             this == mpSchedulerData->mpScheduler );
}

void Scheduler::SetPriority( SchedulerPriority ePriority )
{
    assert( SchedulerPriority::DISPOSED != ePriority );
    assert( SchedulerPriority::DISPOSED != mePriority );
    if ( SchedulerPriority::DISPOSED != mePriority &&
         SchedulerPriority::DISPOSED != ePriority )
    {
        mePriority = ePriority;
        if ( IsActive() )
            ImplGetSVData()->mbNeedsReschedule = true;
    }
}

void Scheduler::SetDeletionFlags()
{
    assert( mpSchedulerData );
    ImplSVData *pSVData = ImplGetSVData();
    if ( !pSVData->mbDeInit )
    {
        osl::MutexGuard aImplGuard( pSVData->mpInvokeMutex );
        mpSchedulerData->mpScheduler = nullptr;
    }
    else
        mpSchedulerData->mpScheduler = nullptr;
}

bool Scheduler::ImplInitScheduler()
{
    ImplSVData *pSVData = ImplGetSVData();

    pSVData->mpFreeListMutex = new ::osl::Mutex();
    if ( nullptr == pSVData->mpFreeListMutex )
        return false;

    pSVData->mpAppendMutex = new ::osl::Mutex();
    if ( nullptr == pSVData->mpAppendMutex )
        return false;

    pSVData->mpInvokeMutex = new ::osl::Mutex();
    if ( nullptr == pSVData->mpInvokeMutex )
        return false;

    pSVData->maInvokeCondition = osl_createCondition();
    if ( nullptr == pSVData->maInvokeCondition )
        return false;

    return true;
}

void Scheduler::ImplDeInitScheduler()
{
    ImplSVData *pSVData = ImplGetSVData();

    if (pSVData->mpSalTimer) pSVData->mpSalTimer->Stop();
    DELETEZ( pSVData->mpSalTimer );

    if ( pSVData->mpFreeListMutex ) pSVData->mpFreeListMutex->acquire();
    DELETEZ( pSVData->mpFreeListMutex );
    if ( pSVData->mpAppendMutex ) pSVData->mpAppendMutex->acquire();
    DELETEZ( pSVData->mpAppendMutex );
    if ( pSVData->mpInvokeMutex ) pSVData->mpInvokeMutex->acquire();
    DELETEZ( pSVData->mpInvokeMutex );

    osl_destroyCondition( pSVData->maInvokeCondition );
    pSVData->maInvokeCondition = nullptr;

    // Free active tasks
    ImplSchedulerData *pSchedulerData = pSVData->mpFirstSchedulerData;
    while ( pSchedulerData )
    {
        ImplSchedulerData* pNextSchedulerData = pSchedulerData->mpNext;
        delete pSchedulerData;
        pSchedulerData = pNextSchedulerData;
    }

    // Free "deleted" tasks
    pSchedulerData = pSVData->mpFreeSchedulerData;
    while ( pSchedulerData )
    {
        assert( !pSchedulerData->mpScheduler );
        ImplSchedulerData* pNextSchedulerData = pSchedulerData->mpNext;
        delete pSchedulerData;
        pSchedulerData = pNextSchedulerData;
    }

    pSVData->mpFirstSchedulerData = nullptr;
    pSVData->mpFreeSchedulerData  = nullptr;
    pSVData->mnTimerPeriod        = 0;
}

/**
 * Start a new timer if we need to for nMS duration.
 *
 * if this is longer than the existing duration we're
 * waiting for, do nothing.
 */
void Scheduler::ImplStartTimer( sal_uInt64 nMS, bool bForce )
{
    ImplSVData* pSVData = ImplGetSVData();
    if (pSVData->mbDeInit)
    {
        // do not start new timers during shutdown - if that happens after
        // ImplSalStopTimer() on WNT the timer queue is restarted and never ends
        return;
    }

    DBG_TESTSOLARMUTEX();

    /**
    * Initialize the platform specific timer on which all the
    * platform independent timers are built
    */
    if (!pSVData->mpSalTimer)
    {
        pSVData->mnTimerPeriod = InfiniteTimeoutMs;
        pSVData->mpSalTimer = pSVData->mpDefInst->CreateSalTimer();
        pSVData->mpSalTimer->SetCallback(Scheduler::CallbackTaskScheduling);
    }

    // Only if smaller timeout, to avoid skipping.
    // The system timer is supposed to be single-shot, so we have to restart
    // it when forced from the Scheduler run!
    if (bForce || nMS < pSVData->mnTimerPeriod)
    {
        pSVData->mnTimerPeriod = nMS;
        pSVData->mpSalTimer->Start(nMS);
    }
}

void Scheduler::CallbackTaskScheduling()
{
    // this function is for the saltimer callback
    ImplSVData* pSVData = ImplGetSVData();
    pSVData->mbNeedsReschedule = true;
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

inline bool Scheduler::HasPendingEvents( const ImplSVData* pSVData, const sal_uInt64 nTime )
{
   return ( pSVData->mbNeedsReschedule || ((pSVData->mnTimerPeriod != InfiniteTimeoutMs)
       && (nTime >= pSVData->mnLastUpdate + pSVData->mnTimerPeriod)) );
}

bool Scheduler::HasPendingEvents()
{
    ImplSVData*  pSVData = ImplGetSVData();
    sal_uInt64   nTime = tools::Time::GetSystemTicks();
    return HasPendingEvents( pSVData, nTime );
}

inline void Scheduler::UpdateMinPeriod( ImplSchedulerData *pSchedulerData,
                                        const sal_uInt64 nTime, sal_uInt64 &nMinPeriod )
{
    if ( nMinPeriod > ImmediateTimeoutMs )
        pSchedulerData->mpScheduler->UpdateMinPeriod( nTime, nMinPeriod );
}

bool Scheduler::ProcessTaskScheduling( IdleRunPolicy eIdleRunPolicy )
{
    ImplSVData*        pSVData = ImplGetSVData();
    sal_uInt64         nTime = tools::Time::GetSystemTicks();
    if ( !HasPendingEvents( pSVData, nTime ) )
        return false;
    pSVData->mbNeedsReschedule = false;

    ImplSchedulerData *pSchedulerData = pSVData->mpFirstSchedulerData;
    ImplSchedulerData *pPrevSchedulerData = nullptr;
    ImplSchedulerData *pPrevMostUrgent = nullptr;
    ImplSchedulerData *pFirstFreedSchedulerData = nullptr;
    ImplSchedulerData *pLastFreedSchedulerData = nullptr;
    ImplSchedulerData *pMostUrgent = nullptr;
    sal_uInt64         nMinPeriod = InfiniteTimeoutMs;
    bool               bIsNestedCall = false;

    DBG_TESTSOLARMUTEX();

    SAL_INFO( "vcl.schedule", "==========  Start  ==========" );

    while ( pSchedulerData )
    {
        const Timer *timer = dynamic_cast<Timer*>( pSchedulerData->mpScheduler );
        if ( timer )
            SAL_INFO( "vcl.schedule", pSchedulerData << " "
                << pSchedulerData->mbInScheduler << " " << *timer );
        else if ( pSchedulerData->mpScheduler )
            SAL_INFO( "vcl.schedule", pSchedulerData << " "
                << pSchedulerData->mbInScheduler << " " << *pSchedulerData->mpScheduler );
        else
            SAL_INFO( "vcl.schedule", pSchedulerData << " "
                << pSchedulerData->mbInScheduler << " (to be deleted)" );

        // Skip invoked task
        if ( pSchedulerData->mbInScheduler )
        {
            bIsNestedCall = true;
            goto next_entry;
        }

        // Can this task be removed from scheduling?
        if ( !pSchedulerData->mpScheduler )
        {
            // last element can be changed by Start(), so needs protection
            bool bIsLastElement = nullptr == pSchedulerData->mpNext;
            if ( bIsLastElement )
                 pSVData->mpAppendMutex->acquire();
            ImplSchedulerData* pNextSchedulerData = pSchedulerData->mpNext;
            // if Start() has appended a task, while we waited for the mutex,
            // we're not the last anymore, so release the mutex
            if ( bIsLastElement && pNextSchedulerData )
            {
                pSVData->mpAppendMutex->release();
                bIsLastElement = false;
            }

            if ( pPrevSchedulerData )
                pPrevSchedulerData->mpNext = pNextSchedulerData;
            else
                pSVData->mpFirstSchedulerData = pNextSchedulerData;

            if ( bIsLastElement )
            {
                pSVData->mpLastSchedulerData = pPrevSchedulerData;
                // critical section passed - release the mutex
                pSVData->mpAppendMutex->release();
            }

            if ( !pLastFreedSchedulerData )
                pLastFreedSchedulerData = pSchedulerData;
            pSchedulerData->mpNext = pFirstFreedSchedulerData;
            pFirstFreedSchedulerData = pSchedulerData;
            pSchedulerData = pNextSchedulerData;
            pSVData->mbTaskRemoved = true;
            continue;
        }

        assert( pSchedulerData->mpScheduler );
        if ( !pSchedulerData->mpScheduler->ReadyForSchedule( nTime )
            // skip ready tasks with lower priority than the most urgent (numerical lower is higher)
            || (pMostUrgent && (pSchedulerData->mpScheduler->GetPriority() >= pMostUrgent->mpScheduler->GetPriority())) )
        {
            UpdateMinPeriod( pSchedulerData, nTime, nMinPeriod );
        }
        else
        {
            // Just account previous most urgent tasks
            if ( pMostUrgent )
                UpdateMinPeriod( pMostUrgent, nTime, nMinPeriod );
            pPrevMostUrgent = pPrevSchedulerData;
            pMostUrgent = pSchedulerData;
        }

next_entry:
        pPrevSchedulerData = pSchedulerData;
        pSchedulerData = pSchedulerData->mpNext;
    }

    assert( !pSchedulerData );

    // Prepend freed scheduler data objects
    if ( pFirstFreedSchedulerData )
    {
        osl::MutexGuard aImplGuard( pSVData->mpFreeListMutex );
        pLastFreedSchedulerData->mpNext = pSVData->mpFreeSchedulerData;
        pSVData->mpFreeSchedulerData = pFirstFreedSchedulerData;
    }

    // We just have to handle removed tasks for nested calls
    if ( !bIsNestedCall )
        pSVData->mbTaskRemoved = false;

    if ( pMostUrgent )
    {
        assert( pPrevMostUrgent != pMostUrgent );
        assert( !pPrevMostUrgent || (pPrevMostUrgent->mpNext == pMostUrgent) );

        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks() << " "
            << pMostUrgent << "  invoke     " << *pMostUrgent->mpScheduler );
        pMostUrgent->Invoke();
        SAL_INFO_IF( !pMostUrgent->mpScheduler, "vcl.schedule",
            tools::Time::GetSystemTicks() << " " << pMostUrgent << "  tag-rm" );

        // If there were some tasks removed, our list pointers may be invalid,
        // except pMostUrgent, which is protected by mbInScheduler
        if ( pSVData->mbTaskRemoved )
        {
            nMinPeriod = ImmediateTimeoutMs;
            pPrevSchedulerData = pMostUrgent;
        }

        // do some simple round-robin scheduling
        if ( pMostUrgent->mpScheduler )
        {
            pMostUrgent->mnUpdateTime = nTime;
            UpdateMinPeriod( pMostUrgent, nTime, nMinPeriod );

            // nothing to do, if we're already the last element
            if ( pMostUrgent->mpNext )
            {
                // if a nested call has removed items, pPrevMostUrgent is
                // wrong, so we have to find the correct pPrevMostUrgent
                if ( pSVData->mbTaskRemoved )
                {
                    pPrevMostUrgent = pSVData->mpFirstSchedulerData;
                    if ( pPrevMostUrgent != pMostUrgent )
                    {
                        while ( pPrevMostUrgent && pPrevMostUrgent->mpNext != pMostUrgent )
                            pPrevMostUrgent = pPrevMostUrgent->mpNext;
                        assert( pPrevMostUrgent );
                    }
                    else
                        pPrevMostUrgent = nullptr;
                }
                if ( pPrevMostUrgent )
                    pPrevMostUrgent->mpNext = pMostUrgent->mpNext;
                else
                    pSVData->mpFirstSchedulerData = pMostUrgent->mpNext;

                pMostUrgent->mpNext = nullptr;
                osl::MutexGuard aImplGuard( pSVData->mpAppendMutex );
                pSVData->mpLastSchedulerData->mpNext = pMostUrgent;
                pSVData->mpLastSchedulerData = pMostUrgent;
            }
        }
    }

    if ( nMinPeriod != InfiniteTimeoutMs
            && ((eIdleRunPolicy == IdleRunPolicy::IDLE_VIA_TIMER)
                || (nMinPeriod > ImmediateTimeoutMs)) )
    {
        SAL_INFO( "vcl.schedule", "Scheduler sleep timeout: " << nMinPeriod );
        ImplStartTimer( nMinPeriod, true );
    }
    else if ( pSVData->mpSalTimer )
    {
        SAL_INFO( "vcl.schedule", "Stopping scheduler system timer" );
        pSVData->mpSalTimer->Stop();
    }

    SAL_INFO( "vcl.schedule", "==========   End   ==========" );

    pSVData->mnTimerPeriod = nMinPeriod;
    pSVData->mnLastUpdate = nTime;

    return pMostUrgent != nullptr;
}

void Scheduler::Start()
{
    ImplSVData *const pSVData = ImplGetSVData();
    if (pSVData->mbDeInit || SchedulerPriority::DISPOSED == mePriority)
        return;

    if ( mpSchedulerData && mpSchedulerData->mpNext )
        SetDeletionFlags();
    if ( !IsActive() )
    {
        // Try fetching free Scheduler object from list
        ImplSchedulerData *pNewData = nullptr;
        {
            osl::MutexGuard aImplGuard( pSVData->mpFreeListMutex );
            if ( pSVData->mpFreeSchedulerData )
            {
                pNewData = pSVData->mpFreeSchedulerData;
                pSVData->mpFreeSchedulerData = pNewData->mpNext;
            }
        }
        if ( !pNewData )
            pNewData = new ImplSchedulerData;
        pNewData->mpScheduler   = this;
        pNewData->mbInScheduler = false;
        pNewData->mpNext        = nullptr;

        // insert last due to SFX!
        {
            osl::MutexGuard aImplGuard( pSVData->mpAppendMutex );
            if ( IsActive() )
                SetDeletionFlags();

            mpSchedulerData = pNewData;
            if ( pSVData->mpFirstSchedulerData == nullptr )
            {
                assert( pSVData->mpLastSchedulerData == nullptr );
                pSVData->mpFirstSchedulerData = mpSchedulerData;
                pSVData->mpLastSchedulerData = mpSchedulerData;
            }
            else
            {
                assert( pSVData->mpLastSchedulerData != nullptr );
                pSVData->mpLastSchedulerData->mpNext = mpSchedulerData;
                pSVData->mpLastSchedulerData = mpSchedulerData;
            }
        }
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
            << " " << mpSchedulerData << "  added      " << *this );
    }
    else
        SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
            << " " << mpSchedulerData << "  restarted  " << *this );

    // Have we lost a race with an other caller?
    if( mpSchedulerData->mpScheduler == this )
    {
        mpSchedulerData->mnUpdateTime = tools::Time::GetSystemTicks();
        pSVData->mbNeedsReschedule = true;
    }
}

void Scheduler::Stop()
{
    if ( !IsActive() )
        return;
    SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
              << " " << mpSchedulerData << "  stopped    " << *this );
    Scheduler::SetDeletionFlags();
}

void Scheduler::Dispose( DisposePolicy ePolicy )
{
    mePriority = SchedulerPriority::DISPOSED;
    if ( !mpSchedulerData )
        return;
    const ImplSVData *pSVData = ImplGetSVData();
    if ( pSVData->mbDeInit )
    {
        mpSchedulerData = nullptr;
        return;
    }
    const bool bUseLocks = (DisposePolicy::WAIT_INVOKE == ePolicy);
    if ( bUseLocks )
    {
        pSVData->mpInvokeMutex->acquire();
        if ( mpSchedulerData->mbInScheduler )
        {
            // Don't free invoking events
            while ( true )
            {
                pSVData->mpInvokeMutex->release();
                osl_waitCondition( pSVData->maInvokeCondition, nullptr );
                pSVData->mpInvokeMutex->acquire();
                if ( !mpSchedulerData->mbInScheduler )
                    break;
                else
                    osl_resetCondition( pSVData->maInvokeCondition );
            }
        }
    }
    if ( pSVData->mbDeInit )
        return;

    SAL_INFO( "vcl.schedule", tools::Time::GetSystemTicks()
        << " " << mpSchedulerData << "  disposed   " << *this );
    if ( mpSchedulerData )
    {
        mpSchedulerData->mpScheduler = nullptr;
        mpSchedulerData = nullptr;
    }
    if ( bUseLocks )
        pSVData->mpInvokeMutex->release();
}

Scheduler& Scheduler::operator=( const Scheduler& rScheduler )
{
    if ( IsActive() )
        Stop();

    mePriority = rScheduler.mePriority;

    if ( rScheduler.IsActive() )
        Start();

    return *this;
}

Scheduler::Scheduler(const sal_Char *pDebugName):
    mpSchedulerData(nullptr),
    mpDebugName(pDebugName),
    mePriority(SchedulerPriority::DEFAULT)
{
}

Scheduler::Scheduler( const Scheduler& rScheduler ):
    mpSchedulerData(nullptr),
    mpDebugName(rScheduler.mpDebugName),
    mePriority(rScheduler.mePriority)
{
    if ( rScheduler.IsActive() )
        Start();
}

Scheduler::~Scheduler()
{
    Dispose();
}

const char *ImplSchedulerData::GetDebugName() const
{
    return mpScheduler && mpScheduler->GetDebugName() ?
        mpScheduler->GetDebugName() : "unknown";
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
