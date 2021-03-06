#include <p18cxxx.h>
#include "device_io.h"
#ifdef LINUX_BUILD
#include "../Shared/general_io.h"
#else
#include "..\Shared\general_io.h"
#endif

/* Definitions for MMC/SDC command */
#define CMD0   (0x40+0) /* GO_IDLE_STATE */
#define CMD1   (0x40+1) /* SEND_OP_COND (MMC) */
#define  ACMD41   (0xC0+41)   /* SEND_OP_COND (SDC) */
#define CMD8   (0x40+8) /* SEND_IF_COND */
#define CMD16  (0x40+16)   /* SET_BLOCKLEN */
#define CMD17  (0x40+17)   /* READ_SINGLE_BLOCK */
#define CMD24  (0x40+24)   /* WRITE_BLOCK */
#define CMD55  (0x40+55)   /* APP_CMD */
#define CMD58  (0x40+58)   /* READ_OCR */


void INIT_SPI(void)
{
   SPI_CS_TRIS = 0;
   DESELECT();

   SPI_DIN_TRIS = 1;

   SPI_DOUT_TRIS = 0;
   SPI_DOUT_PIN = 1;

   SPI_SCK_TRIS = 0;
   SPI_SCK_PIN = 0;
}



BYTE XFER_SPI(BYTE output)
{
   char BitCount;
   static char input;
   BitCount = 8;
   input = output;

   // SCK idles low
   // Data output after falling edge of SCK
   // Data sampled before rising edge of SCK
   if(input&0x80)
      SPI_DOUT_PIN = 1;
   else
      SPI_DOUT_PIN = 0;                // Set Dout to MSB of data
   Nop();                          // Adjust for jitter
   Nop();
   do                              // Loop 8 times
   {
      STATUSbits.C = 0;       // Set the carry bit according
      if(SPI_DIN_PIN)          // to the Din pin
         STATUSbits.C = 1;
      SPI_SCK_PIN = 1;         // Set the SCK pin
      _asm
         rlcf  input,1,1
         _endasm
         //              Rlcf(input);            // Rotate the carry into the data byte
         Nop();                  // Produces a 50% duty cycle clock
      Nop();
      Nop();
      Nop();
      Nop();
      Nop();
      Nop();
      Nop();
      Nop();
      Nop();
      Nop();
      SPI_SCK_PIN = 0;         // Clear the SCK pin
      if(input&0x80)          // the MSB of data
         SPI_DOUT_PIN = 1;
      else
         SPI_DOUT_PIN = 0;        // Set Dout to the next bit according to

      BitCount--;             // Count iterations through loop
   } while(BitCount);

   return input;
}



/*--------------------------------------------------------------------------

Module Private Functions

---------------------------------------------------------------------------*/

BYTE CardType;


/*-----------------------------------------------------------------------*/
/* Deselect the card and release SPI bus                                 */
/*-----------------------------------------------------------------------*/

static
void release_spi (void)
{
   DESELECT();
   XFER_SPI(0xff);
}


/*-----------------------------------------------------------------------*/
/* Send a command packet to MMC                                          */
/*-----------------------------------------------------------------------*/

static
BYTE send_cmd (
               BYTE cmd,      /* Command byte */
               DWORD arg      /* Argument */
               )
{
   BYTE n, res;

   union
   {
      BYTE b[4];
      DWORD d;
   }
   argbroke;
   argbroke.d = arg;

   if (cmd & 0x80) { /* ACMD<n> is the command sequense of CMD55-CMD<n> */
      cmd &= 0x7F;
      res = send_cmd(CMD55, 0);
      if (res > 1) return res;
   }

   /* Select the card */
   DESELECT();
   XFER_SPI(0xff);
   SELECT();
   XFER_SPI(0xff);

   /* Send a command packet */
   XFER_SPI(cmd);                /* Start + Command index */
   XFER_SPI(argbroke.b[3]);            /* Argument[31..24] */
   XFER_SPI(argbroke.b[2]);      /* Argument[23..16] */
   XFER_SPI(argbroke.b[1]);         /* Argument[15..8] */
   XFER_SPI(argbroke.b[0]);            /* Argument[7..0] */
   n = 0x01;                     /* Dummy CRC + Stop */
   if (cmd == CMD0) n = 0x95;       /* Valid CRC for CMD0(0) */
   if (cmd == CMD8) n = 0x87;       /* Valid CRC for CMD8(0x1AA) */
   XFER_SPI(n);

   /* Receive a command response */
   n = 10;                       /* Wait for a valid response in timeout of 10 attempts */
   do {
      res = XFER_SPI(0xff);
   } while ((res & 0x80) && --n);

   return res;       /* Return with the response value */
}



/*--------------------------------------------------------------------------

Public Functions

---------------------------------------------------------------------------*/


/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (void)
{
   BYTE n, cmd, ty, ocr[4];
   WORD tmr;

   GREENLEDON();

   INIT_SPI();

#if _WRITE_FUNC
   if (MMC_SEL) disk_writep(0, 0);     /* Finalize write process if it is in progress */
#endif
   for (n = 11; n; --n) XFER_SPI(0xff);   /* Dummy clocks */

   ty = 0;
   if (send_cmd(CMD0, 0) == 1)
   {
      /* Enter Idle state */
      if (send_cmd(CMD8, 0x1AA) == 1)
      {  /* SDv2 */
         for (n = 0; n < 4; n++)
         {
            ocr[n] = XFER_SPI(0xff);      /* Get trailing return value of R7 resp */
         }
         if (ocr[2] == 0x01 && ocr[3] == 0xAA)
         {
            /* The card can work at vdd range of 2.7-3.6V */
            for (tmr = 12000; tmr && send_cmd(ACMD41, 1UL << 30); tmr--) ; /* Wait for leaving idle state (ACMD41 with HCS bit) */

            if (tmr && send_cmd(CMD58, 0) == 0)
            {
               /* Check CCS bit in the OCR */
               for (n = 0; n < 4; n++) ocr[n] = XFER_SPI(0xff);
               ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2; /* SDv2 (HC or SC) */
            }
         }
      }
      else
      {                    /* SDv1 or MMCv3 */
         if (send_cmd(ACMD41, 0) <= 1)
         {
            ty = CT_SD1; cmd = ACMD41; /* SDv1 */
         }
         else
         {
            ty = CT_MMC; cmd = CMD1;   /* MMCv3 */
         }
         for (tmr = 25000; tmr && send_cmd(cmd, 0); tmr--) ;   /* Wait for leaving idle state */

         if (!tmr || send_cmd(CMD16, 512) != 0)       /* Set R/W block length to 512 */
         {
            ty = 0;
         }
      }
   }
   CardType = ty;
   release_spi();

   GREENLEDOFF();

   return ty ? 0 : STA_NOINIT;
}



/*-----------------------------------------------------------------------*/
/* Read partial sector                                                   */
/*-----------------------------------------------------------------------*/

DRESULT disk_readp (
                    BYTE *buff,    /* Pointer to the read buffer (NULL:Read bytes are forwarded to the stream) */
                    DWORD lba,     /* Sector number (LBA) */
                    WORD ofs,      /* Byte offset to read from (0..511) */
                    WORD cnt    /* Number of bytes to read (ofs + cnt mus be <= 512) */
                    )
{
   DRESULT res;
   BYTE rc;
   WORD bc;

   GREENLEDON();

   if (!(CardType & CT_BLOCK)) lba *= 512;      /* Convert to byte address if needed */

   res = RES_ERROR;
   if (send_cmd(CMD17, lba) == 0) {    /* READ_SINGLE_BLOCK */

      bc = 30000;
      do {                    /* Wait for data packet in timeout of 100ms */
         rc = XFER_SPI(0xff);
      } while (rc == 0xFF && --bc);

      if (rc == 0xFE) {          /* A data packet arrived */
         bc = 514 - ofs - cnt;

         /* Skip leading bytes */
         if (ofs) {
            do XFER_SPI(0xff); while (--ofs);
         }

         /* Receive a part of the sector */
         if (buff) { /* Store data to the memory */
            do
            *buff++ = XFER_SPI(0xff);
            while (--cnt);
         } /*else {  // Forward data to the outgoing stream (depends on the project)
           do
           xmit(XFER_SPI(0xff));   // (Console output)
           while (--cnt);
           }*/

         /* Skip trailing bytes and CRC */
         do XFER_SPI(0xff); while (--bc);

         res = RES_OK;
      }
   }

   release_spi();

   GREENLEDOFF();

   return res;
}



/*-----------------------------------------------------------------------*/
/* Write partial sector                                                  */
/*-----------------------------------------------------------------------*/

DRESULT disk_writep (
                     BYTE *buff, /* Pointer to the bytes to be written (NULL:Initiate/Finalize sector write) */
                     DWORD sa       /* Number of bytes to send, Sector number (LBA) or zero */
                     )
{
   DRESULT res;
   WORD bc;
   static WORD wc;

   GREENLEDON();

   res = RES_ERROR;

   if (buff) {    /* Send data bytes */
      bc = (WORD)sa;
      while (bc && wc) {      /* Send data bytes to the card */
         XFER_SPI(*buff++);
         wc--; bc--;
      }
      res = RES_OK;
   } else {
      if (sa) {   /* Initiate sector write process */
         if (!(CardType & CT_BLOCK)) sa *= 512; /* Convert to byte address if needed */
         if (send_cmd(CMD24, sa) == 0) {        /* WRITE_SINGLE_BLOCK */
            XFER_SPI(0xFF); XFER_SPI(0xFE);     /* Data block header */
            wc = 512;                     /* Set byte counter */
            res = RES_OK;
         }
      } else { /* Finalize sector write process */
         bc = wc + 2;
         while (bc--) XFER_SPI(0);  /* Fill left bytes and CRC with zeros */
         if ((XFER_SPI(0xff) & 0x1F) == 0x05) { /* Receive data resp and wait for end of write process in timeout of 300ms */
            for (bc = 65000; XFER_SPI(0xff) != 0xFF && bc; bc--) ;   /* Wait ready */
            if (bc) res = RES_OK;
         }
         release_spi();
      }
   }

   GREENLEDOFF();

   return res;
}

