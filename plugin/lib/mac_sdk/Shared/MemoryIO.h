#pragma once

#include "IAPEIO.h"

namespace APE
{

class CMemoryIO : public IAPEIO
{
public:
    // construction / destruction
    CMemoryIO(unsigned char * pBuffer, int nBufferBytes);
    ~CMemoryIO();

    // open / close
    int Open(const str_utfn * pName, bool bOpenReadOnly = false) APE_OVERRIDE;
    int Close() APE_OVERRIDE;

    // read / write
    int Read(void * pBuffer, unsigned int nBytesToRead, unsigned int * pBytesRead) APE_OVERRIDE;
    int Write(const void * pBuffer, unsigned int nBytesToWrite, unsigned int * pBytesWritten = APE_NULL) APE_OVERRIDE;

    // seek
    int Seek(int64 nPosition, SeekMethod nMethod) APE_OVERRIDE;

    // other functions
    int SetEOF() APE_OVERRIDE;
    unsigned char * GetBuffer(int * pnBufferBytes) APE_OVERRIDE;

    // creation / destruction
    int Create(const str_utfn * pName) APE_OVERRIDE;
    int Delete() APE_OVERRIDE;

    // attributes
    int64 GetPosition() APE_OVERRIDE;
    int64 GetSize() APE_OVERRIDE;
    
private:
    unsigned char * m_pBuffer;
    int m_nBufferBytes;

    int m_nPosition;
};

}
