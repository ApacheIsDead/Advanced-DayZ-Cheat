#pragma once
#include <Windows.h>
#include<stdio.h>
#include <thread>
//#include<iostream>
#pragma message("__sizeof(void*) = " _CRT_STRINGIZE(sizeof(void*)))
#pragma message("__sizeof(PVOID) = " _CRT_STRINGIZE(sizeof(PVOID)))
HANDLE h_write;
HANDLE h_read;
HANDLE hClientEvent = NULL;
HANDLE hDriverEvent = NULL;

SIZE_T DestSize = 4096;
UM_Msg* ToDriver = new UM_Msg();
UM_Msg* read_view; // read_address()
UM_Msg* write_view; // read_address()
uint32_t process_id; // get_process_id()
void clean()
{
	SetLastError(0);

	if (hDriverEvent)
	{
		CloseHandle(hDriverEvent);
	}

	if (hClientEvent)
	{
		CloseHandle(hClientEvent);
	}

	if (h_read)
	{
		CloseHandle(h_read);
	}

	if (h_write)
	{
		CloseHandle(h_write);
	}

	if (ToDriver)
	{
		delete ToDriver;
	}

	printf("[+] Cleaned : %d\n", GetLastError());
}

UM_Msg* InitMsg(UM_Msg* msg, SIZE_T size)
{

	msg->address = NULL;
	msg->dataSize = size;
	memset(msg->data, 0, sizeof(msg->data));
	return msg;
}

bool OpenSharedMemory()
{
	h_write = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, "Global\\SharedSection");

	if (h_write == NULL || h_write == INVALID_HANDLE_VALUE)
	{
		printf("[-] failed to get handle for write: %X\n", GetLastError());
		return false;
	}

	h_read = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, "Global\\SharedSection");

	if (h_read == NULL || h_read == INVALID_HANDLE_VALUE)
	{
		printf("[-] failed to get handle for read: %X\n", GetLastError());
		return false;
	}

	return true;
}


bool OpenNamedEvents()
{
	hDriverEvent = OpenEvent(EVENT_ALL_ACCESS, FALSE, "Global\\KM");
	if (!hDriverEvent)
	{
		printf("[-] failed to open driver event: %X\n", GetLastError());
		return false;
	}
	hClientEvent = OpenEvent(EVENT_ALL_ACCESS, FALSE, "Global\\UM");
	if (!hClientEvent)
	{
		printf("[-] failed to open client event: %X\n", GetLastError());
		return false;
	}
	return true;
}

bool write_address(PVOID addr, const void* data, SIZE_T dataSize, int procId)
{
	// Prepare your message structure.
	UM_Msg* msg = InitMsg(ToDriver, dataSize);
	msg->address = reinterpret_cast<ULONGLONG>(addr); //***
	msg->opType = OPERATION_TYPE::OP_WRITE;
	msg->ProcId = procId;

	// Copy the provided data (of any type) into our fixed buffer.
	RtlCopyMemory(msg->data, data, dataSize);

	// Map the view for writing.
	UM_Msg* ViewBase = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);
	if (ViewBase == NULL)
	{
		printf("[-] failed to get ViewBase 2 : %X\n", GetLastError());
		return false;
	}

	size_t len = sizeof(UM_Msg);
	if (len <= DestSize)
	{
		RtlCopyMemory(ViewBase, msg, len);
		MemoryBarrier();
		auto isSet = SetEvent(hClientEvent);
		if (!isSet)
		{
			printf("[-] %s : SetEvent failed\n", __FUNCTION__);
			return false;
		}
	}
	else
	{
		printf("[-] didn't copy memory, conditions not verified\n");
		UnmapViewOfFile(ViewBase);
		return false;
	}

	UnmapViewOfFile(ViewBase);
	return true;
}

bool write_address_virtual(PVOID addr, const void* data, SIZE_T dataSize, int procId)
{
	// Prepare your message structure.
	UM_Msg* msg = InitMsg(ToDriver, dataSize);
	msg->address = reinterpret_cast<ULONGLONG>(addr); //***
	msg->opType = OPERATION_TYPE::OP_WRITE_VIRTUAL;
	msg->ProcId = procId;

	// Copy the provided data (of any type) into our fixed buffer.
	RtlCopyMemory(msg->data, data, dataSize);

	// Map the view for writing.
	UM_Msg* ViewBase = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);
	if (ViewBase == NULL)
	{
		printf("[-] failed to get ViewBase 2 : %X\n", GetLastError());
		return false;
	}

	size_t len = sizeof(UM_Msg);
	if (len <= DestSize)
	{
		RtlCopyMemory(ViewBase, msg, len);
		MemoryBarrier();
		auto isSet = SetEvent(hClientEvent);
		if (!isSet)
		{
			printf("[-] %s : SetEvent failed\n", __FUNCTION__);
			return false;
		}
	}
	else
	{
		printf("[-] didn't copy memory, conditions not verified\n");
		UnmapViewOfFile(ViewBase);
		return false;
	}

	UnmapViewOfFile(ViewBase);
	return true;
}

VOID* read_address(PVOID addr, SIZE_T InSize, int procId)
{

	auto msg = InitMsg(ToDriver, InSize);
	msg->address = reinterpret_cast<ULONGLONG>(addr); //***
	msg->opType = OPERATION_TYPE::OP_READ;
	msg->ProcId = procId;

	if (write_view)
	{
		size_t len = sizeof(UM_Msg);

		if (write_view != NULL && msg != NULL && len <= DestSize)
		{
			RtlCopyMemory(write_view, msg, len);
			MemoryBarrier();
			auto isSet = SetEvent(hClientEvent);
			if (!isSet)
			{
				printf("[-] %s : SetEvent failed\n", __FUNCTION__);
				return NULL;
			}
		}
		else
		{
			printf("[-] didnt copy memory , conditions not verified\n");
			return NULL;
		}
		if (!read_view)
		{
			read_view = (UM_Msg*)MapViewOfFile(h_read, FILE_MAP_READ, 0, 0, DestSize);
			if (read_view == NULL)
			{
				printf("[-] failed to get ReadMap: %X\n", GetLastError());
				return NULL;
			}
		}

		auto result = WaitForSingleObject(hDriverEvent, INFINITE);
		if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
		{
			return read_view->data;
		}
		else
		{
			printf("[-] %s : WaitForSingleObject failed or magic: %X\n", __FUNCTION__, GetLastError());
			return NULL;
		}
	}

	write_view = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);

	if (write_view == NULL)
	{
		printf("[-] failed to get ViewBase 1: %X\n", GetLastError());
		return NULL;
	}

	size_t len = sizeof(UM_Msg);

	if (write_view != NULL && msg != NULL && len <= DestSize)
	{
		RtlCopyMemory(write_view, msg, len);
		MemoryBarrier();
		auto isSet = SetEvent(hClientEvent);
		if (!isSet)
		{
			printf("[-] %s : SetEvent failed\n", __FUNCTION__);
			return NULL;
		}
	}
	else
	{
		printf("[-] didnt copy memory , conditions not verified\n");
		return NULL;
	}
	if (!read_view)
	{
		read_view = (UM_Msg*)MapViewOfFile(h_read, FILE_MAP_READ, 0, 0, DestSize);
		if (read_view == NULL)
		{
			printf("[-] failed to get ReadMap: %X\n", GetLastError());
			return NULL;
		}
	}
	auto result = WaitForSingleObject(hDriverEvent, INFINITE);
	if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
	{
		return read_view->data;
	}
	else
	{
		printf("[-] %s : WaitForSingleObject failed or magic: %X\n", __FUNCTION__, GetLastError());
		return NULL;
	}
}

void ExitSystemThread()
{
	SetLastError(0);
	auto cunt = InitMsg(ToDriver,0);
	cunt->opType = OPERATION_TYPE::OP_EXIT;

	auto ViewBase = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);

	if (ViewBase == NULL)
	{
		printf("[-] failed to get ViewBase 3 : %X\n", GetLastError());
		return;
	}

	size_t len = sizeof(UM_Msg);

	if (ViewBase != NULL && cunt != NULL && len <= DestSize)
	{
		RtlCopyMemory(ViewBase, cunt, len);
		MemoryBarrier();
		auto isSet = SetEvent(hClientEvent);
		if (!isSet)
		{
			printf("[-] %s : SetEvent failed\n", __FUNCTION__);
			return;
		}
	}
	else
	{
		printf("[-] didnt copy memory , conditions not verified\n");
		return;
	}

	UnmapViewOfFile(ViewBase);
	printf("[+] ExitSystemThread : %d\n", GetLastError());
	return;
}

uintptr_t GetBaseAddr(const std::string& ProcName)
{
	 process_id = get_process_id(ProcName);
	if (!process_id)
	{
		printf("[-] failed to get process id : %X\n", GetLastError());
		return NULL;
	}

	auto pussy = InitMsg(ToDriver,0);
	pussy->ProcId = process_id;
	pussy->opType = OPERATION_TYPE::OP_BASE;

	if (read_view)
	{
		UnmapViewOfFile(read_view);
	}

	read_view = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);

	if (read_view == NULL)
	{
		printf("[-] failed to get ViewBase 4 : %X\n", GetLastError());
		return NULL;
	}

	size_t len = sizeof(UM_Msg);

	if (read_view != NULL && pussy != NULL && len <= DestSize)
	{
		RtlCopyMemory(read_view, pussy, len);
		MemoryBarrier();
		auto isSet = SetEvent(hClientEvent);
		if (!isSet)
		{
			printf("[-] %s : SetEvent failed\n", __FUNCTION__);
			return NULL;
		}
	}
	else
	{
		printf("[-] didnt copy memory , conditions not verified\n");
		return NULL;
	}

	auto result = WaitForSingleObject(hDriverEvent, INFINITE);
	if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
	{
		return static_cast<uintptr_t>(read_view->address); //***
	}
	else
	{
		printf("[-] %s : WaitForSingleObject failed or magic: %X\n", __FUNCTION__, GetLastError());
		return NULL;
	}
}

uintptr_t AllocateMemory(int PID, unsigned char* shellcode, size_t shellcodeSize) {
	if (!shellcode || shellcodeSize == 0 || PID == 0)
		return NULL;

	// Clamp data size
	if (shellcodeSize > sizeof(UM_Msg::data))
		shellcodeSize = sizeof(UM_Msg::data);

	auto msg = InitMsg(ToDriver, shellcodeSize);
	msg->ProcId = PID;
	msg->opType = OPERATION_TYPE::OP_ALLOCATE_MEM;
	msg->dataSize = shellcodeSize;
	memcpy(msg->data, shellcode, shellcodeSize);

	// Send to shared memory
	if (!write_view)
	{
		write_view = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);
		if (!write_view)
		{
			printf("[-] failed to map write_view: %X\n", GetLastError());
			return NULL;
		}
	}

	if (msg && write_view && sizeof(UM_Msg) <= DestSize)
	{
		RtlCopyMemory(write_view, msg, sizeof(UM_Msg));
		MemoryBarrier();

		if (!SetEvent(hClientEvent))
		{
			printf("[-] SetEvent failed: %X\n", GetLastError());
			return NULL;
		}
	}
	else
	{
		printf("[-] write_view or msg null or size mismatch\n");
		return NULL;
	}

	// Receive response
	if (!read_view)
	{
		read_view = (UM_Msg*)MapViewOfFile(h_read, FILE_MAP_READ, 0, 0, DestSize);
		if (!read_view)
		{
			printf("[-] failed to map read_view: %X\n", GetLastError());
			return NULL;
		}
	}

	auto result = WaitForSingleObject(hDriverEvent, INFINITE);
	if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
	{
		return static_cast<uintptr_t>(read_view->address); 
	}
	else
	{
		printf("[-] Wait failed or invalid magic: %X\n", GetLastError());
		return NULL;
	}
}

uintptr_t GetModuleAddress(const std::string& ProcName)
{
	process_id = get_process_id(ProcName);
	if (!process_id)
	{
		printf("[-] failed to get process id : %X\n", GetLastError());
		return NULL;
	}

	auto pussy = InitMsg(ToDriver, 0);
	pussy->ProcId = process_id;
	pussy->opType = OPERATION_TYPE::OP_MODULE_BASE;

	if (read_view)
	{
		UnmapViewOfFile(read_view);
	}

	read_view = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);

	if (read_view == NULL)
	{
		printf("[-] failed to get ViewBase 5 : %X\n", GetLastError());
		return NULL;
	}

	size_t len = sizeof(UM_Msg);

	if (read_view != NULL && pussy != NULL && len <= DestSize)
	{
		RtlCopyMemory(read_view, pussy, len);
		MemoryBarrier();
		auto isSet = SetEvent(hClientEvent);
		if (!isSet)
		{
			printf("[-] %s : SetEvent failed\n", __FUNCTION__);
			return NULL;
		}
	}
	else
	{
		printf("[-] didnt copy memory , conditions not verified\n");
		return NULL;
	}

	auto result = WaitForSingleObject(hDriverEvent, INFINITE);
	if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
	{
		return static_cast<uintptr_t>(read_view->address); //***
	}
	else
	{
		printf("[-] %s : WaitForSingleObject failed or magic: %X\n", __FUNCTION__, GetLastError());
		return NULL;
	}

}
// Get LoadLibraryA address — it's at the same VA in every process (kernel32 ASLR is system-wide)
uintptr_t GetLoadLibraryAddr() {
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	if (!k32) return 0;
	return (uintptr_t)GetProcAddress(k32, "LoadLibraryA");
}



uintptr_t InjectAPC(int PID, PVOID codeAddr, PVOID normalContext = NULL) {

	auto msg = InitMsg(ToDriver, sizeof(PVOID));  // we send 8 bytes: the context pointer
	msg->ProcId = PID;
	msg->opType = OPERATION_TYPE::OP_INJECT_APC;
	msg->address = reinterpret_cast<ULONGLONG>(codeAddr);

	// Pack NormalContext into msg->data[0..7]
	// Driver reads this back as the parameter passed to shellcode (RCX)
	memcpy(msg->data, &normalContext, sizeof(PVOID));

	// Send to shared memory
	if (!write_view)
	{
		write_view = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);
		if (!write_view)
		{
			printf("[-] APC: failed to map write_view: %X\n", GetLastError());
			return NULL;
		}
	}

	if (msg && write_view)
	{
		RtlCopyMemory(write_view, msg, sizeof(UM_Msg));
		MemoryBarrier();

		if (!SetEvent(hClientEvent))
		{
			printf("[-] APC: SetEvent failed: %X\n", GetLastError());
			return NULL;
		}
	}
	else
	{
		printf("[-] APC: write_view or msg null\n");
		return NULL;
	}

	// Receive response
	if (!read_view)
	{
		read_view = (UM_Msg*)MapViewOfFile(h_read, FILE_MAP_READ, 0, 0, DestSize);
		if (!read_view)
		{
			printf("[-] APC: failed to map read_view: %X\n", GetLastError());
			return NULL;
		}
	}

	auto result = WaitForSingleObject(hDriverEvent, INFINITE);
	if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
	{
		uintptr_t ret = static_cast<uintptr_t>(read_view->address);
		if (ret == 1)
			printf("[+] APC: Queued shellcode=%p ctx=%p in PID %d\n", codeAddr, normalContext, PID);
		else
			printf("[-] APC: Driver returned failure\n");
		return ret;
	}
	else
	{
		printf("[-] APC: Wait failed or invalid magic: %X\n", GetLastError());
		return NULL;
	}
}


bool InjectDLL(int PID, const char* dllPath) {
	if (!PID || !dllPath || !dllPath[0]) {
		printf("[-] InjectDLL: bad params\n");
		return false;
	}

	size_t pathLen = strlen(dllPath) + 1;
	if (pathLen > 260) {
		printf("[-] InjectDLL: path too long\n");
		return false;
	}

	// 1. Get LoadLibraryA address (same in all processes due to system-wide ASLR)
	uintptr_t loadLibAddr = GetLoadLibraryAddr();
	if (!loadLibAddr) {
		printf("[-] InjectDLL: can't find LoadLibraryA\n");
		return false;
	}
	printf("[+] LoadLibraryA = 0x%llX\n", loadLibAddr);

	// 2. Allocate DLL path string in target process
	uintptr_t pathAddr = AllocateMemory(PID, (unsigned char*)dllPath, pathLen);
	if (!pathAddr) {
		printf("[-] InjectDLL: failed to allocate path in target\n");
		return false;
	}
	printf("[+] DLL path allocated at 0x%llX\n", pathAddr);

	// 3. Build shellcode
	//    APC NormalRoutine signature: void NTAPI func(PVOID NormalContext, PVOID, PVOID)
	//    RCX = NormalContext = pointer to DLL path string
	//
	//    shellcode:
	//      sub rsp, 0x28          ; shadow space + alignment
	//      mov rax, <LoadLibraryA>; absolute address
	//      call rax               ; LoadLibraryA(RCX) — RCX already = path from APC
	//      add rsp, 0x28
	//      ret                    ; return to alertable wait

	unsigned char shellcode[] = {
		0x48, 0x83, 0xEC, 0x28,                         // sub rsp, 0x28
		0x48, 0xB8,                                      // mov rax, imm64
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // <LoadLibraryA address> [offset 6]
		0xFF, 0xD0,                                      // call rax
		0x48, 0x83, 0xC4, 0x28,                         // add rsp, 0x28
		0xC3                                             // ret
	};

	// Patch LoadLibraryA address into shellcode at offset 6
	memcpy(&shellcode[6], &loadLibAddr, sizeof(uintptr_t));

	// 4. Allocate shellcode in target
	uintptr_t shellAddr = AllocateMemory(PID, shellcode, sizeof(shellcode));
	if (!shellAddr) {
		printf("[-] InjectDLL: failed to allocate shellcode in target\n");
		return false;
	}
	printf("[+] Shellcode allocated at 0x%llX\n", shellAddr);

	// 5. Fire APC — NormalContext = pathAddr (lands in RCX = LoadLibraryA's first arg)
	uintptr_t result = InjectAPC(PID, (PVOID)shellAddr, (PVOID)pathAddr);
	if (result == 1) {
		printf("[+] InjectDLL: APC queued! DLL=%s\n", dllPath);
		return true;
	}

	printf("[-] InjectDLL: APC failed\n");
	return false;
}

struct BatchEntry { uintptr_t addr; uint32_t size; };

// Batch read — sends up to 200 addresses, gets all results in ONE round trip
// Returns pointer to packed result data (inside read_view->data), or NULL
// Results are packed sequentially: result[0] at byte 0, result[1] at byte entries[0].size, etc.
void* read_batch(BatchEntry* entries, int count, int procId)
{
	if (count <= 0 || count > 200) return NULL;

	auto msg = InitMsg(ToDriver, 0);
	msg->opType = OPERATION_TYPE::OP_READ_BATCH;
	msg->ProcId = procId;

	// Pack request: [count:4][{addr:8, size:4} × count]
	*(uint32_t*)msg->data = (uint32_t)count;
	struct PackedReq { uint64_t addr; uint32_t size; };
	PackedReq* packed = (PackedReq*)(msg->data + 4);
	for (int i = 0; i < count; i++)
	{
		packed[i].addr = entries[i].addr;
		packed[i].size = entries[i].size;
	}

	// Send via shared memory
	if (!write_view)
	{
		write_view = (UM_Msg*)MapViewOfFile(h_write, FILE_MAP_WRITE, 0, 0, DestSize);
		if (!write_view) return NULL;
	}

	RtlCopyMemory(write_view, msg, sizeof(UM_Msg));
	MemoryBarrier();
	if (!SetEvent(hClientEvent)) return NULL;

	if (!read_view)
	{
		read_view = (UM_Msg*)MapViewOfFile(h_read, FILE_MAP_READ, 0, 0, DestSize);
		if (!read_view) return NULL;
	}

	auto result = WaitForSingleObject(hDriverEvent, INFINITE);
	if (result == WAIT_OBJECT_0 && read_view->magic == MAGIC)
	{
		return read_view->data;
	}
	return NULL;
}

template <typename T>
T read(const uintptr_t addr, int procId)
{
	T* value{};
	value = reinterpret_cast<T*>(read_address(reinterpret_cast<PVOID>(addr), sizeof(T), procId));
	if (!value)
	{
		printf("Failed to read memory.\n");
		clean();
		exit(1);
	}
	return *value;
}

template <typename T>
void allocate(unsigned char bytes[], int PID)
{
	AllocateMemory(PID, bytes, sizeof(bytes)); 
}

template <typename T>
void write(uintptr_t addr, const T& value, int procId)
{
	write_address(reinterpret_cast<PVOID>(addr), &value, sizeof(T), procId);
}


template <typename T>
void write_virtual(uintptr_t addr, const T& value, int procId)
{
	write_address_virtual(reinterpret_cast<PVOID>(addr), &value, sizeof(T), procId);
}
// reduce remapping


