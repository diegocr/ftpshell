PROG	= ftpshell
ARCH	= m68k-amigaos
CC	= $(ARCH)-gcc $(CPU) -noixemul
LIB	= -Wl,-Map,$@.map,--cref -lamiga
DEFINES	= -DSTACK_SIZE=262144 -DVERSION='"0.1"'
WARNS	= -W -Wall -Winline
CFLAGS	= -O3 -funroll-loops -fomit-frame-pointer $(WARNS) $(DEFINES)
LDFLAGS	= -nostdlib
OBJDIR	= .objs$(SYS)
RM	= rm -frv

ifdef DEBUG
	CFLAGS += -DDEBUG -g
endif
ifdef SMALLCODE
# main.c uses everything inlined, but using small-code causes the link error:
# "Trying to force a pcrel thing into absolute mode while in small code mode"
	CFLAGS += -msmall-code
endif

OBJS =	\
	$(OBJDIR)/startup.o	\
	$(OBJDIR)/main.o	\
	$(OBJDIR)/debug.o

all:
	make $(PROG) SYS=_060 CPU="-m68060 -m68881"
#	make $(PROG)_020 SYS=_020 CPU=-m68020

$(PROG): $(OBJDIR) $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(LIB)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: %.c
	@echo Compiling $@
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/startup.o: startup.c
	@echo Compiling $@
	@$(CC) $(CFLAGS) -fwritable-strings -c $< -o $@

clean:
	$(RM) $(PROG)_0*0 $(OBJDIR)_0*0

############################################################################
# depends

$(OBJDIR)/main.o:	debug.h
$(OBJDIR)/utils.o:	utils.h debug.h
$(OBJDIR)/startup.o:	makefile
$(OBJDIR)/rdargs.o:	rdargs.h debug.h
$(OBJDIR)/fonts.o:	fonts.h rdargs.h
$(OBJDIR)/genimage.o:	fonts.h rdargs.h debug.h
