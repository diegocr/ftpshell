/* ***** BEGIN LICENSE BLOCK *****
 * Version: GPL License
 * 
 * Copyright (C) 2006 Diego Casorran <dcasorran@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>
 * 
 * ***** END LICENSE BLOCK ***** */


#ifdef DEBUG
# include <proto/exec.h>
# include <stdarg.h>
# include <SDI_compiler.h>

#ifndef RawPutChar
#define RawPutChar(c) ({ \
  ULONG _RawPutChar_c = (c); \
  { \
  register struct Library * const __RawPutChar__bn __asm("a6") = SysBase;\
  register ULONG __RawPutChar_c __asm("d0") = (_RawPutChar_c); \
  __asm volatile ("jsr a6@(-516:W)" \
  : \
  : "r"(__RawPutChar__bn), "r"(__RawPutChar_c) \
  : "d0", "d1", "a0", "a1", "fp0", "fp1", "cc", "memory"); \
  } \
})
#endif

VOID KPutC(UBYTE ch)
{
	RawPutChar(ch);
}

VOID KPutStr(CONST_STRPTR string)
{
	UBYTE ch;
	
	while((ch = *string++))
		KPutC( ch );
}

STATIC VOID ASM RawPutC(REG(d0,UBYTE ch))
{
	KPutC(ch); 
}

VOID KPrintF(CONST_STRPTR fmt, ...)
{
	va_list args;
	
	va_start(args,fmt);
	RawDoFmt((STRPTR)fmt, args,(VOID (*)())RawPutC, NULL );
	va_end(args);
}

#endif /* DEBUG */
