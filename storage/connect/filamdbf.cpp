/*********** File AM Dbf C++ Program Source Code File (.CPP) ****************/
/* PROGRAM NAME: FILAMDBF                                                   */
/* -------------                                                            */
/*  Version 1.6                                                             */
/*                                                                          */
/* COPYRIGHT:                                                               */
/* ----------                                                               */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2013         */
/*                                                                          */
/* WHAT THIS PROGRAM DOES:                                                  */
/* -----------------------                                                  */
/*  This program are the DBF file access method classes.                    */
/*                                                                          */
/* ACKNOWLEDGEMENT:                                                         */
/* ----------------                                                         */
/*  Somerset Data Systems, Inc.  (908) 766-5845                             */
/*  Version 1.2     April 6, 1991                                           */
/*  Programmer:     Jay Parsons                                             */
/****************************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
#include <io.h>
#include <fcntl.h>
//#include <errno.h>
//#include <windows.h>
#else   // !WIN32
#if defined(UNIX)
#include <errno.h>
#include <unistd.h>
#else   // !UNIX
//#include <io.h>
#endif  // !UNIX
//#include <fcntl.h>
#endif  // !WIN32
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tabdos.h    is header containing the TABDOS class declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
//#include "catalog.h"
//#include "kindex.h"
#include "filamdbf.h"
#include "tabdos.h"
#include "valblk.h"
#define  NO_FUNC
#include "plgcnx.h"                       // For DB types
#include "resource.h"

/****************************************************************************/
/*  Definitions.                                                            */
/****************************************************************************/
#define HEADLEN       32            /* sizeof ( mainhead or thisfield )     */
//efine MEMOLEN       10            /* length of memo field in .dbf         */
#define DBFTYPE        3            /* value of bits 0 and 1 if .dbf        */
#define EOH         0x0D            /* end-of-header marker in .dbf file    */

/****************************************************************************/
/*  Catalog utility function.                                               */
/****************************************************************************/
PQRYRES PlgAllocResult(PGLOBAL, int, int, int, int *, int *,
                unsigned int *, bool blank = true, bool nonull = false);
bool    PushWarning(PGLOBAL, PTDBASE);

extern "C" int trace;	         		 // The general trace value  

/****************************************************************************/
/*  First 32 bytes of a .dbf file.                                          */
/*  Note: some reserved fields are used here to store info (Fields)         */
/****************************************************************************/
typedef struct _dbfheader {
//uchar  Dbf   :2;                  /*  both 1 for dBASE III or IV .dbf     */
//uchar        :1;
//uchar  Db4dbt:1;                  /*  1 if a dBASE IV-type .dbt exists    */
//uchar  Dbfox :4;                  /*  FoxPro if equal to 3                */
  uchar  Version;                   /*  Version information flags           */
  char   Filedate[3];               /*  date, YYMMDD, binary. YY=year-1900  */
  uint   Records;                   /*  records in the file                 */
  ushort Headlen;                   /*  bytes in the header                 */
  ushort Reclen;                    /*  bytes in a record                   */
  ushort Fields;                    /*  Reserved but used to store fields   */
  char   Incompleteflag;            /*  01 if incomplete, else 00           */
  char   Encryptflag;               /*  01 if encrypted, else 00            */
  char   Reserved2[12];             /*  for LAN use                         */
  char   Mdxflag;                   /*  01 if production .mdx, else 00      */
  char   Language;                  /*  Codepage                            */
  char   Reserved3[2];
  } DBFHEADER;

/****************************************************************************/
/*  Column field descriptor of a .dbf file.                                 */
/****************************************************************************/
typedef struct _descriptor {
  char  Name[11];                   /*  field name, in capitals, null filled*/
  char  Type;                       /*  field type, C, D, F, L, M or N      */
  uint  Offset;                     /*  used in memvars, not in files.      */
  uchar Length;                     /*  field length                        */
  uchar Decimals;                   /*  number of decimal places            */
  short Reserved4;
  char  Workarea;                   /*  ???                                 */
  char  Reserved5[2];
  char  Setfield;                   /*  ???                                 */
  char  Reserved6[7];
  char  Mdxfield;                   /* 01 if tag field in production .mdx   */
  } DESCRIPTOR;

/****************************************************************************/
/*  dbfhead: Routine to analyze a .dbf header.                              */
/*  Parameters:                                                             */
/*      PGLOBAL g       -- pointer to the Plug Global structure             */
/*      FILE *file      -- pointer to file to analyze                       */
/*      PSZ   fn        -- pathname of the file to analyze                  */
/*      DBFHEADER *buf  -- pointer to _dbfheader structure                  */
/*  Returns:                                                                */
/*      RC_OK, RC_NF, RC_INFO, or RC_FX if error.                           */
/*  Side effects:                                                           */
/*      Moves file pointer to byte 32; fills buffer at buf with             */
/*  first 32 bytes of file.                                                 */
/****************************************************************************/
static int dbfhead(PGLOBAL g, FILE *file, PSZ fn, DBFHEADER *buf)
  {
  char endmark[2];
  int  dbc = 2, rc = RC_OK;

  *g->Message = '\0';

  // Read the first 32 bytes into buffer
  if (fread(buf, HEADLEN, 1, file) != 1) {
    strcpy(g->Message, MSG(NO_READ_32));
    return RC_NF;
    } // endif fread

  // Check first byte to be sure of .dbf type
  if ((buf->Version & 0x03) != DBFTYPE) {
    strcpy(g->Message, MSG(NOT_A_DBF_FILE));
    rc = RC_INFO;

    if ((buf->Version & 0x30) == 0x30) {
      strcpy(g->Message, MSG(FOXPRO_FILE));
      dbc = 264;             // FoxPro database container
      } // endif Version

  } else
    strcpy(g->Message, MSG(DBASE_FILE));

  // Check last byte(s) of header
  if (fseek(file, buf->Headlen - dbc, SEEK_SET) != 0) {
    sprintf(g->Message, MSG(BAD_HEADER), fn);
    return RC_FX;
    } // endif fseek

  if (fread(&endmark, 2, 1, file) != 1) {
    strcpy(g->Message, MSG(BAD_HEAD_END));
    return RC_FX;
    } // endif fread

  // Some files have just 1D others have 1D00 following fields
  if (endmark[0] != EOH && endmark[1] != EOH) {
    sprintf(g->Message, MSG(NO_0DH_HEAD), dbc);

    if (rc == RC_OK)
      return RC_FX;

    } // endif endmark

  // Calculate here the number of fields while we have the dbc info
  buf->Fields = (buf->Headlen - dbc - 1) / 32;
  fseek(file, HEADLEN, SEEK_SET);
  return rc;
  } // end of dbfhead

/* -------------------------- Function DBFColumns ------------------------- */

/****************************************************************************/
/*  DBFColumns: constructs the result blocks containing the description     */
/*  of all the columns of a DBF file that will be retrieved by #GetData.    */
/****************************************************************************/
PQRYRES DBFColumns(PGLOBAL g, char *fn, BOOL info)
  {
  static int dbtype[] = {DB_CHAR,  DB_SHORT, DB_CHAR,
                         DB_INT,  DB_INT,  DB_SHORT};
  static int buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                         TYPE_INT,   TYPE_INT, TYPE_SHORT};
  static unsigned int length[] = {11, 6, 8, 10, 10, 6};
  char       buf[2], filename[_MAX_PATH];
  int        ncol = sizeof(dbtype) / sizeof(int);
  int        rc, type, len, field, fields;
  BOOL       bad;
  DBFHEADER  mainhead;
  DESCRIPTOR thisfield;
  FILE      *infile;
  PQRYRES    qrp;
  PCOLRES    crp;

	if (trace)
		htrc("DBFColumns: File %s\n", SVP(fn));

  if (!fn) {
    strcpy(g->Message, MSG(MISSING_FNAME));
    return NULL;
    } // endif fn

  /**************************************************************************/
  /*  Open the input file.                                                  */
  /**************************************************************************/
  PlugSetPath(filename, fn, PlgGetDataPath(g));

  if (!(infile= global_fopen(g, MSGID_CANNOT_OPEN, filename, "rb")))
    return NULL;

  /**************************************************************************/
  /*  Get the first 32 bytes of the header.                                 */
  /**************************************************************************/
  if ((rc = dbfhead(g, infile, filename, &mainhead)) == RC_FX) {
    fclose(infile);
    return NULL;
    } // endif dbfhead

  /**************************************************************************/
  /*  Allocate the structures used to refer to the result set.              */
  /**************************************************************************/
//fields = (mainhead.Headlen - 33) / 32;
  fields = mainhead.Fields;
  qrp = PlgAllocResult(g, ncol, fields, IDS_COLUMNS + 3,
                                        dbtype, buftyp, length);
  qrp->Info = info || (rc == RC_INFO);

	if (trace) {
		htrc("Structure of %s\n", filename);
		htrc("headlen=%hd reclen=%hd degree=%d\n",
					mainhead.Headlen, mainhead.Reclen, fields);
		htrc("flags(iem)=%d,%d,%d cp=%d\n", mainhead.Incompleteflag,
					mainhead.Encryptflag, mainhead.Mdxflag, mainhead.Language);
		htrc("%hd records, last changed %02d/%02d/%d\n",
					mainhead.Records, mainhead.Filedate[1], mainhead.Filedate[2],
					mainhead.Filedate[0] + (mainhead.Filedate[0] <= 30) ? 2000 : 1900);
		htrc("Field    Type  Offset  Len  Dec  Set  Mdx\n");
		} // endif trace

  buf[1] = '\0';

  /**************************************************************************/
  /*  Do it field by field.  We are at byte 32 of file.                     */
  /**************************************************************************/
  for (field = 0; field < fields; field++) {
    bad = FALSE;

    if (fread(&thisfield, HEADLEN, 1, infile) != 1) {
      sprintf(g->Message, MSG(ERR_READING_REC), field+1, fn);
      goto err;
    } else
      len = thisfield.Length;

		if (trace)
			htrc("%-11s %c  %6ld  %3d   %2d  %3d  %3d\n",
					 thisfield.Name, thisfield.Type, thisfield.Offset, len,
					 thisfield.Decimals, thisfield.Setfield, thisfield.Mdxfield);

    /************************************************************************/
    /*  Now get the results into blocks.                                    */
    /************************************************************************/
    switch (thisfield.Type) {
      case 'C':                      // Characters
      case 'L':                      // Logical 'T' or 'F'
        type = TYPE_STRING;
        break;
      case 'N':
        type = (thisfield.Decimals) ? TYPE_FLOAT
             : (len > 10) ? TYPE_BIGINT : TYPE_INT;
        break;
      case 'F':
        type = TYPE_FLOAT;
        break;
      case 'D':
        type = TYPE_DATE;            // Is this correct ???
        break;
      default:
        if (!info) {
          sprintf(g->Message, MSG(BAD_DBF_TYPE), thisfield.Type);
          goto err;
          } // endif info

        type = TYPE_ERROR;
        bad = TRUE;
      } // endswitch Type

    crp = qrp->Colresp;                    // Column Name
    crp->Kdata->SetValue(thisfield.Name, field);
    crp = crp->Next;                       // Data Type
    crp->Kdata->SetValue((int)type, field);
    crp = crp->Next;                       // Type Name

    if (bad) {
      buf[0] = thisfield.Type;
      crp->Kdata->SetValue(buf, field);
    } else
      crp->Kdata->SetValue(GetTypeName(type), field);

    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue((int)thisfield.Length, field);
    crp = crp->Next;                       // Length
    crp->Kdata->SetValue((int)thisfield.Length, field);
    crp = crp->Next;                       // Scale (precision)
    crp->Kdata->SetValue((int)thisfield.Decimals, field);
    } // endfor field

  qrp->Nblin = field;
  fclose(infile);

  if (info) {
    /************************************************************************/
    /*  Prepare return message for dbfinfo command.                         */
    /************************************************************************/
    char buf[64];

    sprintf(buf,
      "Ver=%02x ncol=%hu nlin=%u lrecl=%hu headlen=%hu date=%02d/%02d/%02d",
      mainhead.Version, fields, mainhead.Records, mainhead.Reclen,
      mainhead.Headlen, mainhead.Filedate[0], mainhead.Filedate[1],
      mainhead.Filedate[2]);

    strcat(g->Message, buf);
    } // endif info

  /**************************************************************************/
  /*  Return the result pointer for use by GetData routines.                */
  /**************************************************************************/
  return qrp;

 err:
  fclose(infile);
  return NULL;
  } // end of DBFColumns

/* ---------------------------- Class DBFBASE ----------------------------- */

/****************************************************************************/
/*  Constructors.                                                           */
/****************************************************************************/
DBFBASE::DBFBASE(PDOSDEF tdp)
  {
  Records = 0;
  Nerr = 0;
  Maxerr = tdp->Maxerr;
  Accept = tdp->Accept;
  ReadMode = tdp->ReadMode;
  } // end of DBFBASE standard constructor

DBFBASE::DBFBASE(DBFBASE *txfp)
  {
  Records = txfp->Records;
  Nerr = txfp->Nerr;
  Maxerr = txfp->Maxerr;
  Accept = txfp->Accept;
  ReadMode = txfp->ReadMode;
  } // end of DBFBASE copy constructor

/****************************************************************************/
/*  ScanHeader: scan the DBF file header for number of records, record size,*/
/*  and header length. Set Records, check that Reclen is equal to lrecl and */
/*  return the header length or 0 in case of error.                         */
/****************************************************************************/
int DBFBASE::ScanHeader(PGLOBAL g, PSZ fname, int lrecl, char *defpath)
  {
  int       rc;
  char      filename[_MAX_PATH];
  DBFHEADER header;
  FILE     *infile;

  /************************************************************************/
  /*  Open the input file.                                                */
  /************************************************************************/
  PlugSetPath(filename, fname, defpath);

  if (!(infile= global_fopen(g, MSGID_CANNOT_OPEN, filename, "rb")))
    return 0;              // Assume file does not exist

  /************************************************************************/
  /*  Get the first 32 bytes of the header.                               */
  /************************************************************************/
  rc = dbfhead(g, infile, filename, &header);
  fclose(infile);

  if (rc == RC_NF) {
    Records = 0;
    return 0;
  } else if (rc == RC_FX)
    return -1;

  if ((int)header.Reclen != lrecl) {
    sprintf(g->Message, MSG(BAD_LRECL), lrecl, header.Reclen);
    return -1;
    } // endif Lrecl

  Records = (int)header.Records;
  return (int)header.Headlen;
  } // end of ScanHeader

/* ---------------------------- Class DBFFAM ------------------------------ */

/****************************************************************************/
/*  Cardinality: returns table cardinality in number of rows.               */
/*  This function can be called with a null argument to test the            */
/*  availability of Cardinality implementation (1 yes, 0 no).               */
/****************************************************************************/
int DBFFAM::Cardinality(PGLOBAL g)
  {
  if (!g)
    return 1;

  if (!Headlen)
    if ((Headlen = ScanHeader(g, To_File, Lrecl, Tdbp->GetPath())) < 0)
      return -1;                // Error in ScanHeader

  // Set number of blocks for later use
  Block = (Records > 0) ? (Records + Nrec - 1) / Nrec : 0;
  return Records;
  } // end of Cardinality

#if 0      // Not compatible with ROWID block optimization
/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int DBFFAM::GetRowID(void)
  {
  return Rows;
  } // end of GetRowID
#endif

/***********************************************************************/
/*  OpenTableFile: Open a DBF table file using C standard I/Os.        */
/*  Binary mode cannot be used on Insert because of EOF (CTRL+Z) char. */
/***********************************************************************/
bool DBFFAM::OpenTableFile(PGLOBAL g)
  {
  char    opmode[4], filename[_MAX_PATH];
//int     ftype = Tdbp->GetFtype();
  MODE    mode = Tdbp->GetMode();
  PDBUSER dbuserp = PlgGetUser(g);

  switch (mode) {
    case MODE_READ:
      strcpy(opmode, "rb");
      break;
    case MODE_DELETE:
      if (!Tdbp->GetNext()) {
        // Store the number of deleted lines
        DelRows = -1;      // Means all lines deleted
//      DelRows = Cardinality(g); no good because of soft deleted lines

        // This will erase the entire file
        strcpy(opmode, "w");
        Tdbp->ResetSize();
        Records = 0;
        break;
        } // endif

      // Selective delete, pass thru
    case MODE_UPDATE:
      UseTemp = Tdbp->IsUsingTemp(g);
      strcpy(opmode, (UseTemp) ? "rb" : "r+b");
      break;
    case MODE_INSERT:
      // Must be in text mode to remove an eventual EOF character
      strcpy(opmode, "a+");
      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch Mode

  // Now open the file stream
  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (!(Stream = PlugOpenFile(g, filename, opmode))) {
#ifdef DEBTRACE
 htrc("%s\n", g->Message);
#endif
		return (errno == ENOENT) ? PushWarning(g, Tdbp) : true;
    } // endif Stream

#ifdef DEBTRACE
 htrc("File %s is open in mode %s\n", filename, opmode);
#endif

  To_Fb = dbuserp->Openlist;     // Keep track of File block

  /*********************************************************************/
  /*  Allocate the line buffer. For mode Delete a bigger buffer has to */
  /*  be allocated because is it also used to move lines into the file.*/
  /*********************************************************************/
  return AllocateBuffer(g);
  } // end of OpenTableFile

/****************************************************************************/
/*  Allocate the block buffer for the table.                                */
/****************************************************************************/
bool DBFFAM::AllocateBuffer(PGLOBAL g)
  {
  char c;
  int  rc;
  MODE mode = Tdbp->GetMode();

  Buflen = Blksize;
  To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);

  if (mode == MODE_INSERT) {
#if defined(WIN32)
    /************************************************************************/
    /*  Now we can revert to binary mode in particular because the eventual */
    /*  writing of a new header must be done in binary mode to avoid        */
    /*  translating 0A bytes (LF) into 0D0A (CRLF) by Windows in text mode. */
    /************************************************************************/
    if (_setmode(_fileno(Stream), _O_BINARY) == -1) {
      sprintf(g->Message, MSG(BIN_MODE_FAIL), strerror(errno));
      return true;
      } // endif setmode
#endif   // WIN32

    /************************************************************************/
    /*  If this is a new file, the header must be generated.                */
    /************************************************************************/
    int len = GetFileLength(g);

    if (!len) {
      // Make the header for this DBF table file
      struct tm  *datm;
      int         hlen, n = 0, reclen = 1;
      time_t      t;
      DBFHEADER  *header;
      DESCRIPTOR *descp;
      PCOLDEF     cdp;
      PDOSDEF     tdp = (PDOSDEF)Tdbp->GetDef();

      // Count the number of columns
      for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext()) {
        reclen += cdp->GetLong();
        n++;
        } // endfor cdp

      if (Lrecl != reclen) {
        sprintf(g->Message, MSG(BAD_LRECL), Lrecl, reclen);
        return true;
        } // endif Lrecl

      hlen = HEADLEN * (n + 1) + 2;
      header = (DBFHEADER*)PlugSubAlloc(g, NULL, hlen);
      memset(header, 0, hlen);
      header->Version = DBFTYPE;
      t = time(NULL) - (time_t)DTVAL::GetShift();
      datm = gmtime(&t);
      header->Filedate[0] = datm->tm_year - 100;
      header->Filedate[1] = datm->tm_mon + 1;
      header->Filedate[2] = datm->tm_mday;
      header->Headlen = (ushort)hlen;
      header->Reclen = (ushort)reclen;
      descp = (DESCRIPTOR*)header;

      // Currently only standard Xbase types are supported
      for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext()) {
        descp++;

        switch ((c = *GetFormatType(cdp->GetType()))) {
          case 'S':           // Short integer
          case 'L':           // Large (big) integer
            c = 'N';          // Numeric
          case 'N':           // Numeric (integer)
          case 'F':           // Float (double)
            descp->Decimals = (uchar)cdp->F.Prec;
          case 'C':           // Char
          case 'D':           // Date
            break;
          default:            // Should never happen
            sprintf(g->Message, "Unsupported DBF type %c for column %s",
                                c, cdp->GetName());
            return true;
          } // endswitch c

        strncpy(descp->Name, cdp->GetName(), 11);
        descp->Type = c;
        descp->Length = (uchar)cdp->GetLong();
        } // endfor cdp

      *(char*)(++descp) = EOH;

      //  Now write the header
      if (fwrite(header, 1, hlen, Stream) != (unsigned)hlen) {
        sprintf(g->Message, MSG(FWRITE_ERROR), strerror(errno));
        return true;
        } // endif fwrite

      Records = 0;
      Headlen = hlen;
    } else if (len < 0)
      return true;            // Error in GetFileLength

    /************************************************************************/
    /*  For Insert the buffer must be prepared.                             */
    /************************************************************************/
    memset(To_Buf, ' ', Buflen);
    Rbuf = Nrec;                     // To be used by WriteDB
  } else if (UseTemp) {
    // Allocate a separate buffer so block reading can be kept
    Dbflen = Nrec;
    DelBuf = PlugSubAlloc(g, NULL, Blksize);
  } // endif's

  if (!Headlen) {
    /************************************************************************/
    /*  Here is a good place to process the DBF file header                 */
    /************************************************************************/
    DBFHEADER header;

    if ((rc = dbfhead(g, Stream, Tdbp->GetFile(g), &header)) == RC_OK) {
      if (Lrecl != (int)header.Reclen) {
        sprintf(g->Message, MSG(BAD_LRECL), Lrecl, header.Reclen);
        return true;
        } // endif Lrecl

      Records = (int)header.Records;
      Headlen = (int)header.Headlen;
    } else if (rc == RC_NF) {
      Records = 0;
      Headlen = 0;
    } else              // RC_FX
      return true;                  // Error in dbfhead

    } // endif Headlen

  /**************************************************************************/
  /*  Position the file at the begining of the data.                        */
  /**************************************************************************/
  if (Tdbp->GetMode() == MODE_INSERT)
    rc = fseek(Stream, 0, SEEK_END);
  else
    rc = fseek(Stream, Headlen, SEEK_SET);

  if (rc) {
    sprintf(g->Message, MSG(BAD_DBF_FILE), Tdbp->GetFile(g));
    return true;
    } // endif fseek

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  Reset buffer access according to indexing and to mode.             */
/*  >>>>>>>>>>>>>> TO BE RE-VISITED AND CHECKED <<<<<<<<<<<<<<<<<<<<<< */
/***********************************************************************/
void DBFFAM::ResetBuffer(PGLOBAL g)
  {
  /*********************************************************************/
  /*  If access is random, performances can be much better when the    */
  /*  reads are done on only one row, except for small tables that can */
  /*  be entirely read in one block. If the index is just used as a    */
  /*  bitmap filter, as for Update or delete, reading will be          */
  /*  sequential and we better keep block reading.                     */
  /*********************************************************************/
  if (Tdbp->GetKindex() && Tdbp->GetMode() == MODE_READ &&
      ReadBlks != 1) {
    Nrec = 1;                       // Better for random access
    Rbuf = 0;
    Blksize = Lrecl;
    OldBlk = -2;                    // Has no meaning anymore
    Block = Tdbp->Cardinality(g);   // Blocks are one line now
    } // endif Mode

  } // end of ResetBuffer

/***********************************************************************/
/*  ReadBuffer: Read one line for a DBF file.                          */
/***********************************************************************/
int DBFFAM::ReadBuffer(PGLOBAL g)
  {
  if (!Placed && !Closing && GetRowID() == Records)
    return RC_EF;

  int rc = FIXFAM::ReadBuffer(g);

  if (rc != RC_OK || Closing)
    return rc;

  switch (*Tdbp->GetLine()) {
    case '*':
      if (!ReadMode)
        rc = RC_NF;                      // Deleted line
      else
        Rows++;

      break;
    case ' ':
      if (ReadMode < 2)
        Rows++;                          // Non deleted line
      else
        rc = RC_NF;

      break;
    default:
      if (++Nerr >= Maxerr && !Accept) {
        sprintf(g->Message, MSG(BAD_DBF_REC), Tdbp->GetFile(g), GetRowID());
        rc = RC_FX;
      } else
        rc = (Accept) ? RC_OK : RC_NF;

    } // endswitch To_Buf

  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  Copy the header into the temporary file.                           */
/***********************************************************************/
bool DBFFAM::CopyHeader(PGLOBAL g)
  {
  bool rc = true;

  if (Headlen) {
    void  *hdr = PlugSubAlloc(g, NULL, Headlen);
    size_t n, hlen = (size_t)Headlen;
    int   pos = ftell(Stream);

    if (fseek(Stream, 0, SEEK_SET))
      strcpy(g->Message, "Seek error in CopyHeader");
    else if ((n = fread(hdr, 1, hlen, Stream)) != hlen)
      sprintf(g->Message, MSG(BAD_READ_NUMBER), n, To_File);
    else if ((n = fwrite(hdr, 1, hlen, T_Stream)) != hlen)
      sprintf(g->Message, MSG(WRITE_STRERROR), To_Fbt->Fname
                                             , strerror(errno));
    else if (fseek(Stream, pos, SEEK_SET))
      strcpy(g->Message, "Seek error in CopyHeader");
    else
      rc = false;

  } else
    rc = false;

  return rc;
  } // end of CopyHeader

/***********************************************************************/
/*  Data Base delete line routine for DBF access methods.              */
/*  Deleted lines are just flagged in the first buffer character.      */
/***********************************************************************/
int DBFFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  if (irc == RC_OK) {
    // T_Stream is the temporary stream or the table file stream itself
    if (!T_Stream)
      if (UseTemp) {
        if (OpenTempFile(g))
          return RC_FX;

        if (CopyHeader(g))           // For DBF tables
          return RC_FX;

      } else
        T_Stream = Stream;

    *Tdbp->GetLine() = '*';
    Modif++;                         // Modified line in Delete mode
    } // endif irc

  return RC_OK;
  } // end of DeleteRecords

/***********************************************************************/
/*  Rewind routine for DBF access method.                              */
/***********************************************************************/
void DBFFAM::Rewind(void)
  {
  BLKFAM::Rewind();
  Nerr = 0;
  } // end of Rewind

/***********************************************************************/
/*  Table file close routine for DBF access method.                    */
/***********************************************************************/
void DBFFAM::CloseTableFile(PGLOBAL g)
  {
  int rc = RC_OK, wrc = RC_OK;
  MODE mode = Tdbp->GetMode();

  // Closing is True if last Write was in error
  if (mode == MODE_INSERT && CurNum && !Closing) {
    // Some more inserted lines remain to be written
    Rbuf = CurNum--;
//  Closing = true;
    wrc = WriteBuffer(g);
  } else if (mode == MODE_UPDATE || mode == MODE_DELETE) {
    if (Modif && !Closing) {
      // Last updated block remains to be written
      Closing = true;
      wrc = ReadBuffer(g);
      } // endif Modif

    if (UseTemp && T_Stream && wrc == RC_OK) {
      // Copy any remaining lines
      bool b;

      Fpos = Tdbp->Cardinality(g);

      if ((rc = MoveIntermediateLines(g, &b)) == RC_OK) {
        // Delete the old file and rename the new temp file.
        RenameTempFile(g);
        goto fin;
        } // endif rc

      } // endif UseTemp

  } // endif's mode

  if (Tdbp->GetMode() == MODE_INSERT) {
    int n = ftell(Stream) - Headlen;

    rc = PlugCloseFile(g, To_Fb);

    if (n >= 0 && !(n % Lrecl)) {
      n /= Lrecl;                       // New number of lines

      if (n > Records) {
        // Update the number of rows in the file header
        char filename[_MAX_PATH];

        PlugSetPath(filename, To_File, Tdbp->GetPath());
        if ((Stream= global_fopen(g, MSGID_OPEN_MODE_STRERROR, filename, "r+b")))
        {
          fseek(Stream, 4, SEEK_SET);     // Get header.Records position
          fwrite(&n, sizeof(int), 1, Stream);
          fclose(Stream);
          Stream= NULL;
          Records= n;                    // Update Records value
        }
      } // endif n

    } // endif n

  } else  // Finally close the file
    rc = PlugCloseFile(g, To_Fb);

 fin:
#ifdef DEBTRACE
 htrc("DBF CloseTableFile: closing %s mode=%d wrc=%d rc=%d\n",
  To_File, mode, wrc, rc);
#endif
  Stream = NULL;           // So we can know whether table is open
  } // end of CloseTableFile

/* ---------------------------- Class DBMFAM ------------------------------ */

/****************************************************************************/
/*  Cardinality: returns table cardinality in number of rows.               */
/*  This function can be called with a null argument to test the            */
/*  availability of Cardinality implementation (1 yes, 0 no).               */
/****************************************************************************/
int DBMFAM::Cardinality(PGLOBAL g)
  {
  if (!g)
    return 1;

  if (!Headlen)
    if ((Headlen = ScanHeader(g, To_File, Lrecl, Tdbp->GetPath())) < 0)
      return -1;                // Error in ScanHeader

  // Set number of blocks for later use
  Block = (Records > 0) ? (Records + Nrec - 1) / Nrec : 0;
  return Records;
  } // end of Cardinality

#if 0      // Not compatible with ROWID block optimization
/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int DBMFAM::GetRowID(void)
  {
  return Rows;
  } // end of GetRowID
#endif

/***********************************************************************/
/*  Just check that on all deletion the unknown deleted line number is */
/*  sent back because Cardinality doesn't count soft deleted lines.    */
/***********************************************************************/
int DBMFAM::GetDelRows(void)
  {
  if (Tdbp->GetMode() == MODE_DELETE && !Tdbp->GetNext())
    return -1;                 // Means all lines deleted
  else
    return DelRows;

  } // end of GetDelRows

/****************************************************************************/
/*  Allocate the block buffer for the table.                                */
/****************************************************************************/
bool DBMFAM::AllocateBuffer(PGLOBAL g)
  {
  if (!Headlen) {
    /************************************************************************/
    /*  Here is a good place to process the DBF file header                 */
    /************************************************************************/
    DBFHEADER *hp = (DBFHEADER*)Memory;

    if (Lrecl != (int)hp->Reclen) {
      sprintf(g->Message, MSG(BAD_LRECL), Lrecl, hp->Reclen);
      return true;
      } // endif Lrecl

    Records = (int)hp->Records;
    Headlen = (int)hp->Headlen;
    } // endif Headlen

  /**************************************************************************/
  /*  Position the file at the begining of the data.                        */
  /**************************************************************************/
  Fpos = Mempos = Memory + Headlen;
  Top--;                               // Because of EOF marker
  return false;
  } // end of AllocateBuffer

/****************************************************************************/
/*  ReadBuffer: Read one line for a FIX file.                               */
/****************************************************************************/
int DBMFAM::ReadBuffer(PGLOBAL g)
  {
//  if (!Placed && GetRowID() == Records)
//    return RC_EF;

  int rc = MPXFAM::ReadBuffer(g);

  if (rc != RC_OK)
    return rc;

  switch (*Fpos) {
    case '*':
      if (!ReadMode)
        rc = RC_NF;                      // Deleted line
      else
        Rows++;

      break;
    case ' ':
      if (ReadMode < 2)
        Rows++;                          // Non deleted line
      else
        rc = RC_NF;

      break;
    default:
      if (++Nerr >= Maxerr && !Accept) {
        sprintf(g->Message, MSG(BAD_DBF_REC), Tdbp->GetFile(g), GetRowID());
        rc = RC_FX;
      } else
        rc = (Accept) ? RC_OK : RC_NF;
    } // endswitch To_Buf

  return rc;
  } // end of ReadBuffer

/****************************************************************************/
/*  Data Base delete line routine for DBF access methods.                   */
/*  Deleted lines are just flagged in the first buffer character.           */
/****************************************************************************/
int DBMFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  if (irc == RC_OK)
    *Fpos = '*';

  return RC_OK;
  } // end of DeleteRecords

/***********************************************************************/
/*  Rewind routine for DBF access method.                              */
/***********************************************************************/
void DBMFAM::Rewind(void)
  {
  MBKFAM::Rewind();
  Nerr = 0;
  } // end of Rewind

/* --------------------------------- EOF ---------------------------------- */
