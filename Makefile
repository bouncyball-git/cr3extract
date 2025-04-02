#
# Compiler flags
#
CC     = gcc
CFLAGS = -Wall -Werror -Wextra

#
# Project files
#
SRCS = cr3extract.c
OBJS = $(SRCS:.c=.o)
EXE  = cr3extract

#
# Debug build settings
#
DBGDIR = debug
DBGEXE = $(DBGDIR)/$(EXE)
DBGOBJS = $(addprefix $(DBGDIR)/, $(OBJS))
DBGCFLAGS = -g -O0 -DDEBUG

#
# Release build settings
#
RELDIR = release
RELEXE = $(RELDIR)/$(EXE)
RELOBJS = $(addprefix $(RELDIR)/, $(OBJS))
RELCFLAGS = -O3 -DNDEBUG

.PHONY: all clean debug prep release remake

# Default build
all: prep release debug

#
# Debug rules
#
debug: $(DBGEXE)
$(DBGEXE): $(DBGOBJS)
	$(CC) -o $(DBGEXE) $^
$(DBGDIR)/%.o: %.c
	$(CC) -c $(CFLAGS) $(DBGCFLAGS) -o $@ $<

#
# Release rules
#
release: $(RELEXE)
	strip $(RELEXE)
$(RELEXE): $(RELOBJS)
	$(CC) -o $(RELEXE) $^
$(RELDIR)/%.o: %.c
	$(CC) -c $(CFLAGS) $(RELCFLAGS) -o $@ $<

#
# Other rules
#
prep:
	@mkdir -p $(DBGDIR) $(RELDIR)

remake: clean all

clean:
	rm -f $(RELEXE) $(RELOBJS) $(DBGEXE) $(DBGOBJS)
