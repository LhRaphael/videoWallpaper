/*
 * config.c — ver config.h.
 */
#include "config.h"
#include <shlobj.h>
#include <wchar.h>
#include <stdio.h>

static const wchar_t SECTION[]   = L"VideoWallpaper";
static const wchar_t RUN_KEY[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t RUN_VALUE[] = L"VideoWallpaper";

static BOOL GetConfigFile(wchar_t *out, DWORD cch)
{
    wchar_t appData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appData)))
        return FALSE;
    if (_snwprintf(out, cch, L"%s\\VideoWallpaper", appData) < 0) return FALSE;
    out[cch - 1] = 0;
    CreateDirectoryW(out, NULL);
    size_t len = wcslen(out);
    if (_snwprintf(out + len, cch - len, L"\\config.ini") < 0) return FALSE;
    out[cch - 1] = 0;
    return TRUE;
}

/* As funções *PrivateProfile* só gravam Unicode se o arquivo já existir
 * em UTF-16 LE com BOM; sem isso, caminhos com acentos se perdem. */
static void EnsureUnicodeIni(const wchar_t *file)
{
    if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    static const BYTE bom[2] = { 0xFF, 0xFE };
    DWORD written;
    WriteFile(h, bom, sizeof(bom), &written, NULL);
    CloseHandle(h);
}

static int ReadInt(const wchar_t *file, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(SECTION, key, def, file);
}

static void WriteInt(const wchar_t *file, const wchar_t *key, int value)
{
    wchar_t buf[16];
    _snwprintf(buf, 16, L"%d", value);
    buf[15] = 0;
    WritePrivateProfileStringW(SECTION, key, buf, file);
}

void Config_Load(Config *c)
{
    ZeroMemory(c, sizeof(*c));
    c->muted           = TRUE;
    c->volume          = 50;
    c->scaleMode       = 0;
    c->resumeOnStart   = TRUE;
    c->pauseFullscreen = TRUE;
    c->pauseMaximized  = FALSE;
    c->closeToTray     = TRUE;

    wchar_t file[MAX_PATH + 64];
    if (!GetConfigFile(file, (DWORD)(sizeof(file) / sizeof(file[0])))) return;

    GetPrivateProfileStringW(SECTION, L"VideoPath", L"", c->videoPath, CONFIG_PATH_MAX, file);
    c->muted           = ReadInt(file, L"Muted", c->muted) != 0;
    c->volume          = ReadInt(file, L"Volume", c->volume);
    c->scaleMode       = ReadInt(file, L"ScaleMode", c->scaleMode);
    c->resumeOnStart   = ReadInt(file, L"ResumeOnStart", c->resumeOnStart) != 0;
    c->wasPlaying      = ReadInt(file, L"WasPlaying", 0) != 0;
    c->pauseFullscreen = ReadInt(file, L"PauseFullscreen", c->pauseFullscreen) != 0;
    c->pauseMaximized  = ReadInt(file, L"PauseMaximized", c->pauseMaximized) != 0;
    c->closeToTray     = ReadInt(file, L"CloseToTray", c->closeToTray) != 0;
    c->trayHintShown   = ReadInt(file, L"TrayHintShown", 0) != 0;

    if (c->volume < 0) c->volume = 0;
    if (c->volume > 100) c->volume = 100;
    if (c->scaleMode < 0 || c->scaleMode > 2) c->scaleMode = 0;
}

void Config_Save(const Config *c)
{
    wchar_t file[MAX_PATH + 64];
    if (!GetConfigFile(file, (DWORD)(sizeof(file) / sizeof(file[0])))) return;
    EnsureUnicodeIni(file);

    WritePrivateProfileStringW(SECTION, L"VideoPath", c->videoPath, file);
    WriteInt(file, L"Muted", c->muted);
    WriteInt(file, L"Volume", c->volume);
    WriteInt(file, L"ScaleMode", c->scaleMode);
    WriteInt(file, L"ResumeOnStart", c->resumeOnStart);
    WriteInt(file, L"WasPlaying", c->wasPlaying);
    WriteInt(file, L"PauseFullscreen", c->pauseFullscreen);
    WriteInt(file, L"PauseMaximized", c->pauseMaximized);
    WriteInt(file, L"CloseToTray", c->closeToTray);
    WriteInt(file, L"TrayHintShown", c->trayHintShown);
}

BOOL Autostart_IsEnabled(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegQueryValueExW(key, RUN_VALUE, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

BOOL Autostart_Set(BOOL enable)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;

    LONG r;
    if (enable) {
        wchar_t exe[MAX_PATH * 2];
        wchar_t cmd[MAX_PATH * 2 + 16];
        DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        if (n == 0 || n >= sizeof(exe) / sizeof(exe[0])) { RegCloseKey(key); return FALSE; }
        _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%s\" /tray", exe);
        cmd[sizeof(cmd) / sizeof(cmd[0]) - 1] = 0;
        r = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, RUN_VALUE);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}
