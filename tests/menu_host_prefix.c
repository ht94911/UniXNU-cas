// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define STATIC static
#define VOID void
#define CONST const
#define IN
#define OUT
#define EFIAPI
#define TRUE 1
#define FALSE 0
typedef size_t UINTN;typedef intptr_t INTN;typedef uint32_t UINT32;typedef int32_t INT32;typedef uint16_t CHAR16;typedef char CHAR8;typedef int BOOLEAN;typedef uint64_t EFI_STATUS;typedef void *EFI_HANDLE;typedef void *EFI_EVENT;typedef int EFI_GUID;
typedef uint32_t EFI_GRAPHICS_OUTPUT_BLT_PIXEL;
typedef struct{UINTN ScanCode;CHAR16 UnicodeChar;}EFI_INPUT_KEY;
typedef struct _INPUT{EFI_STATUS (*ReadKeyStroke)(struct _INPUT *,EFI_INPUT_KEY *);EFI_EVENT WaitForKey;}INPUT;
typedef struct{INPUT *ConIn;}SYSTEM;
typedef struct{EFI_STATUS (*LocateHandleBuffer)(UINTN,EFI_GUID*,VOID*,UINTN*,EFI_HANDLE**);EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID*,VOID**);EFI_STATUS (*CreateEvent)(UINTN,UINTN,VOID*,VOID*,EFI_EVENT*);EFI_STATUS (*SetTimer)(EFI_EVENT,UINTN,uint64_t);EFI_STATUS (*WaitForEvent)(UINTN,EFI_EVENT*,UINTN*);EFI_STATUS (*CloseEvent)(EFI_EVENT);}SERVICES;
static SYSTEM System,*gST=&System;static SERVICES Services,*gBS=&Services;static INPUT Input;
static EFI_GUID gEfiSimpleTextInProtocolGuid,gEfiDevicePathProtocolGuid;
#define EFI_ERROR(S) ((S)!=0)
#define ByProtocol 0
#define EFI_WHITE 1
#define EFI_BACKGROUND_RED 2
#define EFI_LIGHTGRAY 3
#define EFI_BACKGROUND_BLACK 4
#define EVT_TIMER 1
#define TPL_CALLBACK 1
#define TimerPeriodic 1
#define TimerCancel 0
#define EFI_TIMER_PERIOD_MILLISECONDS(N) (N)
#define SCAN_UP 1
#define SCAN_DOWN 2
#define SCAN_HOME 3
#define SCAN_END 4
#define SCAN_RIGHT 5
#define CHAR_CARRIAGE_RETURN 13
#define CHAR_LINEFEED 10
#define MIN(A,B) ((A)<(B)?(A):(B))
#define MAX(A,B) ((A)>(B)?(A):(B))
#define UNIXNU_GLYPH_HEIGHT 16
#define UNIXNU_GLYPH_WIDTH 8
#define PORTRAIT_TEXT_MAX 128
enum{UniXNUUiText,UniXNUUiGraphics};
typedef struct{CHAR16 *Title;CHAR8 *Id;}UNIXNU_ENTRY;
typedef struct{UINTN EntryCount,DefaultIndex,FallbackTimeout,RotateSeconds;INTN Timeout;UNIXNU_ENTRY *Entries;}UNIXNU_CONFIG;
typedef struct{UINTN Mode,Scale,Width,Height,Margin,CardHeight,MenuTop,RowHeight,MenuLeft,MenuWidth,VisibleRows,FirstVisible;BOOLEAN Portrait;CHAR16 Notice[96];}UNIXNU_UI;
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAccent=0x10,mRow=0x20,mMarker=0x30,mBadgeOn=0x40,mBadge=0x50,mText=0x60,mDim=0x70;
static int Drawn[64],Highlight[64],BoundsError,Physical;static const char *Script;static UINTN Position,WaitCount;
static UINTN StrLen(CONST CHAR16 *P){UINTN N=0;while(P[N])N++;return N;}
static UINTN UnicodeSPrint(CHAR16 *Out,UINTN Bytes,CONST CHAR16 *Format,...){va_list A;UINTN N=0;unsigned Value=0;char Digits[24];UINTN D=0;
 va_start(A,Format);if(Format[0]=='%' && Format[1]=='u')Value=va_arg(A,unsigned);else if(Format[0]==' '){(void)va_arg(A,int);Value=va_arg(A,unsigned);}else{while(Format[N] && (N+1)*2<Bytes){Out[N]=Format[N];N++;}Out[N]=0;va_end(A);return N;}
 do{Digits[D++]=(char)('0'+Value%10);Value/=10;}while(Value);while(D && (N+1)*2<Bytes)Out[N++]=(CHAR16)Digits[--D];Out[N]=0;va_end(A);return N;
}
static UINTN FooterScale(UNIXNU_UI *Ui){(void)Ui;return 1;}
static CONST CHAR16 *TypeTag(UNIXNU_ENTRY *E){(void)E;return L"efi";}
static VOID FitText(CONST CHAR16 *S,UINTN Max,CHAR16 *Out){UINTN I=0;while(S[I] && I<Max && I<PORTRAIT_TEXT_MAX){Out[I]=S[I];I++;}Out[I]=0;}
static VOID FillRect(UNIXNU_UI *Ui,UINTN X,UINTN Y,UINTN Width,UINTN Height,EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color){UINTN Slot;(void)X;
 if((Ui->Portrait && Width==Ui->Width-2*Ui->Margin && Height==Ui->CardHeight) || (!Ui->Portrait && Width==Ui->MenuWidth && Height==Ui->RowHeight-Ui->Scale)){
  if(Y<Ui->MenuTop || (Y-Ui->MenuTop)%Ui->RowHeight){BoundsError++;return;}Slot=(Y-Ui->MenuTop)/Ui->RowHeight;if(Slot>=Ui->VisibleRows){BoundsError++;return;}Highlight[Slot]=Color==mAccent;
 }
}
static VOID DrawText(UNIXNU_UI *Ui,UINTN X,UINTN Y,CONST CHAR16 *Text,UINTN Scale,EFI_GRAPHICS_OUTPUT_BLT_PIXEL Fore,EFI_GRAPHICS_OUTPUT_BLT_PIXEL Back,UINTN Width){UINTN Slot,N=0;(void)X;(void)Scale;(void)Fore;(void)Back;(void)Width;
 if(Text[0]<'0'||Text[0]>'9')return;if(Y<Ui->MenuTop){BoundsError++;return;}Slot=(Y-Ui->MenuTop)/Ui->RowHeight;
 while(*Text>='0' && *Text<='9')N=N*10+(*Text++-'0');if(Slot>=Ui->VisibleRows){BoundsError++;return;}Drawn[Slot]=(int)N;
}
static VOID TextPrintAt(UNIXNU_UI *Ui,UINTN Y,UINTN Color,CONST CHAR16 *Text){UINTN N=0,Slot=Y-Ui->MenuTop;while(*Text>='0' && *Text<='9')N=N*10+(*Text++-'0');if(Slot>=Ui->VisibleRows){BoundsError++;return;}Drawn[Slot]=(int)N;Highlight[Slot]=Color==(EFI_WHITE|EFI_BACKGROUND_RED);}
static VOID UniXNUUiDrawStatus(UNIXNU_UI *Ui,UINTN L,CONST CHAR16 *S){(void)Ui;(void)L;(void)S;}
static VOID UniXNUUiDrawFrame(UNIXNU_UI *Ui,UNIXNU_CONFIG *Cfg){(void)Ui;(void)Cfg;memset(Drawn,0,sizeof(Drawn));memset(Highlight,0,sizeof(Highlight));}
static VOID UniXNULog(CONST CHAR8 *F,...){(void)F;}
static VOID FreePool(VOID *P){free(P);}
static EFI_STATUS Locate(UINTN Type,EFI_GUID *Guid,VOID *Reg,UINTN *Count,EFI_HANDLE **Handles){(void)Type;(void)Guid;(void)Reg;*Count=Physical?1:0;*Handles=malloc(sizeof(EFI_HANDLE));(*Handles)[0]=(void *)1;return 0;}
static EFI_STATUS Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){(void)H;(void)G;*P=(void *)1;return 0;}
static EFI_STATUS Create(UINTN T,UINTN P,VOID *F,VOID *C,EFI_EVENT *E){(void)T;(void)P;(void)F;(void)C;*E=(void *)2;return 0;}
static EFI_STATUS Timer(EFI_EVENT E,UINTN T,uint64_t N){(void)E;(void)T;(void)N;return 0;}
static EFI_STATUS Wait(UINTN N,EFI_EVENT *E,UINTN *Which){(void)N;(void)E;if(++WaitCount>1000)abort();*Which=Script[Position]?1:0;return 0;}
static EFI_STATUS Close(EFI_EVENT E){(void)E;return 0;}
static EFI_STATUS Read(INPUT *I,EFI_INPUT_KEY *K){char C=Script[Position++];(void)I;K->ScanCode=0;K->UnicodeChar=0;
 if(C=='d')K->ScanCode=SCAN_DOWN;else if(C=='u')K->ScanCode=SCAN_UP;else if(C=='H')K->ScanCode=SCAN_HOME;else if(C=='N')K->ScanCode=SCAN_END;else if(C=='E')K->UnicodeChar=13;else K->UnicodeChar=(CHAR16)C;return 0;
}
