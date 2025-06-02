/*
    winview.cpp
    =======================
    버전 1.4.3f-update (Registry & Theme 포함) - [2025-05-31]
      - 타이틀바 제거, 창 내용 드래그 이동, 드롭다운 위치(위쪽) 조정
      - 콤보박스의 선택 상태 보존 및 각 항목(윈도우 타이틀)을 주기적으로 업데이트
      - ComboSubclassProc, ListSubclassProc 콜백에 extern "C" 명시 (이름 맹글링 문제 해결)
      - **수정:** WM_TIMER에서 미리보기 대상 창의 썸네일 destination rectangle 변경 시, 
                잔여 이미지가 남는 문제를 해결하기 위해 먼저 fVisible=FALSE로 업데이트한 후 
                fVisible=TRUE로 재노출 처리 적용.
*/
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tchar.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <wchar.h>
#include <cmath> // std::round를 사용하기 위해 추가
#include <uxtheme.h>
#include "resource.h"

//=============================================================================
// 매크로 및 상수 정의
//=============================================================================
#define MAX_SEGMENTS 32              // 최대 미리보기 창 개수
#define IDC_COMBO1   101             // 드롭다운(콤보박스) 시작 ID (이후 IDC_COMBO1+i)
#define ID_TIMER     1               // 타이머 식별자
#define NUM_SEGMENTS_DEFAULT 3       // 초기 미리보기 창 개수

// 컨텍스트 메뉴 항목 ID
#define IDM_ALWAYS_ON_TOP    40001
#define IDM_RUN_AT_STARTUP   40002
#define IDM_INITIALIZE       40003  // "초기화 후 종료"
#define IDM_EXIT             40004
#define IDM_ADD_PREVIEW      40005
#define IDM_REMOVE_PREVIEW   40006

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// 클라이언트 영역 크기 (드롭다운 영역 + 미리보기 영역)
const int DROP_HEIGHT = 25;               // 드롭다운 영역 높이
const int PREVIEW_HEIGHT = 300;           // 미리보기 영역 높이
const int TOTAL_HEIGHT = DROP_HEIGHT + PREVIEW_HEIGHT; // 325

//=============================================================================
// 전역 변수
//=============================================================================
int g_numSegments = NUM_SEGMENTS_DEFAULT;    // 현재 미리보기 창 개수
int g_maxSegments = 0;                         // 화면 너비 대비 최대 미리보기 창 개수
int g_windowWidth = 0;                         // 미리보기 영역 전체 누적 가로폭 (클라이언트 기준)
const int g_windowHeight = TOTAL_HEIGHT;       // 325

HWND g_ComboBoxes[MAX_SEGMENTS] = { NULL };
HWND g_Selected[MAX_SEGMENTS]   = { NULL };  // 각 콤보박스에 선택된 창 핸들
HBITMAP g_Bitmaps[MAX_SEGMENTS]    = { NULL };
HTHUMBNAIL g_Thumbnails[MAX_SEGMENTS] = { NULL };

bool g_alwaysOnTop = 0; // false = 0, true = 1
bool g_runAtStartup = false;
HINSTANCE g_hInst = NULL;
// 초기화(Reset) 요청 플래그: Reset 명령 실행 시 true로 설정
bool g_resetRequested = false;
// 드롭다운 팝업 활성 상태 플래그
bool g_dropdownActive = false;

const TCHAR* g_excludedSubstrings[] = { _T("설정"), _T("Windows 입력"), _T("팝업 호스트"), _T("GeForce Overlay"), _T("위젯"), _T("작업 전환") };
const size_t g_excludedCount = sizeof(g_excludedSubstrings) / sizeof(g_excludedSubstrings[0]);
const bool g_excludeOwnerWindows = false;

HFONT g_hFont = CreateFont(18, 0, 0, 0,
                     FW_NORMAL, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,  // 부드러운 렌더링
                     DEFAULT_PITCH | FF_DONTCARE,
                     L"맑은 고딕");

//=============================================================================
// 함수 프로토타입
//=============================================================================
void SaveSettings(HWND hWnd);
void LoadSettings(HWND hWnd);
void SetRunAtStartup(bool enable);
void LoadRunAtStartup();         // 부팅시 실행 옵션 로드
void LoadStartupSettings();      // 미리보기 창 갯수(PreviewCount) 로드
void ResetRegistrySettings();
void RecreatePreviews(HWND hWnd);
void UpdateComboBoxItem(HWND hCombo);
extern "C" LRESULT CALLBACK ComboSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
extern "C" LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
HBITMAP CaptureWindow(HWND hwnd);
void ShowContextMenu(HWND hWnd);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

//=============================================================================
// 레지스트리 관련 함수 (저장/불러오기/초기화, 부팅시 실행)
//=============================================================================
void LoadRunAtStartup()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, 
       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        DWORD dwType;
        DWORD dwSize = 0;
        if (RegQueryValueEx(hKey, L"MultiWindowViewer", NULL, &dwType, NULL, &dwSize) == ERROR_SUCCESS)
            g_runAtStartup = true;
        else
            g_runAtStartup = false;
        RegCloseKey(hKey);
    }
    else
    {
        g_runAtStartup = false;
    }
}

void LoadStartupSettings()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\MultiWindowViewer", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        DWORD dwPreview = 0;
        DWORD dwSize = sizeof(dwPreview);
        DWORD dwType = 0;
        if (RegQueryValueEx(hKey, L"PreviewCount", NULL, &dwType, (LPBYTE)&dwPreview, &dwSize) == ERROR_SUCCESS)
        {
            if (dwPreview > 0 && dwPreview <= MAX_SEGMENTS)
                g_numSegments = (int)dwPreview;
        }
        RegCloseKey(hKey);
    }
}

void SaveSettings(HWND hWnd)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\MultiWindowViewer", 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RECT rc;
        if (GetWindowRect(hWnd, &rc))
        {
            DWORD dwVal = rc.left;
            RegSetValueEx(hKey, L"WindowLeft", 0, REG_DWORD, (const BYTE*)&dwVal, sizeof(dwVal));
            dwVal = rc.top;
            RegSetValueEx(hKey, L"WindowTop", 0, REG_DWORD, (const BYTE*)&dwVal, sizeof(dwVal));
        }
        DWORD dwAlways = (DWORD)g_alwaysOnTop;
        RegSetValueEx(hKey, L"AlwaysOnTop", 0, REG_DWORD, (const BYTE*)&dwAlways, sizeof(dwAlways));
        DWORD dwPreview = (DWORD)g_numSegments;
        RegSetValueEx(hKey, L"PreviewCount", 0, REG_DWORD, (const BYTE*)&dwPreview, sizeof(dwPreview));
        RegCloseKey(hKey);
    }
}

void LoadSettings(HWND hWnd)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\MultiWindowViewer", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        RECT rc = {0};
        DWORD dwType = 0, dwSize = sizeof(rc.left);
        RegQueryValueEx(hKey, L"WindowLeft", NULL, &dwType, (LPBYTE)&rc.left, &dwSize);
        dwSize = sizeof(rc.top);
        RegQueryValueEx(hKey, L"WindowTop", NULL, &dwType, (LPBYTE)&rc.top, &dwSize);
        // 여기서는 위치만 지정. 크기는 g_windowWidth, g_windowHeight로 결정
        SetWindowPos(hWnd, NULL, rc.left, rc.top, g_windowWidth, g_windowHeight, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        DWORD dwAlways = 0;
        dwSize = sizeof(dwAlways);
        if (RegQueryValueEx(hKey, L"AlwaysOnTop", NULL, &dwType, (LPBYTE)&dwAlways, &dwSize) == ERROR_SUCCESS)
        {
            g_alwaysOnTop = (bool)dwAlways;
        }
        RegCloseKey(hKey);
    }
}

void SetRunAtStartup(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        if (enable)
        {
            wchar_t szPath[MAX_PATH] = {0};
            GetModuleFileName(NULL, szPath, MAX_PATH);
            RegSetValueEx(hKey, L"MultiWindowViewer", 0, REG_SZ,
                          (const BYTE*)szPath, (wcslen(szPath)+1)*sizeof(wchar_t));
        }
        else
        {
            RegDeleteValue(hKey, L"MultiWindowViewer");
        }
        RegCloseKey(hKey);
    }
}

void ResetRegistrySettings()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software", 0, KEY_WRITE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteKeyEx(hKey, L"MultiWindowViewer", KEY_WOW64_64KEY, 0);
        RegCloseKey(hKey);
    }
}

//=============================================================================
// UpdateComboBoxItem: 각 콤보박스의 항목(윈도우 타이틀)을 업데이트
//=============================================================================
void UpdateComboBoxItem(HWND hCombo)
{
    int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
    for (int index = 0; index < count; index++) {
        HWND hwnd = (HWND)SendMessage(hCombo, CB_GETITEMDATA, index, 0);
        if (hwnd && IsWindow(hwnd)) {
            wchar_t newTitle[256] = {0}, currTitle[256] = {0};
            GetWindowText(hwnd, newTitle, 256);
            SendMessage(hCombo, CB_GETLBTEXT, index, (LPARAM)currTitle);

            // 🚀 추가: 빈 제목이 반환되면 기존 제목을 유지함
            if (wcslen(newTitle) > 0 && wcscmp(newTitle, currTitle) != 0) {
                int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                SendMessage(hCombo, CB_DELETESTRING, index, 0);
                int newIndex = (int)SendMessage(hCombo, CB_INSERTSTRING, index, (LPARAM)newTitle);
                SendMessage(hCombo, CB_SETITEMDATA, newIndex, (LPARAM)hwnd);
                if (sel == index) {
                    SendMessage(hCombo, CB_SETCURSEL, newIndex, 0);
                }
            }
        } else {
            SendMessage(hCombo, CB_DELETESTRING, index, 0);
            index--; count--;
        }
    }
}

//=============================================================================
// RecreatePreviews: 콤보박스(드롭다운) 컨트롤들을 새로 생성하며, 기존 선택 상태 보존 및 목록 재채우기
//=============================================================================
void RecreatePreviews(HWND hWnd)
{
    // 1. 기존 선택된 창 핸들을 임시 저장
    HWND tempSelected[MAX_SEGMENTS] = { NULL };
    for (int i = 0; i < g_numSegments; i++) {
        tempSelected[i] = g_Selected[i];
    }
    
    // 2. 기존 콤보박스 제거 및 선택 상태 초기화
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (g_ComboBoxes[i]) {
            RemoveWindowSubclass(g_ComboBoxes[i], ComboSubclassProc, i);
            DestroyWindow(g_ComboBoxes[i]);
            g_ComboBoxes[i] = NULL;
        }
        g_Selected[i] = NULL; // 명확히 초기화
    }
    
    int comboHeight = DROP_HEIGHT;
    int defaultPreviewWidth = (PREVIEW_HEIGHT * 16) / 9;
    
    // 3. 새로운 콤보박스 생성 (g_numSegments 개 만큼)
    for (int i = 0; i < g_numSegments; i++) {
        int x = i * defaultPreviewWidth;
        g_ComboBoxes[i] = CreateWindowEx(0, TEXT("COMBOBOX"), NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            x, 0, defaultPreviewWidth, comboHeight,
            hWnd, (HMENU)(INT_PTR)(IDC_COMBO1 + i),
            g_hInst, NULL);
        SetWindowSubclass(g_ComboBoxes[i], ComboSubclassProc, i, 0);
        SendMessage(g_ComboBoxes[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        // 필요에 따라 SetWindowTheme()도 적용 (테마 관련 처리)
        SetWindowTheme(g_ComboBoxes[i], L"", L"");
        // 초기에는 g_Selected에 임시 저장한 값 복원 (이후 EnumWindowsProc에서 항목이 채워짐)
        g_Selected[i] = tempSelected[i];
    }
    
    // 4. EnumWindows로 현재 실행 중인 창들을 각 콤보박스에 추가/업데이트
    EnumWindows(EnumWindowsProc, (LPARAM)hWnd);
    
    // 5. 저장된 선택 상태 복원: 각 콤보박스에서 tempSelected[i]와 일치하는 항목을 찾아 선택
    for (int i = 0; i < g_numSegments; i++) {
        if (g_ComboBoxes[i]) {
            int count = (int)SendMessage(g_ComboBoxes[i], CB_GETCOUNT, 0, 0);
            int selIndex = CB_ERR;
            // 만약 임시 저장된 선택 값이 있다면
            if (tempSelected[i] != NULL) {
                for (int j = 0; j < count; j++) {
                    HWND h = (HWND)SendMessage(g_ComboBoxes[i], CB_GETITEMDATA, j, 0);
                    if (h == tempSelected[i]) {
                        selIndex = j;
                        break;
                    }
                }
            }
            if (selIndex != CB_ERR) {
                SendMessage(g_ComboBoxes[i], CB_SETCURSEL, selIndex, 0);
                g_Selected[i] = tempSelected[i];
            } else {
                // 이전 선택 항목이 존재하지 않는다면 선택을 해제 (또는 원하는 기본 선택 동작 적용)
                SendMessage(g_ComboBoxes[i], CB_SETCURSEL, -1, 0);
                g_Selected[i] = NULL;
                // 만약 썸네일 핸들이 남아 있다면 해제
                if (g_Thumbnails[i]) {
                    DwmUnregisterThumbnail(g_Thumbnails[i]);
                    g_Thumbnails[i] = NULL;
                }
            }
        }
    }
}
//=============================================================================
// ComboSubclassProc: 콤보박스 서브클래스 콜백 (extern "C")
//=============================================================================
extern "C" LRESULT CALLBACK ComboSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
        case WM_MOUSEWHEEL:
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int curSel = (int)SendMessage(hWnd, CB_GETCURSEL, 0, 0);
            int count = (int)SendMessage(hWnd, CB_GETCOUNT, 0, 0);
            if (delta > 0 && curSel > 0)
            {
                curSel--;
                SendMessage(hWnd, CB_SETCURSEL, curSel, 0);
                SendMessage(GetParent(hWnd), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hWnd), CBN_SELCHANGE), (LPARAM)hWnd);
            }
            else if (delta < 0 && curSel < count - 1)
            {
                curSel++;
                SendMessage(hWnd, CB_SETCURSEL, curSel, 0);
                SendMessage(GetParent(hWnd), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hWnd), CBN_SELCHANGE), (LPARAM)hWnd);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

//=============================================================================
// ListSubclassProc: 드롭다운 리스트 서브클래스 콜백 (extern "C")
//=============================================================================
extern "C" LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
        case WM_MOUSEWHEEL:
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            HWND hCombo = (HWND)dwRefData;
            int curSel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
            int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
            if (delta > 0 && curSel > 0)
            {
                curSel--;
                SendMessage(hCombo, CB_SETCURSEL, curSel, 0);
                SendMessage(GetParent(hCombo), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hCombo), CBN_SELCHANGE), (LPARAM)hCombo);
            }
            else if (delta < 0 && curSel < count - 1)
            {
                curSel++;
                SendMessage(hCombo, CB_SETCURSEL, curSel, 0);
                SendMessage(GetParent(hCombo), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hCombo), CBN_SELCHANGE), (LPARAM)hCombo);
            }
            return 0;
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

//=============================================================================
// EnumWindowsProc: 실행 중인 창들을 열거하여 각 콤보박스에 항목 추가/업데이트
//=============================================================================
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    HWND hMain = (HWND)lParam;
    if (hwnd == hMain)
        return TRUE;
    if (g_excludeOwnerWindows && (GetWindow(hwnd, GW_OWNER) != NULL))
        return TRUE;
    //if (GetWindowLong(hwnd, GWL_EXSTYLE))
    //if (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    //    return TRUE;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    TCHAR title[256];
    GetWindowText(hwnd, title, 256);
    if (_tcslen(title) == 0)
        return TRUE;
    for (size_t i = 0; i < g_excludedCount; i++) {
        if (_tcsstr(title, g_excludedSubstrings[i]) != NULL)
            return TRUE;
    }

    for (int i = 0; i < g_numSegments; i++) {
        if (!g_ComboBoxes[i]) continue;
        bool found = false;
        int count = (int)SendMessage(g_ComboBoxes[i], CB_GETCOUNT, 0, 0);
        for (int j = 0; j < count; j++) {
            HWND hwndStored = (HWND)SendMessage(g_ComboBoxes[i], CB_GETITEMDATA, j, 0);
            if (hwndStored == hwnd) {
                found = true;
                break;
            }
        }
        if (!found) {
            // 🚀 추가: 빈 제목이면 항목 추가하지 않음
            if (_tcslen(title) > 0) {
                int index = (int)SendMessage(g_ComboBoxes[i], CB_ADDSTRING, 0, (LPARAM)title);
                SendMessage(g_ComboBoxes[i], CB_SETITEMDATA, index, (LPARAM)hwnd);
            }
        }
    }
    return TRUE;
}

//=============================================================================
// CaptureWindow: 선택한 창의 클라이언트 영역 캡처 (fallback)
//=============================================================================
HBITMAP CaptureWindow(HWND hwnd)
{
    HDC hdcScreen = GetDC(NULL);
    HDC hdcWindow = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    RECT rect;
    if (!GetClientRect(hwnd, &rect))
    {
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    if (!hBitmap)
    {
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }
    SelectObject(hdcMem, hBitmap);
    BOOL bSuccess = BitBlt(hdcMem, 0, 0, width, height, hdcWindow, 0, 0, SRCCOPY);
    if (!bSuccess)
    {
        if (!PrintWindow(hwnd, hdcMem, 0))
        {
            DeleteObject(hBitmap);
            hBitmap = NULL;
        }
    }
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);
    ReleaseDC(NULL, hdcScreen);
    return hBitmap;
}

//=============================================================================
// HandleDoubleClick: 미리보기 영역 더블클릭 시 해당 창 활성화
//=============================================================================
LRESULT HandleDoubleClick(HWND hWnd, LPARAM lParam)
{
    POINT pt;
    pt.x = LOWORD(lParam);
    pt.y = HIWORD(lParam);
    if (pt.y < DROP_HEIGHT || pt.y > DROP_HEIGHT + PREVIEW_HEIGHT)
        return 0;
    int cumulativeWidth = 0, indexFound = -1;
    for (int i = 0; i < g_numSegments; i++)
    {
        //int previewWidth = 0;
        int slotWidth = 0; // 각 미리보기 슬롯의 너비
        if (g_Selected[i])
        {
            SIZE srcSize = {0};
            // HandleDoubleClick에서는 썸네일이 아직 등록되지 않았을 수 있으므로 g_Thumbnails[i] 검사는 WM_TIMER에 비해서는 덜 중요.
            // 하지만 정확한 srcSize를 얻기 위해 DwmQueryThumbnailSourceSize를 시도하는 것이 가장 좋음.
            if (g_Thumbnails[i] && SUCCEEDED(DwmQueryThumbnailSourceSize(g_Thumbnails[i], &srcSize)) && srcSize.cy > 0)
            {
                // WM_TIMER와 동일한 확대 방지 로직 적용
                if (srcSize.cy <= PREVIEW_HEIGHT)
                {
                    slotWidth = srcSize.cx; // 확대하지 않음
                }
                else
                {
                    double scale = (double)PREVIEW_HEIGHT / srcSize.cy;
                    slotWidth = (int)std::round(srcSize.cx * scale); // 축소
                }
            }
            else
            {
                slotWidth = (PREVIEW_HEIGHT * 16) / 9;
            }
        }
        else
        {
            slotWidth = (PREVIEW_HEIGHT * 16) / 9;
        }
        if (pt.x >= cumulativeWidth && pt.x < cumulativeWidth + slotWidth)
        {
            indexFound = i;
            break;
        }
        cumulativeWidth += slotWidth;
    }
    if (indexFound != -1 && g_Selected[indexFound] && IsWindow(g_Selected[indexFound]))
    {
        if (IsIconic(g_Selected[indexFound]))
        {
            ShowWindow(g_Selected[indexFound], SW_RESTORE);
        }
        // 활성화 순서 개선: 먼저 창을 전면으로 가져오고, 활성창으로 설정
        BringWindowToTop(g_Selected[indexFound]);
        SetForegroundWindow(g_Selected[indexFound]);
        SetActiveWindow(g_Selected[indexFound]);
        // 또는, 이 대신 SwitchToThisWindow()를 사용할 수도 있음 (단, 비공식 API)
        // SwitchToThisWindow(g_Selected[indexFound], TRUE);
    }
    return 0;
}

//=============================================================================
// ShowContextMenu: 우클릭 컨텍스트 메뉴 ("항상 위에", "부팅시 실행", "초기화 후 종료", "창+1", "창-1", "종료")
//=============================================================================
void ShowContextMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING | (g_alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, L"항상 위에");
    AppendMenu(hMenu, MF_STRING | (g_runAtStartup ? MF_CHECKED : 0), IDM_RUN_AT_STARTUP, L"부팅시 실행");
    AppendMenu(hMenu, MF_STRING, IDM_INITIALIZE, L"초기화 후 종료");
    AppendMenu(hMenu, MF_STRING, IDM_ADD_PREVIEW, L"창+1");
    AppendMenu(hMenu, MF_STRING, IDM_REMOVE_PREVIEW, L"창-1");
    AppendMenu(hMenu, MF_STRING, IDM_EXIT, L"종료");
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

//=============================================================================
// WndProc: 메인 윈도우 프로시저
//=============================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
        {
            INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES };
            InitCommonControlsEx(&icex);
            g_hInst = ((LPCREATESTRUCT)lParam)->hInstance;
            /*
            HMODULE hShell32 = LoadLibrary(L"shell32.dll");
            if (hShell32)
            {
                HICON hIconSmall = ExtractIcon(g_hInst, L"shell32.dll", 34);
                HICON hIconLarge = ExtractIcon(g_hInst, L"shell32.dll", 34);
                if (hIconSmall != (HICON)1 && hIconLarge != (HICON)1)
                {
                    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
                    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconLarge);
                }
                FreeLibrary(hShell32);
            }
            */
            HICON hIconSmall = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
            HICON hIconLarge = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
            
            if (hIconSmall) {
                SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
            }
            if (hIconLarge) {
                SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconLarge);
            }
            LoadSettings(hWnd);
            LoadRunAtStartup();
            RECT rc;
            if (GetWindowRect(hWnd, &rc))
            {
                SetWindowPos(hWnd, NULL, rc.left, rc.top, g_windowWidth, g_windowHeight,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RecreatePreviews(hWnd);
            SetTimer(hWnd, ID_TIMER, 500, NULL);
        }
        break;
        
        case WM_LBUTTONDOWN:
        {
            ReleaseCapture();
            SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        break;
        
        case WM_LBUTTONDBLCLK:
            return HandleDoubleClick(hWnd, lParam);
            
        case WM_ERASEBKGND:
            return 1;
            
        case WM_RBUTTONUP:
            ShowContextMenu(hWnd);
            break;
        
        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if ((id >= IDC_COMBO1 && id < IDC_COMBO1 + g_numSegments))
            {
                if (code == CBN_DROPDOWN)
                {
                    g_dropdownActive = true;
                    LONG style = GetWindowLong(hWnd, GWL_STYLE);
                    SetWindowLong(hWnd, GWL_STYLE, style & ~WS_CLIPCHILDREN);
                    
                    int index = id - IDC_COMBO1;
                    HWND hCombo = g_ComboBoxes[index];
                    COMBOBOXINFO cbi = {0};
                    cbi.cbSize = sizeof(cbi);
                    if (GetComboBoxInfo(hCombo, &cbi) && cbi.hwndList)
                    {
                        SetWindowTheme(cbi.hwndList, L"", L"");
                        SendMessage(cbi.hwndList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
                        RECT rcCombo;
                        GetWindowRect(hCombo, &rcCombo);
                        int comboWidth = rcCombo.right - rcCombo.left;
                        const int LIST_HEIGHT = 200;
                        SetWindowPos(cbi.hwndList, NULL,
                                     rcCombo.left, rcCombo.top - LIST_HEIGHT,
                                     comboWidth, LIST_HEIGHT,
                                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                        SendMessage(hCombo, CB_SETDROPPEDWIDTH, (WPARAM)comboWidth, 0);
                    }
                }
                else if (code == CBN_CLOSEUP)
                {
                    g_dropdownActive = false;
                    LONG style = GetWindowLong(hWnd, GWL_STYLE);
                    SetWindowLong(hWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
                }
                else if (code == CBN_SELCHANGE)
                {
                    int index = id - IDC_COMBO1;
                    HWND hCombo = g_ComboBoxes[index];
                    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                    if (sel != CB_ERR)
                    {
                        HWND hwndTarget = (HWND)SendMessage(hCombo, CB_GETITEMDATA, sel, 0);
                        g_Selected[index] = hwndTarget;
                        if (g_Thumbnails[index])
                        {
                            DwmUnregisterThumbnail(g_Thumbnails[index]);
                            g_Thumbnails[index] = NULL;
                        }
                    }
                }
            }
            
            if (id == IDM_ALWAYS_ON_TOP)
            {
                g_alwaysOnTop = !g_alwaysOnTop;
                SetWindowPos(hWnd, (g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST),
                             0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            else if (id == IDM_RUN_AT_STARTUP)
            {
                g_runAtStartup = !g_runAtStartup;
                SetRunAtStartup(g_runAtStartup);
            }
            else if (id == IDM_INITIALIZE)
            {
                g_resetRequested = true;
                g_alwaysOnTop = false;
                g_runAtStartup = false;
                g_numSegments = NUM_SEGMENTS_DEFAULT;
                int previewWidth = (PREVIEW_HEIGHT * 16) / 9;
                g_windowWidth = g_numSegments * previewWidth;
                SetWindowPos(hWnd, NULL, CW_USEDEFAULT, CW_USEDEFAULT, g_windowWidth, g_windowHeight,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                ResetRegistrySettings();
                {
                    HKEY hRunKey;
                    if (RegOpenKeyEx(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hRunKey) == ERROR_SUCCESS)
                    {
                        RegDeleteValue(hRunKey, L"MultiWindowViewer");
                        RegCloseKey(hRunKey);
                    }
                }
                DestroyWindow(hWnd);
            }
            else if (id == IDM_ADD_PREVIEW)
            {
                if (g_numSegments < g_maxSegments)
                {
                    SendMessage(hWnd, WM_SETREDRAW, FALSE, 0);
                    g_numSegments++;
                    RecreatePreviews(hWnd);
                    SendMessage(hWnd, WM_SETREDRAW, TRUE, 0);
                    RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
                }
                else
                {
                    wchar_t buf[128];
                    wsprintf(buf, L"최대 창의 갯수는 %d개 입니다.", g_maxSegments);
                    MessageBox(hWnd, buf, L"알림", MB_OK | MB_ICONINFORMATION);
                }
            }
            else if (id == IDM_REMOVE_PREVIEW)
            {
                if (g_numSegments > 1)
                {
                    SendMessage(hWnd, WM_SETREDRAW, FALSE, 0);
                    g_numSegments--;
                    RecreatePreviews(hWnd);
                    SendMessage(hWnd, WM_SETREDRAW, TRUE, 0);
                    RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
                }
                else
                {
                    MessageBox(hWnd, L"최소 창의 갯수는 1개 입니다.", L"알림", MB_OK | MB_ICONINFORMATION);
                }
            }
            else if (id == IDM_EXIT)
            {
                DestroyWindow(hWnd);
            }
        }
        break;
        
        case WM_TIMER:
        {
            if (!g_dropdownActive)
            {
                int cumulativeWidth = 0;
                int newWidths[MAX_SEGMENTS] = {0};
                RECT destRect;
                // 창 목록 업데이트
                EnumWindows(EnumWindowsProc, (LPARAM)hWnd);
                for (int i = 0; i < g_numSegments; i++)
                {
                    int currentPreviewWidth = 0;   // 실제 썸네일이 그려질 너비
                    int currentPreviewHeight = 0;  // 실제 썸네일이 그려질 높이
                    //int previewWidth = 0;
                    if (g_Selected[i])
                    {
                        SIZE srcSize = {0};
                        if (!g_Thumbnails[i])
                        {
                            HRESULT hr = DwmRegisterThumbnail(hWnd, g_Selected[i], &g_Thumbnails[i]);
                        }
                        if (g_Thumbnails[i] && SUCCEEDED(DwmQueryThumbnailSourceSize(g_Thumbnails[i], &srcSize)) && srcSize.cy > 0)
                        {
                            // 원본 창의 높이가 PREVIEW_HEIGHT보다 작으면 확대하지 않습니다.
                            if (srcSize.cy <= PREVIEW_HEIGHT)
                            {
                                currentPreviewHeight = srcSize.cy;
                                currentPreviewWidth = srcSize.cx;
                            }
                            else // 원본 창의 높이가 PREVIEW_HEIGHT보다 크면 축소합니다.
                            {
                                double scale = (double)PREVIEW_HEIGHT / srcSize.cy;
                                currentPreviewHeight = PREVIEW_HEIGHT;
                                currentPreviewWidth = (int)std::round(srcSize.cx * scale);
                            }
                        }
                        else
                        {
                            // DWM 썸네일 사용 불가 시 기본 16:9 비율 및 PREVIEW_HEIGHT 사용
                            currentPreviewHeight = PREVIEW_HEIGHT;
                            currentPreviewWidth = (PREVIEW_HEIGHT * 16) / 9;
                        }
                    }
                    else
                    {
                        // 선택된 창이 없을 때의 기본 크기
                        currentPreviewHeight = PREVIEW_HEIGHT;
                        currentPreviewWidth = (PREVIEW_HEIGHT * 16) / 9;
                    }
                    //newWidths[i] = previewWidth;
                    newWidths[i] = currentPreviewWidth; // 이 배열은 콤보박스 너비 조절에 사용됨                    
                    destRect.left = cumulativeWidth;
                    destRect.top = DROP_HEIGHT;
                    destRect.right = cumulativeWidth + currentPreviewWidth; // 실제 썸네일 너비 적용
                    destRect.bottom = DROP_HEIGHT + currentPreviewHeight;   // 실제 썸네일 높이 적용
                    
                    // --- 수정된 부분: Thumbnail 업데이트 시 잔여 이미지 제거를 위해 먼저 숨김 처리 후 재노출
                    if (g_Thumbnails[i])
                    {
                        DWM_THUMBNAIL_PROPERTIES propsHide = {0};
                        propsHide.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
                        propsHide.fVisible = FALSE;
                        propsHide.rcDestination = destRect;
                        DwmUpdateThumbnailProperties(g_Thumbnails[i], &propsHide);
                        
                        DWM_THUMBNAIL_PROPERTIES propsShow = {0};
                        propsShow.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                                            DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
                        propsShow.fVisible = TRUE;
                        propsShow.fSourceClientAreaOnly = TRUE;
                        propsShow.opacity = 255;
                        propsShow.rcDestination = destRect;
                        DwmUpdateThumbnailProperties(g_Thumbnails[i], &propsShow);
                    }
                    cumulativeWidth += currentPreviewWidth;
                }
                if (cumulativeWidth != g_windowWidth)
                {
                    g_windowWidth = cumulativeWidth;
                    RECT rcClient = {0, 0, g_windowWidth, g_windowHeight};
                    AdjustWindowRect(&rcClient, GetWindowLong(hWnd, GWL_STYLE), FALSE);
                    int newW = rcClient.right - rcClient.left;
                    int newH = rcClient.bottom - rcClient.top;
                    RECT rc;
                    GetWindowRect(hWnd, &rc);
                    SetWindowPos(hWnd, NULL, rc.left, rc.top, newW, newH,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                int cumulativeX = 0;
                for (int i = 0; i < g_numSegments; i++)
                {
                    if (g_ComboBoxes[i])
                    {
                        MoveWindow(g_ComboBoxes[i], cumulativeX, 0, newWidths[i], DROP_HEIGHT, TRUE);
                        SendMessage(g_ComboBoxes[i], CB_SETDROPPEDWIDTH, (WPARAM)newWidths[i], 0);
                    }
                    cumulativeX += newWidths[i];
                }
                for (int i = 0; i < g_numSegments; i++)
                {
                    if (g_ComboBoxes[i])
                    {
                        UpdateComboBoxItem(g_ComboBoxes[i]);
                    }
                }
            }
            InvalidateRect(hWnd, NULL, TRUE);
        }
        break;
        
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBMP = CreateCompatibleBitmap(hdc, g_windowWidth, g_windowHeight);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBMP);
            RECT rc;
            SetRect(&rc, 0, 0, g_windowWidth, g_windowHeight);
            FillRect(memDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetStretchBltMode(memDC, HALFTONE);
            BitBlt(hdc, 0, 0, g_windowWidth, g_windowHeight, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBMP);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
        }
        break;
        
        case WM_DESTROY:
        {
            KillTimer(hWnd, ID_TIMER);
            if (!g_resetRequested)
            {
                SaveSettings(hWnd);
            }
            for (int i = 0; i < MAX_SEGMENTS; i++)
            {
                if (g_Thumbnails[i])
                {
                    DwmUnregisterThumbnail(g_Thumbnails[i]);
                    g_Thumbnails[i] = NULL;
                }
                if (g_Bitmaps[i])
                {
                    DeleteObject(g_Bitmaps[i]);
                    g_Bitmaps[i] = NULL;
                }
            }
            DeleteObject(g_hFont);
            PostQuitMessage(0);
        }
        break;
        
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

//=============================================================================
// wWinMain: 프로그램 진입점
// - 클라이언트 영역 높이: DROP_HEIGHT + PREVIEW_HEIGHT = 325px
// - 전체 가로폭은 모든 미리보기 창의 누적 폭 (초기 g_windowWidth는 미리보기 개수 * 기본PreviewWidth)
// - 타이틀바 제거(WS_POPUP) 및 창 내용 드래그로 이동
//=============================================================================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    int defaultPreviewWidth = (PREVIEW_HEIGHT * 16) / 9;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_maxSegments = screenWidth / defaultPreviewWidth;
    if (g_maxSegments < 1)
        g_maxSegments = 1;
    LoadStartupSettings(); // 미리보기 창 개수를 레지스트리에서 로드
    if (g_numSegments > g_maxSegments)
        g_numSegments = g_maxSegments;
    
    g_windowWidth = g_numSegments * defaultPreviewWidth; // 초기 전체 가로폭 결정
    
    MSG msg;
    WNDCLASS wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = TEXT("MultiWindowViewer");
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    
    if (!RegisterClass(&wc))
        return -1;
    
    // WS_POPUP | WS_CLIPCHILDREN: 타이틀바 제거
    HWND hWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        TEXT("MultiWindowViewer"),
        TEXT("실시간 윈도우 모니터링"),
        WS_POPUP | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        g_windowWidth,
        g_windowHeight,
        NULL,
        NULL,
        hInstance,
        NULL);
    if (!hWnd)
        return -1;
    
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    if (g_alwaysOnTop)
    {
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return (int)msg.wParam;
}
// =============================================================================
// WinMain 래퍼 함수 (wWinMain을 호출하기 위해 링커 오류를 해결)
// MinGW 환경에서 wWinMain 진입점을 찾지 못할 때 사용
// =============================================================================
#ifdef __cplusplus
extern "C" {
#endif

// wWinMain 함수의 프로토타입을 다시 선언하여 WinMain에서 호출할 수 있도록 합니다.
// (일반적으로 파일 상단에 이미 있지만, 안전을 위해 추가)
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPWSTR lpCmdLine, int nCmdShow);

// WinMain을 정의하여 링커가 찾을 수 있도록 합니다.
// 이 함수는 lpCmdLine(LPSTR)을 LPWSTR로 변환한 후 wWinMain을 호출합니다.
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPSTR lpCmdLine, int nCmdShow)
{
    // lpCmdLine (LPSTR)을 lpCmdLine (LPWSTR)으로 변환합니다.
    // 필요한 버퍼 크기를 먼저 얻습니다.
    int cchWideChar = MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, NULL, 0);
    LPWSTR wideCmdLine = NULL;

    if (cchWideChar > 0) {
        wideCmdLine = (LPWSTR)malloc(cchWideChar * sizeof(WCHAR));
        if (wideCmdLine) {
            MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, wideCmdLine, cchWideChar);
        }
        // 메모리 할당 실패 시 wideCmdLine은 NULL로 유지됩니다.
        // wWinMain이 NULL lpCmdLine을 처리할 수 있다고 가정합니다.
    }

    // 실제 wWinMain 함수를 호출합니다.
    int result = wWinMain(hInstance, hPrevInstance, wideCmdLine, nCmdShow);

    // 할당된 메모리를 해제합니다.
    if (wideCmdLine) {
        free(wideCmdLine);
    }

    return result;
}

#ifdef __cplusplus
}
#endif