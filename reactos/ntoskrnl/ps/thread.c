/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ps/thread.c
 * PURPOSE:         Process Manager: Thread Management
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Thomas Weidenmueller (w3seek@reactos.org)
 */

/* INCLUDES ****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <internal/debug.h>

/* GLOBALS ******************************************************************/

extern LIST_ENTRY PsActiveProcessHead;
extern PEPROCESS PsIdleProcess;
extern PVOID PspSystemDllEntryPoint;
extern PVOID PspSystemDllBase;
extern PHANDLE_TABLE PspCidTable;
extern BOOLEAN CcPfEnablePrefetcher;
extern ULONG MmReadClusterSize;
POBJECT_TYPE PsThreadType = NULL;

/* FUNCTIONS ***************************************************************/

VOID
NTAPI
PspUserThreadStartup(PKSTART_ROUTINE StartRoutine,
                     PVOID StartContext)
{
    PETHREAD Thread;
    PTEB Teb;
    BOOLEAN DeadThread = FALSE;
    PAGED_CODE();

    /* Go to Passive Level */
    KeLowerIrql(PASSIVE_LEVEL);
    Thread = PsGetCurrentThread();

    /* Check if the thread is dead */
    if (Thread->DeadThread)
    {
        /* Remember that we're dead */
        DeadThread = TRUE;
    }
    else
    {
        /* Get the Locale ID and save Preferred Proc */
        Teb =  NtCurrentTeb();
        Teb->CurrentLocale = MmGetSessionLocaleId();
        Teb->IdealProcessor = Thread->Tcb.IdealProcessor;
    }

    /* Check if this is a system thread, or if we're hiding */
    if (!(Thread->SystemThread) && !(Thread->HideFromDebugger))
    {
        /* We're not, so notify the debugger */
        DbgkCreateThread(StartContext);
    }

    /* Make sure we're not already dead */
    if (!DeadThread)
    {
        /* Check if the Prefetcher is enabled */
        if (CcPfEnablePrefetcher)
        {
            /* FIXME: Prepare to prefetch this process */
        }

        /* Raise to APC */
        KfRaiseIrql(APC_LEVEL);

        /* Queue the User APC */
        KiInitializeUserApc(NULL,
                            (PVOID)((ULONG_PTR)Thread->Tcb.InitialStack -
                            sizeof(KTRAP_FRAME) -
                            sizeof(FX_SAVE_AREA)),
                            PspSystemDllEntryPoint,
                            NULL,
                            PspSystemDllBase,
                            NULL);

        /* Lower it back to passive */
        KeLowerIrql(PASSIVE_LEVEL);
    }
    else
    {
        /* We're dead, kill us now */
        PspTerminateThreadByPointer(Thread,
                                    STATUS_THREAD_IS_TERMINATING,
                                    TRUE);
    }

    /* Do we have a cookie set yet? */
    if (!SharedUserData->Cookie)
    {
        /*
         * FIXME: Generate cookie
         * Formula (roughly): Per-CPU Page Fault ^ Per-CPU Interrupt Time ^
         *                    Global System Time ^ Stack Address of where
         *                    the LARGE_INTEGER containing the Global System
         *                    Time is.
         */
    }
}

VOID
NTAPI
PspSystemThreadStartup(PKSTART_ROUTINE StartRoutine,
                       PVOID StartContext)
{
    PETHREAD Thread;

    /* Unlock the dispatcher Database */
    KeLowerIrql(PASSIVE_LEVEL);
    Thread = PsGetCurrentThread();

    /* Make sure the thread isn't gone */
    if (!(Thread->Terminated) || !(Thread->DeadThread))
    {
        /* Call it the Start Routine */
        StartRoutine(StartContext);
    }

    /* Exit the thread */
    PspTerminateThreadByPointer(Thread, STATUS_SUCCESS, TRUE);
}

NTSTATUS
NTAPI
PspCreateThread(OUT PHANDLE ThreadHandle,
                IN ACCESS_MASK DesiredAccess,
                IN POBJECT_ATTRIBUTES ObjectAttributes  OPTIONAL,
                IN HANDLE ProcessHandle,
                IN PEPROCESS TargetProcess,
                OUT PCLIENT_ID ClientId,
                IN PCONTEXT ThreadContext,
                IN PINITIAL_TEB InitialTeb,
                IN BOOLEAN CreateSuspended,
                IN PKSTART_ROUTINE StartRoutine OPTIONAL,
                IN PVOID StartContext OPTIONAL)
{
    HANDLE hThread;
    PEPROCESS Process;
    PETHREAD Thread;
    PTEB TebBase = NULL;
    KIRQL OldIrql;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status;
    HANDLE_TABLE_ENTRY CidEntry;
    PAGED_CODE();

    /* If we were called from PsCreateSystemThread, then we're kernel mode */
    if (StartRoutine) PreviousMode = KernelMode;

    /* Reference the Process by handle or pointer, depending on what we got */
    if (ProcessHandle)
    {
        /* Normal thread or System Thread */
        Status = ObReferenceObjectByHandle(ProcessHandle,
                                           PROCESS_CREATE_THREAD,
                                           PsProcessType,
                                           PreviousMode,
                                           (PVOID*)&Process,
                                           NULL);
    }
    else
    {
        /* System thread inside System Process, or Normal Thread with a bug */
        if (StartRoutine)
        {
            /* Reference the Process by Pointer */
            ObReferenceObject(TargetProcess);
            Process = TargetProcess;
            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Fake ObReference returning this */
            Status = STATUS_INVALID_HANDLE;
        }
    }

    /* Check for success */
    if (!NT_SUCCESS(Status)) return Status;

    /* Also make sure that User-Mode isn't trying to create a system thread */
    if ((PreviousMode != KernelMode) && (Process == PsInitialSystemProcess))
    {
        ObDereferenceObject(Process);
        return STATUS_INVALID_HANDLE;
    }

    /* Create Thread Object */
    Status = ObCreateObject(PreviousMode,
                            PsThreadType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(ETHREAD),
                            0,
                            0,
                            (PVOID*)&Thread);
    if (!NT_SUCCESS(Status))
    {
        /* We failed; dereference the process and exit */
        ObDereferenceObject(Process);
        return Status;
    }

    /* Zero the Object entirely */
    RtlZeroMemory(Thread, sizeof(ETHREAD));

    /* Initialize rundown protection */
    ExInitializeRundownProtection(&Thread->RundownProtect);

    /* Set the Process CID */
    Thread->ThreadsProcess = Process;
    Thread->Cid.UniqueProcess = Process->UniqueProcessId;

    /* Create Cid Handle */
    CidEntry.Object = Thread;
    CidEntry.GrantedAccess = 0;
    Thread->Cid.UniqueThread = ExCreateHandle(PspCidTable, &CidEntry);
    if (!Thread->Cid.UniqueThread)
    {
        /* We couldn't create the CID, dereference the thread and fail */
        ObDereferenceObject(Thread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Save the read cluster size */
    Thread->ReadClusterSize = MmReadClusterSize;

    /* Initialize the LPC Reply Semaphore */
    KeInitializeSemaphore(&Thread->LpcReplySemaphore, 0, 1);

    /* Initialize the list heads and locks */
    InitializeListHead(&Thread->LpcReplyChain);
    InitializeListHead(&Thread->IrpList);
    InitializeListHead(&Thread->PostBlockList);
    InitializeListHead(&Thread->ActiveTimerListHead);
    KeInitializeSpinLock(&Thread->ActiveTimerListLock);

    /* Acquire rundown protection */
    ExAcquireRundownProtection(&Process->RundownProtect);

    /* Now let the kernel initialize the context */
    if (ThreadContext)
    {
        /* User-mode Thread, create Teb */
        TebBase = MmCreateTeb(Process, &Thread->Cid, InitialTeb);
        if (!TebBase)
        {
            /* Failed to create the TEB. Release rundown and dereference */
            ExReleaseRundownProtection(&Process->RundownProtect);
            ObDereferenceObject(Thread);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* Set the Start Addresses */
        Thread->StartAddress = (PVOID)ThreadContext->Eip;
        Thread->Win32StartAddress = (PVOID)ThreadContext->Eax;

        /* Let the kernel intialize the Thread */
        Status = KeInitThread(&Thread->Tcb,
                              NULL,
                              PspUserThreadStartup,
                              NULL,
                              Thread->StartAddress,
                              ThreadContext,
                              TebBase,
                              &Process->Pcb);
    }
    else
    {
        /* System Thread */
        Thread->StartAddress = StartRoutine;
        InterlockedOr((PLONG)&Thread->CrossThreadFlags, CT_SYSTEM_THREAD_BIT);

        /* Let the kernel intialize the Thread */
        Status = KeInitThread(&Thread->Tcb,
                              NULL,
                              PspSystemThreadStartup,
                              StartRoutine,
                              StartContext,
                              NULL,
                              NULL,
                              &Process->Pcb);
    }

    /* Check if we failed */
    if (!NT_SUCCESS(Status))
    {
        /* Delete the TEB if we had done */
        if (TebBase) MmDeleteTeb(Process, TebBase);

        /* Release rundown and dereference */
        ExReleaseRundownProtection(&Process->RundownProtect);
        ObDereferenceObject(Thread);
        return Status;
    }

    /* FIXME: Acquire exclusive pushlock */

    /*
     * Insert the Thread into the Process's Thread List
     * Note, this is the ETHREAD Thread List. It is removed in
     * ps/kill.c!PspExitThread.
     */
    InsertTailList(&Process->ThreadListHead, &Thread->ThreadListEntry);
    Process->ActiveThreads++;

    /* Start the thread */
    KeStartThread(&Thread->Tcb);

    /* FIXME: Wake pushlock */

    /* Release rundown */
    ExReleaseRundownProtection(&Process->RundownProtect);

    /* Notify WMI */
    //WmiTraceProcess(Process, TRUE);
    //WmiTraceThread(Thread, InitialTeb, TRUE);

    /* Notify Thread Creation */
    PspRunCreateThreadNotifyRoutines(Thread, TRUE);

    /* Suspend the Thread if we have to */
    if (CreateSuspended) KeSuspendThread(&Thread->Tcb);

    /* Check if we were already terminated */
    if (Thread->Terminated)
    {
        /* Force us to wake up to terminate */
        KeForceResumeThread(&Thread->Tcb);
    }

    /* Reference ourselves as a keep-alive */
    ObReferenceObject(Thread);

    /* Insert the Thread into the Object Manager */
    Status = ObInsertObject((PVOID)Thread,
                            NULL,
                            DesiredAccess,
                            0,
                            NULL,
                            &hThread);
    if(NT_SUCCESS(Status))
    {
        /* Wrap in SEH to protect against bad user-mode pointers */
        _SEH_TRY
        {
            /* Return Cid and Handle */
            if(ClientId) *ClientId = Thread->Cid;
            *ThreadHandle = hThread;
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;
    }

    /* FIXME: SECURITY */

    /* Dispatch thread */
    OldIrql = KeAcquireDispatcherDatabaseLock ();
    KiReadyThread(&Thread->Tcb);
    KeReleaseDispatcherDatabaseLock(OldIrql);

    /* Return */
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsCreateSystemThread(PHANDLE ThreadHandle,
                     ACCESS_MASK DesiredAccess,
                     POBJECT_ATTRIBUTES ObjectAttributes,
                     HANDLE ProcessHandle,
                     PCLIENT_ID ClientId,
                     PKSTART_ROUTINE StartRoutine,
                     PVOID StartContext)
{
    PEPROCESS TargetProcess = NULL;
    HANDLE Handle = ProcessHandle;
    PAGED_CODE();

    /* Check if we have a handle. If not, use the System Process */
    if (!ProcessHandle)
    {
        Handle = NULL;
        TargetProcess = PsInitialSystemProcess;
    }

    /* Call the shared function */
    return PspCreateThread(ThreadHandle,
                           DesiredAccess,
                           ObjectAttributes,
                           Handle,
                           TargetProcess,
                           ClientId,
                           NULL,
                           NULL,
                           FALSE,
                           StartRoutine,
                           StartContext);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsLookupThreadByThreadId(IN HANDLE ThreadId,
                         OUT PETHREAD *Thread)
{
    PHANDLE_TABLE_ENTRY CidEntry;
    PETHREAD FoundThread;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    PAGED_CODE();
    KeEnterCriticalRegion();

    /* Get the CID Handle Entry */
    if ((CidEntry = ExMapHandleToPointer(PspCidTable,
                                         ThreadId)))
    {
        /* Get the Process */
        FoundThread = CidEntry->Object;

        /* Make sure it's really a process */
        if (FoundThread->Tcb.DispatcherHeader.Type == ThreadObject)
        {
            /* Reference and return it */
            ObReferenceObject(FoundThread);
            *Thread = FoundThread;
            Status = STATUS_SUCCESS;
        }

        /* Unlock the Entry */
        ExUnlockHandleTableEntry(PspCidTable, CidEntry);
    }
    
    /* Return to caller */
    KeLeaveCriticalRegion();
    return Status;
}

/*
 * @implemented
 */
HANDLE
NTAPI
PsGetCurrentThreadId(VOID)
{
    return(PsGetCurrentThread()->Cid.UniqueThread);
}

/*
 * @implemented
 */
ULONG
NTAPI
PsGetThreadFreezeCount(PETHREAD Thread)
{
    return Thread->Tcb.FreezeCount;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
PsGetThreadHardErrorsAreDisabled(PETHREAD Thread)
{
    return Thread->HardErrorsAreDisabled;
}

/*
 * @implemented
 */
HANDLE
NTAPI
PsGetThreadId(PETHREAD Thread)
{
    return Thread->Cid.UniqueThread;
}

/*
 * @implemented
 */
PEPROCESS
NTAPI
PsGetThreadProcess(PETHREAD Thread)
{
    return Thread->ThreadsProcess;
}

/*
 * @implemented
 */
HANDLE
NTAPI
PsGetThreadProcessId(PETHREAD Thread)
{
    return Thread->Cid.UniqueProcess;
}

/*
 * @implemented
 */
HANDLE
NTAPI
PsGetThreadSessionId(PETHREAD Thread)
{
    return (HANDLE)Thread->ThreadsProcess->Session;
}

/*
 * @implemented
 */
PTEB
NTAPI
PsGetThreadTeb(PETHREAD Thread)
{
    return Thread->Tcb.Teb;
}

/*
 * @implemented
 */
PVOID
NTAPI
PsGetThreadWin32Thread(PETHREAD Thread)
{
    return Thread->Tcb.Win32Thread;
}

/*
 * @implemented
 */
KPROCESSOR_MODE
NTAPI
PsGetCurrentThreadPreviousMode(VOID)
{
    return (KPROCESSOR_MODE)PsGetCurrentThread()->Tcb.PreviousMode;
}

/*
 * @implemented
 */
PVOID
NTAPI
PsGetCurrentThreadStackBase(VOID)
{
    return PsGetCurrentThread()->Tcb.StackBase;
}

/*
 * @implemented
 */
PVOID
NTAPI
PsGetCurrentThreadStackLimit(VOID)
{
    return (PVOID)PsGetCurrentThread()->Tcb.StackLimit;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
PsIsThreadTerminating(IN PETHREAD Thread)
{
    return (Thread->Terminated ? TRUE : FALSE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
PsIsSystemThread(PETHREAD Thread)
{
    return (Thread->SystemThread ? TRUE: FALSE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
PsIsThreadImpersonating(PETHREAD Thread)
{
    return Thread->ActiveImpersonationInfo;
}

/*
 * @implemented
 */
VOID
NTAPI
PsSetThreadHardErrorsAreDisabled(PETHREAD Thread,
                                 BOOLEAN HardErrorsAreDisabled)
{
    Thread->HardErrorsAreDisabled = HardErrorsAreDisabled;
}

/*
 * @implemented
 */
VOID
NTAPI
PsSetThreadWin32Thread(PETHREAD Thread,
                       PVOID Win32Thread)
{
    Thread->Tcb.Win32Thread = Win32Thread;
}

NTSTATUS
NTAPI
NtCreateThread(OUT PHANDLE ThreadHandle,
               IN ACCESS_MASK DesiredAccess,
               IN POBJECT_ATTRIBUTES ObjectAttributes  OPTIONAL,
               IN HANDLE ProcessHandle,
               OUT PCLIENT_ID ClientId,
               IN PCONTEXT ThreadContext,
               IN PINITIAL_TEB InitialTeb,
               IN BOOLEAN CreateSuspended)
{
    INITIAL_TEB SafeInitialTeb;
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    /* Check if this was from user-mode */
    if(KeGetPreviousMode() != KernelMode)
    {
        /* Make sure that we got a context */
        if (!ThreadContext) return STATUS_INVALID_PARAMETER;

        /* Protect checks */
        _SEH_TRY
        {
            /* Make sure the handle pointer we got is valid */
            ProbeForWriteHandle(ThreadHandle);

            /* Check if the caller wants a client id */
            if(ClientId)
            {
                /* Make sure we can write to it */
                ProbeForWrite(ClientId, sizeof(CLIENT_ID), sizeof(ULONG));
            }

            /* Make sure that the entire context is readable */
            ProbeForRead(ThreadContext, sizeof(CONTEXT), sizeof(ULONG));

            /* Check the Initial TEB */
            ProbeForRead(InitialTeb, sizeof(INITIAL_TEB), sizeof(ULONG));
            SafeInitialTeb = *InitialTeb;
            }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;

        /* Handle any failures in our SEH checks */
        if (!NT_SUCCESS(Status)) return Status;
    }
    else
    {
        /* Use the Initial TEB as is */
        SafeInitialTeb = *InitialTeb;
    }

    /* Call the shared function */
    return PspCreateThread(ThreadHandle,
                           DesiredAccess,
                           ObjectAttributes,
                           ProcessHandle,
                           NULL,
                           ClientId,
                           ThreadContext,
                           &SafeInitialTeb,
                           CreateSuspended,
                           NULL,
                           NULL);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
NtOpenThread(OUT PHANDLE ThreadHandle,
             IN ACCESS_MASK DesiredAccess,
             IN POBJECT_ATTRIBUTES ObjectAttributes,
             IN PCLIENT_ID ClientId  OPTIONAL)
{
    KPROCESSOR_MODE PreviousMode;
    CLIENT_ID SafeClientId;
    ULONG Attributes = 0;
    HANDLE hThread = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    PETHREAD Thread;
    BOOLEAN HasObjectName = FALSE;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    /* Probe the paraemeters */
    if(PreviousMode != KernelMode)
    {
        _SEH_TRY
        {
            ProbeForWriteHandle(ThreadHandle);

            if(ClientId != NULL)
            {
                ProbeForRead(ClientId,
                             sizeof(CLIENT_ID),
                             sizeof(ULONG));

                SafeClientId = *ClientId;
                ClientId = &SafeClientId;
            }

            /* just probe the object attributes structure, don't capture it
               completely. This is done later if necessary */
            ProbeForRead(ObjectAttributes,
                         sizeof(OBJECT_ATTRIBUTES),
                         sizeof(ULONG));
            HasObjectName = (ObjectAttributes->ObjectName != NULL);
            Attributes = ObjectAttributes->Attributes;
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;

        if(!NT_SUCCESS(Status)) return Status;
    }
    else
    {
        HasObjectName = (ObjectAttributes->ObjectName != NULL);
        Attributes = ObjectAttributes->Attributes;
    }
    
    if (HasObjectName && ClientId != NULL)
    {
        /* can't pass both, n object name and a client id */
        return STATUS_INVALID_PARAMETER_MIX;
    }

    /* Open by name if one was given */
    if (HasObjectName)
    {
        /* Open it */
        Status = ObOpenObjectByName(ObjectAttributes,
                                    PsThreadType,
                                    PreviousMode,
                                    NULL,
                                    DesiredAccess,
                                    NULL,
                                    &hThread);
    }
    else if (ClientId)
    {
        /* Open by Thread ID */
        if (ClientId->UniqueProcess)
        {
            /* Get the Process */
            Status = PsLookupProcessThreadByCid(ClientId,
                                                NULL,
                                                &Thread);
        }
        else
        {
            /* Get the Process */
            Status = PsLookupThreadByThreadId(ClientId->UniqueThread,
                                              &Thread);
        }

        /* Fail if we didn't find anything */
        if(!NT_SUCCESS(Status)) return Status;

        /* Open the Thread Object */
        Status = ObOpenObjectByPointer(Thread,
                                       Attributes,
                                       NULL,
                                       DesiredAccess,
                                       PsThreadType,
                                       PreviousMode,
                                       &hThread);

        /* Dereference the thread */
        ObDereferenceObject(Thread);
    }
    else
    {
        /* neither an object name nor a client id was passed */
        return STATUS_INVALID_PARAMETER_MIX;
    }

    /* Check for success */
    if (NT_SUCCESS(Status))
    {
        /* Protect against bad user-mode pointers */
        _SEH_TRY
        {
            /* Write back the handle */
            *ThreadHandle = hThread;
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
NtYieldExecution(VOID)
{
    KiDispatchThread(Ready);
    return(STATUS_SUCCESS);
}

/*
 * @implemented
 */
KPROCESSOR_MODE
NTAPI
ExGetPreviousMode (VOID)
{
    return (KPROCESSOR_MODE)PsGetCurrentThread()->Tcb.PreviousMode;
}

/* EOF */
