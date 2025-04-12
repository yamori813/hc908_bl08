// TODO POSIX COMPATIBLE IOCTL
// Note page size for JB8 has been temporarily changed to 32
/*
File: bl08.c

Copyright (C) 2004,2008  Kustaa Nyholm

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public
License as published by the Free Software Foundation; either
version 2.0 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/
// This code compiles cleanly with i686-apple-darwin8-gcc-4.0.1 with command:
// gcc -Wall -std=c99 -pedantic -o bl08 bl08.c
// and uses only POSIX standard headers so this should be pretty 
// easy to port anywhere: Mac OS X,  Linux , MinGW, Cygwin
//
// Having said that I'm pretty sure there is implicit assumptions that
// int is 32 bits, char is signed 8 bits
// I also expect that extending this code to handle S-records in the +2G range
// will uncover more implicit assumptions.
//
// cheers Kusti
// additions ... override value for non supported CPUs
// reset control with DTR
#include <stdlib.h>
#include <fcntl.h>
#include <termios.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>

// Terrible hack here because POSIX says 'ioctl()' is in <stropts.h> but
// for example Mac OS X Tiger does not have this header, OTOH I think
// that <sys/ictl.h> is not POSIX either so how do you write
// actual POSIX compliant code that compiles cleanly on POSIX...
//extern int ioctl (int filedes, int command, ...);
// End of hack
	

 
#define PIDFILENAME "bl08PIDfile.temp"

char* COM = "/dev/tty.usbserial-FTOXM3NX";
//char* COM = "/dev/ttyS0";

int com;

char version[] = "1.0.0.0";

unsigned char image[ 0x10000 ]; // HC908 memory image


// give some initial meaningfull values (based on MC68HC908GZ16)
int FLASH=0xE000; // Flash start address
int PUTBYTE=0xFEAF; // Receive byte routine address
int GETBYTE=0x1C00; // Receive byte routine address
int RDVRRNG=0x1C03; // Read/verify flash routine address
int ERARRNG=0x1C06; // Erase flash routine address
int PRGRNGE=0x1C09; // Flash programming routine address
int FLBPR=0xFF7E;	// Flash block proctection register address
int CPUSPEED=8; // 2 x Fbus freq, e.g.  ext osc 16 MHz -> Fbus == 4 Mh => CPUSPEED==2
int MONDATA=0x48; // Flashing routines parameter block address
int MONRTN = 0xFE20; // Monitor mode return jump address
int EADDR = 0xFF7E; // For FLBPR in Flash the mass erase must use FLBPR as the erase address
// these are calculated
int DATABUF; // Flashing routines data buffer address (==MONDATA+4)
int PAGESIZE; // Databuffer size
int WORKRAM; // Work storage needed for calling flashing routines
int WORKTOP; // Topmost work storage address
int CTRLBYT; // Address of flashing routine control variable (==MONDATA+0)
int CPUSPD; // Address of flashing routine cpu speed variable (==MONDATA+1)
int LADDR;  // Address of flashing routine last address variable (==MONDATA+2)

// HC908GZ16 Memory usage (note for some HC908 variants the ROM routines use memory starting from 0x80):
// 0x40 - 0x47 reserved for future ROM routine expansion needs
// 0x48 - 0x4B ROM routine parameters
// 0x4C - 0x6C ROM routine data buffer (64 bytes as used in this code)
// 0xAC - 0xFF Working storage for calling the ROM routines (about 17 bytes used)


// Cavets
// DATABUF size
// security erase at FLBPR, power cycle

int tickP1=15;
int tickP2=1023;
int verbose = 1;
int size = sizeof(image);
int useStdin=0;
int dumpStart=0;
int dumpSize=0;
char* dumpFormat="hex";
int eraseFlash=0;
int verify=0;
int baudRate=B9600;
char* executeCode=NULL;
int pageErase=0;
int uploadOnly=0;
int terminalMode=0;
int connected=0;
int useFastProg=0;
int resetPulse=0;
int killPrevious=0;
int loadOnly=0;

void comErr(char *fmt, ...) {
	char buf[ 500 ];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	fprintf(stderr,"%s", buf);
	perror(COM);
	va_end(va);
	abort(); 
	}

void flsprintf(FILE* f, char *fmt, ...) {
	char buf[ 500 ];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	fprintf(f,"%s", buf);
	fflush(f);
	va_end(va);
	}
	
void initSerialPort() {
	com =  open(COM, O_RDWR | O_NOCTTY | O_NDELAY);
	if (com <0) 
		comErr("Failed to open seria port");
		
	fcntl(com, F_SETFL, 0);
		
	struct termios opts;
	
	tcgetattr(com, &opts);

	opts.c_lflag  &=  ~(ICANON | ECHO | ECHOE | ISIG);

	opts.c_cflag |=  (CLOCAL | CREAD);
	opts.c_cflag &=  ~PARENB;
	opts.c_cflag |=  CSTOPB; // two stop bits
	opts.c_cflag &=  ~CSIZE;
	opts.c_cflag |=  CS8;
	
	opts.c_oflag &=  ~OPOST;
	
	opts.c_iflag &=  ~INPCK;
	opts.c_iflag &=  ~(IXON | IXOFF | IXANY);	
	opts.c_cc[ VMIN ] = 0;
	opts.c_cc[ VTIME ] = 10;//0.1 sec
	

	cfsetispeed(&opts, baudRate);   
	cfsetospeed(&opts, baudRate);   
	
	if (tcsetattr(com, TCSANOW, &opts) != 0) {
		perror(COM); 
		abort(); 
		}
		
	tcflush(com,TCIOFLUSH); // just in case some crap is the buffers
		
	char buf = -2;
	while (read(com, &buf, 1)>0) {
		if (verbose)
			printf("Unexpected data from serial port: %02X\n",buf & 0xFF);
		}

	}
	

	
void putByte(int byte) {
	char buf = byte;
	if (verbose>3)
		flsprintf(stdout,"TX: 0x%02X\n", byte);
	int n = write(com, &buf, 1);
	if (n != 1)
		comErr("Serial port failed to send a byte, write returned %d\n", n);
	}
	
int getByte() {
	char buf;
	int n = read(com, &buf, 1);
	if (verbose>3)
		flsprintf(stdout,n<1?"RX: fail\n":"RX:  0x%02X\n", buf & 0xFF);
	if (n == 1)
		return buf & 0xFF;
	
	comErr("Serial port failed to receive a byte, read returned %d\n", n);
	return -1; // never reached
	}

// This reads away break 'character' from the serial line
void flushBreak() { 
	int i;
	for (i=0; i<2; ++i) {
		char buf;
		int n = read(com, &buf, 1);
		if (verbose>3)
			flsprintf(stdout,n<1?"FL: nothing\n":"FL:  0x%02X\n", buf & 0xFF);
		}
	}
		
void sendByte(int byte) {
	byte &=  0xFF;
	putByte(byte);
	char buf;
	if (read(com, &buf, 1)!=1)
		comErr("Loopback failed, nothing was received\n");
	int rx=buf &0xFF;
	if (byte !=  rx)
		comErr("Loopback failed, sent 0x%02X, got 0x%02X\n", byte,  rx);
	rx = getByte();
	if (byte !=  rx)
		comErr("Target echo failed, sent 0x%02X, got 0x%02X\n", byte, rx);
	}
	

void readMemory(int addr, int n,int tick) {
	if (verbose>2) 
		flsprintf(stdout,"Read memory address %04X size %04X\n",addr,n);
	unsigned char* p = &image[ addr ];
	sendByte(0x4A); // Monitor mode READ command
	sendByte(addr >> 8);
	sendByte(addr & 0xFF);
	*(p++) = getByte();
	n--;
	int tc=0;
	while (n>0) {
		sendByte(0x1A); // Monitor mode IREAD command
		int b1 = getByte();
		int b2 = getByte();
		*(p++) = b1;
		n--;
		if (n > 0)
			*(p++) = b2;
		n--;
		if (tick) {
			tc++;
			if ((tc & tickP1)==0)
				flsprintf(stdout,".");
			//if ((tc & tickP2)==0)
			//	flsprintf(stdout,"\n");
			}
		}
	}

void writeMemory(int addr, int n, int tick) {
	if (verbose>2) 
		flsprintf(stdout,"Write memory address %04X size %04X\n",addr,n);
	unsigned char* p = &image[ addr ];
	sendByte(0x49); // Monitor mode WRITE command
	sendByte(addr >> 8);
	sendByte(addr & 0xFF);
	sendByte(*(p++));
	int tc=1;
	while (n>1) {
		sendByte(0x19); // Monitor mode IWRITE command
		sendByte(*(p++));
		n -= 1;
		if (tick) {
			tc++;
			if ((tc & tickP1)==0)
				flsprintf(stdout,".");
			//if ((tc & tickP2)==0)
			//	flsprintf(stdout,"\n");
			}
		}
	}
	
void connectTarget() {
	int j;
	if (connected)
		return;
	// Hmmm, following does not work, how should we do this
	// For blank device we need to send FF, but for non blank something else, oth
	// reprogramming a non blank device, does it make sense
	for (j = 0; j<8; ++j)  
		sendByte(0xFF);
	flushBreak();
	readMemory(0x40, 1 , 0);
	connected=1;
	if ((image[ 0x40 ] & 0x40) ==0)
		flsprintf(stdout,"Failed to unlock the security");
	
	// in case FLBPR is RAM based we clear it first by just writing it
	image[FLBPR]=0xFF;
	writeMemory(FLBPR,1,0);
	}
	

void dumpMemory(int addr, int n) {
	unsigned char* p = &image[ addr ];
	int i;
	for (i = 0; i<n; ++i) {
		if ((i&0xF) == 0)
			flsprintf(stdout,"%04X  ", addr+i);
		flsprintf(stdout,"%02X ", *(p++) & 0xFF);
		if ((i&0xF) == 7)
			flsprintf(stdout," ");
		if ((i&0xF) == 15)
			flsprintf(stdout,"\n");
		}
	if ((i&0xF) != 0)
		flsprintf(stdout,"\n");
	}
	
	
void dumpMemorySrec(int addr, int size) {
	unsigned char* p = &image[ addr ];
	while (size>0) {
		int n = size>16 ? 16 : size;
		int bc=2+n+1;
		flsprintf(stdout,"S1%02X%04X",bc,addr);
		int s=(addr >> 8) + (addr & 0xFF) + bc;	
		int i;
		for (i=0; i<n; ++i) {
			int bty=*p & 0xFF;
			s += bty;
			flsprintf(stdout,"%02X",bty);
			++p;
			++addr;
			}
		size -= n;
		flsprintf(stdout,"%02X\n",~s & 0xFF);
		}
	}

int readSP() {
	if (verbose>2) 
		flsprintf(stdout,"Read Stak Pointer\n");
	sendByte(0x0C); // Monitor mode READSP command
	return  (((getByte() << 8) | (getByte() & 0xFF)) - 1) & 0xFFFF;
	}	

int runFrom(int PC, int A, int CC, int HX) {
	int SP=readSP();
	if (verbose>2) 
		flsprintf(stdout,"Execute code PC=%04X A=%02X CC=%02X H:X=%04X SP=%04X\n",PC,A,CC,HX,SP);
	image[ SP + 1 ] = HX >> 8;
	image[ SP + 2 ] = CC;
	image[ SP + 3 ] = A;
	image[ SP + 4 ] = HX & 0xFF;
	image[ SP + 5 ] = PC >> 8;
	image[ SP + 6 ] = PC & 0xFF;
	writeMemory(SP + 1 , 6 , 0);
	sendByte(0x28); // Monitor mode RUN command
	return SP;
	}	
			
int lastMon=-1;

int callMonitor(int mon, int ctrlbyt, int accu, int faddr, int laddr) {
	int SP = readSP();
	image[ CTRLBYT ] = ctrlbyt; // CTRLBYT BIT 6  =  1  = > mass erase
	image[ CPUSPD ] = CPUSPEED; // CPUSPD  =  16 MHz ext clock  = > 4 MHz Fbus speed  = > 8
	image[ LADDR ] = laddr>>8;
	image[ LADDR+1 ] = laddr&0xFF;
	writeMemory(MONDATA, 4, 0);
		
	if (WORKRAM>0xFF) {
		flsprintf(stderr,"Work RAM must be on zero page");
		abort();
		}

	if (lastMon!=mon) {
		// construct small HC908 code fragment to call the monitor function and return results
		int i = WORKRAM+2;
		
		image[ i++ ] = 0xCD; // JSR mon ; calls the monitor routine
		image[ i++ ] = mon>>8;
		image[ i++ ] = mon&0xFF;
		
		image[ i++ ] = 0x87; // PSHA ; save condition A
		image[ i++ ] = 0x85; // TPA  ; condition codes to A
		image[ i++ ] = 0xB7; // STA  ; store to WORKRAM
		image[ i++ ] = WORKRAM;
		image[ i++ ] = 0x86; // PULA ; restore A
		image[ i++ ] = 0xB7; // STA  ; store to WORKRAM+1
		image[ i++ ] = WORKRAM+1;

		image[ i++ ] = 0x45; // LDHX #SP+1 ; restore stack pointer
		image[ i++ ] = (SP+1) >> 8;
		image[ i++ ] = (SP+1) & 0xFF;
		image[ i++ ] = 0x94; // TXS 
		
		image[ i++ ] = 0xCC; // JMP back to MON (this is the only way)
		image[ i++ ] = MONRTN>>8;
		image[ i++ ] = MONRTN&0xFF;
		
		if (WORKRAM>=WORKTOP) { // leave some stack space for monitor routines
			flsprintf(stderr,"Not enough WORKRAM on target");
			abort();
			}
		
		writeMemory(WORKRAM , i-WORKRAM , 0);
		lastMon=mon;
		}
	
	// now execute the fragment
	runFrom(WORKRAM+2, accu, 0x00, faddr); 

	// fetch the return values 
	readMemory(WORKRAM , 2, 0);
	return ((image[ WORKRAM ] & 0xFF) << 8) | (image[ WORKRAM+1 ] & 0xFF); // return condition codes and accu
	}	
	
int fastProg(int faddr,int n) {
	static int n_addr;
	static int last_n;
	if (WORKRAM>0xFF) {
		flsprintf(stderr,"Work RAM must be on zero page");
		abort();
		}


	if (lastMon!=-1) {
		int SP = readSP();
		image[ CTRLBYT ] = 0; // CTRLBYT =0
		image[ CPUSPD ] = CPUSPEED; // CPUSPD  =  16 MHz ext clock  = > 4 MHz Fbus speed  = > 8

		// construct small HC908 code fragment to call the monitor function and return results
		int i = WORKRAM;
		image[ i++ ] = 0x9D; // NOP / reserve space
		image[ i++ ] = 0x9D; // NOP / reserve space
				
		image[ i++ ] = 0x35; // STHX WORKRAM (dir)
		image[ i++ ] = WORKRAM;
		
		image[ i++ ] = 0x45; // LDHX #DATABUF
		image[ i++ ] = DATABUF>>8;
		image[ i++ ] = DATABUF&0xFF;
		
		image[ i++ ] = 0xCD; // JSR GETBYTE ; calls the monitor routine
		image[ i++ ] = GETBYTE>>8;
		image[ i++ ] = GETBYTE&0xFF;
//		image[ i++ ] = 0xFE9C>>8;  ; // 0xFE9C = GET WITH echo
//		image[ i++ ] = 0xFE9C&0xFF;

		image[ i++ ] = 0xF7; // STA ,X
		
		image[ i++ ] = 0x5C; // INCX
		
		image[ i++ ] = 0xA3; // cpx #DATABUF+n
		n_addr=i;
		image[ i++ ] = 0; // place holder

		image[ i++ ] = 0x25; // BLO *-10
		image[ i++ ] = (-9)&0xFF;
		
		image[ i++ ] = 0x55;
		image[ i++ ] = WORKRAM;
		
		image[ i++ ] = 0xCD; // JSR PRGRNGE ; calls the monitor routine
		image[ i++ ] = PRGRNGE>>8;
		image[ i++ ] = PRGRNGE&0xFF;
		
		image[ i++ ] = 0x45; // LDHX #SP+1 ; restore stack pointer
		image[ i++ ] = (SP+1) >> 8;
		image[ i++ ] = (SP+1) & 0xFF;
		
		image[ i++ ] = 0x94; // TXS 
		
		image[ i++ ] = 0xCC; // JMP back to MON (this is the only way)
		image[ i++ ] = MONRTN>>8;
		image[ i++ ] = MONRTN&0xFF;
		
		if (WORKRAM>=WORKTOP) { // leave some stack space for monitor routines
			flsprintf(stderr,"Not enough WORKRAM on target");
			abort();
			}
		
		writeMemory(WORKRAM , i-WORKRAM , 0);
		//dumpMemorySrec(WORKRAM , i-WORKRAM);
		lastMon=-1;
		}
		
	if (last_n!=n) {
		image[ n_addr ] = DATABUF+n;
		writeMemory(n_addr,1,0);
		last_n=n;
		}
		
	int laddr = faddr+n-1;
	image[ LADDR ] = laddr>>8;
	image[ LADDR+1 ] = laddr&0xFF;
	writeMemory(LADDR, 2, 0); 
	
	// now execute the fragment
	runFrom(WORKRAM+2, 0x00, 0x00, faddr); // NOTE! to override flash security and erase FLBPR at be used as the erase address for mass erase
	
	int i;
	for (i=0; i<n; ++i) 
		putByte(image[faddr+i]);
	for (i=0; i<n; ++i) 
		getByte();
		
	readSP();
	return 0;
	}	
	
int fastProg2(int faddr,int progn) {
	if (WORKRAM>0xFF) {
		flsprintf(stderr,"Work RAM must be on zero page");
		abort();
		}


	int SP = readSP();
	image[ CTRLBYT ] = 0; // CTRLBYT =0
	image[ CPUSPD ] = CPUSPEED; // CPUSPD=  16 MHz ext  => 4 MHz Fbus  = > 8

	// construct small HC908 code fragment to call the monitor function 
	int i = WORKRAM;
	int j;
	
	int FADDR=i;
	image[ i++ ] = faddr>>8; // FADDR initial contents
	image[ i++ ] = faddr&0xFF; 
	
	int PROGN=i;
	image[ i++ ] = progn>>8; // PROGN initial contents
	image[ i++ ] = progn&0xFF;	
		
	int start=i;
	
	if (lastMon!=-1) {
		image[ i++ ] = 0x55 ; //	LDHX PROGN  get bytes left to program
		image[ i++ ] = PROGN;		
		
		image[ i++ ] = 0x27 ; //	BEQ	DONE branch if all done
		int patchDone=i;
		image[ i++ ] = 0 ;
		
		image[ i++ ] = 0x65 ; //	CPHX #PAGESIZE  only page full at a time
		image[ i++ ] = 0 ; 
		image[ i++ ] = PAGESIZE; 
		
		
		image[ i++ ] = 0x25 ; //	BLO DOIT
		image[ i++ ] = 2 ; 
		
		image[ i++ ] = 0xAE ; //	LDX #PAGESIZE
		image[ i++ ] = PAGESIZE;  
		
		image[ i++ ] = 0x9F ; //	TXA
		
		image[ i++ ] = 0x87 ; //	PSHA
		
		image[ i++ ] = 0xB6 ; //	LDA  PROGN+1
		image[ i++ ] = PROGN+1;	
		
		image[ i++ ] = 0x9E; //		SUB 1,SP	
		image[ i++ ] = 0xE0;	
		image[ i++ ] = 1;	
		
		image[ i++ ] = 0xB7 ; //	STA PROGN+1
		image[ i++ ] = PROGN+1;	
		
		image[ i++ ] = 0xB6 ; //	LDA  PROGN
		image[ i++ ] = PROGN;	
		
		image[ i++ ] = 0xA2 ; //	SBC #0
		image[ i++ ] = 0 ; 
		
		image[ i++ ] = 0xB7 ; //	STA PROGN
		image[ i++ ] = PROGN;	

		image[ i++ ] = 0x9F ; //	TXA
				
		image[ i++ ] = 0x4A ; //	DECA
		
		image[ i++ ] = 0xBB ; //	ADD FADDR+1
		image[ i++ ] = FADDR+1;	
			
		image[ i++ ] = 0xB7 ; //	STA LADDR+1
		image[ i++ ] = LADDR+1;	
			
		image[ i++ ] = 0xB6 ; //	LDA  FADDR
		image[ i++ ] = FADDR;	
		
		image[ i++ ] = 0xA9 ; //	ADC #0
		image[ i++ ] = 0 ; 
		
		image[ i++ ] = 0xB7 ; //	STA  LADDR
		image[ i++ ] = LADDR;	
		
		image[ i++ ] = 0x45; //		LDHX #DATABUF
		image[ i++ ] = DATABUF>>8;
		image[ i++ ] = DATABUF&0xFF;
		
		image[ i++ ] = 0x86 ;//		PULA		

		int getMore=i;
		
		image[ i++ ] = 0x87 ; //	PSHA
				
		image[ i++ ] = 0xCD; //		JSR GETBYTE 
		image[ i++ ] = GETBYTE>>8;
		image[ i++ ] = GETBYTE&0xFF;
		
		image[ i++ ] = 0xF7; //		STA ,X
		
		image[ i++ ] = 0x5C; //		INCX
		
		image[ i++ ] = 0x86 ; //	PULA
		
		image[ i++ ] = 0x4A ; //	DECA
		
		image[ i++ ] = 0x26 ; //	BNE GETMORE
		j=i;
		image[ i++ ] = (getMore-(j+1))&0xFF ; 
		
		image[ i++ ] = 0x55 ; //	LDHX FADDR
		image[ i++ ] = FADDR;		

		image[ i++ ] = 0xCD; //		JSR PRGRNGE 
		image[ i++ ] = PRGRNGE>>8;
		image[ i++ ] = PRGRNGE&0xFF;
		
		image[ i++ ] = 0x8B;		// PSHH Annoyingly JB8 RDVRNG leaves H:X off by one
		image[ i++ ] = 0x89;		// PSHX compared to GZ16 necessiating this pus/pul sequence
		
		
		image[ i++ ] = 0x55 ;//		LDHX FADDR
		image[ i++ ] = FADDR;		

		image[ i++ ] = 0xA6 ; //	LDAA #1
		image[ i++ ] = 1 ; 
		
		image[ i++ ] = 0xCD; //		JSR RDVRRNG 
		image[ i++ ] = RDVRRNG>>8;
		image[ i++ ] = RDVRRNG&0xFF;
		
		image[ i++ ] = 0x88;		// PULX
		image[ i++ ] = 0x8A;		// PULH

		image[ i++ ] = 0x35 ; //	STHX FADDR
		image[ i++ ] = FADDR;	
		
		image[ i++ ] = 0xCD; //		JSR PUTBYTE 
		image[ i++ ] = PUTBYTE>>8;
		image[ i++ ] = PUTBYTE&0xFF;
							
		image[ i++ ] = 0x20 ; //	BRA start
		j=i;
		image[ i++ ] = (start-(j+1))&0xFF;
		
		image[patchDone] = (i-(patchDone+1))&0xFF;
		
		image[ i++ ] = 0x45; // LDHX #SP+1 ; restore stack pointer
		image[ i++ ] = (SP+1) >> 8;
		image[ i++ ] = (SP+1) & 0xFF;
		
		image[ i++ ] = 0x94; // TXS 
		
		image[ i++ ] = 0xCC; // JMP back to MON (this is the only way)
		image[ i++ ] = MONRTN>>8;
		image[ i++ ] = MONRTN&0xFF;
	
		if (i>=WORKTOP) { // leave some stack space for monitor routines
			flsprintf(stderr,"Not enough WORKRAM on target");
			abort();
			}
		
		writeMemory(WORKRAM , i-WORKRAM , 0);
		lastMon=-1;
		}
	else
		writeMemory(WORKRAM , 4 , 0);
		
	runFrom(start, 0x00, 0x00, 0); 
	
	while (progn) {
		int n = progn < PAGESIZE ? progn : PAGESIZE;
		int sum=0;
		for (i=0; i<n; ++i) {
			putByte(image[faddr+i]);
			}
		for (i=0; i<n; ++i) {
			int b=getByte();
			//flsprintf(stderr,"%d %02X\n",i,b);
			sum = (sum+b)&0xFF;
			if (b != image[faddr+i])
				flsprintf(stderr,"Program data transfer error, at %04X sent %02X got %02\n",faddr+i,image[faddr+i],b);
			}
		int back=getByte();
			//flsprintf(stderr,"%02X\n",back);
		if (back != sum) 
			flsprintf(stderr,"Program checksum failure, at %04X size %04X checksum calculated %02X received %02X\n",faddr,n,sum,back);
				
		progn -= n;
		faddr += n;
		if (verbose)
			flsprintf(stdout,".");
		}
	return 0;
	}	
		
void massErase() {
// NOTE! to override flash security and erase FLBPR must be used as the erase address for mass erase
	if (verbose) 
		flsprintf(stdout,"Mass erase\n");
	callMonitor(ERARRNG , 0x40, 0, EADDR, 0);
	}
	
void flashProgram(int addr,int size,int verify) {
	if (addr < FLASH) {
		flsprintf(stdout,"Programming address %04X below flash start address %04X\n",addr,FLASH);
		exit(0);
		}
	
	if (useFastProg) {
		if (verbose) 
			flsprintf(stdout,"Program %04X - %04X ",addr,addr+size-1);
				
		fastProg2(addr,size);
		if (verbose)
			flsprintf(stdout,"\n");
		}
	else {
		while (size>0) {
			int	n=size <= PAGESIZE ? size : PAGESIZE;
			if (verbose) 
				flsprintf(stdout,"Program %04X - %04X ",addr,addr+n-1);
				
			memcpy(&image[ DATABUF ] , &image[addr] , n);
			writeMemory(DATABUF , n , verbose);
			callMonitor(PRGRNGE, 0 , 0, addr , addr+n-1);
			
			if (verify) {
				int cca=callMonitor(RDVRRNG, 0 , 1 , addr , addr+n-1);
				int sum=0;
				int i;
				for (i=0; i<n; ++i) 
					sum = (sum + image[addr+i]) & 0xFF;
				int back= cca & 0xFF; // get Accu from target
				if (sum != back) {
					flsprintf(stderr,"Program checksum failure, at %04X size %04X checksum calculated %02X received %02\n",addr,n,sum,back);
					abort();
					}
					
				if (!(cca & 0x0100)) { // check Carry bit from target
					flsprintf(stderr,"Verify failed\n");
					abort();
					}
				if (verbose)
					flsprintf(stdout,"OK");
				}
			if (verbose)
				flsprintf(stdout,"\n");
			addr += n;
			size -= n;
			}
		}
	}

int readSrec(int verbose,FILE* sf,unsigned char* image, int size,  int base, int* ranges, int rn) {
	if (verbose)
		flsprintf(stdout,"Reading S-records\n");
	memset(image,0xff,size);
	char line[2+2+255*2+2+1]; // Sx + count + 255 bytes for data address & checksum + CR/LF +nul (in windows)
	int amax=0;
	int rc=0;
	while (fgets(line,sizeof(line),sf)!=NULL) {
	   int o=0;
	   if (line[0]=='S') {
			unsigned int n,a;
			sscanf(line+2,"%2x",&n);
			n--;
			if (line[1]=='1') {
				sscanf(line+4,"%4x",&a);
				n=n-2;
				o=8;
			}
			if (line[1]=='2') {
				sscanf(line+4,"%6x",&a);
				n=n-4;
				o=10;
			}
			if (line[1]=='3') {
				sscanf(line+4,"%8x",&a);
				n=n-6;
				o=12;
			}
			if (o!=0) {
				int i,j;
				if (ranges) {
				    for (i=0; i<rc; i+=2) {
						int rlo=ranges[i];
						int rhi=rlo+ranges[i+1];
						if (!((a+n<=rlo) || (rhi<=a))) {
							flsprintf(stderr,"Overlapping S-record ranges %04X,%04X and %0x4 %04X\n",rlo,rhi-rlo,a,n);
							abort();
							}
						}
					if (rc + 2 >= rn) 
						return -1;
					ranges[rc]=a;
					ranges[rc+1]=n;
					rc += 2;
					
					int cf=0;
					do compact: {
						for (i=0; i<rc; i+=2) {
							for (j=i+2; j<rc; j+=2) {
								cf=1;
								if (ranges[i]+ranges[i+1]==ranges[j])
									ranges[i+1] += ranges[j+1];
								else if (ranges[i]==ranges[j]+ranges[j+1])
									ranges[i]-=ranges[j+1];
								else 
									cf=0;
								if (cf) {
									for (i=j+2; i<rc; i++)	
										ranges[i]=ranges[i+2];
									rc-=2;
									cf=0;
									goto compact;
									}
								}
							}
						} while (cf);
					}
				for (i=0; i<n; ++i) {
					unsigned int d;
					sscanf(line+o+i*2,"%2x",&d);
					if ( (a >= base) && (a < base+size)) {
						image[ a - base ] = d;
						a++;
						amax = a>amax ? a : amax;
						}
					}
				}
			}
		if (verbose>1)
			flsprintf(stdout,">>> %s",line);
		if (verbose && o==0)
			flsprintf(stdout,"Line ignored: %s\n",line);
		}
	if (verbose) {
		if (ranges) {
			int i;
			for (i=0; i<rc; i+=2) 
				flsprintf(stdout,"S-record data address %06X size %06X\n",ranges[i],ranges[i+1]);
			}
		flsprintf(stdout,"\n");
		}
	return rc;
	}
void printHelp() {
		flsprintf(stdout,"bl08 burns MC68HC908 Flash memory from S-record file(s) using Monitor mode\n");
		flsprintf(stdout,"Usage: \n");
		flsprintf(stdout," bl08 [-abcdefhiklmnpqrstuvx] [filename...]\n");
		flsprintf(stdout,"  -a address     Set dump memory address (needs -s option too)\n");
		flsprintf(stdout,"  -b baudrate    Set baudrate for target communication\n");
		flsprintf(stdout,"  -c device      Set serial com device used to communicate with target\n"); 
		flsprintf(stdout,"                 typically /dev/ttyS0 \n");
		flsprintf(stdout,"  -d dumpformat  Set dump format, supported formats are: 'srec'\n");
		flsprintf(stdout,"  -e             Erase target using mass erase mode, clearing security bytes\n");
		flsprintf(stdout,"  -f             Use fast programming method");		
		flsprintf(stdout,"  -g address     Go execute target code from address or use '-g reset'\n");
		flsprintf(stdout,"  -h             Print out this help text\n");
		flsprintf(stdout,"  -i             Read input (S-records) from standard inputi\n");
		flsprintf(stdout,"  -k             Kill previous instance of bl08\n");
		flsprintf(stdout,"  -l verbosity   Set verbosity level, valid values are 0-4, default 1\n");
		flsprintf(stdout,"  -m             Terminal emulator mode\n");
		flsprintf(stdout,"  -n             Print bl08 version number\n");
		flsprintf(stdout,"  -o param=value Override target parameter value\n");	
		flsprintf(stdout,"                 param = ROMBASE,FLASH,PUTBYTE,GETBYTE,RDVRRNG,MONRTN\n");
		flsprintf(stdout,"                         ERARRNG,PRGRNGE,FLBPR,MONDATA,PAGESIZE,EADDR\n");	
		flsprintf(stdout,"  -p             Use page erase when programming flash\n");	
		flsprintf(stdout,"  -q             Run quietly, same as -l 0\n");		
		flsprintf(stdout,"  -s size        Set dump memory size\n");
		flsprintf(stdout,"  -r pulse       Pulse DTR for pulse milliseconds\n");
		flsprintf(stdout,"  -t cputype     Set CPU type, valid values are: 'gz16'\n");
		flsprintf(stdout,"  -u             Upload only (do not program flash)\n");
		flsprintf(stdout,"  -v             Verify when programming \n");
		flsprintf(stdout,"  -x cpuspeed    Set CPU speed, typically set for Fbus (in MHz) x 4 \n");
		flsprintf(stdout,"  -z             Do not program, do no upload, just read in the S-rec file \n");
		flsprintf(stdout," addresses and sizes in decimal, for hex prefix with '0x'\n");
		// Example
	exit(0);
	}


void termEmu()
	{
	int STDIN=0;
	int STDOUT=1;
	// get rid of stuff that has been echoed to us
	tcflush(com,TCIFLUSH ); // .. then get rid of the echoes and what ever...


	struct termios newtio,oldtio;
	tcgetattr(STDIN,&oldtio);  // we do not want to change the console setting forever, so keep the old
	tcgetattr(STDIN,&newtio);  // configure the console to return as soon as it a key is pressed
    newtio.c_lflag = 0;    
    newtio.c_cc[VTIME]    = 0;   
    newtio.c_cc[VMIN]     = 1;   
    tcsetattr(STDIN,TCSANOW,&newtio);

	flsprintf(stdout,"\nTerminal mode, press CTRL-C to exit.\n");

    fd_set readfs;    
    int    maxfd;     
    int res;
	char buf[2];
    maxfd = com+1;  
    
    while (1) {
		// wait for something from console or serial port
		FD_SET(STDIN, &readfs);  
		FD_SET(com, &readfs);  
		struct timeval tout;
		tout.tv_usec = 10; 
		tout.tv_sec  = 0; 
		select(maxfd, &readfs, NULL, NULL, &tout);

		// copy console stuff to the serial port
		if (FD_ISSET(STDIN,&readfs))   {    
			res=read(STDIN,buf,1);
			if (buf[0]==3) break; // CTRL-C terminates terminal mode
			write(com,buf,res);
		}
		// copy serial port stuff to console				
		if (FD_ISSET(com,&readfs)){    
			res=read(com,buf,1);
			write(STDOUT,buf,res);
			fflush(stdout);
			}
	}

	tcsetattr(0,TCSANOW,&oldtio); // restore console settings

///	close(con);
	}

void setCPUtype(char* cpu) {
	if (strcmp("gz16",cpu)==0) {
		// These settings depend on the CPU version
		FLASH=0xC000;
		PUTBYTE=0xFEAF; 
		GETBYTE=0x1C00;
		RDVRRNG=0x1C03;
		ERARRNG=0x1C06;
		PRGRNGE=0x1C09;
		MONRTN=0xFE20;
		FLBPR=0xFF7E;
		EADDR=FLBPR;
		MONDATA = 0x48;
		PAGESIZE = 64;
		}
	else if (strcmp("jb8",cpu)==0) {
		// These settings depend on the CPU version
		FLASH=0xDC00;
		PUTBYTE=0xFED6; 
		GETBYTE=0xFC00;
		RDVRRNG=0xFC03;
		ERARRNG=0xFC06;
		PRGRNGE=0xFC09;
		MONRTN=0xFE55;
		FLBPR=0xFE09;
		EADDR=FLASH;
		MONDATA = 0x48;
		PAGESIZE = 64;
		}
	else {
		flsprintf(stderr,"Unsupported CPU type '%s'\n",cpu);
		abort();
		}
	// these are independent of CPU type	
	DATABUF  =  MONDATA+4;
	WORKRAM = DATABUF + PAGESIZE;
	WORKTOP = 0xF0; // this leaves 16 bytes for stack, the deepest ROM routines use 11 so there some for myself too
	CTRLBYT  =  MONDATA+0; 
	CPUSPD  =  MONDATA+1;
	LADDR  =  MONDATA+2;
	if (DATABUF+PAGESIZE>0xFF) {
		flsprintf(stderr,"bl08 limitation, DATABUF+PAGESIZE>0xFF, DATABUF=%04X, PAGESIZE=%04X\n",DATABUF,PAGESIZE);
		abort();
		}
	}
	
	
		
	
int getIntArg(char* arg) {
	if (strlen(arg)>=2 && memcmp(arg,"0x",2)==0) {
		unsigned int u;
		sscanf(arg+2,"%X",&u);
		return u;
		}
	else {
		int d;
		sscanf(arg,"%d",&d);
		return d;
		}
	}
	
	
void parseOverride(char* str) {	
	char* vp=strstr(str,"=");
	if (!vp) {
		flsprintf(stderr,"Bad override syntax, no '=' found\n");
		abort();
		}
	*vp=0;
	vp++;
	int val=getIntArg(vp);
	if (strcmp("ROMBASE",str)==0) {
		GETBYTE=val+0;
		RDVRRNG=val+3;
		ERARRNG=val+6;
		PRGRNGE=val+9;
		}
	else if (strcmp("FLASH",str)==0) FLASH=val;
	else if (strcmp("PUTBYTE",str)==0) PUTBYTE=val;
	else if (strcmp("GETBYTE",str)==0) GETBYTE=val;
	else if (strcmp("RDVRRNG",str)==0) RDVRRNG=val;
	else if (strcmp("ERARRNG",str)==0) ERARRNG=val;
	else if (strcmp("PRGRNGE",str)==0) PRGRNGE=val;
	else if (strcmp("FLBPR",str)==0) FLBPR=val;
	else if (strcmp("MONDATA",str)==0) MONDATA=val;
	else if (strcmp("PAGESIZE",str)==0) PAGESIZE=val;
	else if (strcmp("MONRTN",str)==0) MONRTN=val;
	else if (strcmp("EADDR",str)==0) EADDR=val;
	else {
		flsprintf(stderr,"Attempt to override unrecognized variable %s\n",str);
		abort();
		}
	}
	
void parseArgs(int argc, char *argv[]) {	
	int c;
	while ((c = getopt (argc, argv, "a:b:c:d:efg:hikl:mno:pqr:s:t:uvx:z")) != -1) {
		switch (c) {
			case 'a' :
				dumpStart=getIntArg(optarg);
				break;
			case 'b' : 
				sscanf(optarg,"%d",&baudRate); 
				break;
			case 'c' : 
				COM=optarg;
				break;
			case 'd' :
				dumpFormat=optarg;
				break;
			case 'e' :
				eraseFlash = 1;
				break;
			case 'f' :
				useFastProg = 1;
				break;
			case 'g' :
				executeCode=optarg;
				break;
			case 'h' :
				printHelp();
				break;
			case 'i' :
				useStdin = 1;
				break;
			case 'k' :
				killPrevious = 1;
				break;
			case 'l' :
				sscanf(optarg,"%d",&verbose); 
				break;
			case 'm' :
				terminalMode=1;
				break;
			case 'n' :
				flsprintf(stdout,"%s\n",version);
				break;
			case 'p' :
				pageErase=1;
				break;
			case 'o' :
				parseOverride(optarg);
				break;
			case 'q' :
				verbose=0;
				break;
			case 'r' :
				resetPulse=getIntArg(optarg);
				break;
			case 's' :
				dumpSize=getIntArg(optarg);
				break;
			case 't' :
				setCPUtype(optarg);
				break;
			case 'u' :
				uploadOnly=1;
				break;
			case 'v' :
				verify=1;
				break;
			case 'x' :
				sscanf(optarg,"%d",&CPUSPEED); 
				break;
			case 'z' :
				loadOnly=1; 
				break;
			case '?' :
				if (isprint (optopt))
					fprintf (stderr, "Unknown option `-%c'.\n", optopt);
				else
					fprintf (stderr,"Unknown option character `\\x%x'.\n",optopt);
			  default:
				fprintf (stderr,"Bug, unhandled option '%c'\n",c);
				abort ();
			}
		}
	if (argc<=1) 
		printHelp();
	}

void ioctlErrCheck(int e) {
	if (e) {
		flsprintf(stdout,"ioctl returned %d\n",e);
		abort();
		}
	}

void generateReset() {
	int s;
	ioctlErrCheck(ioctl(com, TIOCMGET, &s)); 
	
	s |= TIOCM_DTR;
	ioctlErrCheck(ioctl(com, TIOCMSET, &s)); 
	
		
	struct timespec tspec;
	tspec.tv_sec=resetPulse/1000;
	tspec.tv_nsec=(resetPulse%1000)*1000000; 
	nanosleep(&tspec,0);

	
	s &= ~TIOCM_DTR;
	ioctlErrCheck(ioctl(com, TIOCMSET, &s)); 
	}


void deletePidFile() {
	int stat=remove(PIDFILENAME);
	if (stat)
		printf("remove returned %d\n",stat);
	}
	
void killPreviousInstance() {
	atexit(deletePidFile);
	int pid;
	FILE* pidf=fopen(PIDFILENAME,"r");
	if (pidf) {
		fscanf(pidf,"%d",&pid);
		int stat=kill(pid,SIGKILL);
		if (stat!=0)
			printf("kill returned %d\n",stat);
			
		fclose(pidf);
		waitpid(pid,&stat,0);
		if (WIFEXITED(stat)==0)
			printf("waitpid returned %d\n",WIFEXITED(stat));
		}
	pidf=fopen(PIDFILENAME,"w");
	fprintf(pidf,"%d\n",getpid());
	fclose(pidf);
	}

int main(int argc, char *argv[]) {	
	
	// default values
	setCPUtype("gz16");
	CPUSPEED=8; 
	baudRate=9600;
	
	parseArgs(argc,argv);

	if (killPrevious)
		killPreviousInstance();
	

	if (verbose)
		flsprintf(stdout,"bl08 - MC68HC908 Bootloader - version %s\n",version);
	
	memset(image,0xFF,sizeof(image));
	
	int maxrc=256;
	int ranges[maxrc];
	int rc=0;
	
	if (useStdin) 
		rc += readSrec(verbose , stdin , image , sizeof(image) , 0x0000 , ranges+rc , maxrc - rc);
	
	int i;
	for (i=optind; i<argc; i++) {
		char* filename=argv[i];
		FILE* sf = fopen(filename, "r");
		if (sf == NULL) { 
			flsprintf(stderr,"Failed to open '%s'\n", filename);
			abort();
			}
		
		int rn=readSrec(verbose , sf , image , sizeof(image) , 0x0000 , ranges+rc , maxrc - rc);
		if (rn<0) {
			flsprintf(stderr,"Too many discontinuous data ranges in S-record file '%s'\n",filename);
			abort();
			}
		rc += rn;		
		fclose(sf);
		}
		
	if (!loadOnly) {
		initSerialPort();
			
		if (resetPulse)
			generateReset();
		
		if (eraseFlash) {
			connectTarget();
			massErase();
			}
		
		if (rc>0) {
			connectTarget();
			if (pageErase) {
				for (i=0; i<rc; ) {
					int addr=ranges[ i++ ];
					int size=ranges[ i++ ];
					int a = addr / PAGESIZE * PAGESIZE;
					while (a<addr+size) {
						if (verbose)
							flsprintf(stdout,"Erase %04X - %04X\n",a,PAGESIZE);
					
						callMonitor(ERARRNG , 0, 0, a, 0);
						a += PAGESIZE;
						}
					}
				}
			else
				massErase();	
		
			int i;
			for (i=0; i<rc;) {
				int addr=ranges[ i++ ];
				int size=ranges[ i++ ];
				if (uploadOnly) {
					if (verbose)
						flsprintf(stdout,"Uploading memory contents\n");
					writeMemory(addr,size,verbose);
					if (verbose)
						flsprintf(stdout,"\n");
					}
				else 
					flashProgram(addr , size , verify);
				if (verbose>1) {
					flsprintf(stdout,"Reading back memory/flash content");
					readMemory(addr , size, verbose);
					flsprintf(stdout,"\n");
					dumpMemory(addr , size);
					}
				}
			}


		if (executeCode) {
			connectTarget();
			int addr;
			if (strcmp("reset",executeCode)==0) {
				readMemory(0xFFFE,2,0);
				addr=((image[0xFFFE]&0xFF)<<8) | (image[0xFFFF]&0xFF);
				}
			else  
				addr=getIntArg(executeCode);

			if (verbose)
				flsprintf(stdout,"Execute code from %04X\n",addr);
			runFrom(addr,0,0,0);
			}
		
		if (terminalMode) 
			termEmu();
		}

	if (dumpSize>0) {
		if (!loadOnly) {
			connectTarget();
			if (verbose) 
				flsprintf(stdout,"Reading memory\n");
			readMemory(dumpStart,dumpSize, verbose);
			}
		if (verbose) 
			flsprintf(stdout,"\n");
		
		if (dumpFormat) {
			if (strcmp("srec",dumpFormat)==0)
				dumpMemorySrec(dumpStart,dumpSize);
			else if (strcmp("hex",dumpFormat)==0)
				dumpMemory(dumpStart,dumpSize);
			else
				flsprintf(stderr,"Unknown dump format '%s'\n",dumpFormat);
			}
		}
			
	return 0;
	}






