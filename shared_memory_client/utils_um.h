#pragma once
#include <string> 
#include <TlHelp32.h>
#include <stdint.h>
#include <cstdio> 

DWORD processId = 0;

uint32_t get_process_id(const std::string& image_name)
{
    // Convert std::string -> std::wstring
    std::wstring imageNameW(image_name.begin(), image_name.end());

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W process;
    process.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &process)) {
        do {
            // Case-insensitive wide-string compare
            if (_wcsicmp(imageNameW.c_str(), process.szExeFile) == 0) {
                processId = process.th32ProcessID;
                CloseHandle(snapshot);
                return processId;
            }
        } while (Process32NextW(snapshot, &process));
    }

    CloseHandle(snapshot);
    return 0;
}

