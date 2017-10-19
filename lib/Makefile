# LVNANOMSG GNU-MAKE BUILD SCRIPT
# for both Windows and Linux targets

SRC = nanomsg_labview.c
ARCH = 32
# WINDOWS BUILD RULES
ifeq ($(OS),Windows_NT)
	#SHELL := C:\Windows\System32\cmd.exe
	# figure out the architecture cl is set up for
	ifeq ($(ARCH),)
		$(error Target architecture undefined)
	else ifeq ($(ARCH),32)
		LABVIEW = "D:\Program Files (x86)\National Instruments\LabVIEW 2014\cintools"
		#LABVIEW = "E:\nanomsg\lvnanomsg\lib\LabVIEW"
	else ifeq ($(ARCH),64)
		LABVIEW = "D:\Program Files\National Instruments\LabVIEW 2014\cintools"
	else
		$(error Unknown architecture on Windows "$(ARCH)")
	endif
	# set appropriate flags
	CC = cl /nologo
	NANOMSG_LIB = "E:\nanomsg\lvnanomsg\lib\nanomsg-1.0.0\lib\Release\nanomsg.lib"
	LABVIEW_LIB = "D:\Program Files (x86)\National Instruments\LabVIEW 2014\cintools\labviewv.lib"
	# note that we define DLL_EXPORT to prevent trying to link to DLL stub functions
	CFLAGS = /O1 /W3 /MD /I . /I $(LABVIEW)
	#CFLAGS = /O1 /W3 /MD /I . /I $(LABVIEW)\cintools
	LDLIBS = /link $(NANOMSG_LIB) user32.lib $(LABVIEW_LIB)
	ifdef DEBUG
		CFLAGS += /DDEBUG
	endif
all : lvnanomsg$(ARCH).dll
# copy the product to the labview directory
	#@del *.obj ..\*.exp ..\*.lib ..\*.manifest
	@copy $< $(LABVIEW)\vi.lib\addons\nanomsg

# LINUX BUILD RULES
else
	# omg so much simpler!
	LABVIEW = ./LabVIEW
	NANOMSG = ./nanomsg-1.0.0
	LVRT = ./x64
	#CFLAGS = -Wall -O3 -m32 -fpic -I $(LABVIEW)/cintools -L $(NANOMSG)/build -L $(LVRT)
	CFLAGS = -Wall -O3 -fpic -I ./ -I $(LABVIEW)/cintools -L $(NANOMSG)/build -L $(LVRT)
	LDLIBS = -lnanomsg -llvrt
	ifdef DEBUG
		CFLAGS += -DDEBUG
	endif

all : lvnanomsg.so
# copy the product to the labview directory
	@cp $< $(LABVIEW)/vi.lib/addons/nanomsg
endif

# Linux shared-library target
lvnanomsg.so : $(SRC)
	$(CC) -shared -o $@	$^ $(CFLAGS) $(LDFLAGS) $(LDLIBS)

# Architecture-dependent build rules -- note explicit checks machine type
lvnanomsg32.dll : $(SRC)
	$(CC) /LD /Fe$@ $^ $(CFLAGS) $(LDFLAGS) $(LDLIBS) /machine:X86

lvnanomsg64.dll : $(SRC)
	$(CC) /LD /Fe$@ $^ $(CFLAGS) $(LDFLAGS) $(LDLIBS) /machine:X64
