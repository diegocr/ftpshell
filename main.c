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


#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/socket.h>
#include <proto/asyncio.h>
#include <SDI_compiler.h>
#include "debug.h"
#include <stdarg.h>
#include <netdb.h>
#include <devices/conunit.h>

#if 0
 /* I need this while compiled using "-O3 -funroll-loops -msmall-code" due:
  * /t/cc4bDrSC.s:25223: Error: Trying to force a pcrel thing into absolute mode while in small code mode
  * (and this happens after adding STOR (put command) feature ONLY)
  */
#undef INLINE
#define INLINE STATIC
#endif

// those are used at i64_inline.h
#define isspace(ch)	((ch) == 0x20)
#define isdigit(ch)	(((ch) >= '0') && ((ch) <= '9'))
#define strncpy( dst, src, len )	\
	({CopyMem( src, dst, len ); dst[len] = 0;})

#define time2ds( time )							\
({									\
	struct DateStamp ds;						\
	ULONG r = (ULONG) time;						\
									\
	ds.ds_Days   = (r / 86400);					\
	ds.ds_Minute = ( (r % 86400) / 60);				\
	ds.ds_Tick   = ( ((r % 86400) % 60) * TICKS_PER_SECOND);	\
									\
	&ds;								\
})
#define ds2time()					\
({							\
	struct DateStamp ds;				\
	ULONG __result;					\
							\
	DateStamp(&ds);					\
							\
	__result= ds.ds_Days * 86400			\
		+ ds.ds_Minute * 60 			\
		+ (ds.ds_Tick / TICKS_PER_SECOND);	\
							\
	__result;					\
})

/* this macro lets us long-align structures on the stack */
#define D_S(type,name)	\
	char a_##name[ sizeof( type ) + 3 ];	\
	type *name = ( type * ) ( ( LONG ) ( a_##name + 3 ) & ~3 )

#ifndef FIB_IS_FILE
# define FIB_IS_FILE(fib)      ((fib)->fib_DirEntryType <  0)
#endif
#ifndef FIB_IS_DRAWER
# define FIB_IS_DRAWER(fib)    ((fib)->fib_DirEntryType >= 0 && \
                               (fib)->fib_DirEntryType != ST_SOFTLINK)
#endif
#ifndef FIB_IS_LINK
# define FIB_IS_LINK(fib)      ((fib)->fib_DirEntryType == ST_SOFTLINK || \
                               (fib)->fib_DirEntryType == ST_LINKDIR || \
                               (fib)->fib_DirEntryType == ST_LINKFILE)
#endif
#ifndef FIB_IS_SOFTLINK
# define FIB_IS_SOFTLINK(fib)  ((fib)->fib_DirEntryType == ST_SOFTLINK)
#endif
#ifndef FIB_IS_LINKDIR
# define FIB_IS_LINKDIR(fib)   ((fib)->fib_DirEntryType == ST_LINKDIR)
#endif
#ifndef EAD_IS_FILE
# define EAD_IS_FILE(ead)    ((ead)->ed_Type <  0)
#endif
#ifndef EAD_IS_DRAWER
# define EAD_IS_DRAWER(ead)  ((ead)->ed_Type >= 0 && \
                             (ead)->ed_Type != ST_SOFTLINK)
#endif
#ifndef EAD_IS_LINK
# define EAD_IS_LINK(ead)   ((ead)->ed_Type == ST_SOFTLINK || \
                             (ead)->ed_Type == ST_LINKDIR || \
                             (ead)->ed_Type == ST_LINKFILE)
#endif
#define MIN(a,b)		\
({				\
	typeof(a) _a = (a);	\
	typeof(b) _b = (b);	\
				\
	(_a > _b) ? _b : _a;	\
})

#define CLOSESOCKET( sfd )			\
	do {					\
		if( sfd != -1 )			\
		{				\
			shutdown( sfd, 2 );	\
			CloseSocket( sfd );	\
			sfd = -1;		\
		}				\
	} while(0)

#define CONSPEED		1024
#define ETHSPEED		(CONSPEED * 100)
#define STOR_MAXBUFSIZ		MIN(site->recv_buffer_size,ETHSPEED)

static const unsigned char __VerID[] = 
	"$VER:ftpshell " VERSION " (c)2006 Diego Casorran - All rights reserved.\r\n";

/*****************************************************************************/
/* most common C runtime library functions, as this prog uses -nostdlib */

struct RawDoFmtStream {
	
	STRPTR Buffer;
	LONG Size;
};

static void RawDoFmtChar( REG(d0,UBYTE c), REG(a3,struct RawDoFmtStream *s))
{
	if(s->Size > 0)
	{
		*(s->Buffer)++ = c;
		 (s->Size)--;
		
		if(s->Size == 1)
		{
			*(s->Buffer)	= '\0';
			  s->Size	= 0;
		}
	}
}

STATIC LONG vsnprintf(STRPTR outbuf, LONG size, CONST_STRPTR fmt, va_list args)
{
	long rc = 0;
	
	if((size > (long)sizeof(char)) && (outbuf != NULL))
	{
		struct RawDoFmtStream s;
		
		s.Buffer = outbuf;
		s.Size	 = size;
		
		RawDoFmt( fmt, (APTR)args, (void (*)())RawDoFmtChar, (APTR)&s);
		
		if((rc = ( size - s.Size )) != size)
			--rc;
	}
	
	return(rc);
}

STATIC LONG snprintf( STRPTR outbuf, LONG size, CONST_STRPTR fmt, ... )
{
	va_list args;
	long rc;
	
	va_start (args, fmt);
	rc = vsnprintf( outbuf, size, fmt, args );
	va_end(args);
	
	return(rc);
}

STATIC ULONG strlen(const char *string)
{ const char *s=string;
  
  if(!(string && *string))
  	return 0;
  
  do;while(*s++); return ~(string-s);
}

STATIC STRPTR strchr(CONST_STRPTR s,UBYTE c)
{
	while( *s != c )
	{	if(!*s++)
		{
			s = (char *)0;
			break;
		}
	}
	return (STRPTR)s;
}

STATIC STRPTR StrDup(STRPTR str)
{
	STRPTR new = NULL;

	if(str != NULL)
	{
		ULONG len = strlen(str);
		
		if((new = AllocVec(len+4, MEMF_PUBLIC)))
		{
			CopyMem( str, new, len );
			new[len] = 0;
		}
	}
	
	return new;
}

STATIC void bzero( void *data, size_t fsize )
{
	register unsigned long * uptr = (unsigned long *) data;
	register unsigned char * sptr;
	long size = (long) fsize;
	
	// first blocks of 32 bits
	while(size >= (long)sizeof(ULONG))
	{
		*uptr++ = 0;
		size -= sizeof(ULONG);
	}
	
	sptr = (unsigned char *) uptr;
	
	// now any pending bytes
	while(size-- > 0)
		*sptr++ = 0;
}

STATIC LONG GetInputString( STRPTR outbuf, int outlen, BOOL disable_echo )
{
	int chs = 0;
	UBYTE ch,*ptr=outbuf;
	LONG rc = -1;
	
	if( disable_echo )
	{
		ULONG crs = 0x9B302070;
		Write(Input(), (STRPTR) &crs, sizeof(ULONG));
		SetMode(Input(), 1);
	}
	
	do {
		if(!WaitForChar(Input(), 40000000))
			goto done;
		
		if((++chs > outlen) || (Read(Input(), &ch, 1) != 1))
		{	rc = 1; goto done;	}
		
		if((ch == '\r') || (ch == '\n'))
			break;
		
		*ptr++ = (UBYTE) ch;
		
	} while(1);
	
	if( disable_echo )
	{
		ULONG crs = 0x9B207000;
		Write(Input(), (STRPTR) &crs, sizeof(ULONG)-1);
		SetMode(Input(), 0);
		FPutC( Output(), '\n');
		Flush( Output() );
	}
	
	rc = *ptr = 0;
	
done:
	return rc;
}

STATIC BPTR CreateDirTreeSub(STRPTR path)
{
	BPTR lock;
	
	DBG_STRING(path);
	
	if ((lock = Lock(path, ACCESS_READ)) == 0)
	{
		STRPTR p = PathPart(path);
		TEXT   z = *p;

		*p = 0;
		lock = CreateDirTreeSub(path);
		*p = z;

		if (lock != 0)
		{
			UnLock(lock);
			lock = CreateDir(path);
		}
	}

	return lock;
}

STATIC BPTR CreateDirTree(CONST_STRPTR path)
{
	TEXT temp[4096];
	
	//strcpy(temp, path);
	CopyMem((APTR) path, temp, sizeof(temp));
	return CreateDirTreeSub(temp);
}

STATIC LONG MakeDir( STRPTR fullpath )
{
	UBYTE subdir[4096];
	char *sep, *xpath=(char *)fullpath;
	LONG rc = 0;
	
	sep = (char *) fullpath;
	sep = strchr(sep, '/');
	
	while( sep )
	{
		BPTR dlock;
		int len;
		
		len = sep - xpath;
		CopyMem( xpath, subdir, len);
		subdir[len] = 0;
		
	//	DBG(" +++ Creating Directory \"%s\"...\n", subdir );
		
		if((dlock = CreateDir( subdir )))
			UnLock( dlock );
		else
		{
			if((rc = IoErr()) == ERROR_OBJECT_EXISTS)
			{
				dlock = Lock( subdir, SHARED_LOCK );
				
				if( !dlock )
				{
					/* this can't happend!, I think.. */
					DBG("\a *** LOCK FAILED\n");
					if((rc = IoErr())==0)
						rc = ERROR_INVALID_LOCK;
				}
				else
				{
					struct FileInfoBlock fib;
					
					if(Examine(dlock,&fib) == DOSFALSE)
					{
						DBG("\a **** Examine FAILED\n");
						rc = IoErr();
					}
					else
					{
						if(fib.fib_DirEntryType > 0)
							rc = 0;
						
						#ifdef DEBUG
						if((rc != 0) || fib.fib_DirEntryType == ST_SOFTLINK)
						{
							DBG("\aDirectory Name exists and %spoint to a file !!!!\n", ((fib.fib_DirEntryType == ST_SOFTLINK) ? "MAY ":""));
						}
						#endif
					}
					
					UnLock( dlock );
				}
			}
			
			if( rc != 0 )
			{
				DBG("\a ERROR: Directory '%s' will be missing\n", fullpath );
				break;
			}
		}
		
		sep = strchr(sep+1, '/');
	}
	
	return(rc);
}

INLINE LONG ConsoleWidth(BPTR fd)
{
	struct FileHandle *fh;
	LONG result = 80;
	
	if((fh = BADDR(fd))->fh_Type != NULL)
	{
		char __id[sizeof(struct InfoData) + 3];
		struct InfoData *id = 
			(struct InfoData *)(((long)__id + 3L) & ~3L);
		
		if(DoPkt(fh->fh_Type,ACTION_DISK_INFO,((ULONG)id)>>2,0,0,0,0))
		{
			struct IOStdReq *ios;
			
			if((ios = (struct IOStdReq *) id->id_InUse) && !((int)ios & 1))
			{
				if(ios->io_Unit && ((struct ConUnit *) ios->io_Unit)->cu_Window == ((struct Window *) id->id_VolumeNode))
				{
					result = ((struct ConUnit *) ios->io_Unit)->cu_XMax + 1;
				}
			}
		}
	}
	
	return result;
}

STATIC STRPTR ioerrstr( STRPTR prompt )
{
	STATIC UBYTE buffer[82];
	
	Fault( IoErr(), prompt, buffer, sizeof(buffer) - 1 );
	
	return (STRPTR) buffer;
}

STATIC VOID GetTimeOfDay(struct timeval *tv)
{
	struct DateStamp t;
	DateStamp(&t); /* Get timestamp */
#ifdef UNIX_EPOCH
	tv->tv_sec=((t.ds_Days+2922)*1440+t.ds_Minute)*60+t.ds_Tick/TICKS_PER_SECOND;
#else
	tv->tv_sec=t.ds_Days*86400 + t.ds_Minute * 60 + (t.ds_Tick/TICKS_PER_SECOND);
#endif
	tv->tv_usec=(t.ds_Tick%TICKS_PER_SECOND)*1000000/TICKS_PER_SECOND;
}

/*****************************************************************************/
#include "i64_inline.h"

#define RECV_BUFFER_SINGLE	262144
#define RECV_BUFFER_SLOTS	4
#define RECV_BUFFER_SIZE	(RECV_BUFFER_SINGLE * RECV_BUFFER_SLOTS)

typedef enum { DLT_FILE, DLT_DIR, DLT_LINK, DLT_UNKNOWN } DirListType;

struct dirlist
{
	struct dirlist * prev;
	struct dirlist * next;
	
	DirListType dlt;
	char	FileName[108]; /* Null terminated. Max 30 chars used for now */
	LONG	Protection;    /* bit mask of protection, rwxd are 3-0.	   */
	bigint	Size;	     /* Number of bytes in file */
	ULONG time;
};

struct transfer_error	/* files which failed to download or upload */
{
	struct transfer_error * next;
	
	STRPTR file, reason;
	bigint transfered, total;
};

struct ftp_site
{
	STRPTR host, user, pass, home, path;
	UWORD port;
	
	STRPTR recv_buffer;
	LONG recv_buffer_size;
	
	LONG sockfd, data_sockfd;
	
	struct dirlist * pwd;
	bigint pwd_bytes;
	ULONG pwd_files, pwd_dirs;
	
	LONG ftprc; // last code returned by ftp_recv()
	
	// statistics (bps/bytes/etc) for put/get command
	struct {
		struct {
			ULONG total;
			ULONG done;
		} files;
		
		struct {
			bigint total;
			bigint done;
		} bytes;
		
		struct transfer_error * te;
		struct timeval tstart,tend;
	} progress;
};

STATIC VOID te_rem(struct transfer_error * te)
{
	if( te != NULL )
	{
		if( te->file != NULL )
			FreeVec( te->file );
		if( te->reason != NULL )
			FreeVec( te->reason );
		FreeMem( te, sizeof(struct transfer_error));
	}
}

STATIC BOOL te_add(struct ftp_site*site,STRPTR file,STRPTR reason,bigint transfered,bigint total)
{
	struct transfer_error * te;
	BOOL rc = FALSE;
	
	if((te=AllocMem(sizeof(struct transfer_error),MEMF_PUBLIC|MEMF_CLEAR)))
	{
		if(!(te->file = StrDup(file))||!(te->reason = StrDup(reason)))
		{
			te_rem( te );
		}
		else
		{
			te->transfered.hi = transfered.hi;
			te->transfered.lo = transfered.lo;
			
			te->total.hi = total.hi;
			te->total.lo = total.lo;
			
			te->next = site->progress.te;
			site->progress.te = te;
			
			rc = TRUE;
		}
	}
	
	return rc;
}

STATIC VOID te_delete(struct ftp_site * site)
{
	struct transfer_error * c, * n;
	
	for( c = site->progress.te ; c ; c = n )
	{
		n = c->next;
		
		te_rem( c );
	}
	site->progress.te = NULL;
}

STATIC VOID te_report(struct ftp_site * site)
{
	struct transfer_error * c;
	LONG width;
	
	if( site->progress.te == NULL ) return;
	
	width = ConsoleWidth(Output());
	PutStr("\n\r\n ++++ The following files failed to transfer:\n\n");
	
	Printf("%-11s%-11s%-40s%s\r\n",
		(long)"Transfered", (long)"Total", (long)"File", (long)"Reason");
	
	while(width-- > 0)
		FPutC( Output(), '=');
	FPutC(Output(), '\n');
	
	for( c = site->progress.te ; c ; c = c->next )
	{
		UBYTE a[32], b[32];
		
		Printf("%-11s%-11s%-40s%s\r\n",
			(long) i64_ToHuman( a, sizeof(a)-1, c->transfered),
			(long) i64_ToHuman( b, sizeof(b)-1, c->total),
			(long) c->file, (long) c->reason );
	}
}

static void free_dirlist(struct dirlist * pwd)
{
	struct dirlist * c, * n;
	
	if(pwd == NULL) return;
	
	for( c = pwd ; c ; c = n )
	{
		n = c->next;
		
		FreeMem( c, sizeof(struct dirlist));
	}
}

static void free_site(struct ftp_site * site)
{
	if( site != NULL )
	{
		free_dirlist(site->pwd);
		te_delete(site);
		
		if( site->host != NULL )
			FreeVec( site->host );
		if(site->path != NULL )
			FreeVec( site->path );
		if(site->home != NULL)
			FreeVec( site->home );
		if(site->user != NULL )
			FreeVec( site->user );
		if(site->pass != NULL )
			FreeVec( site->pass );
		
		CLOSESOCKET( site->data_sockfd );
		if( site->sockfd != -1 )
		{
			send( site->sockfd, "QUIT\r\n", 6, 0);
			CLOSESOCKET( site->sockfd );
		}
		FreeMem( site, sizeof(struct ftp_site));
	}
}

INLINE struct ftp_site * parse_site( STRPTR string )
{
	UBYTE buffer[2048], * dst=buffer, * src=string;
	long maxlen = sizeof(buffer) - 1, len, saved_ioerr = 0;
	struct ftp_site * site = NULL;
	
	if(!Strnicmp( src, "ftp://", 6))
		src += 6;
	
	while(*src && *src != '/' && *src != ':' && *src != '\n' && --maxlen > 0)
		*dst++ = *src++;
	
	*dst = 0;
	
	if(maxlen < 1)
	{
		saved_ioerr = ERROR_BUFFER_OVERFLOW;
		goto error;
	}
	
	if(!(site = AllocMem(sizeof(struct ftp_site), MEMF_PUBLIC|MEMF_CLEAR)))
		return NULL;
	
	#if 0
	site->recv_buffer_size = RECV_BUFFER_SIZE;
	while(!(site->recv_buffer = AllocVec( site->recv_buffer_size+1, MEMF_PUBLIC)))
	{
		if((site->recv_buffer_size /= 2) < 8193) break;
	}
	
	if( site->recv_buffer_size < 8193 )
	{
		goto error;
	}
	#else
	site->recv_buffer_size = RECV_BUFFER_SIZE;
	if(!(site->recv_buffer = AllocVec( site->recv_buffer_size+1, MEMF_PUBLIC)))
		goto error;
	#endif
	
	site->sockfd = -1;
	if(!(site->host = StrDup( buffer )))
		goto error;
	
	if( *src ) // not just a host?
	{
		// was a port given?
		if( *src == ':' )
		{
			len = 0;
			if((maxlen = StrToLong( ++src, &len )) == -1)
				goto error;
			
			if(!(len > 0 && len < 65535))
			{
				saved_ioerr = ERROR_BAD_NUMBER;
				goto error;
			}
			
			site->port = len & 0xffff;
			
			src += maxlen;
		}
		
		// was a full path/filename given?
		if( *src == '/' )
		{
			for( maxlen = sizeof(buffer) - 1, dst=buffer ; *src && *src != '\n' && --maxlen > 0 ; *dst++ = *src++ );
			
			if(maxlen < 1)
			{
				saved_ioerr = ERROR_BUFFER_OVERFLOW;
				goto error;
			}
			
			*dst = 0;
			if(!(site->path = StrDup( buffer )))
				goto error;
		}
	}
	
	if(!site->port)
		site->port = 21;
	
	return site;
error:
	if( saved_ioerr == 0 )
		saved_ioerr = IoErr();
	
	free_site( site );
	
	if( saved_ioerr != 0 )
		SetIoErr( saved_ioerr );
	return NULL;
}

STATIC CONST UWORD DayTable[] =
{	0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335	};

INLINE ULONG DateFormatOR( struct DateStamp * ds, STRPTR dateStr )
{
	LONG year, days, mins, tick, leap=1, month;
	STRPTR buf = dateStr;
	
	/* This function comes from StrToDate()'s AROS sources */
	
	days=ds->ds_Days;
	mins=ds->ds_Minute;
	tick=ds->ds_Tick;
	
	/*
	    Calculate year and the days in the year. Use a short method
	    if the date is between the 1.1.1978 and the 1.1.2100:
	    Every year even divisible by 4 in this time span is a leap year.
	    There are 92 normal and 30 leap years there.
	*/
	if(days<92*365+30*366)
	{
	    /*
		1976 was a leap year so use it as a base to divide the days
		into 4-year blocks (each beginning with a leap year).
	    */
	    days+=366+365;
	    year=4*(days/(366+3*365))+1976;
	    days%=(366+3*365);

	    /* Now divide the 4-year blocks into single years. */
	    if (days>=366)
	    {
		leap=0;
		days--;
		year+=days/365;
		days%=365;
	    }
	}
	else
	{
	    /*
		The rule for calendar calculations are:
		1. Every year even divisible by 4 is a leap year.
		2. As an exception from rule 1 every year even divisible by
		   100 is not.
		3. Every year even divisible by 400 is a leap year as an
		   exception from rule 2.
		So 1996, 2000 and 2004 are leap years - 1900 and 1999 are not.

		Use 2000 as a base to devide the days into 400 year blocks,
		those into 100 year blocks and so on...
	    */
	    days-=17*365+5*366;
	    year=400*(days/(97*366+303*365))+2000;
	    days%=(97*366+303*365);

	    if(days>=366)
	    {
		leap=0;
		days--;
		year+=100*(days/(24*366+76*365));
		days%=(24*366+76*365);

		if(days>=365)
		{
		    leap=1;
		    days++;
		    year+=4*(days/(366+3*365));
		    days%=(366+3*365);

		    if(days>=366)
		    {
			leap=0;
			days--;
			year+=days/365;
			days%=365;
		    }
		}
	    }
	}
	
	if( dateStr == NULL )
		return year;
	
	/************************************************************/
	/* convert the date to FTP's MDTM format, ie 20061210183024 */
	
	/*
	     The starting-day table assumes a leap year - so add one day if
	     the date is after february 28th and the year is no leap year.
	*/
	if(!leap&&days>=31+28)
	    days++;

	for(month=11;;month--)
	{
	    if(days>=DayTable[month])
	    {
		days-=DayTable[month];
		break;
	    }
	}

	/* Remember that 0 means 1.1.1978. */
	days++;
	month++;
	
	// construct the date into the given string buffer
	
	*buf++ = year/1000%10+'0';
	*buf++ = year/100%10+'0';
	*buf++ = year/10%10+'0';
	*buf++ = year%10+'0';
	
	*buf++ = month/10%10+'0';
	*buf++ = month%10+'0';
	
	*buf++ = days/10%10+'0';
	*buf++ = days%10+'0';
	
	*buf++ = mins/(10*60)+'0';
	*buf++ = mins/60%10+'0';
	*buf++ = mins/10%6+'0';
	*buf++ = mins%10+'0';
	*buf++ = tick/(10*TICKS_PER_SECOND)+'0';
	*buf++ = tick/TICKS_PER_SECOND%10+'0';
	*buf = 0;
	
	return (ULONG) dateStr;
}

INLINE BOOL GetFileStamp( STRPTR filename, struct DateStamp * dest_ds_ptr )
{
	D_S(struct FileInfoBlock,fib);
	BOOL rc = FALSE;
	BPTR lock;
	
	if((lock = Lock( filename, SHARED_LOCK)))
	{
		if(Examine( lock, fib ))
		{
			CopyMem( &fib->fib_Date, dest_ds_ptr, sizeof(struct DateStamp));
			rc = TRUE;
		}
		
		UnLock( lock );
	}
	
	return rc;
}

INLINE STRPTR MDTMTimeStamp ( STRPTR filename )
{
	STATIC UBYTE date_str[32];
	struct DateStamp ds;
	
	*date_str = 0;
	
	if(GetFileStamp( filename, &ds ))
	{
		return (STRPTR) DateFormatOR( &ds, date_str );
	}
	
	return NULL;
}

// convert ftp-format date string to number of seconds
INLINE ULONG str2time(STRPTR *date_string)
{
	STATIC UWORD ThisYear = 0;
	struct ClockData clock;
	long val, chs;
	STRPTR ptr = (*date_string);
	ULONG rc = 0;
	
	bzero( &clock, sizeof(struct ClockData));
	
	switch(*ptr++)
	{
		case  'S':
		{
			clock.month = 9; // Sep
			break;
		}
		case  'M':
		{
			if(*(ptr+1) == 'r') // Mar
				clock.month = 3;
			else // May
				clock.month = 5;
			break;
		}
		case  'F':
		{
			clock.month = 2; // Feb
			break;
		}
		case  'J':
		{
			if(*ptr == 'a') // Jan
				clock.month = 1;
			else if(*(ptr+1) == 'n') // Jun
				clock.month = 6;
			else // Jul
				clock.month = 7;
			break;
		}
		case  'A':
		{
			if(*ptr == 'p') // Apr
				clock.month = 4;
			else // Aug
				clock.month = 8;
			break;
		}
		case  'O':
		{
			clock.month = 10; // Oct
			break;
		}
		case  'N':
		{
			clock.month = 11; // Nov
			break;
		}
		case  'D':
		{
			clock.month = 12; // Dec
			break;
		}
		default:
			goto done;
	}
	
	ptr += 2;
	if((chs = StrToLong( ptr, &val )) == -1) goto done;
	clock.mday = val & 0xffff;
	
	for( ptr += chs ; *ptr && *ptr == 0x20 ; ptr++ );
	
	if(ptr[2] == ':')
	{
		chs = StrToLong( &ptr[0], &val ); clock.hour = (UWORD) val;
		chs += StrToLong( &ptr[3], &val ); clock.min  = (UWORD) val;
		
		if(chs != 4) goto done;
		
		if( ThisYear == 0 )
		{
			struct DateStamp ds;
			DateStamp(&ds);
			ThisYear = DateFormatOR ( &ds, NULL ) ;
		}
		
		clock.year = ThisYear;
		
		ptr += 6; // inc ':' and space
	}
	else
	{
		chs = StrToLong( ptr, &val ); clock.year = (UWORD) val;
		
		if(chs == -1) goto done;
		
		ptr += ++chs; // plus space
	}
	
	(*date_string) = ptr;
	
#if 0
	DBG_VALUE(clock.sec);
	DBG_VALUE(clock.min);
	DBG_VALUE(clock.hour);
	DBG_VALUE(clock.mday);
	DBG_VALUE(clock.month);
	DBG_VALUE(clock.year);
	DBG_VALUE(clock.wday);
#endif
	
	rc = CheckDate(&clock);
done:
	return(rc);
}

STATIC struct dirlist * parse_dirlist(STRPTR list, ULONG *files, ULONG *dirs, bigint *bytes)
{
	UBYTE *str, line[4096], *ptr=line;
	struct dirlist * new = NULL, *rc = NULL;
	LONG saved_ioerr = 0, maxlen;
	
	for( str = list ; *str ; ptr=line )
	{
		// process line-by-line
		maxlen = sizeof(line)-1;
		while(*str && *str != '\r' && *str != '\n' && --maxlen > 0)
			*ptr++ = *str++;
		*ptr = 0;
		
		if(maxlen < 1)
		{
			saved_ioerr = ERROR_BUFFER_OVERFLOW;
			goto error;
		}
		
		while(*str == '\r' || *str == '\n') str++;
		
		//DBG_STRING(line);
		
		// line[0] == 't' IF the "total 4545" string
		if(!line[0] || line[0] == 't') continue;
		
		// check for "." and ".." directory names
		if((ptr[-2] == 0x20 && ptr[-1] == '.')
		|| (ptr[-3] == 0x20 && ptr[-2] == '.' && ptr[-1] == '.'))
			continue;
		
		if(!(new = AllocMem(sizeof(struct dirlist), MEMF_CLEAR)))
			goto error;
		
		/* examples:
		 * -rw-rw-rw-   1 user     group     5924722 Oct  8 07:14 2006-10-04_M6.avi
		 * drw-rw-rw-   1 user     group           0 Nov  1 11:27 Libros
		 */
		
		ptr=line;
		switch(*ptr++)
		{
			case '-': 
				new->dlt = DLT_FILE;
				if(files) (*files)++;
				break;
			case 'd':
				new->dlt = DLT_DIR;
				if(dirs) (*dirs)++;
				break;
			case 'l':
				new->dlt = DLT_LINK;
				if(dirs) (*dirs)++;
				break;
			default:
				new->dlt = DLT_UNKNOWN;
				break;
		}
		
		if(*ptr++=='-')	new->Protection |= FIBF_READ;
		if(*ptr++=='-')	new->Protection |= FIBF_WRITE;
		if(*ptr++=='-')	new->Protection |= FIBF_EXECUTE;
		
		// rest of protection bits arent used
		while(*ptr && *ptr != 0x20) ptr++;
		while(*ptr && *ptr == 0x20) ptr++;
		
		// jump to 'user' field
		while(*ptr && *ptr != 0x20) ptr++;
		while(*ptr && *ptr == 0x20) ptr++;
		
		// skip 'user' field
		while(*ptr && *ptr != 0x20) ptr++;
		while(*ptr && *ptr == 0x20) ptr++;
		
		// skip 'group' field
		while(*ptr && *ptr != 0x20) ptr++;
		while(*ptr && *ptr == 0x20) ptr++;
		
		if(!*ptr || !(*ptr >= '0' && *ptr <= '9'))
		{
			DBG("The size isnt where it should\n");
			goto error;
		}
		
		new->Size = i64_atoi(ptr);
		if(bytes) (*bytes) = i64_add((*bytes),new->Size);
		
		// jump to 'date' field
		while(*ptr && *ptr != 0x20) ptr++;
		while(*ptr && *ptr == 0x20) ptr++;
		
		if(!(new->time = str2time( &ptr )))
		{
			DBG("cannot convert time\n");
			goto error;
		}
		
		CopyMem( ptr, new->FileName, sizeof(new->FileName));
		
		if(rc != NULL)
			rc->prev = new;
		
		new->prev = NULL;
		new->next = rc;
		rc = new;
	}
	
	return rc;
error:
	if( saved_ioerr == 0 )
		saved_ioerr = IoErr();
	PutStr("This server's file listing isn't supported\n");
	if( new != NULL )
		FreeMem( new, sizeof(struct dirlist));
	free_dirlist( rc );
	if( saved_ioerr == 0 )
		saved_ioerr = ERROR_BAD_TEMPLATE;
	SetIoErr( saved_ioerr );
	return NULL;
}

INLINE struct dirlist * lookup_dirlist(struct dirlist *list, STRPTR name )
{
	long len;
	
	if(!(name && *name)) return NULL;
	len = strlen(name);
	
	for( ; list ; list = list->next )
	{
		if(!Strnicmp( list->FileName, name, len ))
			return list;
	}
	return NULL;
}

STATIC LONG print_dirlist( struct ftp_site * site, DirListType dlt )
{
	struct dirlist * dl, * prev = NULL;
	LONG items = 0;
	
	if( site->pwd == NULL ) return 0;
	
	for( dl = site->pwd ; dl ; dl = (prev=dl)->next );
	for( dl = prev ; dl ; dl = dl->prev )
	{
		struct DateTime dt;
		UBYTE date[LEN_DATSTRING];
		UBYTE time[LEN_DATSTRING];
		
		if(dlt != dl->dlt)
			continue;
		
		CopyMem(time2ds(dl->time), &dt.dat_Stamp, sizeof(struct DateStamp));
		dt.dat_Format  = FORMAT_DOS;
		dt.dat_Flags   = DTF_SUBST;
		dt.dat_StrDay  = NULL;
		dt.dat_StrDate = date;
		dt.dat_StrTime = time;
		DateToStr(&dt);
		
		switch(dl->dlt)
		{
			case DLT_DIR:
			{	// right align
				
				int x = 9;
				while(x-- > 0)
					FPutC(Output(), 0x20);
				
				PutStr("<Dir>");
			}	break;
			
			case DLT_LINK:
			{	int x = 8;
				while(x-- > 0)
					FPutC(Output(), 0x20);
				
				PutStr("<Link>");
			}	break;
			
			case DLT_FILE:
			{	// damn, thats horrible!...
				ULONG size = (dl->Size.hi|dl->Size.lo) & 0xffffffff;
				
				Printf("%14lu", size );
			}	break;
			
			case DLT_UNKNOWN:
			default:
				break;
		}
		
		time[5] = 0; // seconds are Zero anyway
		Printf("  %-11s%s  %s\n",(long) date,(long) time,(long) dl->FileName);
		
		items++;
	}
	
	return items;
}

INLINE VOID dirlist_report(struct ftp_site *site, STRPTR dirname)
{
	struct DateTime dt;
	UBYTE datestr[LEN_DATSTRING], dow[LEN_DATSTRING], h[32];
	ULONG items;
	
	DateStamp((struct DateStamp *)&dt);
	dt.dat_Format = FORMAT_DEF;
	dt.dat_Flags = 0;
	dt.dat_StrDay = dow;
	dt.dat_StrDate = datestr;
	dt.dat_StrTime = NULL;
	DateToStr(&dt);
	
	Printf("Directory \"%s\" on %s %s:\n\n",(long) dirname,(long) dow,(long) datestr);
	
	items  = print_dirlist( site, DLT_DIR );
	items += print_dirlist( site, DLT_LINK );
	items += print_dirlist( site, DLT_FILE );
	
	if(items != (site->pwd_files+site->pwd_dirs))
	{
		PutStr("There are excluded items on the above list, use 'ls -l' to show all\n");
	}
	
	Printf("\n%ld files - %ld directories - %s total\n", site->pwd_files,
		site->pwd_dirs, (long) i64_ToHuman( h, sizeof(h)-1, site->pwd_bytes));
}

STATIC STRPTR NextLine(STRPTR buf, LONG * code)
{
	while(*buf && *buf++ != '\n');
	
	if(code)
	{
		if(StrToLong( buf, code ) == -1)
			(*code) = -1;
	}
	
	return((*buf != 0) ? buf : NULL);
}

/*****************************************************************************/

STATIC LONG Connect(STRPTR hostname, UWORD port)
{
	LONG s;
	struct hostent *hp;
	struct sockaddr_in sa;
	
	if(!(hp = gethostbyname( hostname )))
	   return -1;
	
	bzero(&sa, sizeof(sa));
	CopyMem(hp->h_addr, &sa.sin_addr, hp->h_length);
	sa.sin_family = hp->h_addrtype;
	sa.sin_port = htons( port );
	if((s=socket(hp->h_addrtype,SOCK_STREAM,0)) < 0)
		return -2;
	
	if (connect(s,(struct sockaddr *) &sa,sizeof(sa))< 0)
	{
		CloseSocket(s);
		return -3;
	}
	return s;
}

// receive data from ftp and return the line's code number
STATIC LONG ftp_recv( struct ftp_site * site )
{
	LONG rc, pos = 0;
	//struct timeval timeout = { 0, 400000 }; // 0.4 seconds
	struct timeval timeout = { 10, 0 };
	fd_set rdset;
	
	FD_ZERO(&rdset);
	FD_SET(site->sockfd, &rdset);
	
	site->recv_buffer[0] = 0;
	
	while(WaitSelect(site->sockfd+1, &rdset, NULL, NULL, &timeout, NULL) > 0)
	{
		long len;
		
		if((len = site->recv_buffer_size-pos) < 1)
		{
			SetIoErr( ERROR_BUFFER_OVERFLOW );
			rc = -1;
			goto done;
		}
		
		rc = recv( site->sockfd, &site->recv_buffer[pos], len, 0);
		
		if( rc < 0 )
		{
			SetIoErr( ERROR_LOCK_TIMEOUT );
			rc = -1;
			goto done;
		}
		
		pos += rc;
		site->recv_buffer[pos] = 0;
		
		timeout.tv_sec  = 0;
		timeout.tv_usec = 0;
	}
	
	if( site->recv_buffer[0] == 0 )
	{
		SetIoErr( ERROR_LOCK_TIMEOUT );
		rc = -1;
	}
	else
	{
		rc = 0;
		if(StrToLong( site->recv_buffer, &rc ) < 0)
		{
			if(IoErr() == 0)
				SetIoErr( ERROR_REQUIRED_ARG_MISSING );
			rc = -2;
		}
	}
done:
	return(site->ftprc = rc);
}

STATIC BOOL ftp_send( struct ftp_site * site, const char * fmt, ... )
{
	va_list args;
	LONG len;
	UBYTE buf[4096];
	
	va_start (args, fmt);
	len = vsnprintf( buf, sizeof(buf)-1, fmt, args );
	va_end(args);
	
	DBG("Sending \"%s\"\n", buf );
	
	if(send( site->sockfd, buf, len, 0) != len )
	{
		return FALSE;
	}
	
	return TRUE;
}

STATIC BOOL ftp_pasv( struct ftp_site * site )
{
	LONG code, i;
	unsigned char ip[4], port[2];
	UBYTE *ptr, datahost[32];
	UWORD dataport;
	
	if(!ftp_send(site,"PASV\r\n"))
	{
		PutStr("send() error\n");
		return FALSE;
	}
	
	if((code = ftp_recv( site )) != 227)
	{
		if(code > 0)
			PutStr(&site->recv_buffer[4]);
		else
			PutStr("recv() error\n");
		return FALSE;
	}	
	
	/* Parse response, one of
	 * "227 Entering passive mode. 127,0,0,1,4,51"
	 * "227 Entering Passive Mode (127,0,0,1,4,51)"
	 * "227 Data transfer will passively listen to 127,0,0,1,4,51"
	 */
	
	for( ptr = site->recv_buffer ; *ptr && *ptr != ',' ; ptr++ );
	
	if(*ptr != ',')
		goto unsup;
	
	for( ptr-- ; *ptr >= '0' && *ptr <= '9' ; ptr-- );
	
	for( i = 0, ptr++ ; i < 6 ; ptr += ++code, i++ )
	{
		long val;
		
		if((code = StrToLong( ptr, &val )) == -1 || !(val >= 0 && val <= 255))
			goto unsup;
		
		if( i < 4 )
			ip[i] = val & 0xff;
		else
			port[i-4] = val & 0xff;
	}
	
	dataport = (port[0]<<8) + port[1];
	snprintf( datahost, sizeof(datahost)-1, "%ld.%ld.%ld.%ld",
		(long) ip[0], (long) ip[1], (long) ip[2], (long) ip[3] );
	
	if((site->data_sockfd = Connect( datahost, dataport )) == -1)
	{
		PutStr("Cannot stablish FTP Data connection!\n");
		return FALSE;
	}
	
	return TRUE;
	
unsup:
	Printf("Unsupported 227-reply: %s", (long) &site->recv_buffer[4]);
	return FALSE;
}

STATIC BOOL ftp_type(struct ftp_site * site, BOOL ascii )
{
	static long __last_type = 0;
	long type = ascii ? 'A':'I';
	
	if(__last_type == type)
		return TRUE;
	
	if(!ftp_send( site, "TYPE %lc\r\n", __last_type = type))
		return FALSE;
	
	if(ftp_recv( site ) != 200)
		return FALSE;
	
	return TRUE;
}

STATIC BOOL ftp_list(struct ftp_site *site, STRPTR pathORfile, STRPTR *unixlist,
	struct dirlist ** dl, ULONG *files, ULONG *dirs, bigint *bytes)
{
	BOOL rc = FALSE, need_226_reply = TRUE;
	LONG r = -1;
	STRPTR nl;
//	struct timeval timeout = { 10, 0 };
//	fd_set rdset;	
	
	if(!ftp_pasv( site ))
		return FALSE;
	
	//r = RECV_BUFFER_SINGLE;
	//setsockopt( site->data_sockfd, SOL_SOCKET, SO_RCVBUF, &r, sizeof(r));
	
	if(!ftp_type( site, TRUE ))
	{
		PutStr("Error setting transfer mode type\n");
		goto done;
	}
	
	if(!ftp_send( site, "LIST %s\r\n", pathORfile ))
	{
		PutStr("send() error\n");
		goto done;
	}
	
	if(ftp_recv( site ) != 150)
	{
		DBG("no 150 reply\n");
		PutStr( site->recv_buffer );
		goto done;
	}
	
	if((nl=NextLine( site->recv_buffer, &r )) != NULL)
	{
		// the reply was a multiline
		if( r != 226 )
		{
			PutStr( nl );
			goto done;
		}
		need_226_reply = FALSE;
	}
	
	r = -1;
/*	FD_ZERO(&rdset);
	FD_SET(site->data_sockfd, &rdset);
	
	if(WaitSelect(site->data_sockfd+1, &rdset, NULL, NULL, &timeout, NULL) > 0)
	*/	r = recv(site->data_sockfd,site->recv_buffer,site->recv_buffer_size,0);
	
	if( r < 1 )
	{
		DBG(" DATA PORT ERROR rc=%ld, errno=%ld\n", r,Errno());
		//PutStr("Error receiving data from ftp data port!\n");
		//goto done;
		r = 0;
	}
	
	site->recv_buffer[r] = 0;
	
	if( unixlist )
	{
		(*unixlist) = *site->recv_buffer ? (char *)StrDup( site->recv_buffer ) : "got no data!";
	}
	else
	{
		if( (*dl) != NULL )
		{
			free_dirlist((*dl));
			(*dl) = NULL;
		}
		if(files) (*files) = 0;
		if(dirs)  (*dirs)  = 0;
		if(bytes) (*bytes) = i64_uset(0);
		SetIoErr( 0 );
		if(!((*dl) = parse_dirlist( site->recv_buffer, files, dirs, bytes )) && IoErr() != 0)
		{
			DBG("parse_disrlist() error\n");
			goto done;
		}
	}
	
	if(need_226_reply && (ftp_recv( site ) != 226))
	{
		DBG("no 226 reply\n");
		PutStr( site->recv_buffer );
		goto done;
	}
	
	rc = TRUE;
done:
	if( rc == FALSE )
	{
		if( unixlist && (*unixlist))
			FreeVec((*unixlist));
		
		if( IoErr() == 0 )
			SetIoErr( ERROR_BAD_TEMPLATE );
	}
	
	CLOSESOCKET( site->data_sockfd );
	
	return rc;
}

STATIC BOOL ftp_pwd(struct ftp_site *site)
{
	UBYTE buffer[1024],  *ptr, *dst=buffer;
	int maxlen = sizeof(buffer)-1;
	
	if(!ftp_send( site, "PWD\r\n"))
	{
		PutStr("send() error\n");
		return FALSE;
	}
	
	if(ftp_recv( site ) != 257)
	{
		PutStr( site->recv_buffer );
		return FALSE;
	}
	
	ptr=site->recv_buffer;
	while(*ptr && *ptr++ != '\"');
	while( *ptr && --maxlen > 0)
	{
		if(*ptr == '\"')
		{
			if(ptr[1] == '\"')
			{
				*dst++ = ptr[1];
				ptr += 2;
			}
			else
			{
				*dst = 0;
				break;
			}
		}
		else *dst++ = *ptr++;
	}
	
	if(!buffer[0])
	{
		PutStr("This server sends a PWD response which I dont understand!\n");
		if(maxlen < 1)
			SetIoErr( ERROR_BUFFER_OVERFLOW );
		return FALSE;
	}
	
	if(site->path != NULL)
		FreeVec( site->path );
	if(!(site->path = StrDup( buffer )))
		return FALSE;
	
	return TRUE;
}

STATIC BOOL ftp_cwd(struct ftp_site *site, STRPTR dirname)
{
	if(!ftp_send( site, "CWD %s\r\n", dirname ))
	{
		PutStr("send() error\n");
		return FALSE;
	}
	
	if(ftp_recv( site ) != 250)
	{
		PutStr( site->recv_buffer );
	}
	
	return TRUE;
}

INLINE DOUBLE Elapsed(struct ftp_site *site)
{
	double total;
	struct timeval elapsed;
	
	GetTimeOfDay(&(site->progress.tend));

	if (site->progress.tstart.tv_usec > site->progress.tend.tv_usec)
	{
		site->progress.tend.tv_usec += 1000000;
		site->progress.tend.tv_sec--;
	}

	elapsed.tv_usec = site->progress.tend.tv_usec - site->progress.tstart.tv_usec;
	elapsed.tv_sec = site->progress.tend.tv_sec - site->progress.tstart.tv_sec;

	total = elapsed.tv_sec + ((double) elapsed.tv_usec / 1e6);
	
	return((total < 0) ? 0.0 : total);
}

STATIC VOID Progress(struct ftp_site *site, STRPTR FileName )
{
	UBYTE buf[512], *p=buf;
	long percent, bps = 0, bpsu = 'B', len, vp_width = 10, vp_dots, vp_rem;
	ULONG h=0, m=0, s=0, width;
	bigint quot, rem;
	
	// Format:
	// 1 percent
	// 2 average bytes per second
	// 3 stimated time remaining
	// 4 filename being hashed
	
	*p++ = '\r';
	*p++ = 0x20;
	
	//percent = (site->progress.files.done/*+1*/)*100/site->progress.files.total;
	i64_udiv(i64_mul( site->progress.bytes.done, i64_uset(100)), site->progress.bytes.total, &quot, &rem);
	if((percent = quot.lo) < 0 || percent > 100) return;
	if(percent < 100)
	{
		snprintf( p, 6, "%2ld%% ", percent );
		
		vp_dots = percent / vp_width;
	}
	else
	{
		//strcpy( p, "100%");
		p[0] = '1'; p[1] = '0'; p[2] = '0'; p[3] = '%'; p[4] = 0;
		
		vp_dots = vp_width;
	}
	p += 4;
	vp_rem = vp_width - vp_dots;
	
	//if((site->progress.bytes.done.hi|site->progress.bytes.done.lo) != 0)
	{
		DOUBLE elapsed;
		
		if((elapsed = Elapsed( site )) > 0.0)
		{
			bigint bibps;
			
			i64_udiv( site->progress.bytes.done, i64_uset((ULONG)elapsed), &bibps, &rem );
			
			if((bps = (bibps.hi|bibps.lo)))
			{
				bigint secs;
				
				i64_udiv(i64_sub( site->progress.bytes.total, site->progress.bytes.done ), bibps, &secs, &rem); //-> secs = ( size - cur ) / cps;
				
				i64_udiv( secs, i64_uset(3600), &quot, &rem);	h = quot.hi | quot.lo;
				i64_udiv( rem, i64_uset(60), &quot, &rem);	m = quot.hi | quot.lo;
										s = rem.hi  | rem.lo;
			}
		}
		
		if( bps > (1024*1024)) {
			bps /= (1024*1024);	bpsu = 'M';
		} else if( bps > 1024) {
			bps /= 1024;		bpsu = 'K';
		}
	}
	
	// visual progress bar
	*p++ = '[';
	while(vp_dots-- > 0) *p++ = '#';
	while(vp_rem-- > 0) *p++ = ' ';
	*p++ = ']';
	*p++ = 0x20;
	
	snprintf( p, 200, "%3ld%lcb/s ETA %02ld:%02ld:%02ld Transfering ", bps, bpsu, h, m, s );
	
	len = strlen(buf);
	width = ConsoleWidth(Output());
	
	//strncat( &buf[len], entry->FileName, MIN(sizeof(buf),width) - len - 1);
	len += snprintf( &buf[len], MIN(sizeof(buf),width) - len, FileName );
	
	for( p = &buf[len/* = strlen(buf)*/] ; width > (unsigned)len++ ; *p++ = 0x20 );
	*p = 0;
	
	PutStr( buf );
	Flush(Output());
}

STATIC STRPTR FullRemotePath(STRPTR root, STRPTR pathpart, STRPTR out, ULONG outlen)
{
	long val;
	
	*out = 0;
	if(!AddPart( out, root, outlen))
	{
		return NULL;
	}
	if(out[val=strlen(out)-1] == ':')
	{
		/* HOME directory MAY contains a colon */
		out[++val] = '/';
		out[++val] = 0;
	}
	if(!AddPart( out, pathpart, outlen-val))
	{
		return NULL;
	}
	return out;
}

STATIC VOID SetEntryAttrs(struct ftp_site *site, struct dirlist *entry,
	STRPTR local_file, STRPTR remote_file )
{
	struct DateStamp * file_ds = time2ds(entry->time);
	
	if( entry->dlt == DLT_FILE )
	{
		UBYTE comment[81];
		
		snprintf( comment, sizeof(comment)-1,
			"ftp://%s@%s%s", site->user, site->host, remote_file );
		
		SetComment( local_file, comment );
	}
	SetFileDate( local_file, file_ds );
	SetProtection( local_file, entry->Protection );
}

STATIC STRPTR NoHomeDir( STRPTR home_path, STRPTR file )
{
	STRPTR l, r;
	
	for( l = file, r = home_path ; *l == *r ; l++, r++ );
	
	// something happened?
	if( r != home_path )
	{
		if(*l == '/') // skip trailing slash
			l++;
	}
	
	return( l );
}

STATIC BOOL ftp_retr_file(struct ftp_site *site,struct dirlist *entry,STRPTR pwd)
{
	BPTR lock = 0;
	AsyncFile * fd = NULL;
	UBYTE remote_file[2048], local_file[2048], *r, *l, * reason = "\0";
	long val, resume_offset = 0, open_mode, saved_ioerr = 0;
	bigint transfered = i64_uset(0), remaining, recV;
	ULONG last_report = 0, now, open_size;
	BOOL rc = FALSE, need_226_reply = TRUE, unsaved = FALSE, 
		delete_file = FALSE;
	
	#ifdef DEBUG
	long __max_bytes_recv = -1;
	#endif /* DEBUG */
	
	if(FullRemotePath(((pwd && *pwd) ? pwd : site->path),entry->FileName, remote_file, sizeof(remote_file)) == NULL)
	{
		unsaved = TRUE;
		goto error;
	}
	
	CopyMem( remote_file, local_file, sizeof(local_file));
	
	// strip HOME directory from local_file
	for( l = local_file, r = site->home ; *l == *r ; l++, r++ );
	
	// something happened?
	if( r != site->home )
	{
		if(*l == '/') // skip trailing slash
			l++;
		
		CopyMem( l, local_file, sizeof(local_file));
	}
	//DBG_STRING(local_file);
	
	#if 0
	if(strchr( local_file, '/'))
	{
		if((lock = CreateDirTree(PathPart( local_file ))))
		{
			UnLock( lock );
			lock = 0;
		}
	}
	#else
	if((val=MakeDir( local_file )) != 0)
	{
		saved_ioerr = val;
		goto error;
	}
	#endif
	
	// file exists?
	if((lock = Lock( local_file, SHARED_LOCK)))
	{
		D_S(struct FileInfoBlock,fib);
		
		Printf("%s: %s, ", (long) local_file,(long)"already exists");
		
		if(Examine( lock, fib ))
		{
			UBYTE buffer[256];
			
			if(FIB_IS_DRAWER(fib))
			{
				PutStr("and is a directory!\n");
				goto done;
			}
			
			Printf("Overwrite");
			
			if((ULONG)fib->fib_Size < (ULONG)(entry->Size.hi|entry->Size.lo))
			{
				PutStr("/Resume (o/r)");
			}
			else PutStr(" (y/n)");
			
			PutStr(" [all] ? ");
			Flush(Output());
			
			*buffer = 0;
			GetInputString( buffer, sizeof(buffer)-1, FALSE );
			
			switch(*buffer)
			{
				case 'y':
				case 'Y':
				case 'o':
				case 'O':
					break;
				case 'r':
				case 'R':
					resume_offset = fib->fib_Size;
					break;
				case 'n':
				case 'N':
					reason = "already exists";
					unsaved = TRUE;
					goto done;
				default:
					unsaved = TRUE;
					reason = "already exists";
					Printf("unknown answer '%lc'\n", *buffer );
					goto done;
			}
		}
		
		UnLock( lock );
		lock = 0;
	}
	
	if( resume_offset )
		open_mode = MODE_APPEND;
	else
		open_mode = MODE_WRITE;
	
	if(((ULONG)(entry->Size.hi|entry->Size.lo) < (ULONG)site->recv_buffer_size)
	|| ((ULONG)(site->recv_buffer_size * 4) > (ULONG)(entry->Size.hi|entry->Size.lo)))
		open_size = (ULONG)(entry->Size.hi|entry->Size.lo);
	else
		open_size = (ULONG) site->recv_buffer_size * 4;
	
	if(!(fd = OpenAsync( local_file, open_mode, open_size )))
	{
		Printf("Cannot write to \"%s\"\n", (long) local_file );
		unsaved = TRUE;
		goto done;
	}
	
	if(!ftp_type( site, FALSE ))
	{
		PutStr("Error setting transfer mode type\n");
		unsaved = TRUE;
		delete_file = !resume_offset;
		goto error;
	}
	
	if(!ftp_pasv( site ))
	{
		unsaved = TRUE;
		delete_file = !resume_offset;
		goto error;
	}
	
	//change the data port recvbuf size
	//val = site->recv_buffer_size;
	val = open_size < RECV_BUFFER_SINGLE ? open_size : RECV_BUFFER_SINGLE;
	//val = 32768;
	if(setsockopt( site->data_sockfd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) != 0)
	{
		DBG("  ++++++ cannot set SO_RCVBUF, errno = %ld\n", Errno());
	}
	
	if( resume_offset )
	{
		if(!ftp_send( site, "REST %ld\r\n", resume_offset ))
		{
			PutStr("send() error\n");
			unsaved = TRUE;
			goto error;
		}
		
		if(ftp_recv( site ) != 350)
		{
			DBG("no 350 reply\n");
			Printf("%s: %s\n",(long)"cannot resume file",(long)site->recv_buffer);
			reason = site->recv_buffer;//"cannot resume file";
			unsaved = TRUE;
			goto done;
		}
		
		recV = i64_uset((ULONG)resume_offset);
		transfered = i64_add(transfered,recV);
		site->progress.bytes.total = i64_sub(site->progress.bytes.total,recV);
	}
	
	if(!ftp_send( site, "RETR %s\r\n", remote_file ))
	{
		PutStr("send() error\n");
		unsaved = TRUE;
		delete_file = !resume_offset;
		goto error;
	}
	
	if(ftp_recv( site ) != 150)
	{
		DBG("no 150 reply\n");
		PutStr( site->recv_buffer );
		unsaved = TRUE;
		reason = site->recv_buffer;
		goto done;
	}
	
	if((r=NextLine( site->recv_buffer, &val )) != NULL)
	{
		// the reply was a multiline
		if( val != 226 )
		{
			PutStr( r );
			unsaved = TRUE;
			delete_file = !resume_offset;
			reason = site->recv_buffer;
			goto done;
		}
		need_226_reply = FALSE;
	}
	
	// receive the file
	while((val=recv(site->data_sockfd,site->recv_buffer,site->recv_buffer_size,0))>0)
	{
		#ifdef DEBUG
		if( val > __max_bytes_recv )
			__max_bytes_recv = val;
		#endif /* DEBUG */
		
		if(WriteAsync( fd, site->recv_buffer, val ) != val)
		{
			PutStr("WriteAsync() error\n");
			unsaved = /*delete_file =*/ TRUE;
			goto done;
		}
		
		recV = i64_uset((ULONG)val);
		transfered = i64_add(transfered,recV);
		site->progress.bytes.done = i64_add(site->progress.bytes.done,recV);
		
		if(((now = ds2time()) - last_report) > 2)
		//if((site->progress.tend.tv_sec - site->progress.tstart.tv_sec) > 2)
		//if(((now = site->progress.tend.tv_sec) - last_report) > 1 || !last_report)
		{
			last_report = now;
			
			Progress ( site, entry->FileName ) ;
		}
	}
	DBG_VALUE(__max_bytes_recv);
	
	if((SetSignal(0L,SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) || Errno()==4)
	{
		// the file will be set as "unsaved" due the 'transfered' size
		
		saved_ioerr = ERROR_BREAK;
	//	shutdown( site->data_sockfd, 2 );
	}
	
	/* aqui deberia usar el shutdown() de arriba y el closesocket() de abajo,
	 * tal como parece requerido para STOR, pero debo comprobar que en vez de
	 * coger un 226 checkar por 426 y remover este del goto
	 */
/*	CloseSocket( site->data_sockfd );
	site->data_sockfd = -1;
*/	
	if(need_226_reply && (ftp_recv( site ) != 226))
	{
		// hence, it means the file wasn't transfered completely?, ok so
		unsaved = /*delete_file =*/ TRUE;
		
		if( saved_ioerr != ERROR_BREAK )
			reason = site->recv_buffer;
		
		DBG("no 226 reply\n");
		PutStr( site->recv_buffer );
		goto done;
	}
	
done:
	rc = TRUE;
error:
	if( saved_ioerr == 0 )
		saved_ioerr = IoErr();
	if( lock != 0 )
	{
		UnLock( lock );
		lock = 0;
	}
	
	CLOSESOCKET( site->data_sockfd );
	
	if( saved_ioerr == ERROR_BREAK )
	{
		// get error message "426 Data connection closed, ..."
		ftp_recv( site );
		
		DBG_VALUE(site->ftprc);
	}
	
	if( fd != NULL )
	{
		CloseAsync( fd );
		
		if((rc == FALSE && !resume_offset) || delete_file )
		{
			DeleteFile( local_file );
		}
		else if( rc == TRUE )
		{
			SetEntryAttrs( site, entry, local_file, remote_file );
		}
	}
	SetIoErr( saved_ioerr );
	
	//if(i64_cmp(transfered,entry->Size) != I64_EQUAL)
	//if(i64_cmp((remaining=i64_sub(transfered,entry->Size)),i64_uset(0)) != I64_EQUAL)
	remaining = i64_sub(transfered,entry->Size);
	if((remaining.hi|remaining.lo) != 0)
	{
		unsaved = TRUE; // not really, but save log
		site->progress.bytes.done = i64_add(site->progress.bytes.done,remaining);
	}
	site->progress.files.done ++;
	
	if( unsaved )
	{
		if(!*reason)
			reason = ioerrstr(NULL);
		
		if(!te_add(site, local_file, reason, transfered, entry->Size))
			rc = FALSE; /* FATAL error (no mem) */
	}
	return rc;
}

STATIC BOOL ftp_retr(struct ftp_site *site,struct dirlist *entry, STRPTR old_path, BOOL calc)
{
	BOOL rc = TRUE;
	
	//DBG(" >>> \"%s\" -> %s\n", entry->FileName,(entry->dlt == DLT_FILE) ? "FILE" : (entry->dlt == DLT_DIR) ? "DIR":"HMM");
	
	if(entry->dlt == DLT_FILE)
	{
		if( calc == FALSE )
		{
			rc = ftp_retr_file(site,entry,old_path);
		}
		else
		{
			site->progress.files.total ++ ;
			site->progress.bytes.total = i64_add(site->progress.bytes.total,entry->Size);
		}
	}
	else if(entry->dlt == DLT_DIR)
	{
		struct dirlist * dl = NULL, *ptr;
		UBYTE fullpath[2048], *p;
		
		/* instead of overloading this using FullRemotePath() 
		 * MAY parse_dirlist() should use a new var with the 
		 * fullpath to the entry ?
		 */
		
		if(FullRemotePath(((old_path && *old_path) ? old_path : site->path),entry->FileName, fullpath, sizeof(fullpath)-1) == NULL)
			return FALSE;
		
		if((rc = ftp_list( site, fullpath, NULL, &dl, NULL, NULL, NULL)))
		{
			for( ptr = dl ; ptr ; ptr = ptr->next )
			{
				if(!(rc = ftp_retr( site, ptr, fullpath, calc )))
					break;
			}
			
			if( calc == FALSE )
			{ // set datestamp for drawers as well
				p = NoHomeDir( site->home, fullpath );
				SetEntryAttrs( site, entry, p, NULL );
			}
			
			free_dirlist( dl );
		}
	}
	else rc = calc ? TRUE : FALSE;
	
	return rc;
}



STATIC BOOL RemoteMakeDir( struct ftp_site *site, STRPTR fullpath )
{
	UBYTE subdir[4096];
	char *sep, *xpath=(char *)fullpath;
	
	sep = (char *) fullpath;
	sep = strchr(sep, '/');
	
	while( sep )
	{
		int len;
		
		len = sep - xpath;
		CopyMem( xpath, subdir, len);
		subdir[len] = 0;
		
		DBG(" +++ Creating Directory \"%s\"...\n", subdir );
		
		if(!ftp_send( site, "MKD %s\r\n", subdir ))
		{
			PutStr("send() error\n");
			return FALSE;
		}
		
		if(ftp_recv( site ) != 257)
		{
			// DAMN, how to know if the dir cannot be created (permissions) or already exists !?
			DBG( site->recv_buffer );
			//break;//return FALSE;
		}
		
		sep = strchr(sep+1, '/');
	}
	
	return(TRUE);
}

STATIC BOOL ftp_stor_file( struct ftp_site *site, STRPTR fullpathtofile, ULONG fileSize )
{
	AsyncFile * fd = NULL;
	UBYTE *r, *file, * reason = "\0";
	long val, resume_offset = 0, saved_ioerr = 0;
	bigint transfered = i64_uset(0), remaining, recV;
	ULONG last_report = 0, now, open_size;
	BOOL rc = FALSE, need_226_reply = TRUE, unsaved = FALSE, 
		delete_file = FALSE;
	
	#ifdef DEBUG
	long __max_bytes_recv = -1;
	#endif /* DEBUG */
	
	if(!(file = strchr( fullpathtofile, ':')))
		file = fullpathtofile;
	else
		file++;
	
	// check if that file exists already
	if(!ftp_send( site, "SIZE %s\r\n", file ))
	{
		PutStr("send() error\n");
		unsaved = TRUE;
		goto error;
	}
	
	if(ftp_recv( site ) == 213) // 213 == exists
	{
		long rsize = 0;
		UBYTE buffer[256];
		
		Printf("%s: %s, ", (long) file,(long)"already exists");
		
		Printf("Overwrite");
		
		StrToLong( &site->recv_buffer[4], &rsize );
		DBG_VALUE(rsize);
		if((ULONG)rsize < fileSize )
		{
			PutStr("/Resume (o/r)");
		}
		else PutStr(" (y/n)");
		
		PutStr(" [all] ? ");
		Flush(Output());
		
		*buffer = 0;
		GetInputString( buffer, sizeof(buffer)-1, FALSE );
		
		switch(*buffer)
		{
			case 'y':
			case 'Y':
			case 'o':
			case 'O':
				break;
			case 'r':
			case 'R':
				resume_offset = rsize;
				break;
			case 'n':
			case 'N':
				reason = "already exists";
				unsaved = TRUE;
				goto done;
			default:
				unsaved = TRUE;
				reason = "already exists";
				Printf("unknown answer '%lc'\n", *buffer );
				goto done;
		}
	}
	else RemoteMakeDir( site, file );
	
	if(((ULONG)(fileSize) < (ULONG)site->recv_buffer_size)
	|| ((ULONG)(site->recv_buffer_size * 4) > (ULONG)(fileSize)))
		open_size = (ULONG)(fileSize);
	else
		open_size = (ULONG) site->recv_buffer_size * 4;
	
	if(!(fd = OpenAsync( fullpathtofile, MODE_READ, open_size )))
	{
		Printf("Cannot open \"%s\"\n", (long) fullpathtofile );
		unsaved = TRUE;
		goto done;
	}
	
	if(!ftp_type( site, FALSE ))
	{
		PutStr("Error setting transfer mode type\n");
		unsaved = TRUE;
		goto error;
	}
	
	if(!ftp_pasv( site ))
	{
		unsaved = TRUE;
		goto error;
	}
	
#if 1
	//change the data port sendbuf size
//trouble->	//val = open_size < RECV_BUFFER_SINGLE ? open_size : RECV_BUFFER_SINGLE;
	//val = 32768;
	val = STOR_MAXBUFSIZ;
	if(setsockopt( site->data_sockfd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) != 0)
	{
		DBG("  ++++++ cannot set SO_RCVBUF, errno = %ld\n", Errno());
	}
#endif
	
	if( resume_offset )
	{
		if(SeekAsync( fd, resume_offset, MODE_START ) == -1 )
		{
			unsaved = TRUE;
			goto done;
		}
		
		if(!ftp_send( site, "REST %ld\r\n", resume_offset ))
		{
			PutStr("send() error\n");
			unsaved = TRUE;
			goto error;
		}
		
		if(ftp_recv( site ) != 350)
		{
			DBG("no 350 reply\n");
			Printf("%s: %s\n",(long)"cannot resume file",(long)site->recv_buffer);
			reason = site->recv_buffer;//"cannot resume file";
			unsaved = TRUE;
			goto done;
		}
		
		recV = i64_uset((ULONG)resume_offset);
		transfered = i64_add(transfered,recV);
		site->progress.bytes.total = i64_sub(site->progress.bytes.total,recV);
	}
	
	if(!ftp_send( site, "STOR %s\r\n", file ))
	{
		PutStr("send() error\n");
		unsaved = TRUE;
		delete_file = !resume_offset;
		goto error;
	}
	
	if(ftp_recv( site ) != 150)
	{
		DBG("no 150 reply\n");
		PutStr( site->recv_buffer );
		unsaved = TRUE;
		reason = site->recv_buffer;
		goto done;
	}
	
	if((r=NextLine( site->recv_buffer, &val )) != NULL)
	{
		// the reply was a multiline
		if( val != 226 )
		{
			PutStr( r );
			unsaved = TRUE;
			delete_file = !resume_offset;
			reason = site->recv_buffer;
			goto done;
		}
		need_226_reply = FALSE;
	}
	
	// send the file
	while((val=ReadAsync( fd, site->recv_buffer,STOR_MAXBUFSIZ))>0)
	{
		#if 0 //def DEBUG
		if( val > __max_bytes_recv )
			__max_bytes_recv = val;
		#endif /* DEBUG */
		
		if(send(site->data_sockfd,site->recv_buffer,val,0) != val)
		{
			reason = "send() error\n";
			unsaved = TRUE;
			goto done;
		}
		
		recV = i64_uset((ULONG)val);
		transfered = i64_add(transfered,recV);
		site->progress.bytes.done = i64_add(site->progress.bytes.done,recV);
		
	//	if(((now = ds2time()) - last_report) > 2)
		//if((site->progress.tend.tv_sec - site->progress.tstart.tv_sec) > 2)
		//if(((now = site->progress.tend.tv_sec) - last_report) > 1 || !last_report)
		{
	//		last_report = now;
			
			Progress ( site, file ) ;
		}
	}
	//DBG_VALUE(__max_bytes_recv);
	
	CloseSocket( site->data_sockfd );
	site->data_sockfd = -1;
	
	if((SetSignal(0L,SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) || Errno()==4)
	{
		saved_ioerr = ERROR_BREAK;
		// the file will be set as "unsaved" due the 'transfered' size
	}
	
	if(need_226_reply && (ftp_recv( site ) != 226))
	{
		// hence, it means the file wasn't transfered completely?, ok so
		unsaved = /*delete_file =*/ TRUE;
		
		if( saved_ioerr != ERROR_BREAK )
			reason = site->recv_buffer;
		
		DBG("no 226 reply\n");
		PutStr( site->recv_buffer );
		goto done;
	}
	
done:
	rc = TRUE;
error:
	if( saved_ioerr == 0 )
		saved_ioerr = IoErr();
	CLOSESOCKET( site->data_sockfd );
	
	if( saved_ioerr == ERROR_BREAK )
	{
		// get error message "426 Data connection closed, ..."
		ftp_recv( site );
		
		DBG_VALUE(site->ftprc);
	}
	
	if( fd != NULL )
	{
		CloseAsync( fd );
	}
	SetIoErr( saved_ioerr );
	
	recV = i64_uset((ULONG)fileSize);
	remaining = i64_sub(transfered,recV);
	if((remaining.hi|remaining.lo) != 0)
	{
		unsaved = TRUE; // not really, but save log
		site->progress.bytes.done = i64_add(site->progress.bytes.done,remaining);
	}
	site->progress.files.done ++;
	
	if( unsaved )
	{
		if(!*reason)
			reason = ioerrstr(NULL);
		
		if(!te_add(site, fullpathtofile, reason, transfered, recV))
			rc = FALSE; /* FATAL error (no mem) */
	}
	else
	{
		STRPTR dateString;
		
		if((dateString = MDTMTimeStamp( fullpathtofile )) != NULL)
		{
			int tryes = 4; // sometimes MDTM fails, dunno why...
			BOOL date_set = FALSE;
			
			while(tryes-- > 0)
			{
				if(!ftp_send( site, "MDTM %s %s\r\n", dateString, file ))
				{
					DBG("Failed sending MDTM command\n");
					break;
				}
				
				if(ftp_recv( site ) == 253)
				{
					date_set = TRUE;
					break;
				}
				
				Delay( 30 );
			}
			
			if( date_set == FALSE )
			{
				if(!te_add(site, fullpathtofile, "NO ERROR, just that I was unable to set the DateStamp on the remote file", transfered, recV))
					rc = FALSE; /* FATAL error (no mem) */
			}
		}
	}
	return rc;
}

STATIC BOOL ftp_stor_tree( struct ftp_site *site, STRPTR root, BOOL calc )
{
	BPTR lock;
	BOOL error = FALSE;
	
	if((lock = Lock( root, SHARED_LOCK)))
	{
		struct ExAllControl *eaControl;
		
		if((eaControl = (struct ExAllControl *) AllocDosObject(DOS_EXALLCONTROL, NULL)))
		{
			struct ExAllData *eaBuffer, *eaData;
			LONG more,eaBuffSize = 300 * sizeof(struct ExAllData);
			
			eaControl->eac_LastKey = 0;
			eaControl->eac_MatchString = (UBYTE *) NULL;
			eaControl->eac_MatchFunc = (struct Hook *) NULL;
			
			if((eaBuffer = (struct ExAllData *) AllocMem(eaBuffSize, MEMF_ANY)))
			{
				do {
					more = ExAll(lock,eaBuffer,eaBuffSize,ED_SIZE,eaControl);
					
					if((!more) && (IoErr() != ERROR_NO_MORE_ENTRIES))
						break;
					
					if(eaControl->eac_Entries == 0)
						continue;
					
					eaData = (struct ExAllData *) eaBuffer;
					while( eaData )
					{
						if(EAD_IS_LINK(eaData))
						{
							struct DevProc * dvp = NULL;
							UBYTE path[4096];
							
							AddPart( path, root, sizeof(path)-1);
							AddPart( path, eaData->ed_Name, sizeof(path)-1);
							
							if((dvp = GetDeviceProc( path, NULL)) != NULL)
							{
								if((long)ReadLink(dvp->dvp_Port,dvp->dvp_Lock,path,path,sizeof(path)-1) > 0)
								{
									BPTR lock;
									
									if((lock = Lock( path, SHARED_LOCK)))
									{
										struct FileInfoBlock fib;
										
										if(Examine(lock, &fib))
										{
											eaData->ed_Type    = fib.fib_DirEntryType;
											eaData->ed_Size    = fib.fib_Size;
										}
										
										UnLock(lock);
									}
								}
								
								FreeDeviceProc(dvp);
							}
						}
						
						if(EAD_IS_DRAWER(eaData))
						{
							char newpath[4096];
							*newpath = 0;
							
							if((error = (
							
							!AddPart( newpath, root, sizeof(newpath))
							|| !AddPart( newpath, eaData->ed_Name, sizeof(newpath))
							
							|| !ftp_stor_tree( site, newpath, calc ))))
								break;
						}
						else if(EAD_IS_FILE(eaData))
						{
							if( calc == TRUE )
							{
								site->progress.files.total ++ ;
								site->progress.bytes.total = i64_add(site->progress.bytes.total,i64_uset((ULONG)eaData->ed_Size));
							}
							else
							{
								char newpath[4096];
								*newpath = 0;
								
								if((error = 
								
								!AddPart( newpath, root, sizeof(newpath))
								|| !AddPart( newpath, eaData->ed_Name, sizeof(newpath))
								
								|| !ftp_stor_file( site, newpath, eaData->ed_Size )))
									break;
							}
						}
						else
						{
							DBG("unknown 'item': %s\n", eaData->ed_Name );
						}
						
						eaData = eaData->ed_Next;
					}
				} while(more);
				
				FreeMem(eaBuffer, eaBuffSize);
			}
			
			FreeDosObject(DOS_EXALLCONTROL, eaControl);
		}
		
		UnLock(lock);
	}
	
	return !error;
}

/*****************************************************************************/

#define FTPCOMMAND_NAME(name)	ftp_command_## name
#define FTPCOMMANDDECL(name)	((APTR) FTPCOMMAND_NAME(name))
#define FTPCOMMAND_ARGS		struct ftp_site *site, STRPTR cmdline
#define FTPCOMMAND(name)	STATIC BOOL FTPCOMMAND_NAME(name)(FTPCOMMAND_ARGS)

//----------------------------------------------------------------------------
	/* ftp commands handlers */

FTPCOMMAND(raw)
{
	struct timeval timeout = { 1, 0 };
	fd_set rdset;
	
	if(!ftp_send( site, "%s\r\n", &cmdline[1] ))
	{
		PutStr("send() error\n");
		return FALSE;
	}
	
	FD_ZERO(&rdset);
	FD_SET(site->sockfd, &rdset);
	
	while(WaitSelect(site->sockfd+1, &rdset, NULL, NULL, &timeout, NULL) > 0)
	{
		ftp_recv( site );
		PutStr( site->recv_buffer );
		if(site->ftprc == 421)
			return FALSE;
		
		if(SetSignal(0L,0L) & SIGBREAKF_CTRL_C)
			return FALSE;
	}
	
	return TRUE;
}

FTPCOMMAND(list)
{
	BOOL unix_listing = FALSE;
	STRPTR unixlist = NULL;
	
	if(*cmdline == ' ' && *(cmdline+1) == '-' && *(cmdline+2) == 'l')
	{
		unix_listing = TRUE;
		cmdline += 3;
	}
	
	if(!ftp_list(site,*cmdline == ' ' ? &cmdline[1] : cmdline, unix_listing ? &unixlist:NULL,
		&site->pwd, &site->pwd_files, &site->pwd_dirs, &site->pwd_bytes ))
	{
		return FALSE;
	}
	
	if(unix_listing)
	{
		DBG("unix listing\n");
		PutStr( unixlist );
		FreeVec( unixlist );
	}
	else
	{
		dirlist_report( site, *cmdline == 0x20 ? (char *)cmdline+1 : (site->path ? (char *)site->path : "/"));
	}
	
	return TRUE;
}

FTPCOMMAND(cwd)
{
	if(*cmdline != ' ')
	{
		/* like the 'CD' AmigaDOS command, if no dir name is supplied
		 * just print the current directory (what is PWD for FTP)
		 */
		Printf("%s\n", (long) site->path );
	}
	else if(!ftp_cwd(site,&cmdline[1]) || !ftp_pwd( site ))
	{
		return FALSE;
	}
	return TRUE;
}

FTPCOMMAND(get)
{
	STATIC struct dirlist stk_entry;
	struct dirlist * entry;
	BOOL rc = TRUE, free_entry = FALSE;
	LONG saved_ioerr = 0;
	UBYTE dir_path[1024];
	
	*dir_path = 0;
	bzero( &stk_entry, sizeof(struct dirlist));
	SetIoErr(0);
	
	if(*cmdline != ' ')
	{
		PutStr("Specify what to get!\n");
	}
	else if(!(entry = lookup_dirlist( site->pwd, &cmdline[1] )))
	{
		DBG("Found no entry \"%s\", listing it...\n", &cmdline[1] );
		
		ftp_list( site, &cmdline[1], NULL, &entry, NULL, NULL, NULL);
		
		if( entry != NULL )
		{
			free_entry = TRUE;
			DBG("item listed, checking...\n");
			
			if(Stricmp( entry->FileName, &cmdline[1] )) // if arent the same, we listed a directory
			{
				UBYTE folder[1024], * ptr, * parent, * Dir;
				struct dirlist * dl = NULL;
				
				// ...hence, list the parent directory
				DBG("seems we listed a directory, trying parent...\n");
				
				bzero( folder, sizeof(folder)-1);
				CopyMem( &cmdline[1], folder, sizeof(folder)-1);
				ptr = folder + strlen(folder) - 1;
				
				if(*ptr == '/') ptr--;
				while(*ptr && *ptr != '/') ptr--;
				if(!*ptr)
				{
					/* current dir */
					parent = "";
					Dir = &cmdline[1];
				}
				else
				{
					*ptr = 0;
					Dir = &ptr[1];
					
					if(!FullRemotePath( site->path, NoHomeDir(site->home,folder), dir_path, sizeof(dir_path)-1))
					{
						if(!(saved_ioerr = IoErr()))
							saved_ioerr = ERROR_BUFFER_OVERFLOW;
					}
					parent = dir_path;
				}
				
				DBG_STRING(parent);
				
				if( saved_ioerr == 0 )
				{
					free_dirlist( entry );
					entry = NULL;
					ftp_list( site, parent, NULL, &dl, NULL, NULL, NULL);
					
					if(dl != NULL)
					{
						DBG("found folder \"%s\", looking entry \"%s\" at here...\n", parent, Dir );
						
						entry = lookup_dirlist( dl, Dir );
						
						if(entry != NULL)
						{
							// I'll free dl, hence need to copy...
							CopyMem( entry, &stk_entry, sizeof(struct dirlist));
							stk_entry.next = stk_entry.prev = NULL;
							entry = &stk_entry;
							free_entry = FALSE;
							
							DBG("finally, got the right entry :-) -> \"%s\"\n", entry->FileName);
						}
						else saved_ioerr = ERROR_OBJECT_NOT_FOUND;
						
						free_dirlist( dl );
					}
					else saved_ioerr = ERROR_OBJECT_NOT_FOUND;
				}
			}
		}
		else saved_ioerr = ERROR_OBJECT_NOT_FOUND;
	}
	//else dlt = entry->dlt;
	
	if( saved_ioerr == 0 )
		saved_ioerr = IoErr();
	
	if( saved_ioerr == 0 )
	{
		site->progress.tend.tv_sec  = 0;
		site->progress.tend.tv_usec = 0;
		site->progress.files.total  = 0;
		site->progress.files.done   = 0;
		site->progress.bytes.total  = i64_uset(0);
		site->progress.bytes.done   = i64_uset(0);
		te_delete(site);
		
		//switch( dlt )
		switch( entry->dlt )
		{
			case DLT_DIR:
			{
				Printf("Calculating '%s' size recursively, please wait...", (long) entry->FileName );
				Flush(Output());
				
				if((rc = ftp_retr( site, entry, *dir_path ? dir_path : NULL, TRUE )))
				{
					UBYTE h[32];
					
					Printf(" Found %ld files, %s, to %s.\n", site->progress.files.total, (long) i64_ToHuman( h, sizeof(h)-1, site->progress.bytes.total ),(long)"download");
					
					GetTimeOfDay(&(site->progress.tstart));
					
					rc = ftp_retr( site, entry, *dir_path ? dir_path : NULL, FALSE );
				}
			}	break;
			
			case DLT_FILE:
			{
				site->progress.files.total = 1;
				site->progress.bytes.total.hi = entry->Size.hi;
				site->progress.bytes.total.lo = entry->Size.lo;
				GetTimeOfDay(&(site->progress.tstart));
				
				rc = ftp_retr( site, entry, NULL, FALSE );
			}	break;
			
			case DLT_LINK:
			case DLT_UNKNOWN:
			default:
				PutStr("We can't handle this file type...\n");
				break;
		}
		
		if( rc == FALSE )
		{
			saved_ioerr = IoErr();
		}
		else if( site->progress.files.total && site->progress.files.total == site->progress.files.done )
		{
			// update the progress bar to show a 100% (unset for single files) and average CPSs
			Progress ( site, entry->FileName ) ;
		}
		
		te_report(site);
	}
	else Printf("Found no entry asociated with name \"%s\"\n",(long) &cmdline[1]);
	
	if( free_entry )
		free_dirlist( entry );
	
	if( saved_ioerr != 0 )
	{
		SetIoErr( saved_ioerr );
		Printf("Error getting \"%s\": %s\n",(long)&cmdline[1],(long) ioerrstr(NULL));
	}
	
	return rc;
}

FTPCOMMAND(put)
{
	BPTR lock;
	BOOL error = TRUE;
	
	if(*cmdline != ' ')
	{
		PutStr("Specify what to put!\n");
		return TRUE;
	}
	
	if((lock = Lock( &cmdline[1], SHARED_LOCK )))
	{
		D_S(struct FileInfoBlock,fib);
		
		if(Examine( lock, fib ))
		{
			site->progress.files.total  = 0;
			site->progress.files.done   = 0;
			site->progress.bytes.total  = i64_uset(0);
			site->progress.bytes.done   = i64_uset(0);
			te_delete(site);
			
			if(FIB_IS_DRAWER(fib))
			{
				UBYTE h[32];
				
				Printf("Calculating '%s' size recursively, please wait...", (long) &cmdline[1] );
				Flush(Output());
				
				ftp_stor_tree( site, &cmdline[1], TRUE );
				
				Printf(" Found %ld files, %s, to %s.\n", site->progress.files.total, (long) i64_ToHuman( h, sizeof(h)-1, site->progress.bytes.total ),(long)"upload");
				
				if( site->progress.files.total )
				{
					GetTimeOfDay(&(site->progress.tstart));
					ftp_stor_tree( site, &cmdline[1], FALSE );
				}
			}
			else if(FIB_IS_FILE(fib))
			{
				site->progress.files.total = 1;
				site->progress.bytes.total = i64_uset((ULONG)fib->fib_Size);
				GetTimeOfDay(&(site->progress.tstart));
				
				ftp_stor_file( site, &cmdline[1], fib->fib_Size );
			}
			else
			{
				PutStr("We can't handle this file type...\n");
			}
			
			if( site->progress.files.total && site->progress.files.total == site->progress.files.done )
			{
				// update the progress bar to show a 100% (unset for single files) and average CPSs
				Progress ( site, &cmdline[1] );
			}
			
			te_report(site);
		}
		
		UnLock( lock );
	}
	
#if 0
	if( error == TRUE )
	{
		if(IoErr() == 0)
			SetIoErr( ERROR_OBJECT_NOT_FOUND );
		
		Printf("Error uploading \"%s\": %s\n",(long)&cmdline[1],(long) ioerrstr(NULL));
	}
#endif
	
	return TRUE;
}

FTPCOMMAND(quit){SetIoErr( 0 );return FALSE;}
//----------------------------------------------------------------------------

STATIC VOID ftp_shell ( struct ftp_site * site )
{
	UBYTE buffer[4096];
	LONG len;
	
	static const struct {
		CONST_STRPTR name;  ULONG len;
		BOOL (*func)(FTPCOMMAND_ARGS);
	} cmds[] = 
	{
		{ "ls",		2,	FTPCOMMANDDECL(list) },
		{ "list",	4,	FTPCOMMANDDECL(list) },
		{ "dir",	3,	FTPCOMMANDDECL(list) },
		{ "quit",	4,	FTPCOMMANDDECL(quit) },
		{ "exit",	4,	FTPCOMMANDDECL(quit) },
		{ "cd",		2,	FTPCOMMANDDECL(cwd) },
		{ "cwd",	3,	FTPCOMMANDDECL(cwd) },
		{ "put",	3,	FTPCOMMANDDECL(put) },
		{ "get",	3,	FTPCOMMANDDECL(get) },
	/*	{ "",	,	FTPCOMMANDDECL() },
		{ "",	,	FTPCOMMANDDECL() },
	*/	{ "raw",	3,	FTPCOMMANDDECL(raw) },
		{ NULL, 	0,	NULL }
	}, *cmd;
	
	do {
		// print "shell" prompt
		FPrintf(Output(), "\r\n%s@%s:%s> ", (long) site->user, 
			(long) site->host, (long) (site->path ? (char *)site->path : "/"));
		Flush(Output());
		
		// wait for user input, aborting if ^C
		do {
			ULONG wait_sigs = SetSignal(0L,0L);
			
			if(wait_sigs & SIGBREAKF_CTRL_C)
			{
				SetIoErr( ERROR_BREAK );
				return;
			}
		} while(WaitForChar(Input(), 2000000) != -1);
		
		if((len = Read(Input(), buffer, sizeof(buffer)-1)) < 1)
		{
			PutStr("Error reading from input stream!\n");
			return;
		}
		
		buffer[len-1] = 0;
		if(!buffer[0])
			continue;
		
		for( cmd = cmds ; cmd->name != NULL ; cmd++ )
		{
			if(!Strnicmp( buffer, cmd->name, cmd->len ))
			{
				BOOL rc;
				
				rc = (*cmd->func)(site, &buffer[cmd->len]);
				
				if(FALSE==rc)
					return;
				break;
			}
		}
		
		if( cmd->name == NULL )
		{
			if(!Strnicmp( buffer, "help", 4 ))
			{
				ULONG x = 0;
				
				PutStr("available commands:\r\n\r\n");
				
				for( cmd = cmds ; cmd->name != NULL ; cmd++ )
				{
					Printf("\t%-8s", (long) cmd->name );
					
					if( ! (++x % 4 ))
						FPutC(Output(), '\n');
				}
			}
			else
				Printf(":: unrecognized command \"%s\"\a\r\n",(long) buffer );
		}
	} while(1);
}

STATIC VOID ftp_session( struct ftp_site * site )
{
	UBYTE user_var[] = "USERNAME", buffer[256], defuser[256];
	STRPTR ptr, old_path = NULL;
	long code;
	
	// Read WELCOME message
	switch((code = ftp_recv( site )))
	{
		case 220:
		//	DBG_STRING(site->recv_buffer);
			PutStr(&site->recv_buffer[4]);
			break;
		case -1:
			PutStr("error receiving welcome message\n");
			return;
		case -2:
			//PutStr("server seems not an FTP server (!?)\n");
			Printf("Server busy (?): %s\n", (long) site->recv_buffer);
			return;
		default:
			Printf("Server returned code %ld when %ld was wanted\n", code, 220);
			return;
	}
	
	if(0> GetVar( user_var, defuser, sizeof(defuser) -1, 0))
	{
		user_var[4] = 0; // "USERNAME" failed, try now "USER"
		
		if(0> GetVar( user_var, defuser, sizeof(defuser) -1, 0))
		{
			// both failed, fall back to...
			CopyMem( "anonymous", defuser, sizeof(defuser));
			buffer[9] = 0;
		}
	}
	
	// if no user supplied, request it
	if( site->user == NULL )
	{
		FPrintf( Output(), "Enter username (%s): ", (long) defuser );
		Flush(Output());
		
		if(GetInputString( buffer, sizeof(buffer)-1, FALSE ) != 0)
		{
			return;
		}
		
		if(!(site->user = StrDup((*buffer != 0) ? buffer : defuser)))
			return;
	}
	
	// send username to ftp server
	if(!ftp_send(site, "USER %s\r\n", site->user ))
	{
		PutStr("send() error\n");
		return;
	}
	
	// get response from server
	if((code = ftp_recv( site )) != 331 && (code != 230))
	{
		if(code > 0)
			PutStr(site->recv_buffer);
		else
			PutStr("error receiving login response\n");
		return;
	}
	
	if( code == 331 )
	{
		// request site's password
		FPrintf( Output(), "Enter password:");
		Flush(Output());
		
		if(GetInputString( buffer, sizeof(buffer)-1, TRUE ) != 0)
		{
			return;
		}
		
		if(!(site->pass = StrDup( buffer )))
			return;
		
		// send password to ftp server
		if(!ftp_send(site, "PASS %s\r\n", site->pass ))
		{
			PutStr("send() error\n");
			return;
		}
		
		// get pass response from server
		code = ftp_recv( site );
	}
	
	if( code != 230 )
	{
		if(code > 0)
			PutStr(site->recv_buffer);
		else
			PutStr("error receiving login response\n");
		return;
	}
	
	ptr = site->recv_buffer;
	while(*ptr && *ptr++ != '\n');
	*ptr = 0;
	PutStr(&site->recv_buffer[4]);
	
	// Log HOME directory
	if(site->path != NULL)
		old_path = StrDup(site->path);
	if(!ftp_pwd( site ))
		return;
	site->home = site->path;
	site->path = old_path;
	
	// check to cd to a different dir other than 'home'
	if(site->path != NULL)
	{
		/* TODO check if site->path point to a file, hence 
		 * download it directly without launching the shell
		 */
		if(!ftp_cwd( site, site->path ))
			return;
	}
	
	// now, "overwrite" site->path with the current progdir
	if(!ftp_pwd( site ))
		return;
	
	DBG_STRING(site->home);
	DBG_STRING(site->path);
	
	ftp_shell ( site ) ;
}

/*****************************************************************************/

LONG __main_interface ( VOID )
{
	STRPTR cmdline = GetArgStr();
	long saved_ioerr = 0;
	struct ftp_site * site = NULL;
	
	if(!(cmdline && *cmdline) || *cmdline == '?')
	{
		PutStr(__VerID);
		//PutStr("clftp, short for Command Line File Transfer Protocol, is as its name indiques 
		goto done;
	}
	
	if(!(site = parse_site( cmdline )))
		goto done;
	
	if((site->sockfd = Connect( site->host, site->port )) < 0)
	{
		PutStr("Cannot connect to host\n");
		goto done;
	}
	
	ftp_session( site );
	
done:
	if(saved_ioerr == 0)
		saved_ioerr = IoErr();
	
	PrintFault( saved_ioerr, "\a\r\n:");
	
	free_site( site );
	
	return saved_ioerr;
}
