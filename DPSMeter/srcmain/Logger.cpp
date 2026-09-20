#include <windows.h>
#include <string>
#include <sstream>
#include <debugapi.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <io.h>
#include <fcntl.h>

#include "Logger.h"


static unsigned sLevel = 0;

// File logger - always active even in Release builds
static FILE* sLogFile = NULL;
static bool sLogFileOpened = false;

static void OpenLogFile()
{
    if (sLogFileOpened) return;
    sLogFileOpened = true;

    // Write log next to the DLL, not the exe
    char path[MAX_PATH];
    HMODULE hMod = NULL;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&OpenLogFile, &hMod);
    if (hMod)
        GetModuleFileNameA(hMod, path, MAX_PATH);
    else
        GetModuleFileNameA(NULL, path, MAX_PATH);

    char *lastSlash = strrchr(path, '\\');
    if (lastSlash)
    {
        strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - path), "DPSMeter.log");
    }

    // Open with shared access so we can read while game is running
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        int fd = _open_osfhandle((intptr_t)hFile, _O_TEXT);
        if (fd != -1)
        {
            sLogFile = _fdopen(fd, "w");
        }
    }

    if (sLogFile)
    {
        // Write header
        time_t now = time(NULL);
        struct tm tmInfo;
        localtime_s(&tmInfo, &now);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmInfo);
        fprintf(sLogFile, "=== DPSMeter Log Started at %s ===\n", timeBuf);
        fprintf(sLogFile, "=== _RELEASE=%d ===\n",
#ifdef _RELEASE
            1
#else
            0
#endif
        );
        fflush(sLogFile);
    }
}

static void WriteLogFile(const char* msg)
{
    if (!sLogFile)
    {
        OpenLogFile();
    }
    if (sLogFile)
    {
        fputs(msg, sLogFile);
        fflush(sLogFile);
    }
}

void Logger::SetLogLevel(unsigned level)
{
    sLevel = level;
}

void Logger::LevelLog(unsigned level, const char *format, ...)
{
    char buf[2048];

    if (level & sLevel)
    {
        va_list args;
        va_start(args, format);
        vsprintf_s(buf, format, args);
        va_end(args);

        OutputDebugString(buf);
        WriteLogFile(buf);
    }
}

void Logger::Logf(const char *format, ...)
{
    char buf[1024];

    va_list args;
    va_start(args, format);
    vsprintf_s(buf, format, args);
    va_end(args);

    OutputDebugString(buf);
    WriteLogFile(buf);
}

std::string number_fmt(unsigned long long n, char sep = ',') 
{
  std::stringstream fmt;
  fmt << n;
  std::string s = fmt.str();
  s.reserve(s.length() + s.length() / 3);

  // loop until the end of the string and use j to keep track of every
  // third loop starting taking into account the leading x digits (this probably
  // can be rewritten in terms of just i, but it seems more clear when you use
  // a seperate variable)
  for (unsigned int i = 0, j = 3 - s.length() % 3; i < s.length(); ++i, ++j)
    if (i != 0 && j % 3 == 0)
      s.insert(i++, 1, sep);

  return s;
}
