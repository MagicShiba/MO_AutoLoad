set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
gcc -O2 -Wall -Wextra -ffreestanding -shared -o dist/AutoLoad.dll src/AutoLoad.c ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--entry,DllMain
  
set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
gcc -O2 -Wall -Wextra -ffreestanding -shared -o dist/AutoLoad_nolog.dll src/AutoLoad.c ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--entry,DllMain -DAUTOLOAD_NOLOG