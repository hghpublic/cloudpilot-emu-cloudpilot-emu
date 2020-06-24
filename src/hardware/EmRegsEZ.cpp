/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
        Copyright (c) 2000-2001 Palm, Inc. or its subsidiaries.
        All rights reserved.

        This file is part of the Palm OS Emulator.

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.
\* ===================================================================== */

#include "EmRegsEZ.h"

#include "Byteswapping.h"  // Canonical
#include "EmCommon.h"
#include "EmHAL.h"     // EmHAL
#include "EmMemory.h"  // gMemAccessFlags, EmMem_memcpy
#include "EmRegsEZPrv.h"
#include "EmSPISlave.h"  // DoExchange
#include "EmSession.h"   // GetDevice
#include "Frame.h"
#include "Logging.h"  // LogAppendMsg
#include "Platform.h"
#include "UAE.h"  // regs, SPCFLAG_INT

#if 0                         // CSTODO
    #include "SessionFile.h"  // WriteHwrDBallEZType, etc.
#endif

// clang-format off
#include "PalmPack.h"
// clang-format on

#define NON_PORTABLE
#include "HwrMiscFlags.h"  // hwrMiscFlagID1

#define hwrEZ328maskID1J83G 0x05

// Some platform-specific -- yet fairly portable -- defines.
#define hwrEZPortGIDDetect 0x04  // (L) ID select (drives kbd)
#define hwrEZPortDKbdCol0 0x01   // (H) Keyboard Column 0	(aka INT0)
#define hwrEZPortDKbdCol1 0x02   // (H) Keyboard Column 1	(aka INT1)
#define hwrEZPortDKbdCol2 0x04   // (H) Keyboard Column 2	(aka INT2)
#define hwrEZPortDKbdCol3 0x08   // (H) Keyboard Column 3	(aka INT3)
#define hwrEZPortDKeyBits 0x0F   // (H) All Keyboard Columns

#undef NON_PORTABLE
#include "PalmPackPop.h"

static const uint16 UPSIZ = 0x1800;  // Mask to get the unprotected memory size from csDSelect.
static const uint16 SIZ = 0x000E;    // Mask to get the memory size from csASelect.
static const uint16 EN = 0x0001;     // Mask to get the enable bit from csASelect.

static const uint16 gBaseAddressShift = 13;  // Shift to get base address from CSGBx register value

// #define LOGGING 0
#ifdef LOGGING
    #define PRINTF log::printf
#else
    #define PRINTF(...) ;
#endif

// Values used to initialize the DragonBallEZ registers.

static const HwrM68EZ328Type kInitial68EZ328RegisterValues = {
    0x1C,                 // Byte		scr;							// $000:
                          // System Control Register
    {},                   // Byte
                          // ___filler0[0x004-0x001];
    hwrEZ328chipIDEZ,     // Byte		chipID;							//
                          // $004: Chip ID Register
    hwrEZ328maskID1J83G,  // Byte		maskID; // $005: Mask ID Register
    0x00,                 // Word		swID;							// $006:
                          // Software ID Register
    {},                   // Byte
                          // ___filler1[0x100-0x008];

    0x0000,  // Word		csAGroupBase;					// $100: Chip Select
             // Group A Base Register
    0x0000,  // Word		csBGroupBase;					// $102: Chip Select
             // Group B Base Register
    0x0000,  // Word		csCGroupBase;					// $104: Chip Select
             // Group C Base Register
    0x0000,  // Word		csDGroupBase;					// $106: Chip Select
             // Group D Base Register

    {},  // Byte
         // ___filler6[0x110-0x108];

    0x00E0,  // Word		csASelect;						// $110:
             // Group A Chip Select Register
    0x0000,  // Word		csBSelect;						// $112:
             // Group B Chip Select Register
    0x0000,  // Word		csCSelect;						// $114:
             // Group C Chip Select Register
    0x0000,  // Word		csDSelect;						// $116:
             // Group D Chip Select Register

    0x0060,  // Word		emuCS;							// $118: EMU
             // Chip Select Register

    {},  // Byte
         // ___filler2[0x200-0x11A];

    0x2430,  // Word		pllControl;						// $200: PLL
             // Control Register
    0x0123,  // Word		pllFreqSel;						// $202: PLL
             // Frequency Select Register
    0,       // !!! ---> Marked as reserved in 1.4 Word		pllTest;
        // // $204: PLL Test Register (do not access)
    0,     // Byte
           // ___filler44;
    0x1F,  // Byte		pwrControl;						// $207:
           // Power Control Register

    {},  // Byte
         // ___filler3[0x300-0x208];

    0x00,    // Byte		intVector;						// $300:
             // Interrupt Vector Register
    0,       // Byte
             // ___filler4;
    0x0000,  // Word		intControl;						// $302:
             // Interrupt Control Register
    0x00FF,  // Word		intMaskHi;						// $304:
             // Interrupt Mask Register/HIGH word
    0xFFFF,  // Word		intMaskLo;						// $306:
             // Interrupt Mask Register/LOW word
    {},      // Byte
             // ___filler7[0x30c-0x308];
    0x0000,  // Word		intStatusHi;					// $30C: Interrupt
             // Status Register/HIGH word
    0x0000,  // Word		intStatusLo;					// $30E: Interrupt
             // Status Register/LOW word
    0x0000,  // Word		intPendingHi;					// $310: Interrupt
             // Pending Register
    0x0000,  // Word		intPendingLo;					// $312: Interrupt
             // Pending Register

    {},  // Byte
         // ___filler4a[0x400-0x314];

    0x00,  // Byte		portADir;						// $400:
           // Port A Direction Register
    0x00,  // Byte		portAData;						// $401:
           // Port A Data Register
    0xFF,  // Byte		portAPullupEn;					// $402: Port A
           // Pullup Enable (similar to Select on DB)
    {},    // Byte
           // ___filler8[5];

    0x00,  // Byte		portBDir;						// $408:
           // Port B Direction Register
    0x00,  // Byte		portBData;						// $409:
           // Port B Data Register
    0xFF,  // Byte		portBPullupEn;					// $40A: Port B
           // Pullup Enable
    0xFF,  // Byte		portBSelect;					// $40B: Port B
           // Select Register

    {},  // Byte
         // ___filler9[4];

    0x00,  // Byte		portCDir;						// $410:
           // Port C Direction Register
    0x00,  // Byte		portCData;						// $411:
           // Port C Data Register
    0xFF,  // Byte		portCPulldnEn;					// $412: Port C
           // Pulldown Enable
    0xFF,  // Byte		portCSelect;					// $413: Port C
           // Select Register

    {},  // Byte
         // ___filler10[4];

    0x00,  // Byte		portDDir;						// $418:
           // Port D Direction Register
    0x00,  // Byte		portDData;						// $419:
           // Port D Data Register
    0xFF,  // Byte		portDPullupEn;					// $41A: Port D
           // Pull-up Enable
    0xF0,  // Byte		portDSelect;					// $41B: Port D
           // Select Register
    0x00,  // Byte		portDPolarity;					// $41C: Port D
           // Polarity Register
    0x00,  // Byte		portDIntReqEn;					// $41D: Port D
           // Interrupt Request Enable
    0x00,  // Byte		portDKbdIntEn;					// $41E: Port D
           // Keyboard Interrupt Enable
    0x00,  // Byte		portDIntEdge;					// $41F: Port D IRQ
           // Edge Register

    0x00,  // Byte		portEDir;						// $420:
           // Port E Direction Register
    0x00,  // Byte		portEData;						// $421:
           // Port E Data Register
    0xFF,  // Byte		portEPullupEn;					// $422: Port E
           // Pull-up Enable
    0xFF,  // Byte		portESelect;					// $423: Port E
           // Select Register

    {},  // Byte
         // ___filler14[4];

    0x00,  // Byte		portFDir;						// $428:
           // Port F Direction Register
    0x00,  // Byte		portFData;						// $429:
           // Port F Data Register
    0xFF,  // Byte		portFPullupdnEn;				// $42A: Port F
           // Pull-up/down Enable
    0x00,  // Byte		portFSelect;					// $42B: Port F
           // Select Register

    {},  // Byte
         // ___filler16[4];

    0x00,  // Byte		portGDir;						// $430:
           // Port G Direction Register
    0x00,  // Byte		portGData;						// $431:
           // Port G Data Register
    0x3D,  // Byte		portGPullupEn;					// $432: Port G
           // Pull-up Enable
    0x08,  // Byte		portGSelect;					// $433: Port G
           // Select Register

    {},  // Byte
         // ___filler2000[0x500-0x434];

    0x0020,  // Word		pwmControl;						// $500: PWM
             // Control Register
    0x00,    // Byte		pwmSampleHi;					// $502: PWM Sample
             // - high byte
    0x00,    // Byte		pwmSampleLo;					// $503: PWM Sample
             // - low byte
    0xFE,    // Byte		pwmPeriod;						// $504: PWM
             // Period
    0x00,    // Byte		pwmCounter;						// $505: PWM
             // Counter

    {},  // Byte
         // ___filler24[0x600-0x506];

    0x0000,  // Word		tmr1Control;					// $600: Timer 1
             // Control Register
    0x0000,  // Word		tmr1Prescaler;					// $602: Timer 1
             // Prescaler Register
    0xFFFF,  // Word		tmr1Compare;					// $604: Timer 1
             // Compare Register
    0x0000,  // Word		tmr1Capture;					// $606: Timer 1
             // Capture Register
    0x0000,  // Word		tmr1Counter;					// $608: Timer 1
             // Counter Register
    0x0000,  // Word		tmr1Status;						// $60A:
             // Timer 1 Status Register

    {},  // Byte
         // ___filler25[0x800-0x61E];

    0x0000,  // Word		spiMasterData;					// $800: SPI Master
             // Data Register
    0x0000,  // Word		spiMasterControl;				// $802: SPI Master
             // Control Register

    {},  // Byte
         // ___filler27[0x900-0x804];

    0x0000,  // Word		uControl;						// $900:
             // Uart Control Register
    0x003F,  // Word		uBaud;							// $902:
             // Uart Baud Control Register
    0x0000,  // Word		uReceive;						// $904:
             // Uart Receive Register
    0x0000,  // Word		uTransmit;						// $906:
             // Uart Transmit Register
    0x0000,  // Word		uMisc;							// $908:
             // Uart Miscellaneous Register
    0x0000,  // Word		uNonIntPresc;					// $90A: Uart IRDA
             // Non-Integer Prescaler

    {},  // Byte
         // ___filler28[0xA00-0x90C];

    0x00000000,  // DWord	lcdStartAddr;					// $A00: Screen
                 // Starting Address Register
    0,           // Byte
                 // ___filler29;
    0xFF,        // Byte		lcdPageWidth;					// $A05: Virtual
                 // Page Width Register
    {},          // Byte
                 // ___filler30[2];
    0x03FF,      // Word		lcdScreenWidth;					// $A08: Screen
                 // Width Register
    0x01FF,      // Word		lcdScreenHeight;				// $A0A: Screen
                 // Height Register
    {},          // Byte
                 // ___filler31[0xA18-0xA0C];
    0x0000,      // Word		lcdCursorXPos;					// $A18: Cursor X
                 // Position
    0x0000,      // Word		lcdCursorYPos;					// $A1A:
                 // Cursor Y Position
    0x0101,      // Word		lcdCursorWidthHeight;			// $A1C: Cursor Width and
                 // Height
    0,           // Byte
                 // ___filler32;
    0x7F,        // Byte		lcdBlinkControl;				// $A1F: Blink
                 // Control Register
    0x00,        // Byte		lcdPanelControl;				// $A20: Panel
                 // Interface Control Register
    0x00,        // Byte		lcdPolarity;					// $A21: Polarity
                 // Config Register
    0,           // Byte
                 // ___filler33;
    0x00,        // Byte		lcdACDRate;						// $A23: ACD
                 // (M) Rate Control Register
    0,           // Byte
                 // ___filler34;
    0x00,        // Byte		lcdPixelClock;					// $A25: Pixel Clock
                 // Divider Register
    0,           // Byte
                 // ___filler35;
    0x40,        // Byte		lcdClockControl;				// $A27: Clocking
                 // Control Register
    0,           // Byte
                 // ___filler36;
    0xFF,        // Byte		lcdRefreshRateAdj;				// $A29: Refresh
                 // Rate Adjustment Register
    {},          // Byte
                 // ___filler2003[0xA2D-0xA2A];
    0x00,        // Byte		lcdPanningOffset;				// $A2D: Panning
                 // Offset Register

    {},  // Byte
         // ___filler37[0xA31-0xA2E];

    0xB9,    // Byte		lcdFrameRate;					// $A31: Frame Rate
             // Control Modulation Register
    0,       // Byte
             // ___filler2004;
    0x84,    // Byte		lcdGrayPalette;					// $A33: Gray
             // Palette Mapping Register
    0x00,    // Byte		lcdReserved;					// $A34: Reserved
    0,       // Byte
             // ___filler2005;
    0x0000,  // Word		lcdContrastControlPWM;			// $A36: Contrast Control

    {},  // Byte
         // ___filler40[0xB00-0xA38];

    0x00000000,  // DWord	rtcHourMinSec;					// $B00: RTC Hours,
                 // Minutes, Seconds Register
    0x00000000,  // DWord	rtcAlarm;						// $B04: RTC
                 // Alarm Register
    {},          // Byte
                 // ___filler2001[0xB0A-0xB08];
    0x0001,      // Word		rtcWatchDog;					// $B0A: RTC
                 // Watchdog Timer
    0x00,        // Word		rtcControl;						// $B0C: RTC
                 // Control Register
    0x00,        // Word		rtcIntStatus;					// $B0E: RTC
                 // Interrupt Status Register
    0x00,        // Word		rtcIntEnable;					// $B10: RTC
                 // Interrupt Enable Register
    0x00,        // Word		stopWatch;						// $B12:
                 // Stopwatch Minutes
    {},          // Byte
                 // ___filler2002[0xB1A-0xB14];
    0x0000,      // Word		rtcDay; // $B1A: RTC Day
    0x0000,      // Word		rtcDayAlarm;					// $B1C: RTC Day
                 // Alarm

    {},  // Byte
         // ___filler41[0xC00-0xB1E];

    0x0000,  // Word		dramConfig;						// $C00:
             // DRAM Memory Config Register
    0x0000,  // Word		dramControl;					// $C02: DRAM
             // Control Register

    {},  // Byte
         // ___filler42[0xD00-0xC04];

    0x00000000,  // DWord	emuAddrCompare;					// $D00: Emulation
                 // Address Compare Register
    0x00000000,  // DWord	emuAddrMask;					// $D04: Emulation
                 // Address Mask Register
    0x0000,      // Word		emuControlCompare;				// $D08: Emulation
                 // Control Compare Register
    0x0000,      // Word		emuControlMask;					// $D0A: Emulation
                 // Control Mask Register
    0x0000,      // Word		emuControl;						// $DOC:
                 // Emulation Control Register
    0x0000       // Word		emuStatus;						// $D0E:
                 // Emulation Status Register
};

using ButtonEventT = ButtonEvent;

// ---------------------------------------------------------------------------
//		� EmRegsEZ::EmRegsEZ
// ---------------------------------------------------------------------------

EmRegsEZ::EmRegsEZ(void)
    : EmRegs(),
      f68EZ328Regs(),
      fHotSyncButtonDown(0),
      fKeyBits(0),
      fLastTmr1Status(0),
      fPortDEdge(0),
      fPortDDataCount(0),
      fHour(0),
      fMin(0),
      fSec(0),
      fTick(0),
      fCycle(0),
      fUART(NULL) {}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::~EmRegsEZ
// ---------------------------------------------------------------------------

EmRegsEZ::~EmRegsEZ(void) {}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::Initialize
// ---------------------------------------------------------------------------

void EmRegsEZ::Initialize(void) {
    EmRegs::Initialize();

    fUART = new EmUARTDragonball(EmUARTDragonball::kUART_DragonballEZ, 0);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::Reset
// ---------------------------------------------------------------------------

void EmRegsEZ::Reset(Bool hardwareReset) {
    EmRegs::Reset(hardwareReset);

    if (hardwareReset) {
        f68EZ328Regs = kInitial68EZ328RegisterValues;

        // Byteswap all the words in the DragonballEZ registers (if necessary).

        Canonical(f68EZ328Regs);
        ByteswapWords(&f68EZ328Regs, sizeof(f68EZ328Regs));

        fKeyBits = 0;
        fLastTmr1Status = 0;
        fPortDEdge = 0;
        fPortDDataCount = 0;

        // React to the new data in the UART registers.

        Bool sendTxData = false;
        EmRegsEZ::UARTStateChanged(sendTxData);
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::Save
// ---------------------------------------------------------------------------

void EmRegsEZ::Save(SessionFile& f) {
    EmRegs::Save(f);

#if 0  // CSTODO

    StWordSwapper swapper1(&f68EZ328Regs, sizeof(f68EZ328Regs));
    //	StCanonical<HwrM68EZ328Type>	swapper2 (f68EZ328Regs);
    f.WriteHwrDBallEZType(f68EZ328Regs);
    f.FixBug(SessionFile::kBugByteswappedStructs);

    const long kCurrentVersion = 3;

    Chunk chunk;
    EmStreamChunk s(chunk);

    s << kCurrentVersion;

    s << fHotSyncButtonDown;
    s << fKeyBits;
    s << fLastTmr1Status;
    s << fPortDEdge;

    // Added in version 2.

    s << fHour;
    s << fMin;
    s << fSec;
    s << fTick;
    s << fCycle;

    // Added in version 3.

    s << fPortDDataCount;

    f.WriteDBallEZState(chunk);
#endif
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::Load
// ---------------------------------------------------------------------------

void EmRegsEZ::Load(SessionFile& f) {
#if 0  // CSTODO
    EmRegs::Load(f);

    if (f.ReadHwrDBallEZType(f68EZ328Regs)) {
        // The Windows version of Poser 2.1d29 and earlier did not write
        // out structs in the correct format.  The fields of the struct
        // were written out in Little-Endian format, not Big-Endian.  To
        // address this problem, the bug has been fixed, and a new field
        // is added to the file format indicating that the bug has been
        // fixed.  With the new field (the "bug bit"), Poser can identify
        // old files from new files and read them in accordingly.
        //
        // With the bug fixed, the .psf files should now be interchangeable
        // across platforms (modulo other bugs...).

        if (!f.IncludesBugFix(SessionFile::kBugByteswappedStructs)) {
            Canonical(f68EZ328Regs);
        }
        ByteswapWords(&f68EZ328Regs, sizeof(f68EZ328Regs));

        // React to the new data in the UART registers.

        Bool sendTxData = false;
        EmRegsEZ::UARTStateChanged(sendTxData);

        // Reset gMemAccessFlags.fProtect_SRAMSet

        gMemAccessFlags.fProtect_SRAMSet = (READ_REGISTER(csDSelect) & 0x2000) != 0;
    } else {
        f.SetCanReload(false);
    }

    Chunk chunk;
    if (f.ReadDBallEZState(chunk)) {
        long version;
        EmStreamChunk s(chunk);

        s >> version;

        if (version >= 1) {
            s >> fHotSyncButtonDown;
            s >> fKeyBits;
            s >> fLastTmr1Status;
            s >> fPortDEdge;
        }

        if (version >= 2) {
            s >> fHour;
            s >> fMin;
            s >> fSec;
            s >> fTick;
            s >> fCycle;
        }

        if (version >= 3) {
            s >> fPortDDataCount;
        }
    } else {
        f.SetCanReload(false);
    }
#endif
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::Dispose
// ---------------------------------------------------------------------------

void EmRegsEZ::Dispose(void) {
    delete fUART;
    fUART = NULL;

    EmRegs::Dispose();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::SetSubBankHandlers
// ---------------------------------------------------------------------------

void EmRegsEZ::SetSubBankHandlers(void) {
    // Install base handlers.

    EmRegs::SetSubBankHandlers();

    // Now add standard/specialized handers for the defined registers.

    INSTALL_HANDLER(StdRead, StdWrite, scr);

    INSTALL_HANDLER(StdRead, NullWrite, chipID);
    INSTALL_HANDLER(StdRead, NullWrite, maskID);
    INSTALL_HANDLER(StdRead, NullWrite, swID);

    INSTALL_HANDLER(StdRead, StdWrite, csAGroupBase);
    INSTALL_HANDLER(StdRead, StdWrite, csBGroupBase);
    INSTALL_HANDLER(StdRead, StdWrite, csCGroupBase);
    INSTALL_HANDLER(StdRead, StdWrite, csDGroupBase);

    INSTALL_HANDLER(StdRead, csASelectWrite, csASelect);
    INSTALL_HANDLER(StdRead, StdWrite, csBSelect);
    INSTALL_HANDLER(StdRead, StdWrite, csCSelect);
    INSTALL_HANDLER(StdRead, csDSelectWrite, csDSelect);

    INSTALL_HANDLER(StdRead, StdWrite, emuCS);

    INSTALL_HANDLER(StdRead, StdWrite, pllControl);
    INSTALL_HANDLER(pllFreqSelRead, StdWrite, pllFreqSel);
    INSTALL_HANDLER(StdRead, StdWrite, pllTest);
    INSTALL_HANDLER(StdRead, StdWrite, pwrControl);

    INSTALL_HANDLER(StdRead, StdWrite, intVector);
    INSTALL_HANDLER(StdRead, StdWrite, intControl);
    INSTALL_HANDLER(StdRead, intMaskHiWrite, intMaskHi);
    INSTALL_HANDLER(StdRead, intMaskLoWrite, intMaskLo);
    INSTALL_HANDLER(StdRead, intStatusHiWrite, intStatusHi);
    INSTALL_HANDLER(StdRead, NullWrite, intStatusLo);
    INSTALL_HANDLER(StdRead, NullWrite, intPendingHi);
    INSTALL_HANDLER(StdRead, NullWrite, intPendingLo);

    INSTALL_HANDLER(StdRead, StdWrite, portADir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portAData);
    INSTALL_HANDLER(StdRead, StdWrite, portAPullupEn);

    INSTALL_HANDLER(StdRead, StdWrite, portBDir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portBData);
    INSTALL_HANDLER(StdRead, StdWrite, portBPullupEn);
    INSTALL_HANDLER(StdRead, StdWrite, portBSelect);

    INSTALL_HANDLER(StdRead, StdWrite, portCDir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portCData);
    INSTALL_HANDLER(StdRead, StdWrite, portCPulldnEn);
    INSTALL_HANDLER(StdRead, StdWrite, portCSelect);

    INSTALL_HANDLER(StdRead, StdWrite, portDDir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portDData);
    INSTALL_HANDLER(StdRead, StdWrite, portDPullupEn);
    INSTALL_HANDLER(StdRead, StdWrite, portDSelect);
    INSTALL_HANDLER(StdRead, StdWrite, portDPolarity);
    INSTALL_HANDLER(StdRead, portDIntReqEnWrite, portDIntReqEn);
    INSTALL_HANDLER(StdRead, StdWrite, portDKbdIntEn);
    INSTALL_HANDLER(StdRead, StdWrite, portDIntEdge);

    INSTALL_HANDLER(StdRead, StdWrite, portEDir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portEData);
    INSTALL_HANDLER(StdRead, StdWrite, portEPullupEn);
    INSTALL_HANDLER(StdRead, StdWrite, portESelect);

    INSTALL_HANDLER(StdRead, StdWrite, portFDir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portFData);
    INSTALL_HANDLER(StdRead, StdWrite, portFPullupdnEn);
    INSTALL_HANDLER(StdRead, StdWrite, portFSelect);

    INSTALL_HANDLER(StdRead, StdWrite, portGDir);
    INSTALL_HANDLER(portXDataRead, portXDataWrite, portGData);
    INSTALL_HANDLER(StdRead, StdWrite, portGPullupEn);
    INSTALL_HANDLER(StdRead, StdWrite, portGSelect);

    INSTALL_HANDLER(StdRead, StdWrite, pwmControl);
    INSTALL_HANDLER(StdRead, StdWrite, pwmSampleHi);
    INSTALL_HANDLER(StdRead, StdWrite, pwmSampleLo);
    INSTALL_HANDLER(StdRead, StdWrite, pwmPeriod);
    INSTALL_HANDLER(StdRead, NullWrite, pwmCounter);

    INSTALL_HANDLER(StdRead, StdWrite, tmr1Control);
    INSTALL_HANDLER(StdRead, StdWrite, tmr1Prescaler);
    INSTALL_HANDLER(StdRead, StdWrite, tmr1Compare);
    INSTALL_HANDLER(StdRead, StdWrite, tmr1Capture);
    INSTALL_HANDLER(StdRead, NullWrite, tmr1Counter);
    INSTALL_HANDLER(tmr1StatusRead, tmr1StatusWrite, tmr1Status);

    INSTALL_HANDLER(StdRead, StdWrite, spiMasterData);
    INSTALL_HANDLER(StdRead, spiMasterControlWrite, spiMasterControl);

    INSTALL_HANDLER(uartRead, uartWrite, uControl);
    INSTALL_HANDLER(uartRead, uartWrite, uBaud);
    INSTALL_HANDLER(uartRead, uartWrite, uReceive);
    INSTALL_HANDLER(uartRead, uartWrite, uTransmit);
    INSTALL_HANDLER(uartRead, uartWrite, uMisc);
    INSTALL_HANDLER(uartRead, uartWrite, uNonIntPresc);

    INSTALL_HANDLER(StdRead, lcdRegisterWrite, lcdStartAddr);
    INSTALL_HANDLER(StdRead, lcdRegisterWrite, lcdPageWidth);
    INSTALL_HANDLER(StdRead, lcdRegisterWrite, lcdScreenWidth);
    INSTALL_HANDLER(StdRead, lcdRegisterWrite, lcdScreenHeight);
    INSTALL_HANDLER(StdRead, StdWrite, lcdCursorXPos);
    INSTALL_HANDLER(StdRead, StdWrite, lcdCursorYPos);
    INSTALL_HANDLER(StdRead, StdWrite, lcdCursorWidthHeight);
    INSTALL_HANDLER(StdRead, StdWrite, lcdBlinkControl);
    INSTALL_HANDLER(StdRead, lcdRegisterWrite, lcdPanelControl);
    INSTALL_HANDLER(StdRead, StdWrite, lcdPolarity);
    INSTALL_HANDLER(StdRead, StdWrite, lcdACDRate);
    INSTALL_HANDLER(StdRead, StdWrite, lcdPixelClock);
    INSTALL_HANDLER(StdRead, StdWrite, lcdClockControl);
    INSTALL_HANDLER(StdRead, StdWrite, lcdRefreshRateAdj);
    INSTALL_HANDLER(StdRead, StdWrite, lcdPanningOffset);
    INSTALL_HANDLER(StdRead, StdWrite, lcdFrameRate);
    INSTALL_HANDLER(StdRead, StdWrite, lcdGrayPalette);
    INSTALL_HANDLER(StdRead, StdWrite, lcdContrastControlPWM);
    INSTALL_HANDLER(rtcHourMinSecRead, StdWrite, rtcHourMinSec);

    INSTALL_HANDLER(StdRead, StdWrite, rtcAlarm);
    INSTALL_HANDLER(StdRead, StdWrite, rtcWatchDog);
    INSTALL_HANDLER(StdRead, rtcControlWrite, rtcControl);
    INSTALL_HANDLER(StdRead, rtcIntStatusWrite, rtcIntStatus);
    INSTALL_HANDLER(StdRead, rtcIntEnableWrite, rtcIntEnable);
    INSTALL_HANDLER(StdRead, StdWrite, stopWatch);
    INSTALL_HANDLER(StdRead, StdWrite, rtcDay);
    INSTALL_HANDLER(StdRead, StdWrite, rtcDayAlarm);

    INSTALL_HANDLER(StdRead, StdWrite, dramConfig);
    INSTALL_HANDLER(StdRead, StdWrite, dramControl);

    INSTALL_HANDLER(StdRead, StdWrite, emuAddrCompare);
    INSTALL_HANDLER(StdRead, StdWrite, emuAddrMask);
    INSTALL_HANDLER(StdRead, StdWrite, emuControlCompare);
    INSTALL_HANDLER(StdRead, StdWrite, emuControlMask);
    INSTALL_HANDLER(StdRead, StdWrite, emuControl);
    INSTALL_HANDLER(StdRead, StdWrite, emuStatus);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetRealAddress
// ---------------------------------------------------------------------------

uint8* EmRegsEZ::GetRealAddress(emuptr address) {
    uint8* loc = ((uint8*)&f68EZ328Regs) + (address - kMemoryStart);

    return loc;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetAddressStart
// ---------------------------------------------------------------------------

emuptr EmRegsEZ::GetAddressStart(void) { return kMemoryStart; }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetAddressRange
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::GetAddressRange(void) { return kMemorySize; }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::Cycle
// ---------------------------------------------------------------------------
// Handles periodic events that need to occur when the processor cycles (like
// updating timer registers).  This function is called in two places from
// Emulator::Execute.  Interestingly, the loop runs 3% FASTER if this function
// is in its own separate function instead of being inline.

void EmRegsEZ::Cycle(Bool sleeping) {
#if _DEBUG
    #define increment 20
#else
    #define increment 4
#endif

    if (GetAsleep()) return;

    // Determine whether timer is enabled.

    if ((READ_REGISTER(tmr1Control) & hwrEZ328TmrControlEnable) != 0) {
        // If so, increment the timer.

        WRITE_REGISTER(tmr1Counter, READ_REGISTER(tmr1Counter) + (sleeping ? 1 : increment));

        // Determine whether the timer has reached the specified count.

        if (sleeping || READ_REGISTER(tmr1Counter) > READ_REGISTER(tmr1Compare)) {
            // Flag the occurrence of the successful comparison.

            WRITE_REGISTER(tmr1Status, READ_REGISTER(tmr1Status) | hwrEZ328TmrStatusCompare);

            // If the Free Run/Restart flag is not set, clear the counter.

            if ((READ_REGISTER(tmr1Control) & hwrEZ328TmrControlFreeRun) == 0) {
                WRITE_REGISTER(tmr1Counter, 0);
            }

            // If the timer interrupt is enabled, post an interrupt.

            if ((READ_REGISTER(tmr1Control) & hwrEZ328TmrControlEnInterrupt) != 0) {
                WRITE_REGISTER(intPendingLo, READ_REGISTER(intPendingLo) | hwrEZ328IntLoTimer);
                EmRegsEZ::UpdateInterrupts();
            }
        }
    }

    if ((fCycle += increment) > READ_REGISTER(tmr1Compare)) {
        fCycle = 0;

        if (++fTick >= 100) {
            fTick = 0;

            if (++fSec >= 60) {
                fSec = 0;

                if (++fMin >= 60) {
                    fMin = 0;

                    if (++fHour >= 24) {
                        fHour = 0;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::CycleSlowly
// ---------------------------------------------------------------------------
// Handles periodic events that need to occur when the processor cycles (like
// updating timer registers).  This function is called in two places from
// Emulator::Execute.  Interestingly, the loop runs 3% FASTER if this function
// is in its own separate function instead of being inline.

void EmRegsEZ::CycleSlowly(Bool sleeping) {
    UNUSED_PARAM(sleeping)

    // See if a hard button is pressed.

    EmAssert(gSession);

    if (gSession->HasButtonEvent()) {
        ButtonEventT event = gSession->NextButtonEvent();
        if (event.GetButton() == ButtonEventT::Button::cradle)
            EmRegsEZ::HotSyncEvent(event.GetType() == ButtonEventT::Type::press);
        else
            EmRegsEZ::ButtonEvent(event);
    }

    // See if there's anything new ("Put the data on the bus")

    EmRegsEZ::UpdateUARTState(false);

    // Check to see if the RTC alarm is ready to go off.  First see
    // if the RTC is enabled, and that the alarm event isn't already
    // registered (the latter check is just an optimization).

    if ((READ_REGISTER(rtcIntEnable) & hwrEZ328RTCIntEnableAlarm) != 0 &&
        (READ_REGISTER(rtcIntStatus) & hwrEZ328RTCIntStatusAlarm) == 0) {
        uint32 rtcAlarm = READ_REGISTER(rtcAlarm);

        long almHour = (rtcAlarm & hwrEZ328RTCAlarmHoursMask) >> hwrEZ328RTCAlarmHoursOffset;
        long almMin = (rtcAlarm & hwrEZ328RTCAlarmMinutesMask) >> hwrEZ328RTCAlarmMinutesOffset;
        long almSec = (rtcAlarm & hwrEZ328RTCAlarmSecondsMask) >> hwrEZ328RTCAlarmSecondsOffset;
        long almInSeconds = (almHour * 60 * 60) + (almMin * 60) + almSec;

        long nowInSeconds = Platform::getMilliseconds() / 1000;

        if (almInSeconds <= nowInSeconds) {
            WRITE_REGISTER(rtcIntStatus, READ_REGISTER(rtcIntStatus) | hwrEZ328RTCIntStatusAlarm);
            EmRegsEZ::UpdateRTCInterrupts();
        }
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::TurnSoundOff
// ---------------------------------------------------------------------------

void EmRegsEZ::TurnSoundOff(void) {
    uint16 pwmControl = READ_REGISTER(pwmControl);
    WRITE_REGISTER(pwmControl, pwmControl & ~hwrEZ328PWMControlEnable);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::ResetTimer
// ---------------------------------------------------------------------------

void EmRegsEZ::ResetTimer(void) { WRITE_REGISTER(tmr1Counter, 0); }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::ResetRTC
// ---------------------------------------------------------------------------

void EmRegsEZ::ResetRTC(void) {
    fHour = 15;
    fMin = 0;
    fSec = 0;
    fTick = 0;
    fCycle = 0;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetInterruptLevel
// ---------------------------------------------------------------------------

int32 EmRegsEZ::GetInterruptLevel(void) {
    uint16 intStatusHi = READ_REGISTER(intStatusHi);
    uint16 intStatusLo = READ_REGISTER(intStatusLo);

    // Level 7 = EMUIRQ.

    if ((intStatusHi & hwrEZ328IntHiEMU) != 0) return 7;

    // Level 6 = IRQ6, TMR, PWM.

    if ((intStatusHi & (hwrEZ328IntHiIRQ6)) != 0) return 6;

    if ((intStatusLo & (hwrEZ328IntLoTimer | hwrEZ328IntLoPWM)) != 0) return 6;

    // Level 5 = PEN.

    if ((intStatusHi & hwrEZ328IntHiPen) != 0) return 5;

    // Level 4 = SPIM, UART, WDT, RTC, RTC Sample, KB, INT0 - INT3.

    if ((intStatusLo & (hwrEZ328IntLoSPIM | hwrEZ328IntLoUART | hwrEZ328IntLoWDT |
                        hwrEZ328IntLoRTC | hwrEZ328IntLoKbd | hwrEZ328IntLoInt3 |
                        hwrEZ328IntLoInt2 | hwrEZ328IntLoInt1 | hwrEZ328IntLoInt0)) != 0)
        return 4;

    if ((intStatusHi & hwrEZ328IntHiSampleTimer) != 0) return 4;

    // Level 3 = IRQ3.

    if ((intStatusHi & hwrEZ328IntHiIRQ3) != 0) return 3;

    // Level 2 = IRQ2.

    if ((intStatusHi & hwrEZ328IntHiIRQ2) != 0) return 2;

    // Level 1 = IRQ1.

    if ((intStatusHi & hwrEZ328IntHiIRQ1) != 0) return 1;

    // Level 0.

    return -1;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetInterruptBase
// ---------------------------------------------------------------------------

int32 EmRegsEZ::GetInterruptBase(void) { return READ_REGISTER(intVector) & 0xF8; }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetLCDHasFrame
// ---------------------------------------------------------------------------

Bool EmRegsEZ::GetLCDHasFrame(void) { return false; }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetLCDBeginEnd
// ---------------------------------------------------------------------------

void EmRegsEZ::GetLCDBeginEnd(emuptr& begin, emuptr& end) {
    emuptr baseAddr = READ_REGISTER(lcdStartAddr);
    int rowBytes = READ_REGISTER(lcdPageWidth) * 2;
    int height = READ_REGISTER(lcdScreenHeight) + 1;

    begin = baseAddr;
    end = baseAddr + rowBytes * height;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetLCDScanlines
// ---------------------------------------------------------------------------

bool EmRegsEZ::CopyLCDFrame(Frame& frame) {
    // Get the screen metrics.

    frame.bpp = 1 << (READ_REGISTER(lcdPanelControl) & 0x03);
    frame.lineWidth = READ_REGISTER(lcdScreenWidth);
    frame.lines = READ_REGISTER(lcdScreenHeight) + 1;
    frame.bytesPerLine = READ_REGISTER(lcdPageWidth) * 2;
    emuptr baseAddr = READ_REGISTER(lcdStartAddr);

    // Determine first and last scanlines to fetch, and fetch them.

    emuptr firstLineAddr = baseAddr;
    emuptr lastLineAddr = baseAddr + frame.lines * frame.bytesPerLine;

    // TODO: probably move to <M68EZ328Hwr.h>
    const long hwrEZ328LcdPageSize = 0x00020000;  // 128K
    const long hwrEZ328LcdPageMask = 0xFFFE0000;

    if (frame.lines * frame.bytesPerLine > hwrEZ328LcdPageSize) return false;

    uint8* dst = frame.GetBuffer();
    emuptr boundaryAddr = ((baseAddr & hwrEZ328LcdPageMask) + hwrEZ328LcdPageSize);

    if (lastLineAddr <= boundaryAddr) {
        // Bits don't cross the 128K boundary
    } else if (firstLineAddr >= boundaryAddr) {
        // Bits are all beyond the 128K boundary

        firstLineAddr -= hwrEZ328LcdPageSize;  // wrap around
        lastLineAddr -= hwrEZ328LcdPageSize;
    } else {
        // Bits straddle the 128K boundary;
        // copy the first part here, the wrapped part below

        EmMem_memcpy((void*)dst, firstLineAddr, boundaryAddr - firstLineAddr);
        dst += (boundaryAddr - firstLineAddr);

        firstLineAddr = boundaryAddr - hwrEZ328LcdPageSize;
        lastLineAddr -= hwrEZ328LcdPageSize;  // wrap around
    }

    EmASSERT(frame.GetBufferSize() >= hwrEZ328LcdPageSize);
    EmMem_memcpy((void*)dst, firstLineAddr, lastLineAddr - firstLineAddr);

    return true;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetUARTDevice
// ---------------------------------------------------------------------------
// Return what sort of device is hooked up to the given UART.

EmUARTDeviceType EmRegsEZ::GetUARTDevice(int /*uartNum*/) {
    Bool serEnabled = this->GetLineDriverState(kUARTSerial);
    Bool irEnabled = this->GetLineDriverState(kUARTIR);

    // It's probably an error to have them both enabled at the same
    // time.  !!! TBD: make this an error message.

    EmAssert(!(serEnabled && irEnabled));

    if (serEnabled) return kUARTSerial;

    if (irEnabled) return kUARTIR;

    return kUARTNone;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetDynamicHeapSize
// ---------------------------------------------------------------------------

int32 EmRegsEZ::GetDynamicHeapSize(void) {
    int32 result = 0;

    uint16 csDSelect = READ_REGISTER(csDSelect);

    switch (csDSelect & UPSIZ) {
        case 0x0000:
            result = 32 * 1024L;
            break;

        case 0x0800:
            result = 64 * 1024L;
            break;

        case 0x1000:
            result = 128 * 1024L;
            break;

        case 0x1800:
            result = 256 * 1024L;
            break;

        default:
            EmAssert(false);
            break;
    }

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetROMSize
// ---------------------------------------------------------------------------

int32 EmRegsEZ::GetROMSize(void) {
    /*
            SIZ	Chip-Select Size

            This field determines the memory range of the chip-select. For CSA
            and CSB, chip-select size is between 128K and 16M. For CSC and CSD,
            chip-select size is between 32K and 4M.

            000 = 128K (32K for CSC and CSD).
            001 = 256K (64K for CSC and CSD).
            010 = 512K (128K for CSC and CSD).
            011 = 1M (256K for CSC and CSD).
            100 = 2M (512K for CSC and CSD).
            101 = 4M (1M for CSC and CSD).
            110 = 8M (2M for CSC and CSD).
            111 = 16M (4M for CSC and CSD).
    */

    uint16 csASelect = READ_REGISTER(csASelect);
    uint32 result = (128 * 1024L) << ((csASelect & SIZ) >> 1);

    if ((csASelect & EN) == 0) {
        result = 16 * 1024L * 1024L;
    }

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetROMBaseAddress
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::GetROMBaseAddress(void) {
    /*
            csAGroupBase: Chip-select Group Base Address register

            The csAGroupBase register stores the base address (bits 14-28 of
            the address) in the top 15 bits.  The low bit is always zero.
            Shifting this value by 13 gives the ROM base address.

            E.g:  If the base address is 0x10C00000, then csAGroupBase will
            contain 0x10C00000 >> 13 (base 10) = 0x8600.

            If the enable bit of the CSA register is low, the chip selects
            have not yet been set up.  In this case, return an invalid value.
    */

    if (!this->ChipSelectsConfigured()) {
        return 0xFFFFFFFF;
    }

    uint16 csAGroupBase = READ_REGISTER(csAGroupBase);
    uint32 result = csAGroupBase << gBaseAddressShift;

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::ChipSelectsConfigured
// ---------------------------------------------------------------------------

Bool EmRegsEZ::ChipSelectsConfigured(void) { return READ_REGISTER(csASelect) & EN; }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetSystemClockFrequency
// ---------------------------------------------------------------------------

int32 EmRegsEZ::GetSystemClockFrequency(void) {
    uint16 pllControl = READ_REGISTER(pllControl);
    uint16 pllFreqSel = READ_REGISTER(pllFreqSel);

    // Convert the 32.768KHz clock (CLK32) into the PLLCLK frequency.

    uint16 PC = (pllFreqSel & 0x00FF);
    uint16 QC = (pllFreqSel & 0x0F00) >> 8;

    uint32 result = 32768L * (14 * (PC + 1) + QC + 1);

    // Divide by the prescaler, if needed.

    if ((pllControl & 0x0020) != 0) {
        result /= 2;
    }

    // Divide by the system clock scaler, if needed.

    switch (pllControl & 0x0F00) {
        case hwrEZ328PLLControlSysDMADiv2:
            result /= 2;
            break;

        case hwrEZ328PLLControlSysDMADiv4:
            result /= 4;
            break;

        case hwrEZ328PLLControlSysDMADiv8:
            result /= 8;
            break;

        case hwrEZ328PLLControlSysDMADiv16:
            result /= 16;
            break;
    }

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetCanStop
// ---------------------------------------------------------------------------

Bool EmRegsEZ::GetCanStop(void) {
    // Make sure Timer is enabled or the RTC interrupt is enabled.

    if ((READ_REGISTER(tmr1Control) & hwrEZ328TmrControlEnable) != 0) return true;

    if ((READ_REGISTER(rtcIntEnable) & hwrEZ328RTCIntEnableAlarm) != 0) return true;

    return false;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetAsleep
// ---------------------------------------------------------------------------

Bool EmRegsEZ::GetAsleep(void) {
    return ((READ_REGISTER(pllControl) & hwrEZ328PLLControlDisable) != 0);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetPortInputValue
// ---------------------------------------------------------------------------
// Return the GPIO values for the pins on the port.  These values are used
// if the select pins are high.

uint8 EmRegsEZ::GetPortInputValue(int port) {
    uint8 result = 0;

    if (port == 'D') {
        result = this->GetPortInternalValue(port);
    }

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetPortInternalValue
// ---------------------------------------------------------------------------
// Return the dedicated values for the pins on the port.  These values are
// used if the select pins are low.

uint8 EmRegsEZ::GetPortInternalValue(int port) {
    uint8 result = 0;

    if (port == 'D') {
        // If the ID_DETECT pin is asserted, load the data lines with the
        // hardware ID.

        if (EmRegsEZ::IDDetectAsserted()) {
            result = EmRegsEZ::GetHardwareID();
        }

        // Otherwise, load the lines with keyboard information.

        else {
            // Get the INT bits that need to be set.

            result = this->GetKeyBits();
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::PortDataChanged
// ---------------------------------------------------------------------------

void EmRegsEZ::PortDataChanged(int port, uint8, uint8 newValue) {
    if (port == 'D') {
        // Clear the interrupt bits that are having a 1 written to them.
        // Only clear them if they're configured as edge-senstive.

        uint8 portDIntEdge = READ_REGISTER(portDIntEdge);

        PRINTF("EmRegsEZ::PortDataChanged (D): fPortDEdge  = 0x%02lX", (uint32)(uint8)fPortDEdge);
        PRINTF("EmRegsEZ::PortDataChanged (D): portDIntEdge = 0x%02lX",
               (uint32)(uint8)portDIntEdge);
        PRINTF("EmRegsEZ::PortDataChanged (D): newValue     = 0x%02lX", (uint32)(uint8)newValue);

        fPortDEdge &= ~(newValue & portDIntEdge);

        PRINTF("EmRegsEZ::PortDataChanged (D): fPortDEdge  = 0x%02lX", (uint32)(uint8)fPortDEdge);

        // Set the new interrupt state.

        EmRegsEZ::UpdatePortDInterrupts();
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::pllFreqSelRead
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::pllFreqSelRead(emuptr address, int size) {
    // Simulate the rising and falling of the CLK32 signal so that functions
    // like HwrPreRAMInit, HwrShutDownPLL, PrvSetPLL, and PrvShutDownPLL
    // won't hang.

    uint16 pllFreqSel = READ_REGISTER(pllFreqSel) ^ 0x8000;
    WRITE_REGISTER(pllFreqSel, pllFreqSel);

    // Finish up by doing a standard read.

    return EmRegsEZ::StdRead(address, size);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::portXDataRead
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::portXDataRead(emuptr address, int) {
    // The value read can come from three different places:
    //
    //	- the value what was written to the data register
    //	- any dedicated inputs
    //	- any GPIO inputs
    //
    // The value returned depends on the settings of the SEL and DIR
    // registers.  So let's get those settings, the values from the three
    // input sources, and build up a return value based on those.

    int port = GetPort(address);

    uint8 sel = StdRead(address + 2, 1);
    uint8 dir = StdRead(address - 1, 1);
    uint8 output = StdRead(address + 0, 1);
    uint8 input = EmHAL::GetPortInputValue(port);
    uint8 intFn = EmHAL::GetPortInternalValue(port);

    if (port == 'D') {
        sel |= 0x0F;  // No "select" bit in low nybble, so set for IO values.

        // The system will poll portD 18 times in KeyBootKeys to see
        // if any keys are down.  Wait at least that long before
        // letting up any boot keys maintained by the session.  When we
        // do call ReleaseBootKeys, set our counter to -1 as a flag not
        // to call it any more.

        if (fPortDDataCount != 0xFFFFFFFF && ++fPortDDataCount >= 18 * 2) {
            fPortDDataCount = 0xFFFFFFFF;
#if 0  // CSTODO
            gSession->ReleaseBootKeys();
#endif
        }
    }

    // Use the internal chip function bits if the "sel" bits are zero.

    intFn &= ~sel;

    // Otherwise, use the I/O bits.

    output &= sel & dir;  // Use the output bits if the "dir" is one.
    input &= sel & ~dir;  // Use the input bits if the "dir" is zero.

    // Assert that there are no overlaps.

    EmAssert((output & input) == 0);
    EmAssert((output & intFn) == 0);
    EmAssert((input & intFn) == 0);

    // Mush everything together.

    uint8 result = output | input | intFn;

    // If this is port D, flip the bits if the POLARITY register says to.
    // (!!! Does this inversion apply only to input bits?  That is, the
    // bits where the "dir" register has 0 bits?)

    if (0 && port == 'D') {
        uint8 polarity = READ_REGISTER(portDPolarity);
        PRINTF("EmRegsEZ::portXDataRead: polarity = 0x%02lX", (uint32)polarity);
        result ^= polarity;
    }

    PRINTF("EmRegsEZ::port%cDataRead: sel    dir    output input  intFn  result", (char)port);
    PRINTF("EmRegsEZ::port%cDataRead: 0x%02lX   0x%02lX   0x%02lX   0x%02lX   0x%02lX   0x%02lX",
           (char)port, (uint32)sel, (uint32)dir, (uint32)output, (uint32)input, (uint32)intFn,
           (uint32)result);

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::tmr1StatusRead
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::tmr1StatusRead(emuptr address, int size) {
    uint16 tmr1Counter = READ_REGISTER(tmr1Counter) + 16;
    uint16 tmr1Compare = READ_REGISTER(tmr1Compare);
    uint16 tmr1Control = READ_REGISTER(tmr1Control);

    // Increment the timer.

    WRITE_REGISTER(tmr1Counter, tmr1Counter);

    // If the timer has passed the specified value...

    if ((tmr1Counter - tmr1Compare) < 16) {
        // Set the flag saying the timer timed out.

        uint16 tmr1Status = READ_REGISTER(tmr1Status) | hwrEZ328TmrStatusCompare;
        WRITE_REGISTER(tmr1Status, tmr1Status);

        // If it's not a free-running timer, reset it to zero.

        if ((tmr1Control & hwrEZ328TmrControlFreeRun) == 0) {
            WRITE_REGISTER(tmr1Counter, 0);
        }
    }

    // Remember this guy for later (see EmRegsEZ::tmr1StatusWrite())

    fLastTmr1Status |= READ_REGISTER(tmr1Status);

    // Finish up by doing a standard read.

    return EmRegsEZ::StdRead(address, size);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::uartRead
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::uartRead(emuptr address, int size) {
    // If this is a full read, get the next byte from the FIFO.

    Bool refreshRxData = (address == addressof(uReceive)) && (size == 2);

    // See if there's anything new ("Put the data on the bus")

    EmRegsEZ::UpdateUARTState(refreshRxData);

    // Finish up by doing a standard read.

    return EmRegsEZ::StdRead(address, size);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::rtcHourMinSecRead
// ---------------------------------------------------------------------------

uint32 EmRegsEZ::rtcHourMinSecRead(emuptr address, int size) {
    // Get the desktop machine's time.

    uint32 hour, min, sec;
    Platform::getTime(hour, min, sec);

    // Update the register.

    WRITE_REGISTER(rtcHourMinSec, (hour << hwrEZ328RTCHourMinSecHoursOffset) |
                                      (min << hwrEZ328RTCHourMinSecMinutesOffset) |
                                      (sec << hwrEZ328RTCHourMinSecSecondsOffset));

    // Finish up by doing a standard read.

    return EmRegsEZ::StdRead(address, size);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::csASelectWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::csASelectWrite(emuptr address, int size, uint32 value) {
    // Get the current value.

    uint16 csASelect = READ_REGISTER(csASelect);

    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Check to see if the unprotected memory range changed.

    if ((csASelect & SIZ) != (READ_REGISTER(csASelect) & SIZ)) {
        EmAssert(gSession);
        gSession->ScheduleResetBanks();
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::csDSelectWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::csDSelectWrite(emuptr address, int size, uint32 value) {
    // Get the current value.

    uint16 csDSelect = READ_REGISTER(csDSelect);

    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Check its new state and update our ram-protect flag.

    gMemAccessFlags.fProtect_SRAMSet = (READ_REGISTER(csDSelect) & 0x2000) != 0;

    // Check to see if the unprotected memory range changed.

    if ((csDSelect & UPSIZ) != (READ_REGISTER(csDSelect) & UPSIZ)) {
        EmAssert(gSession);
        gSession->ScheduleResetBanks();
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::intMaskHiWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::intMaskHiWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::intMaskLoWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::intMaskLoWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::intStatusHiWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::intStatusHiWrite(emuptr address, int size, uint32 value) {
    // IRQ1, IRQ2, IRQ3, IRQ6 and IRQ7 are cleared by writing to their
    // respective status bits.  We handle those there.  Since there are
    // no interrupt status bits like this in intStatusLo, we don't need
    // a handler for that register; we only handle intStatusHi.

    // Even though this is a 16-bit register as defined by the Palm headers,
    // it's a 32-bit register according to DragonballEZ docs, and is in fact
    // accessed that way in the kernal files (cf. HwrIRQ4Handler). In those
    // cases, we're still only interested in access to the IRQ# bits, so we
    // can turn 4-byte accesses into 2-byte accesses.

    if (size == 4) value >>= 16;

    // Take into account the possibility of 1-byte accesses, too. If we're
    // accessing the upper byte, just return. If we're accessing the lower
    // byte, we can treat it as a 2-byte access.

    else if (size == 1 && address == addressof(intStatusHi))
        return;

    // Now we can treat the rest of this function as a word-write to intStatusHi.

    uint16 intPendingHi = READ_REGISTER(intPendingHi);

    //	For each interrupt:
    //		If we're writing to that interrupt's status bit and its edge bit is set:
    //			- clear the interrupt's pending bit
    //			- respond to the new interrupt state.

#undef CLEAR_PENDING_INTERRUPT
#define CLEAR_PENDING_INTERRUPT(edge, irq)                       \
    if ((READ_REGISTER(intControl) & edge) && (value & (irq))) { \
        intPendingHi &= ~(irq);                                  \
    }

    CLEAR_PENDING_INTERRUPT(hwrEZ328IntCtlEdge1, hwrEZ328IntHiIRQ1);
    CLEAR_PENDING_INTERRUPT(hwrEZ328IntCtlEdge2, hwrEZ328IntHiIRQ2);
    CLEAR_PENDING_INTERRUPT(hwrEZ328IntCtlEdge3, hwrEZ328IntHiIRQ3);
    CLEAR_PENDING_INTERRUPT(hwrEZ328IntCtlEdge6, hwrEZ328IntHiIRQ6);

    // IRQ7 is not edge-programmable, so clear it if we're merely writing to it.
    // !!! Double check this for EZ!

    if (value & hwrEZ328IntHiEMU) {
        intPendingHi &= ~(hwrEZ328IntHiEMU);
    }

    // If we're emulating the user pressing the hotsync button, make sure the
    // interrupt stays asserted.  (!!! Should we use the same technique for
    // other buttons, too?  It doesn't seem to be needed right now, but doing
    // that may more closely mirror the hardware.)

    if (fHotSyncButtonDown) {
        intPendingHi |= hwrEZ328IntHiIRQ1;
    } else {
        intPendingHi &= ~hwrEZ328IntHiIRQ1;
    }

    // This makes the power on key work. If the signal is asserted, the
    // unit will not transition between asleep and awake (cf. HwrSleep, HwrWake).

    intPendingHi &= ~hwrEZ328IntHiIRQ6;

    WRITE_REGISTER(intPendingHi, intPendingHi);
    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::portXDataWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::portXDataWrite(emuptr address, int size, uint32 value) {
    // Get the old value before updating it.

    uint8 oldValue = StdRead(address, size);

    // Take a snapshot of the line driver states.

    Bool driverStates[kUARTEnd];
    EmHAL::GetLineDriverStates(driverStates);

    // Now update the value with a standard write.

    StdWrite(address, size, value);

    // Let anyone know that it's changed.

    int port = GetPort(address);
    PRINTF("EmRegsEZ::port%cDataWrite: oldValue = 0x%02lX", (char)port, (uint32)(uint8)oldValue);
    PRINTF("EmRegsEZ::port%cDataWrite: newValue = 0x%02lX", (char)port, (uint32)(uint8)value);

    EmHAL::PortDataChanged(port, oldValue, value);

    // Respond to any changes in the line driver states.

    EmHAL::CompareLineDriverStates(driverStates);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::portDIntReqEnWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::portDIntReqEnWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Set the new interrupt state.

    EmRegsEZ::UpdatePortDInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::tmr1StatusWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::tmr1StatusWrite(emuptr address, int size, uint32 value) {
    UNUSED_PARAM(address)
    UNUSED_PARAM(size)

    EmAssert(size == 2);  // This function's a hell of a lot easier to write if
                          // we assume only full-register access.

    // Get the current value.

    uint16 tmr1Status = READ_REGISTER(tmr1Status);

    // If the user had previously read the status bits while they
    // were set, then it's OK for them to be clear now.  Otherwise,
    // we have to merge any set status bits back in.

    tmr1Status &=
        value | ~fLastTmr1Status;  // fLastTmr1Status was set in EmRegsEZ::tmr1StatusRead()

    WRITE_REGISTER(tmr1Status, tmr1Status);

    fLastTmr1Status = 0;
    if ((tmr1Status & hwrEZ328TmrStatusCompare) == 0) {
        uint16 intPendingLo = READ_REGISTER(intPendingLo) & ~hwrEZ328IntLoTimer;
        WRITE_REGISTER(intPendingLo, intPendingLo);

        // Respond to the new interrupt state.

        EmRegsEZ::UpdateInterrupts();
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::spiMasterControlWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::spiMasterControlWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Get the current value.

    uint16 spiMasterData = READ_REGISTER(spiMasterData);
    uint16 spiMasterControl = READ_REGISTER(spiMasterControl);

    // Check to see if data exchange and interrupts are enabled.

#define BIT_MASK (hwrEZ328SPIMControlExchange | hwrEZ328SPIMControlEnable)
    if ((spiMasterControl & BIT_MASK) == BIT_MASK) {
        // If the SPI is hooked up to something, talk with it.

        EmSPISlave* spiSlave = this->GetSPISlave();
        if (spiSlave) {
            // Write out the old data, read in the new data.

            uint16 newData = spiSlave->DoExchange(spiMasterControl, spiMasterData);

            // Shift in the new data.

            uint16 numBits = (spiMasterControl & hwrEZ328SPIMControlBitsMask) + 1;

            uint16 oldBitsMask = ~0u << numBits;
            uint16 newBitsMask = ~oldBitsMask;

            spiMasterData = /*((spiMasterData << numBits) & oldBitsMask) | */
                (newData & newBitsMask);

            WRITE_REGISTER(spiMasterData, spiMasterData);
        }

        // Assert the interrupt and clear the exchange bit.

        spiMasterControl |= hwrEZ328SPIMControlIntStatus;
        spiMasterControl &= ~hwrEZ328SPIMControlExchange;

        WRITE_REGISTER(spiMasterControl, spiMasterControl);

        // If hwrEZ328SPIMControlIntEnable is set, trigger an interrupt.

        if ((spiMasterControl & hwrEZ328SPIMControlIntEnable) != 0) {
            uint16 intPendingLo = READ_REGISTER(intPendingLo);
            intPendingLo |= hwrEZ328IntLoSPIM;
            WRITE_REGISTER(intPendingLo, intPendingLo);
            this->UpdateInterrupts();
        }
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::uartWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::uartWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // If this write included the TX_DATA field, signal that it needs to
    // be transmitted.

    Bool sendTxData = ((address == addressof(uTransmit)) && (size == 2)) ||
                      ((address == addressof(uTransmit) + 1) && (size == 1));

    // React to any changes.

    EmRegsEZ::UARTStateChanged(sendTxData);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::lcdRegisterWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::lcdRegisterWrite(emuptr address, int size, uint32 value) {
    // First, get the old value in case we need to see what changed.

    uint32 oldValue = EmRegsEZ::StdRead(address, size);

    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Note what changed.

    if (address == addressof(lcdScreenWidth)) {
        // CSTODO EmScreen::InvalidateAll();
    } else if (address == addressof(lcdScreenHeight)) {
        // CSTODO EmScreen::InvalidateAll();
    } else if (address == addressof(lcdPanelControl)) {
        // hwrEZ328LcdPanelControlGrayScale is incorrectly defined as 0x01,
        // so use the hard-coded value of 0x03 here.

        //		if (((value ^ oldValue) & hwrEZ328LcdPanelControlGrayScale) != 0)
#if 0  // CSTODO
        if (((value ^ oldValue) & 0x03) != 0) {
            EmScreen::InvalidateAll();
        }
#endif
    } else if (address == addressof(lcdStartAddr)) {
        // Make sure the low-bit is always zero.
        // Make sure bits 31-29 are always zero.

        uint32 lcdStartAddr = READ_REGISTER(lcdStartAddr) & 0x1FFFFFFE;
        WRITE_REGISTER(lcdStartAddr, lcdStartAddr);

        // CSTODO EmScreen::InvalidateAll();
    } else if (address == addressof(lcdPageWidth)) {
        if (value != oldValue) {
            // CSTODO EmScreen::InvalidateAll();
        }
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::rtcControlWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::rtcControlWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateRTCInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::rtcIntStatusWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::rtcIntStatusWrite(emuptr address, int size, uint32 value) {
    // Status bits are cleared by writing ones to them.

    // If we're doing a byte-write to the upper byte, shift the byte
    // so that we can treat the operation as a word write.  If we're
    // doing a byte-write to the lower byte, this extension will happen
    // automatically.

    if (address == addressof(rtcIntStatus) && size == 1) value <<= 8;

    // Get the current value.

    uint16 rtcIntStatus = READ_REGISTER(rtcIntStatus);

    // Clear the requested bits.

    rtcIntStatus &= ~value;

    // Update the register.

    WRITE_REGISTER(rtcIntStatus, rtcIntStatus);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateRTCInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::rtcIntEnableWrite
// ---------------------------------------------------------------------------

void EmRegsEZ::rtcIntEnableWrite(emuptr address, int size, uint32 value) {
    // Do a standard update of the register.

    EmRegsEZ::StdWrite(address, size, value);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateRTCInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::ButtonEvent
// ---------------------------------------------------------------------------
// Handles a Palm device button event by updating the appropriate registers.

void EmRegsEZ::ButtonEvent(ButtonEventT event) {
    uint16 bitNumber = this->ButtonToBits(event.GetButton());

    // Get the bits that should have been set with the previous set
    // of pressed keys.  We use this old value to update the port D interrupts.

    uint8 oldBits = this->GetKeyBits();

    // Update the set of keys that are currently pressed.

    if (event.GetType() == ButtonEventT::Type::press) {
        fKeyBits |= bitNumber;  // Remember the key bit
    } else {
        fKeyBits &= ~bitNumber;  // Forget the key bit
    }

    // Now get the new set of bits that should be set.

    uint8 newBits = this->GetKeyBits();

    PRINTF("EmRegsEZ::ButtonEvent: fKeyBits = 0x%04lX", (uint32)fKeyBits);
    PRINTF("EmRegsEZ::ButtonEvent: oldBits   = 0x%02lX", (uint32)oldBits);
    PRINTF("EmRegsEZ::ButtonEvent: newBits   = 0x%02lX", (uint32)newBits);

    // Set the interrupt bits for the bits that went from off to on.
    // These get cleared when portDData is written to.

    fPortDEdge |= newBits & ~oldBits;

    PRINTF("EmRegsEZ::ButtonEvent: fPortDEdge = 0x%02lX", (uint32)fPortDEdge);

    // Set the new interrupt state.

    EmRegsEZ::UpdatePortDInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::HotSyncEvent
// ---------------------------------------------------------------------------
// Handles a HotSync button event by updating the appropriate registers.

void EmRegsEZ::HotSyncEvent(Bool buttonIsDown) {
    // If the button changes state, set or clear the HotSync interrupt.

    uint16 intPendingHi = READ_REGISTER(intPendingHi);

    if (buttonIsDown) {
        intPendingHi |= hwrEZ328IntHiIRQ1;
        fHotSyncButtonDown = true;
    } else {
        intPendingHi &= ~hwrEZ328IntHiIRQ1;
        fHotSyncButtonDown = false;
    }

    WRITE_REGISTER(intPendingHi, intPendingHi);

    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetKeyBits
// ---------------------------------------------------------------------------

uint8 EmRegsEZ::GetKeyBits(void) {
    // "Keys" (that is, buttons) are read from the Port D Data register.
    // There are 7 or 8 keys that can be pressed, but only 4 bits are
    // available in the Port D Data register for reporting pressed keys.
    // Therefore, the keys are organized into a matrix, one row or which
    // can be requested and reported at a time.  This function determines
    // what row is being requested, and sets the appropriate Port D Data
    // bits for the keys are are currently pressed.

    int numRows;
    int numCols;
    uint16 keyMap[16];
    Bool rows[4];

    this->GetKeyInfo(&numRows, &numCols, keyMap, rows);

    uint8 keyData = 0;

    // Walk the rows, looking for one that is requested.

    for (int row = 0; row < numRows; ++row) {
        if (rows[row]) {
            // Walk the columns, looking for ones that have a pressed key.

            for (int col = 0; col < numCols; ++col) {
                // Get the key corresponding to this row and column.
                // If we've recorded (in fKeyBits) that this key is
                // pressed, then set its column bit.

                uint16 key = keyMap[row * numCols + col];
                if ((key & fKeyBits) != 0) {
                    keyData |= (1 << col);
                }
            }
        }
    }

#ifdef LOGGING
    UInt8 portFDir = READ_REGISTER(portFDir);
    UInt8 portFData = READ_REGISTER(portFData);

    PRINTF("EmRegsEZ::GetKeyBits: numRows = %d, numCols = %d", numRows, numCols);
    PRINTF("EmRegsEZ::GetKeyBits: portFDir = 0x%02lX, portFData = 0x%02lX", (uint32)portFDir,
           (uint32)portFData);
    PRINTF("EmRegsEZ::GetKeyBits: rows[0] = %d, [1] = %d, [2] = %d, [3] = %d", rows[0], rows[1],
           rows[2], rows[3]);
    //	PRINTF ("EmRegsEZ::GetKeyBits: keyMap[0] = %2d, [1] = %2d, [2] = %2d, [3] = %2d", keyMap[0],
    // keyMap[1], keyMap[2], keyMap[3]); 	PRINTF ("EmRegsEZ::GetKeyBits: keyMap[4] = %2d, [5]
    // = %2d, [6] = %2d, [7] = %2d", keyMap[4], keyMap[5], keyMap[6], keyMap[7]); 	PRINTF
    //("EmRegsEZ::GetKeyBits: keyMap[8] = %2d, [9] = %2d, [A] = %2d, [B] = %2d", keyMap[8],
    // keyMap[9], keyMap[10], keyMap[11]); 	PRINTF ("EmRegsEZ::GetKeyBits: keyMap[C] = %2d, [D]
    // = %2d, [E] = %2d, [F] = %2d", keyMap[12], keyMap[13], keyMap[14], keyMap[15]);
    PRINTF("EmRegsEZ::GetKeyBits: fKeyBits = 0x%04lX", (uint32)fKeyBits);
    PRINTF("EmRegsEZ::GetKeyBits: keyData = 0x%02lX", (uint32)keyData);
#endif

    return keyData;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::ButtonToBits
// ---------------------------------------------------------------------------

uint16 EmRegsEZ::ButtonToBits(ButtonEventT::Button button) {
    switch (button) {
        case ButtonEventT::Button::power:
            return keyBitPower;

        case ButtonEventT::Button::rockerUp:
            return keyBitPageUp;

        case ButtonEventT::Button::rockerDown:
            return keyBitPageDown;

        case ButtonEventT::Button::app1:
            return keyBitHard1;

        case ButtonEventT::Button::app2:
            return keyBitHard2;

        case ButtonEventT::Button::app3:
            return keyBitHard3;

        case ButtonEventT::Button::app4:
            return keyBitHard4;

        case ButtonEventT::Button::cradle:
            return keyBitCradle;

        case ButtonEventT::Button::antenna:
            return keyBitAntenna;

        case ButtonEventT::Button::contrast:
            return keyBitContrast;

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetSPISlave
// ---------------------------------------------------------------------------

EmSPISlave* EmRegsEZ::GetSPISlave(void) { return NULL; }

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UpdateInterrupts
// ---------------------------------------------------------------------------
// Determines whether an interrupt has occurred by copying the Interrupt
// Pending Register to the Interrupt Status Register.

void EmRegsEZ::UpdateInterrupts(void) {
    // Copy the Interrupt Pending Register to the Interrupt Status
    // Register, but ignore interrupts that are being masked.

    // Note: this function is not sensitive to the byte ordering of the registers,
    // so their contents don't need to be accessed via READ_REGISTER or WRITE_REGISTER.

    f68EZ328Regs.intStatusHi = f68EZ328Regs.intPendingHi & ~f68EZ328Regs.intMaskHi;
    f68EZ328Regs.intStatusLo = f68EZ328Regs.intPendingLo & ~f68EZ328Regs.intMaskLo;

    PRINTF("EmRegsEZ::UpdateInterrupts: intMask    = 0x%04lX %04lX", (uint32)f68EZ328Regs.intMaskHi,
           (uint32)f68EZ328Regs.intMaskLo);

    PRINTF("EmRegsEZ::UpdateInterrupts: intPending = 0x%04lX %04lX",
           (uint32)f68EZ328Regs.intPendingHi, (uint32)f68EZ328Regs.intPendingLo);

    // If the Interrupt Status Register isn't clear, flag an interrupt.

    if (f68EZ328Regs.intStatusHi || f68EZ328Regs.intStatusLo) {
        regs.spcflags |= SPCFLAG_INT;

        PRINTF("EmRegsEZ::UpdateInterrupts: intStatus  = 0x%04lX %04lX",
               (uint32)f68EZ328Regs.intStatusHi, (uint32)f68EZ328Regs.intStatusLo);
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UpdatePortDInterrupts
// ---------------------------------------------------------------------------
// Determine what interrupts need to be generated based on the current
// settings in portDData and fPortDEdge.

void EmRegsEZ::UpdatePortDInterrupts(void) {
    // Update INT0-INT3 of the Interrupt-Pending register (bits 8-11 of the low word).

    PRINTF("EmRegsEZ::UpdatePortDInterrupts:");

    // First, get those bits and clear them out.

    uint16 intPendingLo = READ_REGISTER(intPendingLo) & ~hwrEZ328IntLoAllKeys;

    // Initialize the variable to hold the new interrupt settings.

    uint8 newBits = 0;

    // Get some other values we're going to need:

    uint8 portDDir = READ_REGISTER(portDDir);  // Interrupt on inputs only (when pin is low)
    uint8 portDData = EmHAL::GetPortInputValue('D');
    uint8 portDPolarity = READ_REGISTER(portDPolarity);
    uint8 portDIntReqEn = READ_REGISTER(portDIntReqEn);
    uint8 portDKbdIntEn = READ_REGISTER(portDKbdIntEn);
    uint8 portDIntEdge = READ_REGISTER(portDIntEdge);

    // We have a line-level interrupt if:
    //
    //	- line-level interrupts are requested
    //	- the GPIO bit matches the polarity bit

    newBits |= ~portDIntEdge & portDData & portDPolarity;
    newBits |= ~portDIntEdge & ~portDData & ~portDPolarity;

    // We have an edge interrupt if:
    //
    //	- edge interrupts are requested
    //	- an edge has been recorded
    //
    // Note that we should distinguish between rising and falling edges.
    // For historical reasons, that's not done, and the Palm OS doesn't
    // look for them, so it's OK for now.
    //
    // Edge interrupts on INT[3:0] should not wake up a sleeping device.

    uint16 pllControl = READ_REGISTER(pllControl);

    if (pllControl & hwrEZ328PLLControlDisable) {
        newBits |= portDIntEdge & fPortDEdge & portDPolarity & 0xF0;
        newBits |= portDIntEdge & 0 & ~portDPolarity & 0xF0;
    } else {
        newBits |= portDIntEdge & fPortDEdge & portDPolarity;
        newBits |= portDIntEdge & 0 & ~portDPolarity;
    }

    // Only have interrupts if they're enabled and the pin is configured for input.

    newBits &= portDIntReqEn & ~portDDir;

    PRINTF("EmRegsEZ::UpdatePortDInterrupts: Dir  Data Pol  Req  Edg  PDE  bits");
    PRINTF(
        "EmRegsEZ::UpdatePortDInterrupts: 0x%02lX 0x%02lX 0x%02lX 0x%02lX 0x%02lX 0x%02lX 0x%02lX",
        (uint32)portDDir, (uint32)portDData, (uint32)portDPolarity, (uint32)portDIntReqEn,
        (uint32)portDIntEdge, (uint32)fPortDEdge, (uint32)newBits);

    // Determine if the KB interrupt needs to be asserted.  It is if:
    //
    //	A Port D Data bit is on.
    //	The bit is configured for input (?)
    //	The bit is configured to be OR'd into the interrupt.

    uint8 KB = portDData & ~portDDir & portDKbdIntEn;

    if (KB)
        intPendingLo |= hwrEZ328IntLoKbd;
    else
        intPendingLo &= ~hwrEZ328IntLoKbd;

    // Merge in the new values and write out the result.

    intPendingLo |= (((uint16)newBits) << hwrEZ328IntLoInt0Bit) & hwrEZ328IntLoAllKeys;
    WRITE_REGISTER(intPendingLo, intPendingLo);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UpdateRTCInterrupts
// ---------------------------------------------------------------------------
// Determine whether to set or clear the RTC bit in the interrupt pending
// register based on the current RTC register values.

void EmRegsEZ::UpdateRTCInterrupts(void) {
    // See if the RTC is enabled.

    Bool rtcEnabled = (READ_REGISTER(rtcControl) & hwrEZ328RTCControlRTCEnable) != 0;

    // See if there are any RTC events that need to trigger an interrupt.

#define BITS_TO_CHECK                                                                 \
    (hwrEZ328RTCIntEnableSec | hwrEZ328RTCIntEnable24Hr | hwrEZ328RTCIntEnableAlarm | \
     hwrEZ328RTCIntEnableMinute | hwrEZ328RTCIntEnableStopWatch)

    uint16 rtcIntStatus = READ_REGISTER(rtcIntStatus);
    uint16 rtcIntEnable = READ_REGISTER(rtcIntEnable);
    uint16 rtcIntPending = rtcIntStatus & rtcIntEnable & BITS_TO_CHECK;

    Bool havePendingEvents = rtcIntPending != 0;

    // If the RTC is enabled and there are pending events, set the interrupt.
    // Otherwise, clear the interrupt.

    uint16 intPendingLo = READ_REGISTER(intPendingLo);

    if (rtcEnabled && havePendingEvents) {
        intPendingLo |= hwrEZ328IntLoRTC;  // have events, so set interrupt
    } else {
        intPendingLo &= ~hwrEZ328IntLoRTC;  // no events, so clear interrupt
    }

    // Update the interrupt pending register.

    WRITE_REGISTER(intPendingLo, intPendingLo);

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::IDDetectAsserted
// ---------------------------------------------------------------------------
// cf. HwrIdentifyFeatures and HwrPreRAMInit.

Bool EmRegsEZ::IDDetectAsserted(void) {
    uint8 portGDir = READ_REGISTER(portGDir);
    uint8 portGData = READ_REGISTER(portGData);
    uint8 portGPullupEn = READ_REGISTER(portGPullupEn);
    uint8 portGSelect = READ_REGISTER(portGSelect);
    const uint8 kMask = hwrEZPortGIDDetect;

    return (portGDir & kMask) == kMask && (portGData & kMask) == 0 &&
           (portGPullupEn & kMask) == 0 && (portGSelect & kMask) == kMask;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetHardwareID
// ---------------------------------------------------------------------------

UInt8 EmRegsEZ::GetHardwareID(void) {
    // Determine the hardware ID.

    EmAssert(gSession);

    long miscFlags = gSession->GetDevice().HardwareID();

    // Reverse map the following:

    //	GHwrMiscFlags = 0;
    //	if ((keyState & hwrEZPortDKbdCol0) == 0) GHwrMiscFlags |= hwrMiscFlagID1;
    //	if ((keyState & hwrEZPortDKbdCol1) == 0) GHwrMiscFlags |= hwrMiscFlagID2;
    //	if ((keyState & hwrEZPortDKbdCol2) == 0) GHwrMiscFlags |= hwrMiscFlagID3;
    //	if ((keyState & hwrEZPortDKbdCol3) == 0) GHwrMiscFlags |= hwrMiscFlagID4;

    UInt8 keyState = ~0;

    if ((miscFlags & hwrMiscFlagID1) != 0) keyState &= ~hwrEZPortDKbdCol0;
    if ((miscFlags & hwrMiscFlagID2) != 0) keyState &= ~hwrEZPortDKbdCol1;
    if ((miscFlags & hwrMiscFlagID3) != 0) keyState &= ~hwrEZPortDKbdCol2;
    if ((miscFlags & hwrMiscFlagID4) != 0) keyState &= ~hwrEZPortDKbdCol3;

    return keyState;
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UARTStateChanged
// ---------------------------------------------------------------------------

void EmRegsEZ::UARTStateChanged(Bool sendTxData) {
    EmUARTDragonball::State state(EmUARTDragonball::kUART_DragonballEZ);

    EmRegsEZ::MarshalUARTState(state);
    fUART->StateChanged(state, sendTxData);
    EmRegsEZ::UnmarshalUARTState(state);

    EmRegsEZ::UpdateUARTInterrupts(state);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UpdateUARTState
// ---------------------------------------------------------------------------

void EmRegsEZ::UpdateUARTState(Bool refreshRxData) {
    EmUARTDragonball::State state(EmUARTDragonball::kUART_DragonballEZ);

    EmRegsEZ::MarshalUARTState(state);
    fUART->UpdateState(state, refreshRxData);
    EmRegsEZ::UnmarshalUARTState(state);

    EmRegsEZ::UpdateUARTInterrupts(state);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UpdateUARTInterrupts
// ---------------------------------------------------------------------------

void EmRegsEZ::UpdateUARTInterrupts(const EmUARTDragonball::State& state) {
    // Generate the appropriate interrupts.

    if ((state.RX_FULL_ENABLE && state.RX_FIFO_FULL) ||
        (state.RX_HALF_ENABLE && state.RX_FIFO_HALF) || (state.RX_RDY_ENABLE && state.DATA_READY) ||
        (state.TX_EMPTY_ENABLE && state.TX_FIFO_EMPTY) ||
        (state.TX_HALF_ENABLE && state.TX_FIFO_HALF) || (state.TX_AVAIL_ENABLE && state.TX_AVAIL)) {
        // Set the UART interrupt.

        WRITE_REGISTER(intPendingLo, READ_REGISTER(intPendingLo) | hwrEZ328IntLoUART);
    } else {
        // Clear the UART interrupt.

        WRITE_REGISTER(intPendingLo, READ_REGISTER(intPendingLo) & ~hwrEZ328IntLoUART);
    }

    // Respond to the new interrupt state.

    EmRegsEZ::UpdateInterrupts();
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::MarshalUARTState
// ---------------------------------------------------------------------------

void EmRegsEZ::MarshalUARTState(EmUARTDragonball::State& state) {
    uint16 uControl = READ_REGISTER(uControl);
    uint16 uBaud = READ_REGISTER(uBaud);
    uint16 uReceive = READ_REGISTER(uReceive);
    uint16 uTransmit = READ_REGISTER(uTransmit);
    uint16 uMisc = READ_REGISTER(uMisc);

    state.UART_ENABLE = (uControl & hwrEZ328UControlUARTEnable) != 0;
    state.RX_ENABLE = (uControl & hwrEZ328UControlRxEnable) != 0;
    state.TX_ENABLE = (uControl & hwrEZ328UControlTxEnable) != 0;
    state.RX_CLK_CONT = (uControl & hwrEZ328UControlRxClock1xSync) != 0;
    state.PARITY_EN = (uControl & hwrEZ328UControlParityEn) != 0;
    state.ODD_EVEN = (uControl & hwrEZ328UControlParityOdd) != 0;
    state.STOP_BITS = (uControl & hwrEZ328UControlStopBits2) != 0;
    state.CHAR8_7 = (uControl & hwrEZ328UControlDataBits8) != 0;
    //	state.GPIO_DELTA_ENABLE	= (uControl & hwr328UControlGPIODeltaEn) != 0;	// 68328 only
    state.OLD_ENABLE = (uControl & hwrEZ328UControlOldDataEn) != 0;  // 68EZ328 only
    state.CTS_DELTA_ENABLE = (uControl & hwrEZ328UControlCTSDeltaEn) != 0;
    state.RX_FULL_ENABLE = (uControl & hwrEZ328UControlRxFullEn) != 0;
    state.RX_HALF_ENABLE = (uControl & hwrEZ328UControlRxHalfEn) != 0;
    state.RX_RDY_ENABLE = (uControl & hwrEZ328UControlRxRdyEn) != 0;
    state.TX_EMPTY_ENABLE = (uControl & hwrEZ328UControlTxEmptyEn) != 0;
    state.TX_HALF_ENABLE = (uControl & hwrEZ328UControlTxHalfEn) != 0;
    state.TX_AVAIL_ENABLE = (uControl & hwrEZ328UControlTxAvailEn) != 0;

    // Baud control register bits
    // These are all values the user sets; we just look at them.

    //	state.GPIO_DELTA		= (uBaud & hwr328UBaudGPIODelta) != 0;			//
    // 68328 only 	state.GPIO				= (uBaud & hwr328UBaudGPIOData) !=
    // 0;
    //// 68328 only
    //	state.GPIO_DIR			= (uBaud & hwr328UBaudGPIODirOut) != 0;			//
    // 68328 only 	state.GPIO_SRC			= (uBaud & hwrEZ328UBaudGPIOSrcBaudGen) !=
    // 0;
    // // 68328 only
    state.UCLK_DIR = (uBaud & hwrEZ328UBaudUCLKDirOut) != 0;  // 68EZ328 only
    state.BAUD_SRC = (uBaud & hwrEZ328UBaudBaudSrcUCLK) != 0;
    state.DIVIDE = (uBaud & hwrEZ328UBaudDivider) >> hwrEZ328UBaudDivideBitOffset;
    state.PRESCALER = (uBaud & hwrEZ328UBaudPrescaler);

    // Receive register bits
    // These are all input bits; we set them, not the user.

    state.RX_FIFO_FULL = (uReceive & hwrEZ328UReceiveFIFOFull) != 0;
    state.RX_FIFO_HALF = (uReceive & hwrEZ328UReceiveFIFOHalf) != 0;
    state.DATA_READY = (uReceive & hwrEZ328UReceiveDataRdy) != 0;
    state.OLD_DATA = (uReceive & hwrEZ328UReceiveOldData) != 0;  // 68EZ328 only
    state.OVRUN = (uReceive & hwrEZ328UReceiveOverrunErr) != 0;
    state.FRAME_ERROR = (uReceive & hwrEZ328UReceiveFrameErr) != 0;
    state.BREAK = (uReceive & hwrEZ328UReceiveBreakErr) != 0;
    state.PARITY_ERROR = (uReceive & hwrEZ328UReceiveParityErr) != 0;
    state.RX_DATA = (uReceive & hwrEZ328UReceiveData);

    // Transmitter register bits
    // We set everything except TX_DATA; the user sets that
    // value and ONLY that value.

    state.TX_FIFO_EMPTY = (uTransmit & hwrEZ328UTransmitFIFOEmpty) != 0;
    state.TX_FIFO_HALF = (uTransmit & hwrEZ328UTransmitFIFOHalf) != 0;
    state.TX_AVAIL = (uTransmit & hwrEZ328UTransmitTxAvail) != 0;
    state.SEND_BREAK = (uTransmit & hwrEZ328UTransmitSendBreak) != 0;
    state.IGNORE_CTS = (uTransmit & hwrEZ328UTransmitIgnoreCTS) != 0;
    state.BUSY = (uTransmit & hwrEZ328UTransmitBusy) != 0;  // 68EZ328 only
    state.CTS_STATUS = (uTransmit & hwrEZ328UTransmitCTSStatus) != 0;
    state.CTS_DELTA = (uTransmit & hwrEZ328UTransmitCTSDelta) != 0;
    state.TX_DATA = (uTransmit & hwrEZ328UTransmitData);

    // Misc register bits
    // These are all values the user sets; we just look at them.

    state.BAUD_TEST = (uMisc & hwrEZ328UMiscBaudTest) != 0;  // 68EZ328 only
    state.CLK_SRC = (uMisc & hwrEZ328UMiscClkSrcUCLK) != 0;
    state.FORCE_PERR = (uMisc & hwrEZ328UMiscForceParityErr) != 0;
    state.LOOP = (uMisc & hwrEZ328UMiscLoopback) != 0;
    state.BAUD_RESET = (uMisc & hwrEZ328UMiscBaudReset) != 0;  // 68EZ328 only
    state.IR_TEST = (uMisc & hwrEZ328UMiscIRTestEn) != 0;      // 68EZ328 only
    state.RTS_CONT = (uMisc & hwrEZ328UMiscRTSThruFIFO) != 0;
    state.RTS = (uMisc & hwrEZ328UMiscRTSOut) != 0;
    state.IRDA_ENABLE = (uMisc & hwrEZ328UMiscIRDAEn) != 0;
    state.IRDA_LOOP = (uMisc & hwrEZ328UMiscLoopIRDA) != 0;
    state.RX_POL = (uMisc & hwrEZ328UMiscRXPolarityInv) != 0;  // 68EZ328 only
    state.TX_POL = (uMisc & hwrEZ328UMiscTXPolarityInv) != 0;  // 68EZ328 only
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::UnmarshalUARTState
// ---------------------------------------------------------------------------

void EmRegsEZ::UnmarshalUARTState(const EmUARTDragonball::State& state) {
    uint16 uControl = 0;
    uint16 uBaud = 0;
    uint16 uReceive = 0;
    uint16 uTransmit = 0;
    uint16 uMisc = 0;

    if (state.UART_ENABLE) uControl |= hwrEZ328UControlUARTEnable;
    if (state.RX_ENABLE) uControl |= hwrEZ328UControlRxEnable;
    if (state.TX_ENABLE) uControl |= hwrEZ328UControlTxEnable;
    if (state.RX_CLK_CONT) uControl |= hwrEZ328UControlRxClock1xSync;
    if (state.PARITY_EN) uControl |= hwrEZ328UControlParityEn;
    if (state.ODD_EVEN) uControl |= hwrEZ328UControlParityOdd;
    if (state.STOP_BITS) uControl |= hwrEZ328UControlStopBits2;
    if (state.CHAR8_7) uControl |= hwrEZ328UControlDataBits8;
    //	if (state.GPIO_DELTA_ENABLE)uControl |= hwr328UControlGPIODeltaEn;	// 68328 only
    if (state.OLD_ENABLE) uControl |= hwrEZ328UControlOldDataEn;  // 68EZ328 only
    if (state.CTS_DELTA_ENABLE) uControl |= hwrEZ328UControlCTSDeltaEn;
    if (state.RX_FULL_ENABLE) uControl |= hwrEZ328UControlRxFullEn;
    if (state.RX_HALF_ENABLE) uControl |= hwrEZ328UControlRxHalfEn;
    if (state.RX_RDY_ENABLE) uControl |= hwrEZ328UControlRxRdyEn;
    if (state.TX_EMPTY_ENABLE) uControl |= hwrEZ328UControlTxEmptyEn;
    if (state.TX_HALF_ENABLE) uControl |= hwrEZ328UControlTxHalfEn;
    if (state.TX_AVAIL_ENABLE) uControl |= hwrEZ328UControlTxAvailEn;

    // Baud control register bits
    // These are all values the user sets; we just look at them.

    //	if (state.GPIO_DELTA)		uBaud |= hwr328UBaudGPIODelta;		// 68328 only
    //	if (state.GPIO)				uBaud |= hwr328UBaudGPIOData;		// 68328
    // only 	if (state.GPIO_DIR)			uBaud |= hwr328UBaudGPIODirOut;		//
    // 68328
    // only 	if (state.GPIO_SRC)			uBaud |= hwr328UBaudGPIOSrcBaudGen;	//
    // 68328 only
    if (state.UCLK_DIR) uBaud |= hwrEZ328UBaudUCLKDirOut;  // 68EZ328 only
    if (state.BAUD_SRC) uBaud |= hwrEZ328UBaudBaudSrcUCLK;

    uBaud |= (state.DIVIDE << hwrEZ328UBaudDivideBitOffset) & hwrEZ328UBaudDivider;
    uBaud |= (state.PRESCALER) & hwrEZ328UBaudPrescaler;

    // Receive register bits
    // These are all input bits; we set them, not the user.

    if (state.RX_FIFO_FULL) uReceive |= hwrEZ328UReceiveFIFOFull;
    if (state.RX_FIFO_HALF) uReceive |= hwrEZ328UReceiveFIFOHalf;
    if (state.DATA_READY) uReceive |= hwrEZ328UReceiveDataRdy;
    if (state.OLD_DATA) uReceive |= hwrEZ328UReceiveOldData;  // 68EZ328 only
    if (state.OVRUN) uReceive |= hwrEZ328UReceiveOverrunErr;
    if (state.FRAME_ERROR) uReceive |= hwrEZ328UReceiveFrameErr;
    if (state.BREAK) uReceive |= hwrEZ328UReceiveBreakErr;
    if (state.PARITY_ERROR) uReceive |= hwrEZ328UReceiveParityErr;

    uReceive |= (state.RX_DATA) & hwrEZ328UReceiveData;

    // Transmitter register bits
    // We set everything except TX_DATA; the user sets that
    // value and ONLY that value.

    if (state.TX_FIFO_EMPTY) uTransmit |= hwrEZ328UTransmitFIFOEmpty;
    if (state.TX_FIFO_HALF) uTransmit |= hwrEZ328UTransmitFIFOHalf;
    if (state.TX_AVAIL) uTransmit |= hwrEZ328UTransmitTxAvail;
    if (state.SEND_BREAK) uTransmit |= hwrEZ328UTransmitSendBreak;
    if (state.IGNORE_CTS) uTransmit |= hwrEZ328UTransmitIgnoreCTS;
    if (state.BUSY) uTransmit |= hwrEZ328UTransmitBusy;  // 68EZ328 only
    if (state.CTS_STATUS) uTransmit |= hwrEZ328UTransmitCTSStatus;
    if (state.CTS_DELTA) uTransmit |= hwrEZ328UTransmitCTSDelta;

    uTransmit |= (state.TX_DATA) & hwrEZ328UTransmitData;

    // Misc register bits
    // These are all values the user sets; we just look at them.

    if (state.BAUD_TEST) uMisc |= hwrEZ328UMiscBaudTest;  // 68EZ328 only
    if (state.CLK_SRC) uMisc |= hwrEZ328UMiscClkSrcUCLK;
    if (state.FORCE_PERR) uMisc |= hwrEZ328UMiscForceParityErr;
    if (state.LOOP) uMisc |= hwrEZ328UMiscLoopback;
    if (state.BAUD_RESET) uMisc |= hwrEZ328UMiscBaudReset;  // 68EZ328 only
    if (state.IR_TEST) uMisc |= hwrEZ328UMiscIRTestEn;      // 68EZ328 only
    if (state.RTS_CONT) uMisc |= hwrEZ328UMiscRTSThruFIFO;
    if (state.RTS) uMisc |= hwrEZ328UMiscRTSOut;
    if (state.IRDA_ENABLE) uMisc |= hwrEZ328UMiscIRDAEn;
    if (state.IRDA_LOOP) uMisc |= hwrEZ328UMiscLoopIRDA;
    if (state.RX_POL) uMisc |= hwrEZ328UMiscRXPolarityInv;  // 68EZ328 only
    if (state.TX_POL) uMisc |= hwrEZ328UMiscTXPolarityInv;  // 68EZ328 only

    WRITE_REGISTER(uControl, uControl);
    WRITE_REGISTER(uBaud, uBaud);
    WRITE_REGISTER(uReceive, uReceive);
    WRITE_REGISTER(uTransmit, uTransmit);
    WRITE_REGISTER(uMisc, uMisc);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZ::GetPort
// ---------------------------------------------------------------------------
// Given an address, return a value indicating what port it is associated with.

int EmRegsEZ::GetPort(emuptr address) {
    const long MASK = 0x00000FF8;

    switch (address & MASK) {
        case 0x0400:
            return 'A';
        case 0x0408:
            return 'B';
        case 0x0410:
            return 'C';
        case 0x0418:
            return 'D';
        case 0x0420:
            return 'E';
        case 0x0428:
            return 'F';
        case 0x0430:
            return 'G';
    }

    EmAssert(false);
    return 0;
}
