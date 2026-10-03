#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>

namespace aion {
inline std::vector<PROCESSENTRY32W> startupProcesses() {
    std::vector<PROCESSENTRY32W> result;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return result;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(snapshot,&entry))do{result.push_back(entry);}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}
inline DWORD otherPipeProcess() {
    for(const auto& entry:startupProcesses())
        if(entry.th32ProcessID!=GetCurrentProcessId() && !_wcsicmp(entry.szExeFile,L"Aion2Pipe.exe"))return entry.th32ProcessID;
    return 0;
}
// A parent PID can be reused. Its creation time must precede the child's.
inline bool bridgeParentAlive(const PROCESSENTRY32W& child,HANDLE childHandle) {
    HANDLE parent=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,child.th32ParentProcessID);
    if(!parent)return GetLastError()!=ERROR_INVALID_PARAMETER; // Access denied is not proof of an orphan.
    FILETIME pc{},pe{},pk{},pu{},cc{},ce{},ck{},cu{};
    bool alive=WaitForSingleObject(parent,0)==WAIT_TIMEOUT;
    if(alive && GetProcessTimes(parent,&pc,&pe,&pk,&pu) && GetProcessTimes(childHandle,&cc,&ce,&ck,&cu))
        alive=CompareFileTime(&pc,&cc)<=0;
    CloseHandle(parent);return alive;
}
inline DWORD cleanOrphanChatBridges() {
    DWORD blocked=0;
    for(const auto& entry:startupProcesses()) {
        if(_wcsicmp(entry.szExeFile,L"Aion2ChatBridge.exe"))continue;
        HANDLE child=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE|SYNCHRONIZE,FALSE,entry.th32ProcessID);
        if(!child){blocked=entry.th32ProcessID;continue;}
        wchar_t path[32768]{};DWORD length=DWORD(std::size(path));
        if(!QueryFullProcessImageNameW(child,0,path,&length)) {blocked=entry.th32ProcessID;CloseHandle(child);continue;}
        const auto filename=wcsrchr(path,L'\\');
        if(_wcsicmp(filename?filename+1:path,L"Aion2ChatBridge.exe")){CloseHandle(child);continue;}
        DWORD session=0,ours=0;
        if(!ProcessIdToSessionId(entry.th32ProcessID,&session) || !ProcessIdToSessionId(GetCurrentProcessId(),&ours) || session!=ours) {
            blocked=entry.th32ProcessID;CloseHandle(child);continue;
        }
        if(bridgeParentAlive(entry,child))blocked=entry.th32ProcessID;
        else if(!TerminateProcess(child,0) || WaitForSingleObject(child,5000)!=WAIT_OBJECT_0)blocked=entry.th32ProcessID;
        CloseHandle(child);
    }
    return blocked;
}
}
