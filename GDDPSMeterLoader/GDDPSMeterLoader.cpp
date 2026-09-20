#include <windows.h>
#include <stdio.h>
#include "proc.h"

//=============================================================================
//=============================================================================
#define GRIMDAWN L"Grim Dawn.exe"
#define DPSMETERDLL L"DPSMeter.dll"

//=============================================================================
//=============================================================================
void PopMessage(bool isError, const char *msg)
{
    if (isError)
    {
        MessageBoxA(NULL, msg, "Error", MB_OK | MB_ICONERROR);
    }
    else
    {
        MessageBoxA(NULL, msg, "Success", MB_OK | MB_ICONINFORMATION);
    }
}

BOOL FileExists(LPCSTR szPath)
{
	DWORD dwAttrib = GetFileAttributes(szPath);
	return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

//=============================================================================
//=============================================================================
int main()
{
    DWORD dwProcessId = 0;
    bool found = false;
    bool dllLoaded = false;
    bool hideConsole = false;  // show console for debugging

    // Write log file next to exe
    FILE *flog = NULL;
    fopen_s(&flog, "GDDPSMeterLoader.log", "w");
    if (flog) fprintf(flog, "GDDPSMeterLoader started\n");

    if (hideConsole)
    {
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }

    dwProcessId = GetProcId(GRIMDAWN);

    if (flog) fprintf(flog, "GetProcId(Grim Dawn.exe) = %lu\n", dwProcessId);

    if (dwProcessId != 0)
    {
        HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);

        if (flog) fprintf(flog, "OpenProcess = %p\n", hProcess);

        if (hProcess)
        {
            //no reload
            if (GetBaseModuleHandle(dwProcessId, DPSMETERDLL))
            {
                PopMessage(true, "GD DPSMeter is already loaded.");
                if (flog) fprintf(flog, "Already loaded, exit\n");
                fclose(flog);
                return 0;
            }

            const int maxPathLen = 512;
            char path[maxPathLen];
            GetCurrentDirectory(maxPathLen, path);

            if (flog) fprintf(flog, "CurrentDir = %s\n", path);

#ifndef _RELEASE
            std::string dllPath = std::string(path) + "\\DPSMeter.dll";
            //std::string dllPath = std::string(path) + "\\" + CMAKE_INTDIR + "\\DPSMeter.dll";
#else
            std::string dllPath = std::string(path) + "\\DPSMeter.dll";
#endif

            LPCSTR DllPath = dllPath.c_str();

            if (flog) fprintf(flog, "DLL path = %s\n", DllPath);

			//check for dll
			if (!FileExists(DllPath))
			{
				PopMessage(true, "DPSMeter.dll missing.");
                if (flog) fprintf(flog, "DLL NOT FOUND\n");
                fclose(flog);
				return 0;
			}

            if (flog) fprintf(flog, "DLL found, injecting...\n");

            LPVOID pDllPath = VirtualAllocEx(hProcess, 0, strlen(DllPath) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

            if (flog) fprintf(flog, "VirtualAllocEx = %p\n", pDllPath);

            if (pDllPath != NULL)
            {
                BOOL success = WriteProcessMemory(hProcess, pDllPath, (LPVOID)DllPath, strlen(DllPath) + 1, 0);
                while (!success)
                {
                    Sleep(100);
                    success = WriteProcessMemory(hProcess, pDllPath, (LPVOID)DllPath, strlen(DllPath) + 1, 0);
                }

                HMODULE hKern32 = GetModuleHandleA("Kernel32.dll");

                if (hKern32 != NULL && hKern32 != INVALID_HANDLE_VALUE)
                {
                    LPTHREAD_START_ROUTINE pthread = (LPTHREAD_START_ROUTINE)GetProcAddress(hKern32, "LoadLibraryA");
                    HANDLE hLoadThread = CreateRemoteThread(hProcess, 0, 0, pthread, pDllPath, 0, 0);

                    if (hLoadThread)
                    {
                        // wait for the execution to finish
                        WaitForSingleObject(hLoadThread, INFINITE);

                        // close LoadLibrary thread (dll is already loaded into host process)
                        CloseHandle(hLoadThread);
                        dllLoaded = true;
                        if (flog) fprintf(flog, "DLL injected OK!\n");
                    }
                    else
                    {
                        PopMessage(true, "Remote thread failed.");
                        if (flog) fprintf(flog, "RemoteThread FAILED\n");
                    }
                }
                else
                {
                    PopMessage(true, "Failed to load Kernel32.dll.");
                    if (flog) fprintf(flog, "Kernel32 not found\n");
                }

                VirtualFreeEx(hProcess, pDllPath, strlen(DllPath) + 1, MEM_RELEASE);
            }
            else
            {
                PopMessage(true, "Failed to inject dll.");
                if (flog) fprintf(flog, "VirtualAllocEx FAILED\n");
            }
            CloseHandle(hProcess);
        }
        else
        {
            PopMessage(true, "OpenProcess failed.");
            if (flog) fprintf(flog, "OpenProcess FAILED (error %lu)\n", GetLastError());
        }
    }
    else
    {
        PopMessage(true, "Failed to find \"Grim Dawn.exe\" process.");
        if (flog) fprintf(flog, "Game process NOT FOUND\n");
    }

    if (flog) fclose(flog);
    return 0;
}

