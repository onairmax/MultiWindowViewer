원하는 윈도우 창들을 작은 화면으로 확인하려고 제작한 프로그램입니다.

C++ 하나도 모르지만 Copliot과 Gemini 2.5 Flash Preview 05-20의 전폭적인 도움을 받아 제작하였습니다.

제가 직접 코드를 작성하지 않고 오로지 복사 붙여넣기만을 한 결과물입니다.

![image](https://github.com/user-attachments/assets/7f651eac-222e-44ee-b834-b5c083bf9471)

프로그램 화면


![image](https://github.com/user-attachments/assets/492e8bbd-d148-4561-95de-d19c4cecac3b)

미리보기할 윈도우를 선택한 화면


사용법

상단의 콤보박스를 클릭하여 원하는 윈도우 창을 선택하면 해당 창의 내용이 작게 보여집니다.

창의 미리보기 영역에서 더블 클릭을 하면 해당 창이 활성화됩니다.

우클릭을 하면 컨택스트 메뉴가 뜨면서 "항상 위에", "부팅시 실행", "초기화 후 종료", "창 +1", "창 -1", "종료"를 선택 가능합니다.

"항상 위에"와 "부팅시 실행"은 체크 표시로 현재 설정 상태를 확인 가능하며

"창 +1"은 미리보기 창을 한개 추가 합니다. 우측 끝에 창이 하나 추가됩니다.

"창 -1"은 미리보기 창을 한개 제거 합니다. 우측 끝 창이 제거됩니다.

"초기화 후 종료"는 프로그램이 생성하고 저장한 레지스트리 값을 모두 제거한 후 프로그램을 종료합니다.

프로그램이 생성하는 레지스트리는 아래와 같습니다.

\HKEY_CURRENT_USER\Software\MultiWindowViewer

AlwaysOnTop

PreviewCount

WindowLeft

WindowTop


\HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run

위 경로에 부팅시 실행 등록합니다.


창 안의 아무 위치에서 드래그를 하면 창의 위치를 이동시킬수 있습니다.
