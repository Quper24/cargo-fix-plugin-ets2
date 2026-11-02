#include "menu_pointer.h"
#include <cstdlib>
void Check(bool ok) { if(!ok) std::abort(); }
int main() {
    MenuPointer cursor;cursor.Begin(1000,700);
    DIMOUSESTATE2 data{};data.lX=120;data.lY=-80;data.lZ=120;data.rgbButtons[0]=0x80;
    Check(cursor.State(&data,sizeof(data)));
    Check(cursor.x==620 && cursor.y==270 && cursor.buttons[0] && cursor.wheel==1);
    Check(data.lX==0 && data.lY==0 && !data.rgbButtons[0]); // No movement/click reaches the camera.
    // The OS cursor may be reset to centre every frame; this model never reads it.
    for(int frame=0;frame<10;++frame) { data.lX=10;Check(cursor.State(&data,sizeof(data))); }
    Check(cursor.x==720 && cursor.y==270 && !cursor.buttons[0]);
    data.lX=99999;data.lY=-99999;cursor.State(&data,sizeof(data));Check(cursor.x==999 && cursor.y==0);
    cursor.Resize(200,100);Check(cursor.x==199 && cursor.y==0);
    data.lX=5;cursor.State(&data,sizeof(data),false);Check(cursor.x==199 && data.lX==0); // Secondary input path must not double-count.
    DIMOUSESTATE old{};old.lX=-20;old.lY=40;cursor.State(&old,sizeof(old));Check(cursor.x==179 && cursor.y==40 && old.lX==0);
    DIDEVICEOBJECTDATA event{};event.dwOfs=DIMOFS_X;event.dwData=static_cast<DWORD>(-100);cursor.Event(event);Check(cursor.x==79 && event.dwData==0);
    event.dwOfs=DIMOFS_BUTTON0;event.dwData=0x80;cursor.Event(event);Check(cursor.buttons[0]);
    event.dwData=0;cursor.Event(event);Check(!cursor.buttons[0]);
    event.dwOfs=DIMOFS_X;event.dwData=50;cursor.Event(event,false);Check(cursor.x==79 && event.dwData==0); // PEEK is not consumed twice.
    Check(!cursor.State(&old,3));
}
