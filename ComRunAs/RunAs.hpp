#pragma once
#include <Windows.h>
#include <lsalookup.h>
#include <strsafe.h>
#include <subauth.h>
#define _NTDEF_
#include <ntsecapi.h>
#include <atlbase.h> // CRegKey
#include <string>
#include <vector>


/** LSA_HANDLE RAII wrapper */
class LsaWrap {
public:
    LsaWrap() {
    }
    ~LsaWrap() {
        if (obj) {
            LsaClose(obj);
            obj = nullptr;
        }
    }

    operator LSA_HANDLE () {
        return obj;
    }
    LSA_HANDLE* operator & () {
        return &obj;
    }

private:
    LsaWrap(const LsaWrap&) = delete;
    LsaWrap& operator = (const LsaWrap&) = delete;

    LSA_HANDLE obj = nullptr;
};

// Code based on https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/com/fundamentals/dcom/dcomperm

DWORD SetRunAsPassword(const std::wstring& AppID, const std::wstring& username, const std::wstring& password);
DWORD SetAccountRights(const std::wstring& username, const WCHAR privilege[]);
DWORD GetPrincipalSID(const std::wstring& username, /*out*/std::vector<BYTE>& pSid);
BOOL ConstructWellKnownSID(const std::wstring& username, /*out*/std::vector<BYTE>& pSid);


DWORD SetRunAsAccount(const std::wstring AppID, const std::wstring username, const std::wstring password)
{
    std::wstring tszKeyName = L"APPID\\" + AppID;
    CRegKey hkeyRegistry;
    DWORD dwReturnValue = hkeyRegistry.Open(HKEY_CLASSES_ROOT, tszKeyName.c_str(), KEY_ALL_ACCESS);
    if (dwReturnValue != ERROR_SUCCESS) {
        wprintf(L"ERROR: Cannot open AppID registry key (%d).", dwReturnValue);
        return dwReturnValue;
    }

    if (_wcsicmp(username.c_str(), L"LAUNCHING USER") == 0) {
        // default case so delete "RunAs" value 
        dwReturnValue = hkeyRegistry.DeleteValue(L"RunAs");

        if (dwReturnValue == ERROR_FILE_NOT_FOUND) {
            dwReturnValue = ERROR_SUCCESS;
        } else if (dwReturnValue != ERROR_SUCCESS) {
            wprintf(L"ERROR: Cannot remove RunAs registry value (%d).", dwReturnValue);
            return dwReturnValue;
        }
    } else {
        // TODO: Skip password also for "nt authority\localservice" & "nt authority\networkservice"

        if (_wcsicmp(username.c_str(), L"INTERACTIVE USER") == 0) {
            // password not needed
        } else {
            // password needed
            dwReturnValue = SetRunAsPassword(AppID, username, password);
            if (dwReturnValue != ERROR_SUCCESS) {
                wprintf(L"ERROR: Cannot set RunAs password (%d).", dwReturnValue);
                return dwReturnValue;
            }
        }

        dwReturnValue = hkeyRegistry.SetStringValue(L"RunAs", username.c_str());
        if (dwReturnValue != ERROR_SUCCESS) {
            wprintf(L"ERROR: Cannot set RunAs registry value (%d).", dwReturnValue);
            return dwReturnValue;
        }
    }

    return ERROR_SUCCESS;
}

/*---------------------------------------------------------------------------*\
 * NAME: SetRunAsPassword                                                    *
 * --------------------------------------------------------------------------*
 * DESCRIPTION: Sets the RunAs password for an AppID. Note that if you       *
 * have specified the RunAs named value to "Interactive User" you do not     *
 * need to set the RunAs password.                                           *
 * --------------------------------------------------------------------------*
 *  ARGUMENTS:                                                               *
 *                                                                           *
 *  tszAppID - The Application ID you wish to modify                         *
 *  (e.g. "{99999999-9999-9999-9999-00AA00BBF7C7}")                          *
 *                                                                           *
 *  username - Name of the principal you have specified in the RunAs     *
 *  named value under the AppID registry key                                 *
 *                                                                           *
 *  tszPassword - Password of the user you have specified in the RunAs       *
 *  named value under the AppID registry key.                                *
 * --------------------------------------------------------------------------*
 *  RETURNS: WIN32 Error Code                                                *
\*---------------------------------------------------------------------------*/
DWORD SetRunAsPassword(const std::wstring& AppID, const std::wstring& username, const std::wstring& password)
{
    // TODO: Check if password is valid

    std::wstring key = L"SCM:" + AppID;
    LSA_UNICODE_STRING lsaKeyString = {};
    lsaKeyString.Length = (USHORT)(key.length()*sizeof(WCHAR)); // exclude null-termination
    lsaKeyString.MaximumLength = lsaKeyString.Length + sizeof(WCHAR); // include null-termination
    lsaKeyString.Buffer = key.data();

    LSA_UNICODE_STRING lsaPasswordString = {};
    lsaPasswordString.Length = (USHORT)(password.length()*sizeof(WCHAR)); // exclude null-termination
    lsaPasswordString.MaximumLength = lsaPasswordString.Length + sizeof(WCHAR); // include null-termination
    lsaPasswordString.Buffer = const_cast<WCHAR*>(password.data());

    // Open the local security policy
    LSA_OBJECT_ATTRIBUTES objectAttributes = { 0 };
    objectAttributes.Length = sizeof(LSA_OBJECT_ATTRIBUTES);

    LsaWrap hPolicy;
    DWORD dwReturnValue = LsaOpenPolicy(NULL, &objectAttributes, POLICY_CREATE_SECRET, &hPolicy);
    dwReturnValue = LsaNtStatusToWinError(dwReturnValue);
    if (dwReturnValue != ERROR_SUCCESS)
        return dwReturnValue;

    // Store the user's password
    dwReturnValue = LsaStorePrivateData(hPolicy, &lsaKeyString, &lsaPasswordString);
    dwReturnValue = LsaNtStatusToWinError(dwReturnValue);
    if (dwReturnValue != ERROR_SUCCESS)
        return dwReturnValue;


    dwReturnValue = SetAccountRights(username, L"SeBatchLogonRight");
    return dwReturnValue;
}


/*---------------------------------------------------------------------------*\
 * NAME: SetAccountRights                                                    *
 * --------------------------------------------------------------------------*
 * DESCRIPTION: Sets the account right for a given user.                     *
\*---------------------------------------------------------------------------*/
DWORD SetAccountRights(const std::wstring& username, const WCHAR privilege[])
{
    LSA_OBJECT_ATTRIBUTES objectAttributes = {};
    LsaWrap hPolicy;
    DWORD dwReturnValue = LsaOpenPolicy(NULL, &objectAttributes, POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES, &hPolicy);
    dwReturnValue = LsaNtStatusToWinError(dwReturnValue);
    if (dwReturnValue != ERROR_SUCCESS)
        return dwReturnValue;

    std::vector<BYTE> sidPrincipal; // PSID buffer
    dwReturnValue = GetPrincipalSID(username, sidPrincipal);
    if (dwReturnValue != ERROR_SUCCESS)
        return dwReturnValue;

    LSA_UNICODE_STRING lsaPrivilegeString = {};
    lsaPrivilegeString.Length = (USHORT)(wcslen(privilege)*sizeof(WCHAR)); // exclude null-termination
    lsaPrivilegeString.MaximumLength = lsaPrivilegeString.Length + sizeof(WCHAR); // include null-termination
    lsaPrivilegeString.Buffer = const_cast<WCHAR*>(privilege);

    dwReturnValue = LsaAddAccountRights(hPolicy, sidPrincipal.data(), &lsaPrivilegeString, 1);
    dwReturnValue = LsaNtStatusToWinError(dwReturnValue);
    return dwReturnValue;
}

/*---------------------------------------------------------------------------*\
 * NAME: GetPrincipalSID                                                     *
 * --------------------------------------------------------------------------*
 * DESCRIPTION: Creates a SID for the supplied principal.                    *
\*---------------------------------------------------------------------------*/
DWORD GetPrincipalSID(const std::wstring& username, /*out*/std::vector<BYTE>& pSid)
{
    // first check for known in-built SID
    if (ConstructWellKnownSID(username, /*out*/pSid))
        return ERROR_SUCCESS;

    TCHAR        tszRefDomain[256] = { 0 };
    DWORD        cbRefDomain = 255;
    SID_NAME_USE snu;
    DWORD cbSid = 0;
    LookupAccountNameW(NULL, username.c_str(), (PSID)pSid.data(), &cbSid, tszRefDomain, &cbRefDomain, &snu);

    DWORD dwReturnValue = GetLastError();
    if (dwReturnValue != ERROR_INSUFFICIENT_BUFFER)
        return dwReturnValue;

    dwReturnValue = ERROR_SUCCESS;

    pSid.resize(cbSid);
    cbRefDomain = 255;

    if (!LookupAccountNameW(NULL, username.c_str(), (PSID)pSid.data(), &cbSid, tszRefDomain, &cbRefDomain, &snu)) {
        dwReturnValue = GetLastError();
        return dwReturnValue;
    }

    return dwReturnValue;
}


/*---------------------------------------------------------------------------*\
 * NAME: ConstructWellKnownSID                                               *
 * --------------------------------------------------------------------------*
 * DESCRIPTION: This method converts some designated well-known identities   *
 * to a SID.                                                                 *
\*---------------------------------------------------------------------------*/
BOOL ConstructWellKnownSID(const std::wstring& username, /*out*/std::vector<BYTE>& pSid)
{
    // Look for well-known English names
    DWORD dwSubAuthority = 0;
    BOOL fUseWorldAuth = FALSE;
    if (_wcsicmp(username.c_str(), L"Administrators") == 0) {
        dwSubAuthority = DOMAIN_ALIAS_RID_ADMINS;
    } else if (_wcsicmp(username.c_str(), L"Power Users") == 0) {
        dwSubAuthority = DOMAIN_ALIAS_RID_POWER_USERS;
    } else if (_wcsicmp(username.c_str(), L"Everyone") == 0) {
        dwSubAuthority = SECURITY_WORLD_RID;
        fUseWorldAuth = TRUE;
    } else if (_wcsicmp(username.c_str(), L"System") == 0) {
        dwSubAuthority = SECURITY_LOCAL_SYSTEM_RID;
    } else if (_wcsicmp(username.c_str(), L"Self") == 0) {
        dwSubAuthority = SECURITY_PRINCIPAL_SELF_RID;
    } else if (_wcsicmp(username.c_str(), L"Anonymous") == 0) {
        dwSubAuthority = SECURITY_ANONYMOUS_LOGON_RID;
    } else if (_wcsicmp(username.c_str(), L"Interactive") == 0) {
        dwSubAuthority = SECURITY_INTERACTIVE_RID;
    } else {
        return FALSE;
    }

    PSID psidTemp = NULL;
    SID_IDENTIFIER_AUTHORITY SidAuthorityNT = SECURITY_NT_AUTHORITY;
    SID_IDENTIFIER_AUTHORITY SidAuthorityWorld = SECURITY_WORLD_SID_AUTHORITY;
    if (dwSubAuthority == DOMAIN_ALIAS_RID_ADMINS || dwSubAuthority == DOMAIN_ALIAS_RID_POWER_USERS) {
        if (!AllocateAndInitializeSid(
            &SidAuthorityNT,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            dwSubAuthority,
            0, 0, 0, 0, 0, 0,
            &psidTemp
        )) return FALSE;
    } else {
        if (!AllocateAndInitializeSid(
            fUseWorldAuth ? &SidAuthorityWorld : &SidAuthorityNT,
            1,
            dwSubAuthority,
            0, 0, 0, 0, 0, 0, 0,
            &psidTemp
        )) return FALSE;

    }

    if (!IsValidSid(psidTemp))
        return FALSE;
    
    BOOL fRetVal = FALSE;
    DWORD cbSid = GetLengthSid(psidTemp);
    pSid.resize(cbSid); // assign output buffer
    if (!CopySid(cbSid, pSid.data(), psidTemp)) {
        pSid.clear();
    } else {
        fRetVal = TRUE;
    }
    FreeSid(psidTemp);

    return fRetVal;
}
