#pragma once
extern "C"
{
	#include "CreateDriver.h"
}
#include "shared.h"
#include "imports.h"
#include "utils.h"
#include "Common.h"
#include "Physmem.h"

extern PVOID g_SharedSection;
HANDLE hClientEvent = NULL;
HANDLE hDriverEvent = NULL;
PKEVENT pClientEvent = NULL;
PKEVENT pDriverEvent = NULL;

BYTE* data;

typedef struct _LDR_MODULE {
	LIST_ENTRY              InLoadOrderModuleList;
	LIST_ENTRY              InMemoryOrderModuleList;
	LIST_ENTRY              InInitializationOrderModuleList;
	PVOID                   BaseAddress;
	PVOID                   EntryPoint;
	ULONG                   SizeOfImage;
	UNICODE_STRING          FullDllName;
	UNICODE_STRING          BaseDllName;
	ULONG                   Flags;
	SHORT                   LoadCount;
	SHORT                   TlsIndex;
	LIST_ENTRY              HashTableEntry;
	ULONG                   TimeDateStamp;
} LDR_MODULE, * PLDR_MODULE;




typedef struct _SYSTEM_THREAD_INFORMATION
{
	LARGE_INTEGER KernelTime;
	LARGE_INTEGER UserTime;
	LARGE_INTEGER CreateTime;
	ULONG WaitTime;
	PVOID StartAddress;
	CLIENT_ID ClientId;
	KPRIORITY Priority;
	LONG BasePriority;
	ULONG ContextSwitches;
	ULONG ThreadState;
	KWAIT_REASON WaitReason;
}SYSTEM_THREAD_INFORMATION, * PSYSTEM_THREAD_INFORMATION;

typedef struct _SYSTEM_PROCESS_INFO
{
	ULONG NextEntryOffset;
	ULONG NumberOfThreads;
	LARGE_INTEGER WorkingSetPrivateSize;
	ULONG HardFaultCount;
	ULONG NumberOfThreadsHighWatermark;
	ULONGLONG CycleTime;
	LARGE_INTEGER CreateTime;
	LARGE_INTEGER UserTime;
	LARGE_INTEGER KernelTime;
	UNICODE_STRING ImageName;
	KPRIORITY BasePriority;
	HANDLE UniqueProcessId;
	HANDLE InheritedFromUniqueProcessId;
	ULONG HandleCount;
	ULONG SessionId;
	ULONG_PTR UniqueProcessKey;
	SIZE_T PeakVirtualSize;
	SIZE_T VirtualSize;
	ULONG PageFaultCount;
	SIZE_T PeakWorkingSetSize;
	SIZE_T WorkingSetSize;
	SIZE_T QuotaPeakPagedPoolUsage;
	SIZE_T QuotaPagedPoolUsage;
	SIZE_T QuotaPeakNonPagedPoolUsage;
	SIZE_T QuotaNonPagedPoolUsage;
	SIZE_T PagefileUsage;
	SIZE_T PeakPagefileUsage;
	SIZE_T PrivatePageCount;
	LARGE_INTEGER ReadOperationCount;
	LARGE_INTEGER WriteOperationCount;
	LARGE_INTEGER OtherOperationCount;
	LARGE_INTEGER ReadTransferCount;
	LARGE_INTEGER WriteTransferCount;
	LARGE_INTEGER OtherTransferCount;
	SYSTEM_THREAD_INFORMATION Threads[1];
}SYSTEM_PROCESS_INFO, * PSYSTEM_PROCESS_INFO;

typedef enum _KAPC_ENVIRONMENT {
	OriginalApcEnvironment,
	AttachedApcEnvironment,
	CurrentApcEnvironment,
	InsertApcEnvironment
} KAPC_ENVIRONMENT, * PKAPC_ENVIRONMENT;


typedef struct {
	struct _DISPATCHER_HEADER Header;
	struct _LIST_ENTRY ProfileListHead;
	unsigned int DirectoryTableBase;
	unsigned long Asid;
	struct _LIST_ENTRY ThreadListHead;
} __KPROCESS, * __PKPROCESS;

typedef VOID(NTAPI* PKNORMAL_ROUTINE)(
	_In_ PVOID NormalContext,
	_In_ PVOID SystemArgument1,
	_In_ PVOID SystemArgument2);
typedef VOID KKERNEL_ROUTINE(
	_In_ PRKAPC Apc,
	_Inout_opt_ PKNORMAL_ROUTINE* NormalRoutine,
	_Inout_opt_ PVOID* NormalContext,
	_Inout_ PVOID* SystemArgument1,
	_Inout_ PVOID* SystemArgument2
);
typedef KKERNEL_ROUTINE(NTAPI* PKKERNEL_ROUTINE);
typedef VOID(NTAPI* PKRUNDOWN_ROUTINE)(_In_ PRKAPC Apc);
typedef PETHREAD(NTAPI* PsGetNextProcessThread_t)(PEPROCESS process, PETHREAD thread);
VOID KernelApcStub(
	PKAPC Apc,
	PKNORMAL_ROUTINE* NormalRoutine,
	PVOID* NormalContext,
	PVOID* SystemArgument1,
	PVOID* SystemArgument2)
{
	UNREFERENCED_PARAMETER(NormalRoutine);
	UNREFERENCED_PARAMETER(NormalContext);
	UNREFERENCED_PARAMETER(SystemArgument1);
	UNREFERENCED_PARAMETER(SystemArgument2);

	// Free APC object now that it's been delivered
	if (Apc)
		ExFreePoolWithTag(Apc, 'cpaR');
}

extern "C" {
	NTSTATUS
		NTAPI
		ZwProtectVirtualMemory(
			IN HANDLE               ProcessHandle,
			IN OUT PVOID* BaseAddress,
			IN OUT PULONG           NumberOfBytesToProtect,
			IN ULONG                NewAccessProtection,
			OUT PULONG              OldAccessProtection);
	VOID NTAPI KeInitializeApc(
		_Out_ PRKAPC Apc,
		_In_ PRKTHREAD Thread,
		_In_ KAPC_ENVIRONMENT Environment,
		_In_ PKKERNEL_ROUTINE KernelRoutine,
		_In_opt_ PKRUNDOWN_ROUTINE RundownRoutine,
		_In_opt_ PKNORMAL_ROUTINE NormalRoutine,
		_In_opt_ KPROCESSOR_MODE ProcessorMode,
		_In_opt_ PVOID NormalContext
	);
	BOOLEAN NTAPI KeInsertQueueApc(
		_Inout_ PRKAPC Apc,
		_In_opt_ PVOID SystemArgument1,
		_In_opt_ PVOID SystemArgument2,
		_In_ KPRIORITY Increment
	);

}

/* This will be used for the APC injection */
// Forward declaration (undocumented but extremely stable since XP → Win11)
typedef PETHREAD(*PsGetNextProcessThread_t)(
	PEPROCESS Process,
	PETHREAD PreviousThread   // pass NULL to get first thread
	);


NTSTATUS WriteProcessMemoryVirtual(int pid, PVOID Address, PVOID AllocatedBuffer, SIZE_T size, SIZE_T* written)
{
	PEPROCESS pProcess = NULL;
	if (pid == 0) return STATUS_UNSUCCESSFUL;
	NTSTATUS NtRet = PsLookupProcessByProcessId(UlongToHandle(pid), &pProcess);
	if (NtRet != STATUS_SUCCESS) return NtRet;

	// MmCopyVirtualMemory: kernel (self) -> target process virtual memory
	// No CR3, no page walk, no physical translation needed
	if (Address == NULL || AllocatedBuffer == NULL)
	{
		KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
			"[-] %s : Address or AllocatedBuffer is NULL\n", __FUNCTION__));
		return STATUS_UNSUCCESSFUL;
	}
	if (MmIsAddressValid(Address) == FALSE)
	{
		KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
			"[-] %s : Target Address is not valid\n", __FUNCTION__));
		return STATUS_UNSUCCESSFUL;
	}

	SIZE_T BytesWritten = 0;
	NtRet = MmCopyVirtualMemory(
		PsGetCurrentProcess(),  // source: our kernel/usermode caller
		AllocatedBuffer,        // source address (our buffer)
		pProcess,               // target: game process
		Address,                // target address (game memory)
		size,                   // bytes to copy
		KernelMode,             // previous mode
		&BytesWritten           // bytes actually written
	);

	ObDereferenceObject(pProcess);
	*written = BytesWritten;
	return NtRet;
}

static PsGetNextProcessThread_t g_PsGetNextProcessThread = NULL;

PETHREAD FindAlertableThread(PEPROCESS Process)
{
	if (!g_PsGetNextProcessThread)
	{
		UNICODE_STRING fnName;
		RtlInitUnicodeString(&fnName, L"PsGetNextProcessThread");
		g_PsGetNextProcessThread = (PsGetNextProcessThread_t)MmGetSystemRoutineAddress(&fnName);
		if (!g_PsGetNextProcessThread)
		{
			KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
				"[-] APC: PsGetNextProcessThread not resolved\n"));
			return NULL;
		}
	}

	// Walk all threads in the process
	// PsGetNextProcessThread(Process, NULL) → first thread
	// PsGetNextProcessThread(Process, prevThread) → next thread (auto-derefs prev)
	// Returns NULL when no more threads

	PETHREAD thread = NULL;
	PETHREAD fallback = NULL;

	for (;;)
	{
		thread = g_PsGetNextProcessThread(Process, thread);
		if (!thread)
			break;

		// KTHREAD offsets (Win10 22H2 / Win11 23H2+):
		//   +0x184 = State (UCHAR)    — 5 = Waiting
		//   +0x187 = WaitMode (UCHAR) — 1 = UserMode
		//   +0x188 = Alertable (UCHAR)
		//
		// DayZ has multiple threads in alertable user-mode waits
		// (NtWaitForWorkViaWorkerFactory, GetMessage, SleepEx, etc.)

		PUCHAR kthread = (PUCHAR)thread;
		UCHAR state = kthread[0x184];
		UCHAR waitMode = kthread[0x187];
		UCHAR alertable = kthread[0x188];

		if (state == 5 && waitMode == 1 && alertable)
		{
			// Perfect: user-mode alertable wait — APC fires immediately
			KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
				"[+] APC: Found alertable thread %p\n", thread));
			return thread;
		}

		// Keep first waiting thread as fallback
		if (state == 5 && !fallback)
		{
			fallback = thread;
			// Don't return yet — keep scanning for a truly alertable one
			// We need to keep iterating so PsGetNextProcessThread derefs properly,
			// but we also need to hold the fallback. Re-lookup later.
		}
	}

	// Fallback: use any waiting thread
	// APC queues and fires next time thread enters alertable wait
	if (fallback)
	{
		// Re-lookup since PsGetNextProcessThread already deref'd it during iteration
		// We'll reference it via PsLookupThreadByThreadId if needed,
		// but actually KeInsertQueueApc just needs a valid PETHREAD.
		// The fallback pointer was deref'd when we called PsGetNextProcessThread
		// with it as prev. So we need to re-find it.
		//
		// Simpler: just do a second pass and return the first waiting thread.
		thread = NULL;
		for (;;)
		{
			thread = g_PsGetNextProcessThread(Process, thread);
			if (!thread) break;

			PUCHAR kt = (PUCHAR)thread;
			if (kt[0x184] == 5)  // Waiting state
			{
				KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
					"[+] APC: Using fallback waiting thread %p\n", thread));
				return thread;
			}
		}
	}

	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		"[-] APC: No suitable thread found in process\n"));
	return NULL;
}

VOID DriverLoop(PVOID StartContext) // 
{
	UNREFERENCED_PARAMETER(StartContext);
	BeDisableApc(true);
	auto status = CreateSharedMemory();

	if (!NT_SUCCESS(status))
	{
		CleanSharedMemory();
		return;
	}

	do
	{
		status = CreateNamedEvent(L"\\BaseNamedObjects\\KM", SynchronizationEvent, FALSE, &hDriverEvent, &pDriverEvent);
		
		if (!NT_SUCCESS(status))
		{
			KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
				"[-] DriverEvent creation failed!\n"));
			goto cleanUp;
		}

		status = CreateNamedEvent(L"\\BaseNamedObjects\\UM", SynchronizationEvent, FALSE, &hClientEvent, &pClientEvent);

		if (!NT_SUCCESS(status))
		{
			KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
				"[-] ClientEvent creation failed!\n"));;
			goto cleanUp;
		}

		KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
			"[+] Events Created!\n"));

	} while (false);


	while (true)
	{
		
		status = KeWaitForSingleObject(
			pClientEvent,          // Pointer to the event object.
			Executive,       // Wait reason.
			KernelMode,      // Wait in kernel mode.
			FALSE,           // Not alertable.
			NULL             // No timeout; wait indefinitely.
		);

		if (!NT_SUCCESS(status))
		{
			KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
				"[-] KeWaitForSingleObject failed!\n"));
			break;
		}

		ReadSharedMemory();

		if (auto msg = (UM_Msg*)g_SharedSection)
		{
			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_ALLOCATE_MEM) // allocate memory in target process
			{

				KeMemoryBarrier();
				PEPROCESS proc;
				status = PsLookupProcessByProcessId(ULongToHandle(msg->ProcId), &proc);
				if (!NT_SUCCESS(status))
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] PsLookupProcessByProcessId failed!\n"));
					break;
				}
				PVOID baseAddress = NULL;
				// Allocate memory in the target process
				KAPC_STATE apcState;

				KeStackAttachProcess((PRKPROCESS)proc, &apcState);

				status = ZwAllocateVirtualMemory(
					ZwCurrentProcess(),  // Must be the target process handle, see below
					&baseAddress,
					0,
					&msg->dataSize,
					MEM_COMMIT | MEM_RESERVE,
					PAGE_EXECUTE_READWRITE
				);

				RtlCopyMemory(baseAddress, msg->data, msg->dataSize);
				msg->address = reinterpret_cast<ULONGLONG>(baseAddress); //*** return allocated memory address with data
				KeUnstackDetachProcess(&apcState);

				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_BASE) // get process base address
			{
				KeMemoryBarrier();
				auto BaseAddr = GetProcessBaseAddress(msg->ProcId);

				if (BaseAddr == NULL)
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] GetProcessBaseAddress failed!\n"));
					break;
				}

				msg->address = reinterpret_cast<ULONGLONG>(BaseAddr);
				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_MODULE_BASE) // get module base address(dont use it if you are only need the base address of the process (like in assaultcube))
			{
				KeMemoryBarrier();

				PEPROCESS proc;
				status = PsLookupProcessByProcessId(ULongToHandle( msg->ProcId), &proc);
				
				if (!NT_SUCCESS(status)) 
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] PsLookupProcessByProcessId failed!\n"));
					break;
				}

				UNICODE_STRING name;
				RtlInitUnicodeString(&name, L"client.dll"); // for cs2 ... u can change it

				PVOID module_address;
				ULONG module_size;

				status = utils::get_process_module_base(proc, &name, &module_address, &module_size);

				if (!NT_SUCCESS(status))
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] get_process_module_base failed!\n"));
					break;
				}

				msg->address = reinterpret_cast<ULONGLONG>(module_address);
				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_READ) // read
			{
				
				KeMemoryBarrier();
			
				data = (BYTE*)ExAllocatePoolWithTag(NonPagedPool, msg->dataSize, 'bufT');
				if (!data)
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[-] %s Failed to allocate memory\n", __FUNCTION__));
					break;
				}
				
				RtlZeroMemory(data, sizeof(data));
				SIZE_T read;
				

				status = ReadProcessMemory(msg->ProcId,
					reinterpret_cast<PVOID>(msg->address),
					data,
					msg->dataSize,
					&read);
	
				if (!NT_SUCCESS(status))
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] ReadProcessMemory failed\n"));

					ExFreePool(data);
					break;
				}

				RtlCopyMemory(msg->data, data, msg->dataSize);
				ExFreePool(data);

				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_WRITE) // write
			{
				KeMemoryBarrier();

				if (msg->ProcId == 0 || msg->data == NULL || msg->dataSize == 0)
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] Some BS got provided to WriteProcessMemory\n"));
					break;
				}

				SIZE_T written = 0;
				status = WriteProcessMemory(msg->ProcId,         
					reinterpret_cast<PVOID>(msg->address),
					msg->data,           
					msg->dataSize,       
					&written);
				
				if (!NT_SUCCESS(status))
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] WriteProcessMemory failed\n"));
					break;
				}
				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_WRITE_VIRTUAL) // write
			{
				KeMemoryBarrier();

				if (msg->ProcId == 0 || msg->data == NULL || msg->dataSize == 0)
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] Some BS got provided to WriteProcessMemory\n"));
					break;
				}

				SIZE_T written = 0;
				status = WriteProcessMemoryVirtual(msg->ProcId,
					reinterpret_cast<PVOID>(msg->address),
					msg->data,
					msg->dataSize,
					&written);

				if (!NT_SUCCESS(status))
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] WriteProcessMemory failed\n"));
					break;
				}
				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_QUEUE_APC)
			{
				// ── Protocol ──
				// msg->ProcId  = target process PID (DayZ)
				// msg->address = shellcode VA in target (from prior OP_ALLOCATE_MEM)
				// msg->data[0..7] = NormalContext pointer (param struct VA in target)
				//
				// Returns: msg->address = 1 on success, 0 on failure

				KeMemoryBarrier();

				PEPROCESS proc = NULL;
				status = PsLookupProcessByProcessId(ULongToHandle(msg->ProcId), &proc);
				if (!NT_SUCCESS(status))
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] APC: PsLookupProcessByProcessId failed 0x%X\n", status));
					msg->address = 0;
					KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
					continue;   // don't break the loop — recoverable error
				}

				// Find alertable thread
				PETHREAD targetThread = FindAlertableThread(proc);
				if (!targetThread)
				{
					ObDereferenceObject(proc);
					msg->address = 0;
					KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
					continue;
				}

				// Read NormalContext from first 8 bytes of msg->data
				PVOID normalCtx = NULL;
				RtlCopyMemory(&normalCtx, msg->data, sizeof(PVOID));

				// Allocate KAPC from non-paged pool (lives until delivered + freed in stub)
				PKAPC apc = (PKAPC)ExAllocatePoolWithTag(NonPagedPool, sizeof(KAPC), 'cpaR');
				if (!apc)
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] APC: ExAllocatePoolWithTag failed\n"));
					ObDereferenceObject(proc);
					msg->address = 0;
					KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
					continue;
				}

				// KeInitializeApc:
				//   KernelRoutine  = KernelApcStub (runs in kernel, frees KAPC)
				//   NormalRoutine  = shellcode addr (runs in user mode in target)
				//   NormalContext  = param struct addr (becomes RCX in shellcode)
				//   Mode           = UserMode (so NormalRoutine runs in user context)
				KeInitializeApc(
					apc,
					(PRKTHREAD)targetThread,
					OriginalApcEnvironment,
					(PKKERNEL_ROUTINE)KernelApcStub,
					NULL,                               // RundownRoutine
					(PKNORMAL_ROUTINE)msg->address,     // shellcode entry in target
					UserMode,
					normalCtx                           // param struct in target
				);

				BOOLEAN queued = KeInsertQueueApc(apc, NULL, NULL, 0);

				if (queued)
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[+] APC: Queued! shell=%p ctx=%p thread=%p\n",
						(PVOID)msg->address, normalCtx, targetThread));
					msg->address = 1;  // success
				}
				else
				{
					KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
						"[-] APC: KeInsertQueueApc failed\n"));
					ExFreePoolWithTag(apc, 'cpaR');
					msg->address = 0;
				}

				// Don't deref targetThread — PsGetNextProcessThread manages refcount
				// Only deref the process
				ObDereferenceObject(proc);
				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}


			// ══════════════════════════════════════════════════════════════════
			// ══════════════════════════════════════════════════════════════════
//  PASTE THIS INTO DriverLoop() in driver.h
//  BEFORE the OP_EXIT check, AFTER the OP_WRITE_VIRTUAL block
// ══════════════════════════════════════════════════════════════════

			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_READ_BATCH)
			{
				KeMemoryBarrier();

				// ── Parse request ──
				// Layout: [count:4][{addr:8, size:4} × count]
				ULONG count = *(ULONG*)msg->data;
				if (count > 200) count = 200;

				// Copy requests to stack FIRST (we overwrite data[] with results)
				struct BReq { ULONGLONG addr; ULONG size; };
				BReq reqs[200];
				RtlCopyMemory(reqs, msg->data + 4, count * sizeof(BReq));

				// ── Execute all reads, pack results into data[] ──
				ULONG offset = 0;
				for (ULONG i = 0; i < count; i++)
				{
					ULONG sz = reqs[i].size;
					if (sz > 48) sz = 48;
					if (offset + sz > sizeof(msg->data)) break; // prevent overflow

					SIZE_T bytesRead = 0;
					NTSTATUS rs = ReadProcessMemory(
						msg->ProcId,
						(PVOID)reqs[i].addr,
						msg->data + offset,   // write result directly into shared mem
						sz,
						&bytesRead
					);

					if (!NT_SUCCESS(rs))
					{
						// Zero-fill on failure (usermode sees zeroes, same as bad pointer)
						RtlZeroMemory(msg->data + offset, sz);
					}

					offset += sz;
				}

				KeSetEvent(pDriverEvent, IO_NO_INCREMENT, TRUE);
			}
			if (msg->magic == MAGIC && msg->opType == OPERATION_TYPE::OP_EXIT) // exit
			{
				break;
			}
		}
	}


cleanUp:

	if (pDriverEvent)
	{
		ObDereferenceObject(pDriverEvent);
	}
	if (pClientEvent)
	{
		ObDereferenceObject(pClientEvent);
	}
	if (hClientEvent)
	{
		ZwClose(hClientEvent);
	}
	if (hDriverEvent)
	{
		ZwClose(hDriverEvent);
	}
	
	CleanSharedMemory();
	BeDisableApc(false);
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
		"[!] Thread Exited\n"));
}


