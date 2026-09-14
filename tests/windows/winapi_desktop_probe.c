#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
static FILE *log_file;
static unsigned frames;
static void event(const char *kind, long a, long b){fprintf(log_file,"{\"kind\":\"%s\",\"a\":%ld,\"b\":%ld}\n",kind,a,b);fflush(log_file);}
static LRESULT CALLBACK procedure(HWND h,UINT m,WPARAM w,LPARAM l){
 switch(m){
 case WM_PAINT:{PAINTSTRUCT p;HDC dc=BeginPaint(h,&p);RECT r;GetClientRect(h,&r);HBRUSH bg=CreateSolidBrush(RGB(37,99,235));FillRect(dc,&r,bg);DeleteObject(bg);RECT marker={1000+(frames*9)%600,80,1080+(frames*9)%600,220};HBRUSH fg=CreateSolidBrush(RGB(245,158,11));FillRect(dc,&marker,fg);DeleteObject(fg);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));TextOutA(dc,60,70,"LanDesk Windows PE synthetic desktop (Wine test)",49);EndPaint(h,&p);return 0;}
 case WM_TIMER:frames++;InvalidateRect(h,NULL,FALSE);return 0;
 case WM_KEYDOWN:case WM_SYSKEYDOWN:event("key_down",w,l);break;
 case WM_KEYUP:case WM_SYSKEYUP:event("key_up",w,l);break;
 case WM_LBUTTONDOWN:event("mouse_down",(short)LOWORD(l),(short)HIWORD(l));break;
 case WM_LBUTTONUP:event("mouse_up",(short)LOWORD(l),(short)HIWORD(l));break;
 case WM_MOUSEWHEEL:event("wheel",(short)HIWORD(w),0);break;
 case WM_DESTROY:PostQuitMessage(0);return 0;
 }
 return DefWindowProcA(h,m,w,l);
}
int main(int argc,char **argv){
 if(argc!=2)return 2;log_file=fopen(argv[1],"w");if(!log_file)return 3;SetProcessDPIAware();
 WNDCLASSA c={0};c.lpfnWndProc=procedure;c.hInstance=GetModuleHandle(NULL);c.lpszClassName="LanDeskWinApiDesktopProbe";RegisterClassA(&c);
 HWND h=CreateWindowA(c.lpszClassName,"LanDesk Windows synthetic desktop",WS_OVERLAPPEDWINDOW|WS_VISIBLE,50,50,1800,950,NULL,NULL,c.hInstance,NULL);if(!h)return 4;
 SetForegroundWindow(h);SetFocus(h);UpdateWindow(h);SetTimer(h,1,16,NULL);
 POINT origin={0,0};ClientToScreen(h,&origin);fprintf(log_file,"{\"kind\":\"ready\",\"origin\":[%ld,%ld],\"screen\":[%d,%d]}\n",origin.x,origin.y,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));fflush(log_file);
 MSG message;while(GetMessage(&message,NULL,0,0)>0){TranslateMessage(&message);DispatchMessage(&message);}fclose(log_file);return 0;
}
