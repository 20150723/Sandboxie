/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
 * Copyright 2020-2021 David Xanatos, xanasoft.com
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
// Customize and Prepare the Sandbox to Run Programs
//---------------------------------------------------------------------------


#include "dll.h"
#include "common/my_version.h"
#include <stdio.h>


//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


BOOLEAN         CustomizeSandbox(void);

static UCHAR    GetSetCustomLevel(UCHAR SetLevel);

static BOOLEAN  Custom_CreateRegLinks(void);
static BOOLEAN  DisableDCOM(void);
static BOOLEAN  DisableRecycleBin(void);
static BOOLEAN  DisableWinRS(void);
static BOOLEAN  DisableWerFaultUI(void);
static BOOLEAN  EnableMsiDebugging(void);
static BOOLEAN  DisableEdgeBoost(void);
static BOOLEAN  Custom_EnableBrowseNewProcess(void);
static BOOLEAN  Custom_DisableBHOs(void);
static BOOLEAN  Custom_OpenWith(void);
static HANDLE   OpenExplorerKey(
                    HANDLE ParentKey, const WCHAR *SubkeyName, ULONG *error);
static void     DeleteShellAssocKeys(ULONG Wow64);
static void     AutoExec(void);


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


static HANDLE AutoExecHKey = NULL;

static const WCHAR *Custom_PrefixHKLM = L"\\Registry\\Machine";
static const WCHAR *Custom_PrefixHKCU = L"\\Registry\\User\\Current";


//---------------------------------------------------------------------------
// CustomizeSandbox
//---------------------------------------------------------------------------


_FX BOOLEAN CustomizeSandbox(void)
{
    //
    // customize sandbox if we need to
    //

    if ((Dll_ProcessFlags & SBIE_FLAG_PRIVACY_MODE) != 0) {

        Key_CreateBaseKeys();
        //Key_CreateBaseFolders(); // no longer needed those paths will be created on demand
    }

    if (GetSetCustomLevel(0) != '2') {

        Custom_CreateRegLinks();
        DisableDCOM();
        DisableRecycleBin();
        if (SbieApi_QueryConfBool(NULL, L"BlockWinRM", TRUE))
            DisableWinRS();
        DisableWerFaultUI();
        EnableMsiDebugging();
        DisableEdgeBoost();
        Custom_EnableBrowseNewProcess();
        DeleteShellAssocKeys(0);
        Custom_DisableBHOs();
        if (Dll_OsBuild >= 8400) // only on win 8 and later
            Custom_OpenWith();

        GetSetCustomLevel('2');

        //
        // process user-defined AutoExec settings
        //

        if (AutoExecHKey)
            AutoExec();
    }

    //
    // finish
    //

    if (AutoExecHKey)
        NtClose(AutoExecHKey);

    return TRUE;
}


//---------------------------------------------------------------------------
// GetSetCustomLevel
//---------------------------------------------------------------------------


_FX UCHAR GetSetCustomLevel(UCHAR SetLevel)
{
    NTSTATUS status;
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    KEY_VALUE_PARTIAL_INFORMATION value_info;
    ULONG len;
    WCHAR path[256];

    ULONG Wow64 = Dll_IsWin64 ? KEY_WOW64_64KEY : 0;

    if (! SetLevel) {

        //
        // if UseRegDeleteV2 is set, check if RegPaths.dat was loaded
        // if not it means the box was previusly a V1 box,
        // hence return 0 and re run customization
        // 
        // note: DeleteShellAssocKeys deletes the sandboxie shell integration keys
        // so the existence of a RegPaths.dat in a customized box is a reliable indicator
        //

        extern BOOLEAN Key_Delete_v2;
        extern BOOLEAN Key_RegPaths_Loaded;
        if (Key_Delete_v2 && !Key_RegPaths_Loaded)
            return 0;

        //
        // open the AutoExec key, also used to indicate if this sandbox
        // has already been customized
        //

        wcscpy(path, L"\\registry\\user\\");
        wcscat(path, Dll_SidString);
        //wcscpy(path, Dll_BoxKeyPath);
        //wcscat(path, L"\\user\\current");
        wcscat(path, L"\\software\\SandboxAutoExec");

        RtlInitUnicodeString(&uni, path);

        InitializeObjectAttributes(
            &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

        status = Key_OpenOrCreateIfBoxed(
                        &AutoExecHKey, KEY_ALL_ACCESS | Wow64, &objattrs);
        if (status == STATUS_BAD_INITIAL_PC) {
            value_info.Data[0] = 0;
            status = STATUS_SUCCESS;

        } else if (NT_SUCCESS(status)) {

            RtlInitUnicodeString(&uni, L"");

            status = NtQueryValueKey(
                AutoExecHKey, &uni, KeyValuePartialInformation,
                &value_info, sizeof(value_info), &len);

            if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
                value_info.Data[0] = 0;
                status = STATUS_SUCCESS;
            }
        }

        if (! NT_SUCCESS(status)) {
            value_info.Data[0] = 0;
            Sbie_snwprintf(path, 256, L"%d [%08X]", -2, status);
            SbieApi_Log(2206, path);
        }

    } else if (AutoExecHKey) {

        //
        // set flag
        //

        value_info.Data[0] = SetLevel;

        RtlInitUnicodeString(&uni, L"");

        status = NtSetValueKey(
            AutoExecHKey, &uni, 0, REG_BINARY, &value_info.Data, 1);

        if (! NT_SUCCESS(status)) {

            Sbie_snwprintf(path, 256, L"%d [%08X]", -3, status);
            SbieApi_Log(2206, path);
        }
    }

    return value_info.Data[0];
}


//---------------------------------------------------------------------------
// Custom_CreateRegLinks
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_CreateRegLinks(void)
{
    static const WCHAR *_user_current = L"\\user\\current";
    NTSTATUS status;
    WCHAR path[384];
    HKEY hkey1, hkey2;
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    WCHAR err[64];

    ULONG Wow64 = Dll_IsWin64 ? KEY_WOW64_64KEY : 0;

    InitializeObjectAttributes(
        &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

    //
    // create \Registry\User\Current\Software\Classes key as a link
    //

    wcscpy(path, Dll_BoxKeyPath);
    wcscat(path, _user_current);
    wcscat(path, L"\\software\\classes");

    RtlInitUnicodeString(&uni, path);

    status = NtCreateKey(
        &hkey1, KEY_ALL_ACCESS | Wow64, &objattrs,
        0, NULL, REG_OPTION_CREATE_LINK, NULL);

    if (status == STATUS_OBJECT_NAME_COLLISION) {
        // key already exists, possibly indicating OpenKeyPath=*
        return TRUE;
    }

    if (status == STATUS_ACCESS_DENIED) {
        ULONG mp_flags = SbieDll_MatchPath(L'k', path);
        if (PATH_IS_OPEN(mp_flags) || PATH_IS_CLOSED(mp_flags)) {
            // ReadKeyPath=* or ClosedKeyPath=*
            return TRUE;
        }
    }

    if (! NT_SUCCESS(status)) {
        Sbie_snwprintf(err, 64, L"[11 / %08X]", status);
        SbieApi_Log(2326, err);
        return FALSE;
    }

    //
    // create \Registry\User\Current_Classes key
    //

    wcscpy(path, Dll_BoxKeyPath);
    wcscat(path, _user_current);
    wcscat(path, L"_classes");

    RtlInitUnicodeString(&uni, path);

    status = NtCreateKey(
        &hkey2, KEY_ALL_ACCESS | Wow64, &objattrs, 0, NULL, 0, NULL);

    if (NT_SUCCESS(status)) {

        NtClose(hkey2);

    } else if (status != STATUS_OBJECT_NAME_COLLISION) {

		Sbie_snwprintf(err, 64, L"[22 / %08X]", status);
        SbieApi_Log(2326, err);
        NtClose(hkey1);
        return FALSE;
    }

    //
    // point the first key at the second key
    //

    RtlInitUnicodeString(&uni, L"SymbolicLinkValue");

    status = NtSetValueKey(
        hkey1, &uni, 0, REG_LINK, path, wcslen(path) * sizeof(WCHAR));

    NtClose(hkey1);

    if (! NT_SUCCESS(status)) {
		Sbie_snwprintf(err, 64, L"[33 / %08X]", status);
        SbieApi_Log(2326, err);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// DisableDCOM
//---------------------------------------------------------------------------


_FX BOOLEAN DisableDCOM(void)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING objname;
    HANDLE handle;
    WCHAR err[64];

    ULONG Wow64 = Dll_IsWin64 ? KEY_WOW64_64KEY : 0;

    InitializeObjectAttributes(
        &objattrs, &objname, OBJ_CASE_INSENSITIVE, NULL, NULL);

    //
    // disable DCOM
    //

    RtlInitUnicodeString(&objname,
        L"\\registry\\machine\\software\\microsoft\\ole");

    status = Key_OpenIfBoxed(&handle, KEY_ALL_ACCESS | Wow64, &objattrs);
    if (! NT_SUCCESS(status)) {

        if (status != STATUS_BAD_INITIAL_PC &&
            status != STATUS_OBJECT_NAME_NOT_FOUND) {

			Sbie_snwprintf(err, 64, L"[21 / %08X]", status);
            SbieApi_Log(2309, err);
        }

    } else {

        static WCHAR no[2] = { L'N', L'\0' };
        RtlInitUnicodeString(&objname, L"EnableDCOM");
        status = NtSetValueKey(handle, &objname, 0, REG_SZ, &no, sizeof(no));
        if (! NT_SUCCESS(status)) {
			Sbie_snwprintf(err, 64, L"[22 / %08X]", status);
            SbieApi_Log(2309, err);
        }

        NtClose(handle);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// DisableWinRS
//
// WinRM/WinRS allows a sandboxed app to run any command in the host.  Disabling AllowNegotiate is one of the ways we
// block WinRS from running in the sandbox.
//---------------------------------------------------------------------------


_FX BOOLEAN DisableWinRS(void)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING uni;
    HANDLE hKeyRoot;
    HANDLE hKeyWinRM;
    HANDLE hKeyWinRMClient;

    // Open HKLM
    RtlInitUnicodeString(&uni, Custom_PrefixHKLM);
    InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&hKeyRoot, KEY_READ, &objattrs) == STATUS_SUCCESS)
    {
        // open/create WinRM parent key
        RtlInitUnicodeString(&uni, L"software\\policies\\microsoft\\windows\\WinRM");
        InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyRoot, NULL);
        if (Key_OpenOrCreateIfBoxed(&hKeyWinRM, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
        {
            // open/create WinRM Client key
            RtlInitUnicodeString(&uni, L"Client");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyWinRM, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKeyWinRMClient, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // set AllowNegotiate = 0
                ULONG zero = 0;
                RtlInitUnicodeString(&uni, L"AllowNegotiate");
                status = NtSetValueKey(hKeyWinRMClient, &uni, 0, REG_DWORD, &zero, sizeof(zero));
                NtClose(hKeyWinRMClient);
            }
            NtClose(hKeyWinRM);
        }
        NtClose(hKeyRoot);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// EnableMsiDebugging
//
// Enable Msi Server debug output
//---------------------------------------------------------------------------


_FX BOOLEAN EnableMsiDebugging(void)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING uni;
    HANDLE hKeyRoot;
    HANDLE hKeyMSI;
    HANDLE hKeyWin;

    // Open HKLM
    RtlInitUnicodeString(&uni, Custom_PrefixHKLM);
    InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&hKeyRoot, KEY_READ, &objattrs) == STATUS_SUCCESS)
    {
        // open/create WER parent key
        RtlInitUnicodeString(&uni, L"SOFTWARE\\Policies\\Microsoft\\Windows");
        InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyRoot, NULL);
        if (Key_OpenOrCreateIfBoxed(&hKeyWin, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
        {
            // open/create WER key
            RtlInitUnicodeString(&uni, L"Installer");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyWin, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKeyMSI, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // set Debug = 7
                DWORD seven = 7;
                RtlInitUnicodeString(&uni, L"Debug");
                status = NtSetValueKey(hKeyMSI, &uni, 0, REG_DWORD, &seven, sizeof(seven));

                // set Logging = "voicewarmupx"
                static const WCHAR str[] = L"voicewarmupx";
                RtlInitUnicodeString(&uni, L"Logging");
                status = NtSetValueKey(hKeyMSI, &uni, 0, REG_SZ, (BYTE *)&str, sizeof(str));

                NtClose(hKeyMSI);
            }
            NtClose(hKeyWin);
        }
        NtClose(hKeyRoot);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// DisableEdgeBoost
//
// Disable esge startup boost
//---------------------------------------------------------------------------


_FX BOOLEAN DisableEdgeBoost(void)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING uni;
    HANDLE hKeyRoot;
    HANDLE hKeyEdge;

    // Open HKLM
    RtlInitUnicodeString(&uni, Custom_PrefixHKLM);
    InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&hKeyRoot, KEY_READ, &objattrs) == STATUS_SUCCESS)
    {
        // open/create WER parent key
        RtlInitUnicodeString(&uni, L"SOFTWARE\\Policies\\Microsoft\\Edge");
        InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyRoot, NULL);
        if (Key_OpenOrCreateIfBoxed(&hKeyEdge, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
        {
            DWORD StartupBoostEnabled = 0;
            RtlInitUnicodeString(&uni, L"StartupBoostEnabled");
            status = NtSetValueKey(hKeyEdge, &uni, 0, REG_DWORD, &StartupBoostEnabled, sizeof(StartupBoostEnabled));

            NtClose(hKeyEdge);
        }
        NtClose(hKeyRoot);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_OpenWith
// 
// Replace open With dialog as on Win10 it requirers UWP support
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_OpenWith(void)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING uni;
    HANDLE hKeyRoot;
    HANDLE hKey;
    HANDLE hKeyCL;

    ULONG OpenWithSize = (wcslen(Dll_BoxName) + 128) * sizeof(WCHAR);
    WCHAR* OpenWithStr = Dll_AllocTemp(OpenWithSize);
    OpenWithStr[0] = L'\"';
    wcscpy(&OpenWithStr[1], Dll_HomeDosPath);
    wcscat(OpenWithStr, L"\\" START_EXE L"\" open_with \"%1\"");
    OpenWithSize = (wcslen(OpenWithStr) + 1) * sizeof(WCHAR);

    // Open HKLM
    RtlInitUnicodeString(&uni, Custom_PrefixHKLM);
    InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&hKeyRoot, KEY_READ, &objattrs) == STATUS_SUCCESS)
    {
        // open Classes key
        RtlInitUnicodeString(&uni, L"SOFTWARE\\Classes");
        InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyRoot, NULL);
        if (Key_OpenOrCreateIfBoxed(&hKeyCL, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
        {
            // open/create Undecided\shell\open\command key
            RtlInitUnicodeString(&uni, L"Undecided\\shell\\open\\command");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyCL, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKey, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // set @ = "..."
                RtlInitUnicodeString(&uni, L"");
                status = NtSetValueKey(hKey, &uni, 0, REG_SZ, (BYTE *)OpenWithStr, OpenWithSize);

                RtlInitUnicodeString(&uni, L"DelegateExecute");
                NtDeleteValueKey(hKey, &uni);

                NtClose(hKey);
            }

            // open/create Unknown\shell\Open\command key
            RtlInitUnicodeString(&uni, L"Unknown\\shell\\Open\\command");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyCL, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKey, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // set @ = "..."
                RtlInitUnicodeString(&uni, L"");
                status = NtSetValueKey(hKey, &uni, 0, REG_SZ, (BYTE *)OpenWithStr, OpenWithSize);

                RtlInitUnicodeString(&uni, L"DelegateExecute");
                NtDeleteValueKey(hKey, &uni);

                NtClose(hKey);
            }

            // open/create Unknown\shell\openas\command key
            RtlInitUnicodeString(&uni, L"Unknown\\shell\\openas\\command");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyCL, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKey, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // set @ = "..."
                RtlInitUnicodeString(&uni, L"");
                status = NtSetValueKey(hKey, &uni, 0, REG_SZ, (BYTE *)OpenWithStr, OpenWithSize);

                RtlInitUnicodeString(&uni, L"DelegateExecute");
                NtDeleteValueKey(hKey, &uni);

                NtClose(hKey);
            }

            // open/create Unknown\shell\OpenWithSetDefaultOn\command key
            RtlInitUnicodeString(&uni, L"Unknown\\shell\\OpenWithSetDefaultOn\\command");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyCL, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKey, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // set @ = "..."
                RtlInitUnicodeString(&uni, L"");
                status = NtSetValueKey(hKey, &uni, 0, REG_SZ, (BYTE *)OpenWithStr, OpenWithSize);

                RtlInitUnicodeString(&uni, L"DelegateExecute");
                NtDeleteValueKey(hKey, &uni);

                NtClose(hKey);
            }

            NtClose(hKeyCL);
        }
        NtClose(hKeyRoot);
    }

    Dll_Free(OpenWithStr);

    return TRUE;
}


//---------------------------------------------------------------------------
// DisableWerFaultUI
//
// WerFault's GUI doesn't work very well.  We will do our own in proc.c
//---------------------------------------------------------------------------


_FX BOOLEAN DisableWerFaultUI(void)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING uni;
    HANDLE hKeyRoot;
    HANDLE hKeyWER;
    HANDLE hKeyWin;
    HANDLE hKeyLocalDumps;

    // Open HKLM
    RtlInitUnicodeString(&uni, Custom_PrefixHKLM);
    InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtOpenKey(&hKeyRoot, KEY_READ, &objattrs) == STATUS_SUCCESS)
    {
        // open/create WER parent key
        RtlInitUnicodeString(&uni, L"software\\microsoft\\Windows");
        InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyRoot, NULL);
        if (Key_OpenOrCreateIfBoxed(&hKeyWin, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
        {
            // open/create WER key
            RtlInitUnicodeString(&uni, L"Windows Error Reporting");
            InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyWin, NULL);
            if (Key_OpenOrCreateIfBoxed(&hKeyWER, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
            {
                // open/create LocalDumps key
                RtlInitUnicodeString(&uni, L"LocalDumps");
                InitializeObjectAttributes(&objattrs, &uni, OBJ_CASE_INSENSITIVE, hKeyWER, NULL);
                if (Key_OpenOrCreateIfBoxed(&hKeyLocalDumps, KEY_ALL_ACCESS, &objattrs) == STATUS_SUCCESS)
                {
                    // set DontShowUI = 1
                    DWORD one = 1;
                    RtlInitUnicodeString(&uni, L"DontShowUI");
                    status = NtSetValueKey(hKeyWER, &uni, 0, REG_DWORD, &one, sizeof(one));
                    NtClose(hKeyLocalDumps);
                }
                NtClose(hKeyWER);
            }
            NtClose(hKeyWin);
        }
        NtClose(hKeyRoot);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// DisableRecycleBin
//---------------------------------------------------------------------------


_FX BOOLEAN DisableRecycleBin(void)
{
    NTSTATUS status;
    ULONG error;
    HANDLE hkey;
    UNICODE_STRING uni;
    ULONG Wow64;
    ULONG one = 1;

    //
    // Windows 2000/XP Recycle Bin:  settings in HKEY_LOCAL_MACHINE
    //

    if (Dll_IsWin64)
        goto DisableRecycleBinSkipXP;

    error = 0;
    hkey = OpenExplorerKey(HKEY_LOCAL_MACHINE, L"BitBucket", &error);
    if (! hkey) {
        status = STATUS_UNSUCCESSFUL;

    } else if (hkey != INVALID_HANDLE_VALUE) {

        RtlInitUnicodeString(&uni, L"NukeOnDelete");
        status = NtSetValueKey(
            hkey, &uni, 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        if (! NT_SUCCESS(status))
            error = 0x22;

        else {

            RtlInitUnicodeString(&uni, L"UseGlobalSettings");
            status = NtSetValueKey(
                hkey, &uni, 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            if (! NT_SUCCESS(status))
                error = 0x33;
        }

        NtClose(hkey);
    }

    if (error) {
        SbieApi_Log(2311, L"[%02X / %08X]", error, status);
        return FALSE;
    }

DisableRecycleBinSkipXP:

    //
    // Windows Vista/7 Recycle Bin:  settings in HKEY_CURRENT_USER
    //

    Wow64 = Dll_IsWin64 ? KEY_WOW64_64KEY : 0;

    hkey = OpenExplorerKey(HKEY_CURRENT_USER, L"BitBucket", &error);
    if (hkey && hkey != INVALID_HANDLE_VALUE) {

        OBJECT_ATTRIBUTES objattrs;
        UNICODE_STRING objname;
        HANDLE hkey2;

        InitializeObjectAttributes(
            &objattrs, &objname, OBJ_CASE_INSENSITIVE, hkey, NULL);

        RtlInitUnicodeString(&objname, L"Volume");

        status = Key_OpenIfBoxed(&hkey2, KEY_ALL_ACCESS | Wow64, &objattrs);
        if (NT_SUCCESS(status)) {

            ULONG index = 0;
            ULONG len;
            union {
                ULONG info[64];
                KEY_BASIC_INFORMATION kbi;
            } u;
            HANDLE hkey3;

            objattrs.RootDirectory = hkey2;

            while (1) {

                status = NtEnumerateKey(
                    hkey2, index, KeyBasicInformation, &u, sizeof(u), &len);

                if (! NT_SUCCESS(status))
                    break;

                u.kbi.Name[u.kbi.NameLength / sizeof(WCHAR)] = L'\0';
                RtlInitUnicodeString(&objname, u.kbi.Name);

                status = Key_OpenIfBoxed(
                                &hkey3, KEY_ALL_ACCESS | Wow64, &objattrs);
                if (NT_SUCCESS(status)) {

                    RtlInitUnicodeString(&uni, L"NukeOnDelete");
                    status = NtSetValueKey(
                        hkey3, &uni, 0, REG_DWORD,
                        (BYTE *)&one, sizeof(one));

                    NtClose(hkey3);
                }

                ++index;
            }

            NtClose(hkey2);
        }

        NtClose(hkey);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_EnableBrowseNewProcess
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_EnableBrowseNewProcess(void)
{
    static const WCHAR yes[] = L"yes";
    NTSTATUS status;
    ULONG error;
    HANDLE hkey;
    UNICODE_STRING uni;

    // BrowseNewProcess

    error = 0;
    hkey = OpenExplorerKey(HKEY_CURRENT_USER, L"BrowseNewProcess", &error);
    if (! hkey) {
        status = STATUS_UNSUCCESSFUL;

    } else if (hkey != INVALID_HANDLE_VALUE) {

        RtlInitUnicodeString(&uni, L"BrowseNewProcess");
        status = NtSetValueKey(
            hkey, &uni, 0, REG_SZ, (BYTE *)&yes, sizeof(yes));
        if (! NT_SUCCESS(status))
            error = 0x22;

        NtClose(hkey);
    }

    if (error) {
        SbieApi_Log(2312, L"[%02X / %08X]", error, status);
        return FALSE;
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_DisableBHOs
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_DisableBHOs(void)
{
    static const WCHAR *_prefix =
        L"\\registry\\machine\\software\\classes\\clsid\\";
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING objname;
    HANDLE handle;
    WCHAR path[128];

    ULONG Wow64 = Dll_IsWin64 ? KEY_WOW64_64KEY : 0;

    InitializeObjectAttributes(
        &objattrs, &objname, OBJ_CASE_INSENSITIVE, NULL, NULL);

    //
    // disable a NAV plugin that prevents opening Office documents from IE
    //

    wcscpy(path, _prefix);
    wcscat(path, L"{DE1F7EEF-1851-11D3-939E-0004AC1ABE1F}");
    RtlInitUnicodeString(&objname, path);

    status = Key_OpenIfBoxed(&handle, KEY_ALL_ACCESS | Wow64, &objattrs);
    if (NT_SUCCESS(status))
        Key_MarkDeletedAndClose(handle);

    //
    // disable the Java SSVHelper Class that slows down IE
    //

    wcscpy(path, _prefix);
    wcscat(path, L"{761497BB-D6F0-462C-B6EB-D4DAF1D92D43}");
    RtlInitUnicodeString(&objname, path);

    status = Key_OpenIfBoxed(&handle, KEY_ALL_ACCESS | Wow64, &objattrs);
    if (NT_SUCCESS(status))
        Key_MarkDeletedAndClose(handle);

    wcscpy(path, _prefix);
    wcscat(path, L"{DBC80044-A445-435B-BC74-9C25C1C588A9}");
    RtlInitUnicodeString(&objname, path);

    status = Key_OpenIfBoxed(&handle, KEY_ALL_ACCESS | Wow64, &objattrs);
    if (NT_SUCCESS(status))
        Key_MarkDeletedAndClose(handle);

    wcscpy(path, _prefix);
    wcscat(path, L"{E7E6F031-17CE-4C07-BC86-EABFE594F69C}");
    RtlInitUnicodeString(&objname, path);

    status = Key_OpenIfBoxed(&handle, KEY_ALL_ACCESS | Wow64, &objattrs);
    if (NT_SUCCESS(status))
        Key_MarkDeletedAndClose(handle);

    return TRUE;
}


//---------------------------------------------------------------------------
// OpenExplorerKey
//---------------------------------------------------------------------------


_FX HANDLE OpenExplorerKey(
    HANDLE ParentKey, const WCHAR *SubkeyName, ULONG *error)
{
    static const WCHAR *_Explorer =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";

    NTSTATUS status;
    HANDLE HKey_Root;
    HANDLE HKey_Explorer;
    HANDLE HKey_Subkey;
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;

    //
    // open HKLM or HKCU depending on ForCurrentUser
    //

    if (ParentKey == HKEY_LOCAL_MACHINE) {

        RtlInitUnicodeString(&uni, Custom_PrefixHKLM);

    } else if (ParentKey == HKEY_CURRENT_USER) {

        RtlInitUnicodeString(&uni, Custom_PrefixHKCU);

    } else {
        *error = 0xAA;
        return NULL;
    }

    InitializeObjectAttributes(
        &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtOpenKey(&HKey_Root, KEY_READ, &objattrs);

    if (status != STATUS_SUCCESS) {
        *error = 0x99;
        return NULL;
    }

    //
    // open Explorer parent key
    //

    RtlInitUnicodeString(&uni, _Explorer);
    InitializeObjectAttributes(
        &objattrs, &uni, OBJ_CASE_INSENSITIVE, HKey_Root, NULL);
    status = NtOpenKey(&HKey_Explorer, KEY_READ, &objattrs);

    NtClose(HKey_Root);

    if (status != STATUS_SUCCESS) {
        *error = 0x88;
        return NULL;
    }

    //
    // open sub key below Explorer
    //

    RtlInitUnicodeString(&uni, SubkeyName);
    InitializeObjectAttributes(
        &objattrs, &uni, OBJ_CASE_INSENSITIVE, HKey_Explorer, NULL);

    status = Key_OpenOrCreateIfBoxed(
                    &HKey_Subkey, KEY_ALL_ACCESS, &objattrs);

    if (status == STATUS_BAD_INITIAL_PC) {
        *error = 0;
        return INVALID_HANDLE_VALUE;
    }

    NtClose(HKey_Explorer);

    if (status != STATUS_SUCCESS) {
        *error = 0x77;
        return NULL;
    }

    return HKey_Subkey;
}


//---------------------------------------------------------------------------
// DeleteShellAssocKeys
//---------------------------------------------------------------------------


_FX void DeleteShellAssocKeys(ULONG Wow64)
{
    static WCHAR *_Registry = L"\\Registry\\";
    static WCHAR *subkeys[] = {
        L"Machine\\Software\\Classes",
        L"User\\Current\\Software\\Classes",
        NULL
    };
    NTSTATUS status;
    HANDLE hkey;
    WCHAR path[128];
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    ULONG i;

    //
    // if running on 64-bit Windows, restart ourselves twice
    //

    if (Dll_IsWin64 && (! Wow64)) {

        DeleteShellAssocKeys(KEY_WOW64_64KEY);
        DeleteShellAssocKeys(KEY_WOW64_32KEY);
        return;
    }

    //
    // main process
    //

    for (i = 0; subkeys[i]; ++i) {

        //
        // delete (root)\*\shell\sandbox key
        //

        wcscpy(path, _Registry);
        wcscat(path, subkeys[i]);
        wcscat(path, L"\\*\\shell\\sandbox");

        RtlInitUnicodeString(&uni, path);

        InitializeObjectAttributes(
            &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

        status = Key_OpenIfBoxed(&hkey, KEY_ALL_ACCESS | Wow64, &objattrs);
        if (NT_SUCCESS(status)) {

            Key_NtDeleteKeyTree(hkey, TRUE);
            NtClose(hkey);
        }

        //
        // delete (root)\CompressedFolder\shell\open\ddeexec
        //

        wcscpy(path, _Registry);
        wcscat(path, subkeys[i]);
        wcscat(path, L"\\CompressedFolder\\shell\\open\\ddeexec");

        RtlInitUnicodeString(&uni, path);

        InitializeObjectAttributes(
            &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

        status = Key_OpenIfBoxed(&hkey, KEY_ALL_ACCESS | Wow64, &objattrs);
        if (NT_SUCCESS(status)) {

            Key_NtDeleteKeyTree(hkey, TRUE);
            NtClose(hkey);
        }
    }
}


//---------------------------------------------------------------------------
// AutoExec
//---------------------------------------------------------------------------


_FX void AutoExec(void)
{
    NTSTATUS status;
    UNICODE_STRING uni;
    WCHAR error_str[16];
    WCHAR* buf1;
    ULONG buf_len;
    ULONG index;
    KEY_VALUE_BASIC_INFORMATION basic_info;
    ULONG len;

    //
    // query the values in the AutoExec setting
    //

    buf_len = 4096 * sizeof(WCHAR);
    buf1 = Dll_AllocTemp(buf_len);
    memzero(buf1, buf_len);

    index = 0;

    while (1) {

        status = SbieApi_QueryConfAsIs(
                            NULL, L"AutoExec", index, buf1, buf_len - 16);
        if (status != 0)
            break;

        //
        // check the key value matching the setting value
        //

        RtlInitUnicodeString(&uni, buf1);

        if (AutoExecHKey) {
            status = NtQueryValueKey(
                AutoExecHKey, &uni, KeyValueBasicInformation,
                &basic_info, sizeof(basic_info), &len);
        } else
            status = STATUS_OBJECT_NAME_NOT_FOUND;

        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {

            if (AutoExecHKey)
                status = NtSetValueKey(AutoExecHKey, &uni, 0, REG_SZ, NULL, 0);
            else
                status = STATUS_SUCCESS;

            if (NT_SUCCESS(status)) {

                SbieDll_ExpandAndRunProgram(buf1);

            } else {
                Sbie_snwprintf(error_str, 16, L"%d [%08X]", index, status);
                SbieApi_Log(2206, error_str);
            }
        }

        ++index;
    }

    //
    // finish
    //

    Dll_Free(buf1);
}


//---------------------------------------------------------------------------
// SbieDll_ExpandAndRunProgram
//---------------------------------------------------------------------------


_FX BOOLEAN SbieDll_ExpandAndRunProgram(const WCHAR *Command)
{
    ULONG len;
    WCHAR *cmdline, *cmdline2;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    BOOL ok;

    //
    // expand command line
    //

    len = ExpandEnvironmentStrings(Command, NULL, 0);
    if (len == 0)
        return FALSE;

    cmdline = Dll_AllocTemp((len + 8) * sizeof(WCHAR));
    ExpandEnvironmentStrings(Command, cmdline, len);
    cmdline[len] = L'\0';

    //
    // expand sandboxie variables
    //

    len = 8192 * sizeof(WCHAR);
    cmdline2 = Dll_AllocTemp(len);

	const WCHAR* ptr1 = cmdline;
	WCHAR* ptr2 = cmdline2;
	for (;;) {
		const WCHAR* ptr = wcschr(ptr1, L'%');
		if (!ptr)
			break;
		const WCHAR* end = wcschr(ptr + 1, L'%');
		if (!end) 
			break;

		if (ptr != ptr1) { // copy static portion unless we start with a %
			ULONG length = (ULONG)(ptr - ptr1);
			wmemcpy(ptr2, ptr1, length);
			ptr2 += length;
		}
		ptr1 = end + 1;

		ULONG length = (ULONG)(end - ptr + 1);
		if (length <= 64) {
			WCHAR Var[66];
			wmemcpy(Var, ptr, length);
			Var[length] = L'\0';
			if (NT_SUCCESS(SbieApi_QueryConf(NULL, Var, CONF_JUST_EXPAND, ptr2, len - ((ptr2 - cmdline2) * sizeof(WCHAR))))) {
				if (_wcsnicmp(ptr2, L"\\Device\\", 8) == 0)
					SbieDll_TranslateNtToDosPath(ptr2);
				ptr2 += wcslen(ptr2);
				continue; // success - look for the next one
			}
		}
		
		// fallback - not found keep the %something%
		wmemcpy(ptr2, ptr, length);
		ptr2 += len;
	}
	wcscpy(ptr2, ptr1); // copy what's left

    Dll_Free(cmdline);

    //
    // execute command line
    //

    memzero(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memzero(&pi, sizeof(PROCESS_INFORMATION));

    ok = CreateProcess(
            NULL, cmdline2, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    Dll_Free(cmdline2);

    if (ok) {

        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    return (ok ? TRUE : FALSE);
}


//---------------------------------------------------------------------------
// Custom_ComServer
//---------------------------------------------------------------------------


_FX void Custom_ComServer(void)
{
    //
    // the scenario of a forced COM server occurs when the COM server
    // program is forced to run in a sandbox, and it is started by COM
    // outside the sandbox in response to a COM CoCreateInstance request.
    // (typically this is Internet Explorer or Windows Media Player.)
    //
    // the program in the sandbox can't talk to COM outside the sandbox
    // to serve the request, so we need some workaround.  prior to
    // version 4, the workaround was to grant the process full access
    // to the COM IPC objects (like epmapper) and then use the
    // comserver module to simulate a COM server.  This means we talked
    // to the real COM using expected COM interfaces, in order to get
    // the url or file that has to be opened.  then we restarted the
    // process (this time without access to COM IPC objects), specifying
    // the path to the target url or file.
    //
    // in v4 this no longer works because the forced process is running
    // with untrusted privileges so the real COM will not let it sign up
    // as a COM server.  (COM returns error "out of memory" when we try
    // to use CoRegisterClassObject.)  to work around this, the comserver
    // module was moved into SbieSvc, and here we just issue a special
    // SbieDll_RunSandboxed request which runs an instance of SbieSvc
    // outside the sandbox.  SbieSvc (in file core/svc/comserver9.c) then
    // talks to real COM to get the target url or file, then it starts the
    // requested server program in the sandbox.
    //
    // the simulated COM server is implemented in core/svc/comserver9.c
    //

    WCHAR *cmdline;
    WCHAR exepath[MAX_PATH + 4];
    STARTUPINFO StartupInfo;
    PROCESS_INFORMATION ProcessInformation;

    //
    // process is probably a COM server if has both the forced process flag
    // and the protected process flag (see Ipc_IsComServer in core/drv/ipc.c)
    // we also check that several other flags are not set
    //

    const ULONG _FlagsOn    = SBIE_FLAG_FORCED_PROCESS
                            | SBIE_FLAG_PROTECTED_PROCESS;
    const ULONG _FlagsOff   = SBIE_FLAG_IMAGE_FROM_SANDBOX
                            | SBIE_FLAG_PROCESS_IN_PCA_JOB;

    if ((Dll_ProcessFlags & (_FlagsOn | _FlagsOff)) != _FlagsOn)
        return;

    //
    // check if we're running in an IEXPLORE.EXE / WMPLAYER.EXE process,
    // which was given the -Embedding command line switch
    //

    if (    Dll_ImageType != DLL_IMAGE_INTERNET_EXPLORER
        &&  Dll_ImageType != DLL_IMAGE_WINDOWS_MEDIA_PLAYER
        &&  Dll_ImageType != DLL_IMAGE_NULLSOFT_WINAMP
        &&  Dll_ImageType != DLL_IMAGE_PANDORA_KMPLAYER) {

        return;
    }

    cmdline = GetCommandLine();
    if (! cmdline)
        return;
    if (! (wcsstr(cmdline, L"/Embedding") ||
           wcsstr(cmdline, L"-Embedding")))
        return;

    //
    // we are a COM server process that was started by DcomLaunch outside
    // the sandbox but forced to run in the sandbox.  we need to run SbieSvc
    // outside the sandbox so it can talk to COM to get the invoked url,
    // and then restart the server program in the sandbox
    //

    GetModuleFileName(NULL, exepath, MAX_PATH);

    SetEnvironmentVariable(ENV_VAR_PFX L"COMSRV_EXE", exepath);
    SetEnvironmentVariable(ENV_VAR_PFX L"COMSRV_CMD", cmdline);

    memzero(&StartupInfo, sizeof(STARTUPINFO));
    StartupInfo.cb = sizeof(STARTUPINFO);
    StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK;

    if (Proc_ImpersonateSelf(TRUE)) {

        // *COMSRV* as cmd line tells SbieSvc ProcessServer to run
        // SbieSvc SANDBOXIE_ComProxy_ComServer:BoxName
        // see also core/svc/ProcessServer.cpp
        // and      core/svc/comserver9.c
        BOOL ok = SbieDll_RunSandboxed(L"", L"*COMSRV*", L"", 0,
                                       &StartupInfo, &ProcessInformation);

        if (ok)
            WaitForSingleObject(ProcessInformation.hProcess, INFINITE);
    }

    ExitProcess(0);
}


//---------------------------------------------------------------------------
// NsiRpc_Init
//---------------------------------------------------------------------------

#include <objbase.h>

typedef RPC_STATUS (*P_NsiRpcRegisterChangeNotification)(
    LPVOID  p1, LPVOID  p2, LPVOID  p3, LPVOID  p4, LPVOID  p5, LPVOID  p6, LPVOID  p7);

P_NsiRpcRegisterChangeNotification __sys_NsiRpcRegisterChangeNotification = NULL;

static RPC_STATUS NsiRpc_NsiRpcRegisterChangeNotification(
    LPVOID  p1, LPVOID  p2, LPVOID  p3, LPVOID  p4, LPVOID  p5, LPVOID  p6, LPVOID  p7);


_FX BOOLEAN NsiRpc_Init(HMODULE module)
{
    P_NsiRpcRegisterChangeNotification NsiRpcRegisterChangeNotification;

    NsiRpcRegisterChangeNotification = (P_NsiRpcRegisterChangeNotification)
        Ldr_GetProcAddrNew(DllName_winnsi, L"NsiRpcRegisterChangeNotification", "NsiRpcRegisterChangeNotification");

    SBIEDLL_HOOK(NsiRpc_, NsiRpcRegisterChangeNotification);

    return TRUE;
}


//  In Win8.1 WININET initialization needs to register network change events into NSI service (Network Store Interface Service).
//  The communication between guest and NSI service is via epmapper. We currently block epmapper and it blocks guest and NSI service.
//  This causes IE to pop up a dialog "Revocation information for the security certificate for this site is not available. Do you want to proceed?"
//  The fix can be either - 
//  1.  Allowing guest to talk to NSI service by fixing RpcBindingCreateW like what we did for keyiso-crypto and Smart Card service.
//      ( We don't fully know what we actually open to allow guest to talk to NSI service and if it is needed. It has been blocked. )
//  2. Hooking NsiRpcRegisterChangeNotification and silently returning NO_ERROR from the hook.
//      ( Guest app won't get Network Change notification. I am not sure if this is needed for the APP we support. )
//  We choose Fix 2 here. We may need fix 1 if see a need in the future.


_FX RPC_STATUS NsiRpc_NsiRpcRegisterChangeNotification(LPVOID  p1, LPVOID  p2, LPVOID  p3, LPVOID  p4, LPVOID  p5, LPVOID  p6, LPVOID  p7)
{
    RPC_STATUS ret = __sys_NsiRpcRegisterChangeNotification(p1, p2, p3, p4, p5, p6, p7);

    if (EPT_S_NOT_REGISTERED == ret)
    {
        ret = NO_ERROR;
    }
    return ret;
}






//---------------------------------------------------------------------------
// BEYOND HERE BE DRAGONS - workarounds for non windows components
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Custom_SilverlightAgCore
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_SilverlightAgCore(HMODULE module)
{
    NTSTATUS status;
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    HANDLE hKey;

    //
    // turn off Silverlight auto update mode
    //

    InitializeObjectAttributes(
        &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

    RtlInitUnicodeString(&uni,
        L"\\registry\\user\\current\\software"
            L"\\Microsoft\\Silverlight");

    status = Key_OpenIfBoxed(&hKey, KEY_SET_VALUE, &objattrs);
    if (NT_SUCCESS(status)) {

        ULONG two = 2;

        RtlInitUnicodeString(&uni, L"UpdateMode");

        status = NtSetValueKey(
            hKey, &uni, 0, REG_DWORD, &two, sizeof(ULONG));

        NtClose(hKey);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_OsppcDll
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_OsppcDll(HMODULE module)
{
    const WCHAR *ProductNames[] = {
        L"Excel", L"Word", L"PowerPoint", NULL };
    const WCHAR *ValueNames[] = {
        L"DisableAttachmentsInPV",      L"DisableInternetFilesInPV",
        L"DisableUnsafeLocationsInPV",  NULL };

    NTSTATUS status;
    HANDLE hOfficeKey, hKey;
    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    WCHAR path[128];
    ULONG one = 1;
    ULONG zero = 0;
    ULONG ProductIndex, ValueIndex;

    ULONG Wow64 = Dll_IsWow64 ? KEY_WOW64_64KEY : 0;

    //
    // open Microsoft Office 2010 registry key
    //

    InitializeObjectAttributes(
        &objattrs, &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

    RtlInitUnicodeString(&uni,
        L"\\registry\\user\\current\\software"
            L"\\Microsoft\\Office\\14.0");

    status = Key_OpenIfBoxed(&hOfficeKey, KEY_ALL_ACCESS | Wow64, &objattrs);
    if (! NT_SUCCESS(status))
        return TRUE;

    //
    // for each product, open the registry key "Security\ProtectedView"
    //

    for (ProductIndex = 0; ProductNames[ProductIndex]; ++ProductIndex) {

        objattrs.RootDirectory = hOfficeKey;
        wcscpy(path, ProductNames[ProductIndex]);
        wcscat(path, L"\\Security\\ProtectedView");
        RtlInitUnicodeString(&uni, path);

        status = Key_OpenIfBoxed(&hKey, KEY_ALL_ACCESS | Wow64, &objattrs);

        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {

            status = NtCreateKey(
                &hKey, KEY_ALL_ACCESS | Wow64, &objattrs,
                0, NULL, 0, NULL);
        }

        if (NT_SUCCESS(status)) {

            for (ValueIndex = 0; ValueNames[ValueIndex]; ++ValueIndex) {

                RtlInitUnicodeString(&uni, ValueNames[ValueIndex]);
                NtSetValueKey(
                    hKey, &uni, 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            }

            NtClose(hKey);
        }
    }

    //
    // for the Microsoft Office Picture Manager, set the FirstBoot value
    //

    if (1) {

        objattrs.RootDirectory = hOfficeKey;
        RtlInitUnicodeString(&uni, L"OIS");
        status = Key_OpenIfBoxed(&hKey, KEY_ALL_ACCESS | Wow64, &objattrs);
        if (NT_SUCCESS(status)) {

            RtlInitUnicodeString(&uni, L"FirstBoot");
            NtSetValueKey(
                hKey, &uni, 0, REG_DWORD, (BYTE *)&zero, sizeof(zero));

            NtClose(hKey);
        }
    }

    //
    // finish
    //

    NtClose(hOfficeKey);
    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_InternetDownloadManager
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_InternetDownloadManager(HMODULE module)
{
    //
    // outside the sandbox, IEShims.dll makes sure that IDMIECC has a
    // load count of -1, but our Key_NtQueryValueKeyFakeForInternetExplorer
    // routine turns off DetourDialogs and prevents IEShims.dll from loading
    // so we need to emulate that functionality here
    //

    if (Dll_ImageType == DLL_IMAGE_INTERNET_EXPLORER) {

        Ldr_MakeStaticDll((ULONG_PTR)module);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
//
// avast! compatibility hook:  avast hook dll snxhk.dll (also snxhk64.dll)
// calls LdrGetProcedureAddress to get the address of NtDeviceIoControlFile
// and then copies the code.  if the code includes relative jumps (which
// it does, after processing by SbieDll_Hook), this is not fixed up while
// copying, and causes avast to crash.  to work around this, we return a
// small trampoline with the following code which avoids a relative jump:
// mov eax, NtDeviceIoControlFile; jmp eax
//
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Custom_Avast_SnxHk_LdrGetProcedureAddress
//---------------------------------------------------------------------------


typedef NTSTATUS (*P_LdrGetProcedureAddress)(
    HANDLE ModuleHandle, ANSI_STRING *ProcName, ULONG ProcNum,
    ULONG_PTR *Address);

static P_LdrGetProcedureAddress __sys_LdrGetProcedureAddress = NULL;

static NTSTATUS Custom_Avast_SnxHk_LdrGetProcedureAddress(
    HANDLE ModuleHandle, ANSI_STRING *ProcName, ULONG ProcNum,
    ULONG_PTR *Address);


//---------------------------------------------------------------------------
// Custom_Avast_SnxHk_LdrGetProcedureAddress
//---------------------------------------------------------------------------


_FX NTSTATUS Custom_Avast_SnxHk_LdrGetProcedureAddress(
    HANDLE ModuleHandle, ANSI_STRING *ProcName, ULONG ProcNum,
    ULONG_PTR *Address)
{
    NTSTATUS status = __sys_LdrGetProcedureAddress(
                                ModuleHandle, ProcName, ProcNum, Address);

    if (NT_SUCCESS(status) &&
            ModuleHandle == Dll_Ntdll &&
            ProcName->Length == 21 &&
            memcmp(ProcName->Buffer, "NtDeviceIoControlFile", 21) == 0) {

        static UCHAR *code = 0;
        if (! code) {

            code = Dll_AllocCode128();
#ifdef _WIN64
            *(USHORT *)code = 0xB848;
            *(ULONG64 *)(code + 2) = *Address;
            *(USHORT *)(code + 10) = 0xE0FF;
#else ! _WIN64
            *code = 0xB8;
            *(ULONG *)(code + 1) = *Address;
            *(USHORT *)(code + 5) = 0xE0FF;
#endif _WIN64
        }

        *Address = (ULONG_PTR)code;
    }

    return status;
}


//---------------------------------------------------------------------------
// Custom_Avast_SnxHk
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_Avast_SnxHk(HMODULE module)
{
    static BOOLEAN _done = FALSE;
    if (! _done) {
        SBIEDLL_HOOK(Custom_Avast_SnxHk_,LdrGetProcedureAddress);
        _done = TRUE;
    }
    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_SYSFER_DLL
//---------------------------------------------------------------------------


_FX BOOLEAN Custom_SYSFER_DLL(HMODULE hmodule)
{
    //
    // Symantec Endpoint Protection SYSFER.DLL hooks NT API stubs in
    // NTDLL by simply copying code bytes elsewhere.  this doesn't work
    // for us because it doesn't adjust JMP pointers.  we use this
    // workaround to nullify SYSFER.DLL
    //

    extern IMAGE_OPTIONAL_HEADER *Ldr_OptionalHeader(ULONG_PTR ImageBase);
    ULONG_PTR base = (ULONG_PTR)hmodule;
    IMAGE_OPTIONAL_HEADER *opt_hdr = Ldr_OptionalHeader(base);
    UCHAR *entrypoint = (UCHAR *)(base + opt_hdr->AddressOfEntryPoint);
    ULONG old_prot;
    if (VirtualProtect(entrypoint, 16, PAGE_EXECUTE_READWRITE, &old_prot)) {

        //
        // mov al, 1 ; ret
        //
        // we don't bother with RET 12 on x86 because the NTDLL loader
        // restores the stack pointer that it saved before calling
        //

        *(ULONG *)entrypoint = 0x00C301B0;
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Custom_Load_UxTheme
//---------------------------------------------------------------------------


/*_FX void Custom_Load_UxTheme(void)
{
    //
    // Google Chrome sandbox process is started with limited privileges
    // but the first thread is given a normal token for start up.
    // outside the sandbox the process creates a STATIC window, before
    // releasing its start up token, this causes UXTHEME.DLL to load in
    // the process.  in the v4 sandbox, with Google Chrome v28, no such
    // window is created, and UXTHEME.DLL will only be loaded as a result
    // of a later call to SystemParametersInfo(SPI_GETFONTSMOOTHING),
    // however by that time the process has limited privileges and this
    // load fails.  we work around this by loading UXTHEME.DLL here.
    //

    if (Dll_ChromeSandbox) {
        typedef BOOL (*P_SystemParametersInfo)(
            UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
        P_SystemParametersInfo SystemParametersInfo =
            Ldr_GetProcAddrNew(DllName_user32, L"SystemParametersInfoW","SystemParametersInfoW");
        if (SystemParametersInfo) {
            BOOL v;
            SystemParametersInfo(SPI_GETFONTSMOOTHING, 0, &v, 0);
        }
    }
}*/

//---------------------------------------------------------------------------
// Handles ActivClient's acscmonitor.dll which crashes firefox in sandboxie.
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Acscmonitor_LoadLibrary
//---------------------------------------------------------------------------

ULONG CALLBACK Acscmonitor_LoadLibrary(LPVOID lpParam)
{
    // Acscmonitor is a plugin to Firefox which create a thread on initialize.
    // Firefox has a habit of initializing the module right before it's about
    // to unload the DLL, which is causing the crash.
    // Our solution is to prevent the library from ever being removed by holding
    // a reference to it.
    LoadLibraryW(L"acscmonitor.dll");
    return 0;
}

//---------------------------------------------------------------------------
// Acscmonitor_Init
//---------------------------------------------------------------------------

_FX BOOLEAN Acscmonitor_Init(HMODULE hDll)
{
	HANDLE ThreadHandle = CreateThread(NULL, 0, Acscmonitor_LoadLibrary, (LPVOID)0, 0, NULL);
	if (ThreadHandle)
		CloseHandle(ThreadHandle); 
    return TRUE;
}
