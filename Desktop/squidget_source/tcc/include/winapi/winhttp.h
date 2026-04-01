/* Minimal winhttp.h for TCC */
#ifndef _WINHTTP_H_
#define _WINHTTP_H_

#include <windows.h>

typedef LPVOID HINTERNET;
typedef WORD   INTERNET_PORT;
typedef int    INTERNET_SCHEME;

#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY       0
#define WINHTTP_NO_PROXY_NAME                   NULL
#define WINHTTP_NO_PROXY_BYPASS                 NULL
#define WINHTTP_FLAG_SECURE                     0x00800000
#define WINHTTP_NO_ADDITIONAL_HEADERS           NULL
#define WINHTTP_NO_REQUEST_DATA                 NULL
#define WINHTTP_NO_REFERER                      NULL
#define WINHTTP_DEFAULT_ACCEPT_TYPES            NULL
#define WINHTTP_ADDREQS_FLAG_ADD                0x20000000
#define WINHTTP_QUERY_STATUS_CODE               19
#define WINHTTP_QUERY_FLAG_NUMBER               0x20000000
#define WINHTTP_OPTION_SECURITY_FLAGS           31
#define WINHTTP_OPTION_CONNECT_TIMEOUT          3
#define WINHTTP_OPTION_SEND_TIMEOUT             5
#define WINHTTP_OPTION_RECEIVE_TIMEOUT          6
#define WINHTTP_OPTION_REDIRECT_POLICY          88
#define WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS   2
#define SECURITY_FLAG_IGNORE_UNKNOWN_CA         0x00000100
#define SECURITY_FLAG_IGNORE_CERT_CN_INVALID    0x00001000
#define SECURITY_FLAG_IGNORE_CERT_DATE_INVALID  0x00002000
#define SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE   0x00000200
#define ICU_ESCAPE                              0x80000000
#define ICU_DECODE                              0x10000000
#define WINHTTP_DEFAULT_PORT                    0
#define INTERNET_SCHEME_HTTP                    1
#define INTERNET_SCHEME_HTTPS                   2

typedef struct {
    DWORD          dwStructSize;
    LPWSTR         lpszScheme;
    DWORD          dwSchemeLength;
    INTERNET_SCHEME nScheme;
    LPWSTR         lpszHostName;
    DWORD          dwHostNameLength;
    INTERNET_PORT  nPort;
    LPWSTR         lpszUserName;
    DWORD          dwUserNameLength;
    LPWSTR         lpszPassword;
    DWORD          dwPasswordLength;
    LPWSTR         lpszUrlPath;
    DWORD          dwUrlPathLength;
    LPWSTR         lpszExtraInfo;
    DWORD          dwExtraInfoLength;
} URL_COMPONENTS, *LPURL_COMPONENTS;

HINTERNET WINAPI WinHttpOpen(LPCWSTR,DWORD,LPCWSTR,LPCWSTR,DWORD);
HINTERNET WINAPI WinHttpConnect(HINTERNET,LPCWSTR,INTERNET_PORT,DWORD);
HINTERNET WINAPI WinHttpOpenRequest(HINTERNET,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR*,DWORD);
BOOL      WINAPI WinHttpSendRequest(HINTERNET,LPCWSTR,DWORD,LPVOID,DWORD,DWORD,DWORD_PTR);
BOOL      WINAPI WinHttpReceiveResponse(HINTERNET,LPVOID);
BOOL      WINAPI WinHttpQueryHeaders(HINTERNET,DWORD,LPCWSTR,LPVOID,LPDWORD,LPDWORD);
BOOL      WINAPI WinHttpQueryDataAvailable(HINTERNET,LPDWORD);
BOOL      WINAPI WinHttpReadData(HINTERNET,LPVOID,DWORD,LPDWORD);
BOOL      WINAPI WinHttpCloseHandle(HINTERNET);
BOOL      WINAPI WinHttpSetOption(HINTERNET,DWORD,LPVOID,DWORD);
BOOL      WINAPI WinHttpCrackUrl(LPCWSTR,DWORD,DWORD,LPURL_COMPONENTS);

#endif
