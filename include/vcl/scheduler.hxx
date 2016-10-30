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

#ifndef INCLUDED_VCL_SCHEDULER_HXX
#define INCLUDED_VCL_SCHEDULER_HXX

#include <vcl/dllapi.h>
#include <iostream>

class Task;
struct ImplSVData;
struct ImplSchedulerData;
struct ImplSchedulerContext;

class VCL_DLLPUBLIC Scheduler final
{
    friend class Task;
    Scheduler() = delete;

    static inline bool HasPendingTasks( const ImplSchedulerContext & rSchedCtx,
                                        sal_uInt64 nTime );

    static inline void UpdateSystemTimer( ImplSchedulerContext & rSchedCtx,
                                          sal_uInt64 nMinPeriod,
                                          bool bForce, sal_uInt64 nTime );

protected:
    static void ImplStartTimer ( sal_uInt64 nMS, bool bForce, sal_uInt64 nTime );

public:
    static const SAL_CONSTEXPR sal_uInt64 ImmediateTimeoutMs = 0;
    static const SAL_CONSTEXPR sal_uInt64 InfiniteTimeoutMs  = SAL_MAX_UINT64;

    static bool       ImplInitScheduler();
    static void       ImplDeInitScheduler();

    /// Process one pending Timer with highhest priority
    static void       CallbackTaskScheduling();
    /// Are there any pending tasks to process?
    static bool       HasPendingTasks();
    /// Process one pending task ahead of time with highest priority.
    static bool       ProcessTaskScheduling();
    /// Process all events until we are idle
    static void       ProcessEventsToIdle();

    /// Control the deterministic mode.  In this mode, two subsequent runs of
    /// LibreOffice fire about the same amount idles.
    static void       SetDeterministicMode(bool bDeterministic);
    /// Return the current state of deterministic mode.
    static bool       GetDeterministicMode();
};


struct ImplSchedulerData;

enum class TaskPriority
{
    HIGHEST      = 0,
    HIGH         = 1,
    RESIZE       = 2,
    REPAINT      = 3,
    MEDIUM       = 3,
    POST_PAINT   = 4,
    DEFAULT_IDLE = 5,
    LOW          = 6,
    LOWER        = 7,
    LOWEST       = 8
};

enum class DisposePolicy
{
    WAIT_INVOKE,   ///< Wait for Invoke() to finish, if invoked
    IGNORE_INVOKE  ///< Dispose object ignoring Invoke() state (for self-deleting objects)
};

/**
 * Non of the states tell anything about being invoked.
 */
enum class TaskStatus
{
    SCHEDULED,   ///< The task is active and waiting to be invoked (again)
    STOPPED,     ///< The task is stopped
    DISPOSED     ///< The task is disposed, which prevent any further invokes!
};

class VCL_DLLPUBLIC Task
{
    friend class Scheduler;
    friend struct ImplSchedulerData;

    ImplSchedulerData *mpSchedulerData; /// Pointer to the element in scheduler list
    const sal_Char    *mpDebugName;     /// Useful for debugging
    TaskPriority       mePriority;      /// Task priority
    TaskStatus         meStatus;        /// Status of the task

protected:
    static void StartTimer( sal_uInt64 nMS );

    inline const ImplSchedulerData* GetSchedulerData() const { return mpSchedulerData; }

    virtual void SetDeletionFlags();

    /**
     * How long (in MS) until the Task is ready to be dispatched?
     *
     * Simply return Scheduler::ImmediateTimeoutMs if you're ready, like an
     * Idle. If you have to return Scheduler::InfiniteTimeoutMs, you probably
     * need an other mechanism to wake up the Scheduler or rely on other
     * Tasks to be scheduled, or simply use a polling Timer.
     *
     * @param nMinPeriod the currently expected sleep time
     * @param nTimeNow the current time
     * @return the sleep time of the Task to become ready
     */
    virtual sal_uInt64 UpdateMinPeriod( sal_uInt64 nMinPeriod, sal_uInt64 nTimeNow ) const = 0;

public:
    Task( const sal_Char *pDebugName );
    Task( const Task& rTask );
    virtual ~Task();
    Task& operator=( const Task& rTask );

    void            SetPriority(TaskPriority ePriority) { mePriority = ePriority; }
    TaskPriority    GetPriority() const { return mePriority; }

    void            SetDebugName( const sal_Char *pDebugName ) { mpDebugName = pDebugName; }
    const char     *GetDebugName() const { return mpDebugName; }

    // Call handler
    virtual void    Invoke() = 0;

    virtual void    Start();
    void            Stop();

    /**
     * Dispose and clean up the Scheduler object
     *
     * It'll wait until the object is no longer invoked!
     *
     * Also call Dispose(), if Invoke() depends on any resources you're going
     * to destroy.
     *
     * If you have a self-deleting object, i.e. an object composed or inherited
     * from Scheduler, which you delete inside it's Invoke() function, you must
     * explicitly Dispose() the invoked Scheduler object using the
     * DisposePolicy::IGNORE_INVOKE policy, otherwise the Schedulers destructor
     * will deadlock waiting for Invoke() to finish!
     *
     * @param ePolicy Optionally ignore the Schedulers Invoke() state.
     */
    void            Dispose( DisposePolicy ePolicy = DisposePolicy::WAIT_INVOKE );

    /**
     * Is this Task waiting to be invoked?
     */
    bool            IsActive() const;
};

template< typename charT, typename traits >
inline std::basic_ostream<charT, traits> & operator <<(
    std::basic_ostream<charT, traits> & stream, const Task& task )
{
    stream << "a: " << task.IsActive() << " p: " << (int) task.GetPriority();
    const sal_Char *name = task.GetDebugName();
    if( nullptr == name )
        return stream << " (nullptr)";
    else
        return stream << " " << name;
}

#endif // INCLUDED_VCL_SCHEDULER_HXX

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
