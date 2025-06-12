/*
    winview.cpp
    =======================
    버전 1.4.3f-update (Registry & Theme 포함) - [2025-05-31]
      - 타이틀바 제거, 창 내용 드래그 이동, 드롭다운 위치(위쪽) 조정
      - 콤보박스의 선택 상태 보존 및 각 항목(윈도우 타이틀)을 주기적으로 업데이트
      - ComboSubclassProc, ListSubclassProc 콜백에 extern "C" 명시 (이름 맹글링 문제 해결)
      - WM_TIMER에서 미리보기 대상 창의 썸네일 destination rectangle 변경 시,
                잔여 이미지가 남는 문제를 해결하기 위해 먼저 fVisible=FALSE로 업데이트한 후
                fVisible=TRUE로 재노출 처리 적용.
      - 미리보기 창의 기본 비율(현재 4:3)을 헤더 부분에서 설정 가능하도록 수정.
      - 코드 정리 및 주석 추가.
      - WM_TIMER에서 불필요한 InvalidateRect 호출 제거하여 간헐적인 깜박임(플리커링) 현상 해결.
      - DWM 썸네일의 fVisible=FALSE/TRUE 전환을 destRect가 변경될 때만 수행하여 플리커링 추가 개선.
      - '창+1' 및 '창-1' 기능을 우클릭한 창의 위치를 기준으로 동작하도록 변경.
      - 컴파일러 경고 (missing initializer, unused parameter) 해결.
      - 재수정: '창+1'/'창-1' 기능의 정확한 슬롯 이동 및 우클릭 영역 인식 로직 개선.
      - 재재수정: '창+1' 기능이 우클릭한 창의 *오른쪽*에 삽입되도록 `insertIndex` 로직 수정.
      - **재재재수정:** 창 추가/제거 시 `g_Thumbnails` 핸들 관리 로직을 가장 안정적인 방법으로 개선하여 검정 화면 문제 해결.
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
#include <dwmapi.h> // DWM Thumbnail API를 위해 필요
#include <wchar.h>  // wcslen, wcscmp 등을 위해 필요
#include <cmath>    // std::round를 사용하기 위해 추가 (C++11 표준)
#include <uxtheme.h> // SetWindowTheme 함수를 위해 필요
#include "resource.h" // 리소스 파일(아이콘 등)을 위해 필요

//=============================================================================
// 매크로 및 상수 정의
//=============================================================================
#define MAX_SEGMENTS 32              // 미리보기 창의 최대 개수
#define IDC_COMBO1   101             // 첫 번째 콤보박스의 ID (이후 IDC_COMBO1 + 인덱스로 사용)
#define ID_TIMER     1               // WM_TIMER 메시지 식별자
#define NUM_SEGMENTS_DEFAULT 3       // 애플리케이션 시작 시 기본 미리보기 창 개수

// 미리보기 창의 기본 가로세로 비율 설정
// 가로 : 세로 (예: 4:3 또는 16:9)
// 4:3 비율 (기본)
#define PREVIEW_ASPECT_RATIO_NUMERATOR 4    // 가로 (Width)
#define PREVIEW_ASPECT_RATIO_DENOMINATOR 3  // 세로 (Height)

// 16:9 비율로 변경하려면 다음 주석을 해제하고 위 4:3 비율을 주석 처리:
// #define PREVIEW_ASPECT_RATIO_NUMERATOR 16
// #define PREVIEW_ASPECT_RATIO_DENOMINATOR 9

// DWMWA_USE_IMMERSIVE_DARK_MODE 정의 (dwmapi.h에 없을 경우)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// 클라이언트 영역 레이아웃 상수
const int DROP_HEIGHT = 25;               // 드롭다운(콤보박스) 영역 높이
const int PREVIEW_HEIGHT = 300;           // 미리보기 영역 높이 (썸네일이 이 높이에 맞춰 스케일됨)
const int TOTAL_HEIGHT = DROP_HEIGHT + PREVIEW_HEIGHT; // 전체 클라이언트 영역 높이

// 컨텍스트 메뉴 항목 ID
#define IDM_ALWAYS_ON_TOP    40001 // "항상 위에" 메뉴 항목
#define IDM_RUN_AT_STARTUP   40002 // "부팅시 실행" 메뉴 항목
#define IDM_INITIALIZE       40003 // "초기화 후 종료" 메뉴 항목
#define IDM_EXIT             40004 // "종료" 메뉴 항목
#define IDM_ADD_PREVIEW      40005 // "창+1" (미리보기 창 추가) 메뉴 항목
#define IDM_REMOVE_PREVIEW   40006 // "창-1" (미리보기 창 제거) 메뉴 항목

//=============================================================================
// 전역 변수
//=============================================================================
int g_numSegments = NUM_SEGMENTS_DEFAULT;    // 현재 표시되는 미리보기 창 개수
int g_maxSegments = 0;                         // 화면 너비에 따라 계산된 최대 미리보기 창 개수
int g_windowWidth = 0;                         // 메인 윈도우의 클라이언트 영역 전체 가로폭
const int g_windowHeight = TOTAL_HEIGHT;       // 메인 윈도우의 클라이언트 영역 전체 세로폭

HWND g_ComboBoxes[MAX_SEGMENTS] = { NULL };    // 각 미리보기 창에 연결된 콤보박스 핸들 배열
HWND g_Selected[MAX_SEGMENTS]   = { NULL };    // 각 콤bo박스에서 현재 선택된 대상 창의 핸들
HTHUMBNAIL g_Thumbnails[MAX_SEGMENTS] = { NULL }; // 각 콤보박스에 선택된 창의 DWM 썸네일 핸들
// 플리커링 방지를 위해 마지막으로 업데이트된 썸네일의 목적지 사각형을 저장
RECT g_lastDestRects[MAX_SEGMENTS] = {}; // 모든 멤버를 0으로 초기화 (C++11 이상)

bool g_alwaysOnTop = 0; // 메인 윈도우가 항상 최상단에 있을지 여부 (false=0, true=1)
bool g_runAtStartup = false; // 애플리케이션이 부팅 시 자동 실행될지 여부
HINSTANCE g_hInst = NULL; // 애플리케이션 인스턴스 핸들

// 초기화(Reset) 요청 플래그: "초기화 후 종료" 명령 실행 시 true로 설정되어 종료 시 레지스트리 저장 방지
bool g_resetRequested = false;
// 드롭다운 팝업 활성 상태 플래그: 드롭다운이 열려 있을 때는 타이머 업데이트를 일시 중지
bool g_dropdownActive = false;
// 마지막으로 우클릭된 미리보기 슬롯의 인덱스. -1은 빈 공간을 의미
int g_rightClickedSegmentIndex = -1; 

// 윈도우 목록에서 제외할 창 제목의 부분 문자열 목록
const TCHAR* g_excludedSubstrings[] = { _T("설정"), _T("Windows 입력"), _T("팝업 호스트"), _T("GeForce Overlay"), _T("위젯"), _T("작업 전환") };
const size_t g_excludedCount = sizeof(g_excludedSubstrings) / sizeof(g_excludedSubstrings[0]);
// GW_OWNER를 가진 창(주로 부모 창에 종속된 팝업 창 등)을 제외할지 여부
const bool g_excludeOwnerWindows = false;

// UI 폰트 핸들
HFONT g_hFont = CreateFont(18, 0, 0, 0,
                     FW_NORMAL, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,  // 부드러운 텍스트 렌더링
                     DEFAULT_PITCH | FF_DONTCARE,
                     L"맑은 고딕");

//=============================================================================
// 함수 프로토타입
//=============================================================================
void SaveSettings(HWND hWnd);           // 현재 설정(위치, 항상 위에, 미리보기 개수)을 레지스트리에 저장
void LoadSettings(HWND hWnd);           // 저장된 설정(위치, 항상 위에)을 레지스트리에서 로드
void SetRunAtStartup(bool enable);      // 부팅시 자동 실행 설정/해제
void LoadRunAtStartup();                // 부팅시 자동 실행 설정 로드
void LoadStartupSettings();             // 미리보기 창 개수 설정을 레지스트리에서 로드
void ResetRegistrySettings();           // 애플리케이션 관련 레지스트리 설정 초기화
void RecreatePreviews(HWND hWnd);       // 미리보기 콤보박스 컨트롤들을 재생성 및 상태 복원
void UpdateComboBoxItem(HWND hCombo);   // 특정 콤보박스의 항목(윈도우 타이틀)들을 주기적으로 업데이트
extern "C" LRESULT CALLBACK ComboSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData); // 콤보박스 서브클래스 프로시저
extern "C" LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);   // 콤보박스 드롭다운 리스트박스 서브클래스 프로시저
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam); // EnumWindows에 사용될 콜백 함수: 실행 중인 창 목록을 콤보박스에 추가
int GetSegmentIndexAtPoint(POINT pt);   // 주어진 클라이언트 좌표에 해당하는 미리보기 슬롯 인덱스 반환
void ShowContextMenu(HWND hWnd);        // 메인 윈도우 우클릭 시 컨텍스트 메뉴 표시
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam); // 메인 윈도우 프로시저

//=============================================================================
// 레지스트리 관련 함수 (저장/불러오기/초기화, 부팅시 실행)
//=============================================================================

// 부팅 시 애플리케이션 자동 실행 여부를 레지스트리에서 로드
void LoadRunAtStartup()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, 
       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        DWORD dwType;
        DWORD dwSize = 0;
        // "MultiWindowViewer"라는 이름의 값이 존재하는지 확인
        if (RegQueryValueEx(hKey, L"MultiWindowViewer", NULL, &dwType, NULL, &dwSize) == ERROR_SUCCESS)
            g_runAtStartup = true; // 값이 존재하면 자동 실행 설정됨
        else
            g_runAtStartup = false; // 값이 없으면 자동 실행 설정 안 됨
        RegCloseKey(hKey);
    }
    else
    {
        g_runAtStartup = false; // 레지스트리 키를 열 수 없으면 자동 실행 설정 안 됨
    }
}

// 시작 시 미리보기 창의 개수를 레지스트리에서 로드
void LoadStartupSettings()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\MultiWindowViewer", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        DWORD dwPreview = 0;
        DWORD dwSize = sizeof(dwPreview);
        DWORD dwType = 0;
        // "PreviewCount" 값을 읽어옴
        if (RegQueryValueEx(hKey, L"PreviewCount", NULL, &dwType, (LPBYTE)&dwPreview, &dwSize) == ERROR_SUCCESS)
        {
            // 읽어온 값이 유효한 범위 내에 있으면 g_numSegments에 적용
            if (dwPreview > 0 && dwPreview <= MAX_SEGMENTS)
                g_numSegments = (int)dwPreview;
        }
        RegCloseKey(hKey);
    }
}

// 현재 설정(창 위치, 항상 위에, 미리보기 개수)을 레지스트리에 저장
void SaveSettings(HWND hWnd)
{
    HKEY hKey;
    // "Software\\MultiWindowViewer" 키를 생성하거나 열기
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\MultiWindowViewer", 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RECT rc;
        // 메인 윈도우의 화면 좌표를 가져와 저장
        if (GetWindowRect(hWnd, &rc))
        {
            DWORD dwVal = rc.left;
            RegSetValueEx(hKey, L"WindowLeft", 0, REG_DWORD, (const BYTE*)&dwVal, sizeof(dwVal));
            dwVal = rc.top;
            RegSetValueEx(hKey, L"WindowTop", 0, REG_DWORD, (const BYTE*)&dwVal, sizeof(dwVal));
        }
        // "AlwaysOnTop" 상태 저장
        DWORD dwAlways = (DWORD)g_alwaysOnTop;
        RegSetValueEx(hKey, L"AlwaysOnTop", 0, REG_DWORD, (const BYTE*)&dwAlways, sizeof(dwAlways));
        // 현재 미리보기 창 개수 저장
        DWORD dwPreview = (DWORD)g_numSegments;
        RegSetValueEx(hKey, L"PreviewCount", 0, REG_DWORD, (const BYTE*)&dwPreview, sizeof(dwPreview));
        RegCloseKey(hKey);
    }
}

// 저장된 설정(창 위치, 항상 위에)을 레지스트리에서 로드
void LoadSettings(HWND hWnd)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\MultiWindowViewer", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        RECT rc = {}; // 모든 멤버를 0으로 초기화
        DWORD dwType = 0, dwSize = sizeof(rc.left);
        // "WindowLeft" 및 "WindowTop" 값 로드
        RegQueryValueEx(hKey, L"WindowLeft", NULL, &dwType, (LPBYTE)&rc.left, &dwSize);
        dwSize = sizeof(rc.top);
        RegQueryValueEx(hKey, L"WindowTop", NULL, &dwType, (LPBYTE)&rc.top, &dwSize);
        // 로드된 위치로 윈도우 위치 설정 (크기는 WM_CREATE에서 g_windowWidth, g_windowHeight로 재설정됨)
        SetWindowPos(hWnd, NULL, rc.left, rc.top, g_windowWidth, g_windowHeight, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        
        // "AlwaysOnTop" 상태 로드
        DWORD dwAlways = 0;
        dwSize = sizeof(dwAlways);
        if (RegQueryValueEx(hKey, L"AlwaysOnTop", NULL, &dwType, (LPBYTE)&dwAlways, &dwSize) == ERROR_SUCCESS)
        {
            g_alwaysOnTop = (bool)dwAlways;
        }
        RegCloseKey(hKey);
    }
}

// 부팅 시 애플리케이션 자동 실행을 설정하거나 해제
void SetRunAtStartup(bool enable)
{
    HKEY hKey;
    // HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run 키를 열거나 생성
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        if (enable)
        {
            wchar_t szPath[MAX_PATH] = {0};
            GetModuleFileName(NULL, szPath, MAX_PATH); // 현재 실행 파일 경로 가져오기
            // "MultiWindowViewer"라는 이름으로 실행 파일 경로를 REG_SZ 타입으로 저장
            RegSetValueEx(hKey, L"MultiWindowViewer", 0, REG_SZ,
                          (const BYTE*)szPath, (wcslen(szPath)+1)*sizeof(wchar_t));
        }
        else
        {
            // "MultiWindowViewer" 값 삭제
            RegDeleteValue(hKey, L"MultiWindowViewer");
        }
        RegCloseKey(hKey);
    }
}

// 애플리케이션 관련 레지스트리 설정 초기화
void ResetRegistrySettings()
{
    HKEY hKey;
    // "Software" 키를 열고 "MultiWindowViewer" 하위 키를 삭제
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software", 0, KEY_WRITE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteKeyEx(hKey, L"MultiWindowViewer", KEY_WOW64_64KEY, 0);
        RegCloseKey(hKey);
    }
}

//=============================================================================
// UpdateComboBoxItem: 각 콤보박스의 항목(윈도우 타이틀)을 업데이트
// - 대상 윈도우가 유효한지 확인하고, 타이틀이 변경되었으면 업데이트
// - 사라진 윈도우는 목록에서 제거
//=============================================================================
void UpdateComboBoxItem(HWND hCombo)
{
    int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
    for (int index = 0; index < count; index++) {
        HWND hwnd = (HWND)SendMessage(hCombo, CB_GETITEMDATA, index, 0);
        if (hwnd && IsWindow(hwnd)) { // 윈도우 핸들이 유효하고 윈도우가 존재하는지 확인
            wchar_t newTitle[256] = {0}, currTitle[256] = {0};
            GetWindowText(hwnd, newTitle, 256); // 현재 윈도우의 타이틀 가져오기
            SendMessage(hCombo, CB_GETLBTEXT, index, (LPARAM)currTitle); // 콤보박스에 저장된 타이틀 가져오기

            // 새 타이틀이 비어있지 않고, 기존 타이틀과 다르면 업데이트
            if (wcslen(newTitle) > 0 && wcscmp(newTitle, currTitle) != 0) {
                int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0); // 현재 선택된 인덱스 저장
                SendMessage(hCombo, CB_DELETESTRING, index, 0); // 기존 항목 삭제
                int newIndex = (int)SendMessage(hCombo, CB_INSERTSTRING, index, (LPARAM)newTitle); // 새 타이틀로 항목 추가
                SendMessage(hCombo, CB_SETITEMDATA, newIndex, (LPARAM)hwnd); // 윈도우 핸들 다시 저장
                if (sel == index) { // 이전에 선택된 항목이었다면 다시 선택 상태로 만듦
                    SendMessage(hCombo, CB_SETCURSEL, newIndex, 0);
                }
            }
        } else {
            // 윈도우가 유효하지 않으면 (닫혔거나 등) 목록에서 삭제
            SendMessage(hCombo, CB_DELETESTRING, index, 0);
            index--; count--; // 항목이 삭제되었으므로 인덱스와 카운트 조정
        }
    }
}

//=============================================================================
// RecreatePreviews: 콤보박스(드롭다운) 컨트롤들을 새로 생성하며, 기존 선택 상태 보존 및 목록 재채우기
// - 미리보기 창 개수 변경(창+1, 창-1) 시 호출됨
//=============================================================================
void RecreatePreviews(HWND hWnd)
{
    // 1. 기존 콤보박스 제거 (DWM 썸네일 핸들은 WM_COMMAND에서 이미 처리되었음)
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (g_ComboBoxes[i]) {
            RemoveWindowSubclass(g_ComboBoxes[i], ComboSubclassProc, i); // 서브클래스 해제
            DestroyWindow(g_ComboBoxes[i]); // 콤보박스 윈도우 파괴
            g_ComboBoxes[i] = NULL;
        }
    }
    
    int comboHeight = DROP_HEIGHT;
    // 미리보기 영역 높이와 정의된 비율을 사용하여 기본 콤보박스 및 미리보기 슬롯 너비 계산
    int defaultPreviewWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
    
    // 2. g_numSegments 개수만큼 새로운 콤보박스 생성
    for (int i = 0; i < g_numSegments; i++) {
        int x = i * defaultPreviewWidth; // 각 콤보박스의 X 위치 계산
        g_ComboBoxes[i] = CreateWindowEx(0, TEXT("COMBOBOX"), NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS, // 자식 윈도우, 보임, 드롭다운 목록, 문자열 포함
            x, 0, defaultPreviewWidth, comboHeight, // 위치 및 크기
            hWnd, (HMENU)(INT_PTR)(IDC_COMBO1 + i), // 부모 윈도우, 컨트롤 ID
            g_hInst, NULL);
        SetWindowSubclass(g_ComboBoxes[i], ComboSubclassProc, i, 0); // 콤보박스 서브클래스 설정
        SendMessage(g_ComboBoxes[i], WM_SETFONT, (WPARAM)g_hFont, TRUE); // 폰트 설정
        SetWindowTheme(g_ComboBoxes[i], L"", L""); // 콤보박스 테마 초기화 (클래식 스타일 적용 시도)
    }
    
    // 3. EnumWindows로 현재 실행 중인 창들을 열거하여 각 콤보박스에 항목 추가/업데이트
    EnumWindows(EnumWindowsProc, (LPARAM)hWnd);
    
    // 4. g_Selected 배열에 있는 값을 기반으로 콤보박스 선택 상태 복원
    //    g_Selected 배열 자체는 WM_COMMAND에서 이미 올바른 상태로 조정되었음.
    for (int i = 0; i < g_numSegments; i++) {
        if (g_ComboBoxes[i]) {
            int count = (int)SendMessage(g_ComboBoxes[i], CB_GETCOUNT, 0, 0);
            int selIndex = CB_ERR;
            if (g_Selected[i] != NULL) {
                for (int j = 0; j < count; j++) {
                    HWND h = (HWND)SendMessage(g_ComboBoxes[i], CB_GETITEMDATA, j, 0);
                    if (h == g_Selected[i]) { // 저장된 핸들과 일치하는 항목을 찾음
                        selIndex = j;
                        break;
                    }
                }
            }
            if (selIndex != CB_ERR) { // 일치하는 항목을 찾으면 해당 항목을 선택
                SendMessage(g_ComboBoxes[i], CB_SETCURSEL, selIndex, 0);
            } else {
                // g_Selected[i]가 유효하지 않거나 콤보박스에 없는 경우
                SendMessage(g_ComboBoxes[i], CB_SETCURSEL, -1, 0);
                g_Selected[i] = NULL; // 실제 선택 상태를 반영
            }
        }
    }
}

//=============================================================================
// ComboSubclassProc: 콤보박스 서브클래스 콜백 (extern "C" 명시로 이름 맹글링 문제 방지)
// - 콤보박스 자체의 마우스 휠 이벤트를 처리하여 항목 변경
//=============================================================================
extern "C" LRESULT CALLBACK ComboSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    UNREFERENCED_PARAMETER(dwRefData); // dwRefData는 사용되지 않음
    switch (uMsg)
    {
        case WM_MOUSEWHEEL: // 마우스 휠 이벤트 처리
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam); // 휠 스크롤 방향 (양수: 위, 음수: 아래)
            int curSel = (int)SendMessage(hWnd, CB_GETCURSEL, 0, 0); // 현재 선택된 인덱스
            int count = (int)SendMessage(hWnd, CB_GETCOUNT, 0, 0);   // 전체 항목 개수
            if (delta > 0 && curSel > 0) // 위로 스크롤 & 현재 선택이 첫 항목이 아닐 때
            {
                curSel--; // 인덱스 감소
                SendMessage(hWnd, CB_SETCURSEL, curSel, 0); // 새 항목 선택
                // 부모 윈도우에 CBN_SELCHANGE 메시지를 보내 선택 변경 알림
                SendMessage(GetParent(hWnd), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hWnd), CBN_SELCHANGE), (LPARAM)hWnd);
            }
            else if (delta < 0 && curSel < count - 1) // 아래로 스크롤 & 현재 선택이 마지막 항목이 아닐 때
            {
                curSel++; // 인덱스 증가
                SendMessage(hWnd, CB_SETCURSEL, curSel, 0); // 새 항목 선택
                // 부모 윈도우에 CBN_SELCHANGE 메시지를 보내 선택 변경 알림
                SendMessage(GetParent(hWnd), WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(hWnd), CBN_SELCHANGE), (LPARAM)hWnd);
            }
            return 0; // 메시지 처리 완료
        }
        case WM_ERASEBKGND: // 배경 지우기 메시지 무시하여 깜빡임 감소
            return 1;
        case WM_NCDESTROY: // 서브클래스 해제 시 처리 (제거 시점 명확화)
            RemoveWindowSubclass(hWnd, ComboSubclassProc, uIdSubclass);
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam); // 기본 처리기로 메시지 전달
}

//=============================================================================
// ListSubclassProc: 드롭다운 리스트 서브클래스 콜백 (extern "C" 명시로 이름 맹글링 문제 방지)
// - 콤보박스의 드롭다운 리스트박스에 대한 마우스 휠 이벤트를 처리하여 항목 변경
//=============================================================================
extern "C" LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    // dwRefData는 이 리스트박스와 연결된 콤보박스 핸들 (SetWindowSubclass에서 설정)
    HWND hCombo = (HWND)dwRefData; 
    UNREFERENCED_PARAMETER(uIdSubclass); // uIdSubclass는 사용되지 않음

    switch (uMsg)
    {
        case WM_MOUSEWHEEL: // 마우스 휠 이벤트 처리
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
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
            return 0; // 메시지 처리 완료
        }
        case WM_NCDESTROY: // 서브클래스 해제 시 처리
            RemoveWindowSubclass(hWnd, ListSubclassProc, uIdSubclass);
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam); // 기본 처리기로 메시지 전달
}

//=============================================================================
// EnumWindowsProc: 실행 중인 창들을 열거하여 각 콤보박스에 항목 추가/업데이트
// - EnumWindows 함수에 의해 호출되는 콜백 함수
//=============================================================================
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    HWND hMain = (HWND)lParam; // 메인 윈도우 핸들
    
    // 현재 창이 메인 윈도우 자신이거나, 유효하지 않은 창인 경우 건너뛰기
    if (hwnd == hMain || !IsWindow(hwnd))
        return TRUE;
    
    // g_excludeOwnerWindows가 true이고, 창이 GW_OWNER를 가지고 있다면 건너뛰기
    if (g_excludeOwnerWindows && (GetWindow(hwnd, GW_OWNER) != NULL))
        return TRUE;
    
    // 숨겨진 창은 건너뛰기
    if (!IsWindowVisible(hwnd))
        return TRUE;
    
    TCHAR title[256];
    GetWindowText(hwnd, title, 256); // 창 타이틀 가져오기
    
    // 타이틀이 없거나 (빈 문자열), 특정 제외 문자열을 포함하는 경우 건너뛰기
    if (_tcslen(title) == 0)
        return TRUE;
    for (size_t i = 0; i < g_excludedCount; i++) {
        if (_tcsstr(title, g_excludedSubstrings[i]) != NULL)
            return TRUE;
    }

    // 현재 열거된 창을 모든 콤보박스에 추가 (중복 방지)
    for (int i = 0; i < g_numSegments; i++) {
        if (!g_ComboBoxes[i]) continue; // 콤보박스가 유효하지 않으면 건너뛰기
        bool found = false;
        int count = (int)SendMessage(g_ComboBoxes[i], CB_GETCOUNT, 0, 0);
        for (int j = 0; j < count; j++) {
            HWND hwndStored = (HWND)SendMessage(g_ComboBoxes[i], CB_GETITEMDATA, j, 0);
            if (hwndStored == hwnd) { // 이미 목록에 있는 창이면
                found = true;
                break;
            }
        }
        if (!found) { // 목록에 없는 창이면 추가
            int index = (int)SendMessage(g_ComboBoxes[i], CB_ADDSTRING, 0, (LPARAM)title);
            SendMessage(g_ComboBoxes[i], CB_SETITEMDATA, index, (LPARAM)hwnd); // 창 핸들도 함께 저장
        }
    }
    return TRUE; // 계속해서 다음 창 열거
}

//=============================================================================
// GetSegmentIndexAtPoint: 주어진 클라이언트 좌표에 해당하는 미리보기 슬롯 인덱스 반환
// - 더블 클릭 또는 우클릭 시 어떤 슬롯이 클릭되었는지 판별하는 헬퍼 함수
//=============================================================================
int GetSegmentIndexAtPoint(POINT pt)
{
    // 클릭된 Y 좌표가 미리보기 영역 밖이면 -1 반환
    if (pt.y < DROP_HEIGHT || pt.y > DROP_HEIGHT + PREVIEW_HEIGHT)
        return -1;

    int cumulativeWidth = 0; // 각 미리보기 슬롯의 누적 너비

    for (int i = 0; i < g_numSegments; i++)
    {
        int slotWidth = 0; // 현재 미리보기 슬롯의 너비

        // 썸네일이 유효하고 소스 크기를 가져올 수 있으면 실제 썸네일 크기로 계산
        // g_Thumbnails[i]가 NULL일 수 있으므로 먼저 검사
        if (g_Selected[i] && IsWindow(g_Selected[i]) && g_Thumbnails[i])
        {
            SIZE srcSize = {};
            if (SUCCEEDED(DwmQueryThumbnailSourceSize(g_Thumbnails[i], &srcSize)) && srcSize.cy > 0)
            {
                if (srcSize.cy <= PREVIEW_HEIGHT)
                {
                    slotWidth = srcSize.cx;
                }
                else
                {
                    double scale = (double)PREVIEW_HEIGHT / srcSize.cy;
                    slotWidth = (int)std::round(srcSize.cx * scale);
                }
            }
            else // 썸네일은 있으나 소스 크기 가져오기 실패 (예: 대상 창 최소화)
            {
                slotWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
            }
        }
        else // 썸네일이 없거나 (선택되지 않았거나, 대상 창이 닫히거나)
        {
            slotWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
        }

        // 클릭된 X 좌표가 현재 슬롯의 범위 내에 있는지 확인
        if (pt.x >= cumulativeWidth && pt.x < cumulativeWidth + slotWidth)
        {
            return i; // 해당 슬롯의 인덱스 반환
        }
        cumulativeWidth += slotWidth; // 다음 슬롯의 시작 X 좌표 계산
    }
    return -1; // 어떤 슬롯도 클릭되지 않았을 경우
}

//=============================================================================
// HandleDoubleClick: 미리보기 영역 더블클릭 시 해당 창 활성화
//=============================================================================
LRESULT HandleDoubleClick(HWND hWnd, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(hWnd); // hWnd는 이 함수 내에서 직접 사용되지 않음.

    POINT pt;
    pt.x = LOWORD(lParam); // 마우스 클릭 X 좌표
    pt.y = HIWORD(lParam); // 마우스 클릭 Y 좌표

    int indexFound = GetSegmentIndexAtPoint(pt); // 헬퍼 함수를 사용하여 클릭된 슬롯 인덱스 가져오기

    // 클릭된 슬롯이 있고, 해당 슬롯에 유효한 창이 선택되어 있다면
    if (indexFound != -1 && g_Selected[indexFound] && IsWindow(g_Selected[indexFound]))
    {
        // 창이 최소화되어 있다면 복원
        if (IsIconic(g_Selected[indexFound]))
        {
            ShowWindow(g_Selected[indexFound], SW_RESTORE);
        }
        // 창을 전면으로 가져오고 활성화
        BringWindowToTop(g_Selected[indexFound]);
        SetForegroundWindow(g_Selected[indexFound]);
        SetActiveWindow(g_Selected[indexFound]);
    }
    return 0;
}

//=============================================================================
// ShowContextMenu: 메인 윈도우 우클릭 시 컨텍스트 메뉴 ("항상 위에", "부팅시 실행", "초기화 후 종료", "창+1", "창-1", "종료")
//=============================================================================
void ShowContextMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt); // 현재 마우스 커서의 화면 좌표 가져오기
    ScreenToClient(hWnd, &pt); // 클라이언트 좌표로 변환

    // 우클릭된 슬롯의 인덱스를 저장. 메뉴 핸들러에서 사용됨.
    g_rightClickedSegmentIndex = GetSegmentIndexAtPoint(pt); 

    HMENU hMenu = CreatePopupMenu(); // 팝업 메뉴 생성

    // 메뉴 항목 추가 및 현재 상태에 따라 체크 표시
    AppendMenu(hMenu, MF_STRING | (g_alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, L"항상 위에");
    AppendMenu(hMenu, MF_STRING | (g_runAtStartup ? MF_CHECKED : 0), IDM_RUN_AT_STARTUP, L"부팅시 실행");
    AppendMenu(hMenu, MF_STRING, IDM_INITIALIZE, L"초기화 후 종료");
    
    // '창+1' 및 '창-1' 메뉴 활성화/비활성화 조건
    UINT addFlags = MF_STRING;
    if (g_numSegments >= MAX_SEGMENTS) { // 최대 미리보기 개수 도달 시 비활성화
        addFlags |= MF_GRAYED;
    }
    AppendMenu(hMenu, addFlags, IDM_ADD_PREVIEW, L"창+1");

    UINT removeFlags = MF_STRING;
    if (g_numSegments <= 1) { // 최소 미리보기 개수 도달 시 비활성화
        removeFlags |= MF_GRAYED;
    }
    AppendMenu(hMenu, removeFlags, IDM_REMOVE_PREVIEW, L"창-1");

    AppendMenu(hMenu, MF_STRING, IDM_EXIT, L"종료");
    
    // 팝업 메뉴 표시
    GetCursorPos(&pt); // TrackPopupMenu는 화면 좌표를 사용하므로 다시 가져옴
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu); // 메뉴 사용 후 파괴
}

//=============================================================================
// WndProc: 메인 윈도우 프로시저 (메시지 처리 핸들러)
//=============================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE: // 윈도우 생성 시 초기화
        {
            // 공통 컨트롤 라이브러리 초기화
            INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES };
            InitCommonControlsEx(&icex);
            g_hInst = ((LPCREATESTRUCT)lParam)->hInstance; // 인스턴스 핸들 저장
            
            // 애플리케이션 아이콘 로드 및 설정
            HICON hIconSmall = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
            HICON hIconLarge = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
            
            if (hIconSmall) {
                SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
            }
            if (hIconLarge) {
                SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconLarge);
            }

            // 레지스트리에서 이전 설정 로드 (창 위치, 항상 위에 옵션)
            LoadSettings(hWnd);
            // 부팅 시 자동 실행 설정 로드
            LoadRunAtStartup();

            // 윈도우의 현재 위치를 유지하면서 크기를 g_windowWidth, g_windowHeight로 설정
            RECT rc;
            if (GetWindowRect(hWnd, &rc))
            {
                SetWindowPos(hWnd, NULL, rc.left, rc.top, g_windowWidth, g_windowHeight,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            
            RecreatePreviews(hWnd); // 콤보박스 및 미리보기 영역 초기 생성
            SetTimer(hWnd, ID_TIMER, 500, NULL); // 0.5초 간격으로 타이머 설정
        }
        break;
        
        case WM_LBUTTONDOWN: // 마우스 왼쪽 버튼 클릭 (타이틀바 없는 창 이동용)
        {
            ReleaseCapture(); // 혹시 모를 기존 캡처 해제
            // WM_NCLBUTTONDOWN 메시지를 HTCAPTION과 함께 보내 창 이동을 시뮬레이션
            SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        
        case WM_LBUTTONDBLCLK: // 마우스 왼쪽 버튼 더블클릭 (미리보기 창 활성화용)
            return HandleDoubleClick(hWnd, lParam); // HandleDoubleClick 함수 호출
            
        case WM_ERASEBKGND: // 배경 지우기 메시지 (더블 버퍼링 시 깜빡임 방지용)
            return 1; // 배경을 지우지 않도록 1 반환
            
        case WM_RBUTTONUP: // 마우스 오른쪽 버튼 떼기 (컨텍스트 메뉴 표시용)
            ShowContextMenu(hWnd); // 컨텍스트 메뉴 표시 함수 호출
            break;
        
        case WM_COMMAND: // 컨트롤 또는 메뉴 명령 처리
        {
            int id = LOWORD(wParam);   // 컨트롤 ID 또는 메뉴 ID
            int code = HIWORD(wParam); // 알림 코드 (콤보박스 등)

            // 콤보박스 관련 메시지 처리
            if ((id >= IDC_COMBO1 && id < IDC_COMBO1 + g_numSegments))
            {
                if (code == CBN_DROPDOWN) // 콤보박스 드롭다운 목록이 열릴 때
                {
                    g_dropdownActive = true; // 드롭다운 활성 상태로 설정
                    // WS_CLIPCHILDREN 스타일 제거하여 드롭다운 리스트가 부모 창 밖으로 그려지게 허용
                    LONG style = GetWindowLong(hWnd, GWL_STYLE);
                    SetWindowLong(hWnd, GWL_STYLE, style & ~WS_CLIPCHILDREN);
                    
                    int index = id - IDC_COMBO1;
                    HWND hCombo = g_ComboBoxes[index];
                    COMBOBOXINFO cbi = {}; // 모든 멤버를 0으로 초기화
                    cbi.cbSize = sizeof(cbi);
                    // 콤보박스 정보(특히 리스트박스 핸들) 가져오기
                    if (GetComboBoxInfo(hCombo, &cbi) && cbi.hwndList)
                    {
                        SetWindowTheme(cbi.hwndList, L"", L""); // 리스트박스 테마 초기화
                        SendMessage(cbi.hwndList, WM_SETFONT, (WPARAM)g_hFont, TRUE); // 리스트박스 폰트 설정
                        
                        RECT rcCombo;
                        GetWindowRect(hCombo, &rcCombo); // 콤보박스의 화면 좌표 가져오기
                        int comboWidth = rcCombo.right - rcCombo.left;
                        const int LIST_HEIGHT = 200; // 드롭다운 리스트의 고정 높이
                        
                        // 드롭다운 리스트를 콤보박스 위쪽으로 배치
                        SetWindowPos(cbi.hwndList, NULL,
                                     rcCombo.left, rcCombo.top - LIST_HEIGHT, // Y 좌표를 위로 조정
                                     comboWidth, LIST_HEIGHT,
                                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                        SendMessage(hCombo, CB_SETDROPPEDWIDTH, (WPARAM)comboWidth, 0); // 드롭다운 리스트의 너비 설정
                    }
                }
                else if (code == CBN_CLOSEUP) // 콤보박스 드롭down 목록이 닫힐 때
                {
                    g_dropdownActive = false; // 드롭다운 비활성 상태로 설정
                    // WS_CLIPCHILDREN 스타일 복원 (자식 창이 부모 영역을 벗어나지 않도록 함)
                    LONG style = GetWindowLong(hWnd, GWL_STYLE);
                    SetWindowLong(hWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
                }
                else if (code == CBN_SELCHANGE) // 콤보박스 선택 항목이 변경될 때
                {
                    int index = id - IDC_COMBO1;
                    HWND hCombo = g_ComboBoxes[index];
                    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0); // 새로 선택된 항목의 인덱스
                    if (sel != CB_ERR)
                    {
                        HWND hwndTarget = (HWND)SendMessage(hCombo, CB_GETITEMDATA, sel, 0); // 선택된 항목의 창 핸들
                        g_Selected[index] = hwndTarget; // 전역 변수에 선택된 창 핸들 저장
                        // 이전에 등록된 DWM 썸네일이 있다면 해제
                        if (g_Thumbnails[index])
                        {
                            DwmUnregisterThumbnail(g_Thumbnails[index]);
                            g_Thumbnails[index] = NULL;
                        }
                        // 선택이 변경되었으므로, 해당 썸네일의 이전 목적지 사각형을 초기화하여 다음 타이머에서 강제 업데이트 유도
                        g_lastDestRects[index] = {}; // 모든 멤버를 0으로 초기화
                    } else { // 선택이 해제된 경우
                        g_Selected[index] = NULL;
                        if (g_Thumbnails[index])
                        {
                            DwmUnregisterThumbnail(g_Thumbnails[index]);
                            g_Thumbnails[index] = NULL;
                        }
                        g_lastDestRects[index] = {}; // 모든 멤버를 0으로 초기화
                    }
                }
            }
            
            // 메뉴 명령 처리
            if (id == IDM_ALWAYS_ON_TOP) // "항상 위에" 메뉴
            {
                g_alwaysOnTop = !g_alwaysOnTop; // 상태 토글
                // HWND_TOPMOST 또는 HWND_NOTOPMOST를 사용하여 창 최상단 상태 변경
                SetWindowPos(hWnd, (g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST),
                             0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            else if (id == IDM_RUN_AT_STARTUP) // "부팅시 실행" 메뉴
            {
                g_runAtStartup = !g_runAtStartup; // 상태 토글
                SetRunAtStartup(g_runAtStartup); // 레지스트리 설정 업데이트
            }
            else if (id == IDM_INITIALIZE) // "초기화 후 종료" 메뉴
            {
                g_resetRequested = true; // 초기화 요청 플래그 설정
                g_alwaysOnTop = false;   // 초기 상태로 설정
                g_runAtStartup = false;
                g_numSegments = NUM_SEGMENTS_DEFAULT; // 기본 미리보기 개수로 복원
                
                // 윈도우 크기를 기본 미리보기 개수에 맞춰 재설정
                int previewWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
                g_windowWidth = g_numSegments * previewWidth;
                SetWindowPos(hWnd, NULL, CW_USEDEFAULT, CW_USEDEFAULT, g_windowWidth, g_windowHeight,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                
                ResetRegistrySettings(); // 애플리케이션 레지스트리 설정 초기화
                // 부팅 시 자동 실행 레지스트리도 명시적으로 삭제
                {
                    HKEY hRunKey;
                    if (RegOpenKeyEx(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hRunKey) == ERROR_SUCCESS)
                    {
                        RegDeleteValue(hRunKey, L"MultiWindowViewer");
                        RegCloseKey(hRunKey);
                    }
                }
                DestroyWindow(hWnd); // 윈도우 파괴 (종료)
            }
            else if (id == IDM_ADD_PREVIEW) // "창+1" (미리보기 창 추가) 메뉴
            {
                if (g_numSegments < MAX_SEGMENTS) // 최대 개수를 초과하지 않는 경우
                {
                    SendMessage(hWnd, WM_SETREDRAW, FALSE, 0); // 화면 업데이트 일시 중지
                    
                    int insertIndex;
                    // 우클릭된 인덱스가 유효하면 해당 인덱스의 '오른쪽'에 삽입
                    if (g_rightClickedSegmentIndex >= 0 && g_rightClickedSegmentIndex < g_numSegments) {
                        insertIndex = g_rightClickedSegmentIndex + 1;
                    } else {
                        // 유효하지 않은 인덱스 (빈 공간 우클릭) 또는 현재 개수보다 크거나 같으면 맨 마지막에 추가
                        insertIndex = g_numSegments; 
                    }
                    
                    // Step 1: g_Selected 요소들을 오른쪽으로 한 칸씩 이동
                    for (int i = g_numSegments; i > insertIndex; --i) {
                        g_Selected[i] = g_Selected[i-1];
                    }
                    g_Selected[insertIndex] = NULL; // 새로 추가될 슬롯은 비어있음

                    // Step 2: DWM 썸네일 핸들과 관련 상태들을 모두 정리.
                    // 이 위치 이후의 모든 슬롯은 이제 새로운 DWM 썸네일 등록이 필요함.
                    for (int i = insertIndex; i <= g_numSegments; ++i) { // g_numSegments는 증가 전 마지막 인덱스 + 1
                        if (g_Thumbnails[i]) {
                            DwmUnregisterThumbnail(g_Thumbnails[i]); // 기존 핸들 해제
                        }
                        g_Thumbnails[i] = NULL; // 핸들 NULL
                        g_lastDestRects[i] = {}; // 이전 목적지 사각형 초기화
                    }

                    g_numSegments++; // 미리보기 개수 증가
                    RecreatePreviews(hWnd); // 콤보박스 컨트롤들만 재구성 (데이터 배열은 이미 조정됨)
                    SendMessage(hWnd, WM_SETREDRAW, TRUE, 0); // 화면 업데이트 재개
                    RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN); // 전체 다시 그리기
                }
                else
                {
                    wchar_t buf[128];
                    wsprintf(buf, L"최대 창의 갯수는 %d개 입니다.", MAX_SEGMENTS);
                    MessageBox(hWnd, buf, L"알림", MB_OK | MB_ICONINFORMATION);
                }
            }
            else if (id == IDM_REMOVE_PREVIEW) // "창-1" (미리보기 창 제거) 메뉴
            {
                if (g_numSegments > 1) // 최소 개수(1개) 이하로 줄어들지 않도록 함
                {
                    SendMessage(hWnd, WM_SETREDRAW, FALSE, 0); // 화면 업데이트 일시 중지

                    int removeIndex = g_rightClickedSegmentIndex; // 우클릭된 위치를 제거 인덱스로 사용
                    // 우클릭 인덱스가 유효하지 않거나 (빈 공간 우클릭), 현재 개수보다 크거나 같으면 맨 마지막에서 제거
                    if (removeIndex < 0 || removeIndex >= g_numSegments) { 
                        removeIndex = g_numSegments - 1; // 맨 마지막에서 제거
                    }
                    
                    // Step 1: g_Selected 요소들을 왼쪽으로 한 칸씩 이동
                    // (removeIndex부터 시작하여, removeIndex + 1의 내용을 removeIndex로 복사)
                    for (int i = removeIndex; i < g_numSegments - 1; ++i) {
                        g_Selected[i] = g_Selected[i+1];
                    }
                    g_Selected[g_numSegments - 1] = NULL; // 맨 마지막 슬롯은 비어있음

                    // Step 2: DWM 썸네일 핸들과 관련 상태들을 모두 정리.
                    // 이 위치 이후의 모든 슬롯은 이제 새로운 DWM 썸네일 등록이 필요하거나 비어있음.
                    // (g_numSegments는 아직 감소 전이므로 g_numSegments - 1 인덱스까지 반복)
                    for (int i = removeIndex; i < g_numSegments; ++i) { 
                        if (g_Thumbnails[i]) {
                            DwmUnregisterThumbnail(g_Thumbnails[i]); // 기존 핸들 해제
                        }
                        g_Thumbnails[i] = NULL; // 핸들 NULL
                        g_lastDestRects[i] = {}; // 이전 목적지 사각형 초기화
                    }

                    g_numSegments--; // 미리보기 개수 감소
                    RecreatePreviews(hWnd); // 콤보박스 컨트롤들만 재구성 (데이터 배열은 이미 조정됨)
                    SendMessage(hWnd, WM_SETREDRAW, TRUE, 0); // 화면 업데이트 재개
                    RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN); // 전체 다시 그리기
                }
                else
                {
                    MessageBox(hWnd, L"최소 창의 갯수는 1개 입니다.", L"알림", MB_OK | MB_ICONINFORMATION);
                }
            }
            else if (id == IDM_EXIT) // "종료" 메뉴
            {
                DestroyWindow(hWnd); // 윈도우 파괴 (종료)
            }
        }
        break;
        
        case WM_TIMER: // 타이머 메시지 (주기적인 업데이트)
        {
            if (!g_dropdownActive) // 드롭다운이 열려있지 않을 때만 업데이트 진행
            {
                int cumulativeWidth = 0;             // 현재까지의 미리보기 슬롯들의 누적 너비
                int newWidths[MAX_SEGMENTS] = {0};   // 각 썸네일의 새로운 너비를 저장할 배열
                RECT destRect;                       // 썸네일이 그려질 목적지 사각형

                // 1. 현재 실행 중인 창 목록을 모든 콤보박스에 업데이트 (새 창 추가, 닫힌 창 제거)
                EnumWindows(EnumWindowsProc, (LPARAM)hWnd);

                // 2. 각 미리보기 슬롯에 대한 DWM 썸네일 업데이트 및 너비 계산
                for (int i = 0; i < g_numSegments; i++)
                {
                    int currentPreviewWidth = 0;   // 실제 썸네일이 그려질 너비
                    int currentPreviewHeight = 0;  // 실제 썸네일이 그려질 높이
                    
                    bool bThumbnailRegisteredThisCycle = false; // 이번 사이클에 썸네일이 새로 등록되었는지 여부

                    // 대상 창이 선택되어 있고 유효한 경우
                    if (g_Selected[i] && IsWindow(g_Selected[i]))
                    {
                        // 썸네일이 아직 등록되지 않았다면 등록 시도
                        if (!g_Thumbnails[i])
                        {
                            HRESULT hr = DwmRegisterThumbnail(hWnd, g_Selected[i], &g_Thumbnails[i]);
                            if (SUCCEEDED(hr)) {
                                bThumbnailRegisteredThisCycle = true; // 새로 등록됨
                            } else {
                                // 등록 실패 (예: 대상 창이 갑자기 유효하지 않게 됨), 선택되지 않은 상태로 처리
                                // 이 경우 DWM 썸네일이 생성되지 않으므로, 이 슬롯은 빈 상태처럼 동작해야 함.
                                g_Selected[i] = NULL; // 선택 해제 처리하여 아래 'no selection' 블록으로 이동
                            }
                        }

                        if (g_Thumbnails[i]) // 썸네일이 유효한 경우 (기존 또는 방금 등록)
                        {
                            SIZE srcSize = {};
                            if (SUCCEEDED(DwmQueryThumbnailSourceSize(g_Thumbnails[i], &srcSize)) && srcSize.cy > 0)
                            {
                                if (srcSize.cy <= PREVIEW_HEIGHT)
                                {
                                    currentPreviewHeight = srcSize.cy;
                                    currentPreviewWidth = srcSize.cx;
                                }
                                else
                                {
                                    double scale = (double)PREVIEW_HEIGHT / srcSize.cy;
                                    currentPreviewHeight = PREVIEW_HEIGHT;
                                    currentPreviewWidth = (int)std::round(srcSize.cx * scale);
                                }
                            }
                            else // 썸네일은 있으나 소스 크기 가져오기 실패 (예: 대상 창 최소화 또는 DWM 문제)
                            {
                                // 썸네일이 존재하지만 소스 크기를 가져올 수 없는 경우, 여전히 기본 비율 사용
                                currentPreviewHeight = PREVIEW_HEIGHT;
                                currentPreviewWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
                            }
                        }
                        else // g_Selected[i]는 있지만 g_Thumbnails[i]가 NULL인 경우 (방금 등록 실패한 경우 등)
                        {
                            // 선택된 창은 있지만 썸네일이 없는 경우, 기본 비율 사용
                            currentPreviewHeight = PREVIEW_HEIGHT;
                            currentPreviewWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
                        }
                    }
                    else // 콤보박스에 선택된 창이 없거나 유효하지 않은 경우
                    {
                        // 기존 썸네일이 있다면 해제 (WM_COMMAND에서 이미 해제되었을 가능성도 있음)
                        if (g_Thumbnails[i]) {
                            DwmUnregisterThumbnail(g_Thumbnails[i]);
                            g_Thumbnails[i] = NULL;
                        }
                        currentPreviewHeight = PREVIEW_HEIGHT;
                        currentPreviewWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
                    }

                    newWidths[i] = currentPreviewWidth; // 각 콤보박스 너비 조절에 사용될 너비 저장
                    
                    // 썸네일이 그려질 목적지 사각형 설정 (콤보박스 아래, 계산된 너비/높이)
                    destRect.left = cumulativeWidth;
                    destRect.top = DROP_HEIGHT;
                    destRect.right = cumulativeWidth + currentPreviewWidth;
                    destRect.bottom = DROP_HEIGHT + currentPreviewHeight;
                    
                    // DWM 썸네일 업데이트 조건 확인:
                    // 1. 썸네일 핸들이 유효하고 (즉, 대상 창이 선택됨)
                    // 2. 새로 등록되었거나 (bThumbnailRegisteredThisCycle)
                    // 3. 목적지 사각형이 이전과 달라졌을 때
                    if (g_Thumbnails[i] && (bThumbnailRegisteredThisCycle || !EqualRect(&destRect, &g_lastDestRects[i])))
                    {
                        // 잔여 이미지 문제 해결을 위해 일시적으로 숨김 후 재노출
                        DWM_THUMBNAIL_PROPERTIES propsHide = {}; // 모든 멤버를 0으로 초기화
                        propsHide.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
                        propsHide.fVisible = FALSE; // 썸네일을 숨김
                        propsHide.rcDestination = destRect; // 목적지 사각형 업데이트
                        DwmUpdateThumbnailProperties(g_Thumbnails[i], &propsHide);
                        
                        DWM_THUMBNAIL_PROPERTIES propsShow = {}; // 모든 멤버를 0으로 초기화
                        propsShow.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                                            DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
                        propsShow.fVisible = TRUE; // 썸네일을 다시 보이게 함
                        propsShow.fSourceClientAreaOnly = TRUE; // 클라이언트 영역만 표시
                        propsShow.opacity = 255; // 완전 불투명
                        propsShow.rcDestination = destRect; // 최종 목적지 사각형 설정
                        DwmUpdateThumbnailProperties(g_Thumbnails[i], &propsShow);

                        // 마지막으로 업데이트된 목적지 사각형 저장
                        g_lastDestRects[i] = destRect;
                    }
                    // 썸네일이 더 이상 없는데 g_lastDestRects에 이전 값이 남아있다면 초기화
                    else if (!g_Thumbnails[i] && (g_lastDestRects[i].left != 0 || g_lastDestRects[i].top != 0 || g_lastDestRects[i].right != 0 || g_lastDestRects[i].bottom != 0)) {
                        g_lastDestRects[i] = {}; // 모든 멤버를 0으로 초기화
                    }


                    cumulativeWidth += currentPreviewWidth; // 다음 썸네일의 시작 X 좌표 계산
                }

                // 3. 메인 윈도우의 전체 너비 조정 (썸네일들의 누적 너비에 맞춰)
                if (cumulativeWidth != g_windowWidth)
                {
                    g_windowWidth = cumulativeWidth;
                    RECT rcClient = {0, 0, g_windowWidth, g_windowHeight};
                    // 클라이언트 영역 크기에 맞춰 윈도우 실제 크기 계산 (타이틀바 없는 팝업 윈도우이므로 거의 동일)
                    AdjustWindowRect(&rcClient, GetWindowLong(hWnd, GWL_STYLE), FALSE);
                    int newW = rcClient.right - rcClient.left;
                    int newH = rcClient.bottom - rcClient.top;
                    RECT rc;
                    GetWindowRect(hWnd, &rc); // 현재 윈도우의 화면 좌표 가져오기
                    // 윈도우 크기만 변경 (위치 및 Z-오더는 유지)
                    SetWindowPos(hWnd, NULL, rc.left, rc.top, newW, newH,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }

                // 4. 콤보박스들의 위치 및 너비 조정
                int cumulativeX = 0;
                for (int i = 0; i < g_numSegments; i++)
                {
                    if (g_ComboBoxes[i])
                    {
                        // 콤보박스를 계산된 새 너비(newWidths[i])로 이동 및 크기 조절
                        MoveWindow(g_ComboBoxes[i], cumulativeX, 0, newWidths[i], DROP_HEIGHT, TRUE);
                        // 드롭다운 리스트의 너비도 콤보박스 너비에 맞춰 설정
                        SendMessage(g_ComboBoxes[i], CB_SETDROPPEDWIDTH, (WPARAM)newWidths[i], 0);
                    }
                    cumulativeX += newWidths[i]; // 다음 콤보박스의 시작 X 좌표 계산
                }

                // 5. 각 콤보박스의 항목(윈도우 타이틀) 업데이트
                for (int i = 0; i < g_numSegments; i++)
                {
                    if (g_ComboBoxes[i])
                    {
                        UpdateComboBoxItem(g_ComboBoxes[i]);
                    }
                }
            }
        }
        break;
        
        case WM_PAINT: // 윈도우 그리기 메시지 (더블 버퍼링 적용)
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps); // 윈도우 DC 가져오기
            
            HDC memDC = CreateCompatibleDC(hdc); // 메모리 DC 생성
            // 윈도우 크기와 호환되는 비트맵 생성 (더블 버퍼링 버퍼)
            HBITMAP memBMP = CreateCompatibleBitmap(hdc, g_windowWidth, g_windowHeight);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBMP); // 비트맵을 메모리 DC에 선택
            
            RECT rc;
            SetRect(&rc, 0, 0, g_windowWidth, g_windowHeight); // 그릴 영역 설정
            FillRect(memDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); // 배경을 검은색으로 채움
            
            SetStretchBltMode(memDC, HALFTONE); // 이미지 축소/확대 시 부드러운 렌더링 모드 설정
            // 메모리 DC의 내용을 실제 윈도우 DC로 복사 (더블 버퍼링의 최종 단계)
            BitBlt(hdc, 0, 0, g_windowWidth, g_windowHeight, memDC, 0, 0, SRCCOPY);
            
            SelectObject(memDC, oldBmp); // 원래 비트맵으로 복원
            DeleteObject(memBMP);        // 생성한 비트맵 파괴
            DeleteDC(memDC);             // 메모리 DC 파괴
            
            EndPaint(hWnd, &ps); // 그리기 완료
        }
        break;
        
        case WM_DESTROY: // 윈도우 파괴 시 정리 작업
        {
            KillTimer(hWnd, ID_TIMER); // 타이머 해제
            
            if (!g_resetRequested) // "초기화 후 종료"가 아닌 일반 종료인 경우에만 설정 저장
            {
                SaveSettings(hWnd);
            }
            
            // 모든 DWM 썸네일 핸들 해제
            for (int i = 0; i < MAX_SEGMENTS; i++)
            {
                if (g_Thumbnails[i])
                {
                    DwmUnregisterThumbnail(g_Thumbnails[i]);
                    g_Thumbnails[i] = NULL;
                }
            }
            DeleteObject(g_hFont); // 생성한 폰트 객체 파괴
            PostQuitMessage(0); // 메시지 루프 종료를 알림
        }
        break;
        
        default: // 그 외 메시지는 기본 윈도우 프로시저에 위임
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

//=============================================================================
// wWinMain: 프로그램의 유니코드 진입점
// - 클라이언트 영역 높이: DROP_HEIGHT + PREVIEW_HEIGHT = 325px
// - 전체 가로폭은 모든 미리보기 창의 누적 폭 (초기 g_windowWidth는 미리보기 개수 * 기본PreviewWidth)
// - 타이틀바 제거(WS_POPUP) 및 창 내용 드래그로 이동 기능 구현
//=============================================================================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance); // 사용되지 않는 매개변수
    UNREFERENCED_PARAMETER(lpCmdLine);     // 사용되지 않는 매개변수
    UNREFERENCED_PARAMETER(nCmdShow);      // 사용되지 않는 매개변수

    // 화면 해상도에 기반하여 미리보기 슬롯의 기본 너비와 최대 개수 계산
    int defaultPreviewWidth = (PREVIEW_HEIGHT * PREVIEW_ASPECT_RATIO_NUMERATOR) / PREVIEW_ASPECT_RATIO_DENOMINATOR;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN); // 주 모니터의 가로 해상도
    g_maxSegments = screenWidth / defaultPreviewWidth; // 화면 너비에 들어갈 수 있는 최대 미리보기 개수
    if (g_maxSegments < 1) // 최소 1개는 표시 가능하도록 보장
        g_maxSegments = 1;

    LoadStartupSettings(); // 레지스트리에서 저장된 미리보기 창 개수를 로드
    if (g_numSegments > g_maxSegments) // 로드된 개수가 화면의 최대 개수를 초과하면 조정
        g_numSegments = g_maxSegments;
    
    g_windowWidth = g_numSegments * defaultPreviewWidth; // 초기 메인 윈도우의 전체 클라이언트 가로폭 결정
    
    MSG msg;
    WNDCLASS wc = {}; // 모든 멤버를 0으로 초기화
    wc.style = CS_DBLCLKS; // 더블클릭 메시지(WM_LBUTTONDBLCLK) 수신 활성화
    wc.lpfnWndProc = WndProc; // 윈도우 프로시저 설정
    wc.hInstance = hInstance; // 인스턴스 핸들 설정
    wc.lpszClassName = TEXT("MultiWindowViewer"); // 윈도우 클래스 이름 설정
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); // 기본 커서 설정
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); // 배경 브러시 설정 (검은색)
    
    if (!RegisterClass(&wc)) // 윈도우 클래스 등록 실패 시 종료
        return -1;
    
    // 메인 윈도우 생성
    HWND hWnd = CreateWindowEx(
        WS_EX_APPWINDOW, // 작업 표시줄에 표시 (WS_POPUP과 함께 사용)
        TEXT("MultiWindowViewer"),
        TEXT("실시간 윈도우 모니터링"),
        WS_POPUP | WS_CLIPCHILDREN, // WS_POPUP: 타이틀바/테두리 없는 창, WS_CLIPCHILDREN: 자식 창이 부모 영역을 벗어나지 않도록 클립
        CW_USEDEFAULT, // 기본 X 위치
        CW_USEDEFAULT, // 기본 Y 위치
        g_windowWidth, // 초기 윈도우 클라이언트 너비
        g_windowHeight, // 초기 윈도우 클라이언트 높이
        NULL, // 부모 윈도우 없음
        NULL, // 메뉴 없음
        hInstance, // 인스턴스 핸들
        NULL); // 추가 생성 파라미터 없음
    
    if (!hWnd) // 윈도우 생성 실패 시 종료
        return -1;
    
    ShowWindow(hWnd, SW_SHOW); // 윈도우 표시
    UpdateWindow(hWnd);        // 윈도우 업데이트 (WM_PAINT 메시지 발생)

    if (g_alwaysOnTop) // "항상 위에" 설정이 활성화되어 있다면
    {
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); // 창을 최상단으로 설정
    }

    // 메시지 루프
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg); // 키보드 메시지 번역 (WM_KEYDOWN -> WM_CHAR 등)
        DispatchMessage(&msg);  // 윈도우 프로시저로 메시지 전달
    }
    
    return (int)msg.wParam; // 종료 코드 반환
}

// =============================================================================
// WinMain 래퍼 함수 (wWinMain을 호출하기 위해 링커 오류를 해결)
// MinGW 환경에서 wWinMain 진입점을 찾지 못할 때 또는 ANSI 빌드 시 사용될 수 있습니다.
// 유니코드 빌드 환경에서는 wWinMain이 직접 진입점이 됩니다.
// =============================================================================
#ifdef __cplusplus
extern "C" {
#endif

// wWinMain 함수의 프로토타입을 다시 선언하여 WinMain에서 호출할 수 있도록 합니다.
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