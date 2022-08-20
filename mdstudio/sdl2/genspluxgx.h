
#ifndef _MAIN_H_
#define _MAIN_H_

#define MAX_INPUTS 8

#include <Windows.h>

#define		strcasecmp	_stricmp
#define		strncasecmp	_strnicmp
#define		snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)
#define		__func__	__FUNCTION__

#define eInputUp		0
#define eInputDown		1
#define eInputLeft		2
#define eInputRight		3
#define eInputB			4
#define eInputC			5
#define eInputA			6
#define eInputStart		7
#define eInputZ			8
#define eInputY			9
#define eInputX			10
#define eInputMode		11
#define eInput_COUNT	12

#ifdef __cplusplus
extern "C" {
#endif

	extern int debug_on;
	extern int log_error;
	extern int sdl_input_update(void);

	extern int		Init(int width, int height, void* parent, int pal, char region, int use_gamepad);
	extern void		SetWindowPosition(int x, int y);
	extern int		GetWindowXPosition();
	extern int		GetWindowYPosition();
	extern void		BringToFront();
	extern int		LoadRom(const char* path);
	extern int		Reset();
	extern void		SoftReset();
	extern int		Shutdown();

	extern void		ShowSDLWindow();
	extern void		HideSDLWindow();

	extern int		AddBreakpoint(int addr);
	extern void		ClearBreakpoint(int addr);
	extern void		ClearBreakpoints();
	extern int		AddWatchpoint(int fromAddr, int toAddr);
	extern void		ClearWatchpoint(int fromAddr);
	extern void		ClearWatchpoints();

	extern int		StepInto();
	extern int		Resume();
	extern int		Break();
	extern int		IsDebugging();
	extern unsigned int* GetProfilerResults(int* instructionCount);
	extern unsigned int GetInstructionCycleCount(unsigned int address);

	extern int		Update();

	extern int		GetDReg(int index);
	extern int		GetAReg(int index);
	extern int		GetSR();
	extern int		GetCurrentPC();
	extern int		GetZ80Reg(int index);
	extern unsigned char	ReadByte(unsigned int address);
	extern unsigned short	ReadWord(unsigned int address);
	extern unsigned int		ReadLong(unsigned int address);
	extern void		ReadMemory(unsigned int address, unsigned int size, BYTE* memory);
	extern unsigned char	ReadZ80Byte(unsigned int address);

	extern void		SetInputMapping(int input, int mapping);
	extern int		GetInputMapping(int input);

	extern int		GetPaletteEntry(int index);

	extern unsigned char GetVDPRegisterValue(int index);

	extern unsigned int Disassemble(unsigned int address, char* text);
	extern int CleanupBreakpoints(unsigned int* addresses);

	extern void SetVolume(int vol, int isdebugVol);
	extern void PauseAudio(int pause);
	extern unsigned char* GetVRAM();

#ifdef __cplusplus
}
#endif
#endif /* _MAIN_H_ */
