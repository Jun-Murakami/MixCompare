#ifdef IO_USE_STD_LIB_FILE_IO

#pragma once

#include "IAPEIO.h"

namespace APE
{

class CStdLibFileIO : public IAPEIO
{
public:
    // construction / destruction
    CStdLibFileIO();
    ~CStdLibFileIO();

    // open / close
    int Open(const str_utfn * pName, bool bOpenReadOnly = false);
    int Close();

    // read / write
    int Read(void * pBuffer, unsigned int nBytesToRead, unsigned int * pBytesRead);
    int Write(const void * pBuffer, unsigned int nBytesToWrite, unsigned int * pBytesWritten = APE_NULL);

    // seek
    int Seek(int64 nPosition, SeekMethod nMethod);

    // other functions
    int SetEOF();
    unsigned char * GetBuffer(int *) { return APE_NULL; }

    // creation / destruction
    int Create(const str_utfn * pName);
    int Delete();

    // attributes
    int64 GetPosition();
    int64 GetSize();
    int GetName(str_utfn (&rpBuffer)[APE_MAX_PATH]);
    int GetHandle();

private:
    str_utfn m_cFileName[MAX_PATH];
    bool m_bReadOnly;
    bool m_bPipe;
    FILE * m_pFile;
};

}

#endif // #ifdef IO_USE_STD_LIB_FILE_IO
