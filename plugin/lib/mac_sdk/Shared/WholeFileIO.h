#pragma once

#include "IAPEIO.h"

namespace APE
{

class CWholeFileIO : public IAPEIO
{
public:
    // construction / destruction
    CWholeFileIO(IAPEIO * pSource, unsigned char * pBuffer, int64 nFileBytes);
    ~CWholeFileIO();

    // open / close
    int Open(const str_utfn * pName, bool bOpenReadOnly = false) APE_OVERRIDE;
    int Close() APE_OVERRIDE;

    // read / write
    int Read(void * pBuffer, unsigned int nBytesToRead, unsigned int * pBytesRead) APE_OVERRIDE;
    int Write(const void * pBuffer, unsigned int nBytesToWrite, unsigned int * pBytesWritten) APE_OVERRIDE;

    // seek
    int Seek(int64 nPosition, SeekMethod nMethod) APE_OVERRIDE;

    // other functions
    int SetEOF() APE_OVERRIDE;
    unsigned char * GetBuffer(int *) APE_OVERRIDE { return APE_NULL; }

    // creation / destruction
    int Create(const str_utfn * pName) APE_OVERRIDE;
    int Delete() APE_OVERRIDE;

    // attributes
    int64 GetPosition() APE_OVERRIDE;
    int64 GetSize() APE_OVERRIDE;

private:
    CSmartPtr<IAPEIO> m_spSource;
    CSmartPtr<unsigned char> m_spWholeFile;
    CSmartPtr<unsigned char> m_spBuffer;
    int64 m_nWholeFilePointer;
    int64 m_nWholeFileSize;
};

CWholeFileIO * CreateWholeFileIO(IAPEIO * pSource, int64 nSize);

}
