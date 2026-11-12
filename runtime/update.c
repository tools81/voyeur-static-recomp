/* Update check at start-up.

   Asks GitHub for the latest release of UPDATE_REPO (version.h).  If its tag
   is newer than APP_VERSION and it has a "*-portable.zip" asset, the player is
   asked whether to update.  Yes: the zip is downloaded to saves/update/, a
   small PowerShell script waits for this process to exit, unpacks the zip
   over the program folder (saves/ is not in the zip, so saves, settings and
   the disc location are kept), and starts the program again.  No: that
   version is not offered again (saves/update_skip.txt).

   Skipped with --no-update-check, RECOMP_NO_UPDATE=1, headless and scripted
   runs, or when GitHub cannot be reached within a few seconds. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include "version.h"
#include "runtime.h"
#include "game_config.h"

int g_no_update_check;

static int http_get(const wchar_t *host, const wchar_t *path, char **out, size_t *outlen, size_t limit) {
    int ok = 0;
    *out = NULL; *outlen = 0;
    HINTERNET s = WinHttpOpen(L"cdi-recomp-updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"cdi-recomp-updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 3000, 3000, 4000, limit > 1000000 ? 60000 : 4000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r) {
        static const wchar_t hdr[] = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            if (code == 200) {
                size_t cap = 1 << 16, len = 0;
                char *buf = (char *)malloc(cap + 1);
                DWORD avail;
                ok = 1;
                while (buf && WinHttpQueryDataAvailable(r, &avail) && avail) {
                    if (len + avail > limit) { ok = 0; break; }
                    if (len + avail > cap) {
                        while (len + avail > cap) cap *= 2;
                        char *nb = (char *)realloc(buf, cap + 1);
                        if (!nb) { ok = 0; break; }
                        buf = nb;
                    }
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf + len, avail, &got)) { ok = 0; break; }
                    len += got;
                }
                if (ok && buf) { buf[len] = 0; *out = buf; *outlen = len; }
                else free(buf);
            }
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok && *out;
}

/* "v1.2.3" or "1.2.3" -> comparable number */
static long long ver_num(const char *v) {
    int a = 0, b = 0, c = 0;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return (long long)a * 1000000 + b * 1000 + c;
}

/* value of the first "key":"..." after p */
static int json_str(const char *p, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return 0;
    k = strchr(k + strlen(pat), '"');
    if (!k) return 0;
    k++;
    size_t i = 0;
    while (*k && *k != '"' && i + 1 < n) out[i++] = *k++;
    out[i] = 0;
    return *k == '"';
}

/* https://host/path -> host, path (wide) */
static int split_url(const char *url, wchar_t *host, size_t hn, wchar_t *path, size_t pn) {
    if (strncmp(url, "https://", 8)) return 0;
    const char *h = url + 8, *sl = strchr(h, '/');
    if (!sl) return 0;
    char hb[256];
    size_t L = (size_t)(sl - h);
    if (L >= sizeof hb) return 0;
    memcpy(hb, h, L); hb[L] = 0;
    MultiByteToWideChar(CP_UTF8, 0, hb, -1, host, (int)hn);
    MultiByteToWideChar(CP_UTF8, 0, sl, -1, path, (int)pn);
    return 1;
}

/* GitHub answers asset downloads with a redirect to another host: WinHTTP
   follows it on its own. */
static int download(const char *url, const char *dest) {
    wchar_t host[256], path[2048];
    if (!split_url(url, host, 256, path, 2048)) return 0;
    char *data; size_t len;
    if (!http_get(host, path, &data, &len, (size_t)1 << 30)) return 0;
    FILE *f = fopen(dest, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f) fclose(f);
    free(data);
    return ok;
}

void update_check(const char *exe_dir, const char *data_dir) {
    if (g_no_update_check || getenv("RECOMP_NO_UPDATE")) return;
    char *json; size_t len;
    if (!http_get(L"api.github.com", L"/repos/" UPDATE_REPO_W L"/releases/latest", &json, &len, 1 << 20)) {
        LOG("update check: no answer from GitHub");
        return;
    }
    char tag[64] = "", url[1024] = "";
    json_str(json, "tag_name", tag, sizeof tag);
    for (const char *p = json; (p = strstr(p, "\"browser_download_url\"")) != NULL; p++) {
        char u[1024];
        if (json_str(p, "browser_download_url", u, sizeof u)) {
            size_t L = strlen(u);
            if (L > 13 && !_stricmp(u + L - 13, "-portable.zip")) { snprintf(url, sizeof url, "%s", u); break; }
        }
    }
    free(json);
    LOG("update check: this is %s, latest release %s", APP_VERSION, tag[0] ? tag : "?");
    if (!tag[0] || !url[0] || ver_num(tag) <= ver_num(APP_VERSION)) return;

    char skip[600], line[64] = "";
    snprintf(skip, sizeof skip, "%s/update_skip.txt", data_dir);
    FILE *sf = fopen(skip, "r");
    if (sf) { if (fgets(line, sizeof line, sf)) line[strcspn(line, "\r\n")] = 0; fclose(sf); }
    if (!strcmp(line, tag)) return;

    char msg[512];
    snprintf(msg, sizeof msg,
             "A new version of " GAME_TITLE " (%s) is available. You have %s.\n\n"
             "Download and install it now? Your saves and settings are kept.\n\n"
             "(No: don't ask again for this version.)", tag, APP_VERSION);
    if (MessageBoxA(NULL, msg, GAME_TITLE " update", MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        sf = fopen(skip, "w");
        if (sf) { fprintf(sf, "%s\n", tag); fclose(sf); }
        return;
    }
    char udir[600], zip[700], ps1[700], self[600];
    snprintf(udir, sizeof udir, "%s\\update", data_dir);
    _mkdir(udir);
    snprintf(zip, sizeof zip, "%s\\update.zip", udir);
    if (!download(url, zip)) {
        MessageBoxA(NULL, "The update could not be downloaded. The game will start with the current version.",
                    GAME_TITLE " update", MB_ICONWARNING);
        return;
    }
    GetModuleFileNameA(NULL, self, sizeof self);
    snprintf(ps1, sizeof ps1, "%s\\apply_update.ps1", udir);
    FILE *f = fopen(ps1, "w");
    if (!f) return;
    fprintf(f,
            "param([int]$ProcId, [string]$Zip, [string]$Dest, [string]$Exe)\n"
            "try { Wait-Process -Id $ProcId -Timeout 60 -ErrorAction Stop } catch {}\n"
            "Start-Sleep -Milliseconds 500\n"
            "try {\n"
            "  Expand-Archive -LiteralPath $Zip -DestinationPath $Dest -Force -ErrorAction Stop\n"
            "  Remove-Item -LiteralPath $Zip -ErrorAction SilentlyContinue\n"
            "} catch {\n"
            "  Add-Type -AssemblyName PresentationFramework\n"
            "  [System.Windows.MessageBox]::Show(\"The update could not be installed: $($_.Exception.Message)\", '" GAME_TITLE " update') | Out-Null\n"
            "}\n"
            "Start-Process -FilePath $Exe -WorkingDirectory $Dest\n");
    fclose(f);
    char cmd[3000];
    snprintf(cmd, sizeof cmd,
             "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\" -ProcId %lu -Zip \"%s\" -Dest \"%s\" -Exe \"%s\"",
             ps1, (unsigned long)GetCurrentProcessId(), zip, exe_dir, self);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, exe_dir, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        LOG("update: installing %s and restarting", tag);
        exit(0);
    }
}
#else
void update_check(const char *exe_dir, const char *data_dir) { (void)exe_dir; (void)data_dir; }
#endif
