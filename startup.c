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

/**
 * $Id: startup.c,v 0.1 2006/07/06 23:39:43 diegocr Exp $
 */
//asm(".globl __start\njbsr __start\nrts");

#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/dos.h>
#include <workbench/startup.h>

/***************************************************************************/

struct Library * SysBase       = NULL;
struct Library * UtilityBase   = NULL;
struct Library * DOSBase       = NULL;
struct Library * SocketBase    = NULL;
struct Library * AsyncIOBase   = NULL;

GLOBAL LONG __main_interface ( VOID );

/***************************************************************************/

LONG _start( void )
{
	struct Task * me;
	LONG rc = RETURN_ERROR;
	UBYTE __dosname[] = "dos.library";
	
	SysBase = *(struct Library **) 4L;
	
	// This program MUST be run in DOS-Mode ;)
	if(!((struct Process *)(me=FindTask(NULL)))->pr_CLI)
		return(RETURN_WARN);
	
	if(!(DOSBase = OpenLibrary(__dosname, 39)))
	{
		if((DOSBase = OpenLibrary(__dosname, 0)))
		{
			PutStr("This program Requires AmigaOS 3.0 or higher\a\n");
			CloseLibrary( DOSBase );
		}
		return(RETURN_FAIL);
	}
	
	if((SocketBase = OpenLibrary("bsdsocket.library", 4)))
	{
		if((AsyncIOBase = OpenLibrary("asyncio.library", 40)))
		{
			if((UtilityBase = OpenLibrary("utility.library", 0)))
			{
						APTR stk;
						ULONG stkSize;
						
						stkSize = 32 + ((((ULONG)me->tc_SPUpper-(ULONG)me->tc_SPLower+STACK_SIZE) + 31) & ~31);
						
						if((stk = AllocMem( stkSize, MEMF_ANY )))
						{
							struct StackSwapStruct stackswap;
							
							stackswap.stk_Lower   = stk;
							stackswap.stk_Upper   = (ULONG)stk+stkSize;
							stackswap.stk_Pointer = (APTR)stackswap.stk_Upper - 32;
							
							StackSwap(&stackswap);
							
							rc = __main_interface ( /*_WBenchMsg*/ ) ;
							
							StackSwap(&stackswap);
							FreeMem( stk, stkSize );
						}
						else rc = ERROR_NO_FREE_STORE;
				
				CloseLibrary( UtilityBase );
			}
			
			CloseLibrary( AsyncIOBase );
		}
		else PutStr("This program requires asyncio.library v40 for better DISK-IO perfomance...\a\n");
		
		CloseLibrary( SocketBase );
	}
	else PutStr("No AmiTCP-like TCP/IP Stack detected\a\n");
	
	CloseLibrary( DOSBase );
	
	if( rc == RETURN_ERROR )
		rc = ERROR_INVALID_RESIDENT_LIBRARY;
	
	return(rc);
}
