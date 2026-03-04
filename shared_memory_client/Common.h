#pragma once
#ifdef _KERNEL_MODE
#include <ntddk.h>    // or #include <ntdef.h>
#else
#include <Windows.h>
#include <cstdint>
#endif
#define MAGIC 1337
typedef enum _OPERATION_TYPE
{
    OP_BASE = 0,
    OP_READ = 1,
    OP_WRITE = 2,
    OP_EXIT = 3,
    OP_MODULE_BASE = 4,
    OP_ALLOCATE_MEM = 5,
    OP_INJECT_APC = 6,
    OP_WRITE_VIRTUAL = 7,
    OP_READ_BATCH = 8          // NEW — scatter-gather batch read
} OPERATION_TYPE;
#pragma pack(push, 8)
struct UM_Msg
{
    ULONG ProcId;
    ULONG pad1;//
    ULONGLONG address;
    OPERATION_TYPE opType; // 4 bytes
    ULONG pad2;
    ULONGLONG dataSize;
    UINT32 magic = MAGIC;
    BYTE data[3840];           // WAS 256 — expanded for batch reads (still fits in 4096-byte shared section)
};
#pragma pack(pop)
typedef struct _UM_WriteMsg {
    int  ProcId;
    int  Operation;        // OP_WRITE or OP_WRITE_VIRTUAL
    ULONGLONG Address;
    ULONG     Size;        // bytes to write (max 48)
    UCHAR     Data[48];    // payload
} UM_WriteMsg;
// ── Batch read protocol ──
// Request layout in data[]:
//   [0..3]                    = count (uint32_t, max 200)
//   [4..4+count*12-1]         = BatchReq[count]
//
// Response layout in data[]:
//   [0..totalResultBytes-1]   = packed results in request order
//   Failed reads are zero-filled
//
// BatchReq: { uint64_t addr; uint32_t size; } = 12 bytes each
// Max single result: 48 bytes (Matrix3x4)