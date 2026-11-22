#include <windows.h>
#include <tlhelp32.h>
#include <string>
int wmain(int argc,wchar_t** argv){
    DWORD pid=0; if(argc>1) pid=wcstoul(argv[1],nullptr,10);
    if(!pid){HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W e{sizeof(e)};if(Process32FirstW(s,&e)){do{if(!_wcsicmp(e.szExeFile,L"Test_C-Win64-Shipping.exe")){pid=e.th32ProcessID;break;}}while(Process32NextW(s,&e));}CloseHandle(s);}
    if(!pid)return 2; std::wstring dll=std::wstring(argv[0]);auto p=dll.find_last_of(L"\\/");dll=dll.substr(0,p+1)+L"PQOverlay.dll";
    HANDLE h=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);if(!h)return 3;
    SIZE_T n=(dll.size()+1)*sizeof(wchar_t);void* mem=VirtualAllocEx(h,nullptr,n,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!mem){CloseHandle(h);return 4;}
    SIZE_T written{};if(!WriteProcessMemory(h,mem,dll.c_str(),n,&written)||written!=n){VirtualFreeEx(h,mem,0,MEM_RELEASE);CloseHandle(h);return 5;}
    HANDLE t=CreateRemoteThread(h,nullptr,0,(LPTHREAD_START_ROUTINE)LoadLibraryW,mem,0,nullptr);if(!t){VirtualFreeEx(h,mem,0,MEM_RELEASE);CloseHandle(h);return 6;}
    DWORD wait=WaitForSingleObject(t,10000);DWORD module{};BOOL gotResult=(wait==WAIT_OBJECT_0)&&GetExitCodeThread(t,&module);
    VirtualFreeEx(h,mem,0,MEM_RELEASE);CloseHandle(t);CloseHandle(h);
    if(!gotResult)return 7; return module?0:8;
}
