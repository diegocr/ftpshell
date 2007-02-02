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


#ifndef DEBUG_H
#define DEBUG_H

#include <clib/debug_protos.h>

#ifdef DEBUG
static __inline void __dEbuGpRInTF( CONST_STRPTR func_name, CONST LONG line )
{
	KPutStr( func_name );
	KPrintF(",%ld: ", line );
}
# define DBG( fmt... ) \
({ \
	__dEbuGpRInTF(__PRETTY_FUNCTION__, __LINE__); \
	KPrintF( fmt ); \
})
# define DBG_POINTER( ptr )	DBG("%s = 0x%08lx\n", #ptr, (long) ptr )
# define DBG_VALUE( val )	DBG("%s = 0x%08lx,%ld\n", #val,val,val )
# define DBG_STRING( str )	DBG("%s = 0x%08lx,\"%s\"\n", #str,str,str)
# define DBG_ASSERT( expr )	if(!( expr )) DBG(" **** FAILED ASSERTION '%s' ****\n", #expr )
# define DBG_EXPR( expr, bool )						\
({									\
	BOOL res = (expr) ? TRUE : FALSE;				\
									\
	if(res != bool)							\
		DBG("Failed %s expression for '%s'\n", #bool, #expr );	\
									\
	res;								\
})
#else
# define DBG(fmt...)		((void)0)
# define DBG_POINTER( ptr )	((void)0)
# define DBG_VALUE( ptr )	((void)0)
# define DBG_STRING( str )	((void)0)
# define DBG_ASSERT( expr )	((void)0)
# define DBG_EXPR( expr, bool )	expr
#endif

#define DBG_TRUE( expr )	DBG_EXPR( expr, TRUE )
#define DBG_FALSE( expr )	DBG_EXPR( expr, FALSE)

#endif /* DEBUG_H */
