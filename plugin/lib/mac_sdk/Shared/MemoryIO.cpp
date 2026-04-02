#include "All.h"
#include "MemoryIO.h"

namespace APE
{

CMemoryIO::CMemoryIO(unsigned char * pBuffer, int nBufferBytes)
{
    m_pBuffer = pBuffer;
    m_nBufferBytes = nBufferBytes;

    m_nPosition = 0;
}

CMemoryIO::~CMemoryIO()
{
}

int CMemoryIO::Open(const str_utfn * pName, bool bOpenReadOnly)
{
    (void) pName;
    (void) bOpenReadOnly;

    return ERROR_UNDEFINED;
}

int CMemoryIO::Close()
{
    return ERROR_UNDEFINED;
}

int CMemoryIO::Read(void * pBuffer, unsigned int nBytesToRead, unsigned int * pBytesRead)
{
    *pBytesRead = APE_MIN(nBytesToRead, static_cast<unsigned int>(m_nBufferBytes - m_nPosition));
    memcpy(pBuffer, m_pBuffer + m_nPosition, *pBytesRead);
    m_nPosition += *pBytesRead;
    return ERROR_SUCCESS;
}

int CMemoryIO::Write(const void * pBuffer, unsigned int nBytesToWrite, unsigned int * pBytesWritten)
{
    unsigned int nBytesWritten = APE_MIN(nBytesToWrite, static_cast<unsigned int>(m_nBufferBytes - m_nPosition));
    memcpy(m_pBuffer + m_nPosition, pBuffer, nBytesWritten);
    m_nPosition += nBytesWritten;
    if (pBytesWritten != APE_NULL)
        *pBytesWritten = nBytesWritten;
    return ERROR_SUCCESS;
}

int CMemoryIO::Seek(int64 nPosition, SeekMethod nMethod)
{
    switch (nMethod)
    {
    case SeekFileBegin:
        if (nPosition > m_nBufferBytes)
            return ERROR_UNDEFINED;
        m_nPosition = static_cast<int>(nPosition);
        break;
    case SeekFileEnd:
        if (nPosition > m_nBufferBytes)
            return ERROR_UNDEFINED;
        m_nPosition = static_cast<int>(m_nBufferBytes - nPosition);
        break;
    case SeekFileCurrent:
        if (m_nPosition + nPosition < 0 || m_nPosition + nPosition > m_nBufferBytes)
            return ERROR_UNDEFINED;
        m_nPosition = static_cast<int>(m_nPosition + nPosition);
        break;
    }

    return ERROR_SUCCESS;
}

int CMemoryIO::SetEOF()
{
    return ERROR_UNDEFINED;
}

unsigned char * CMemoryIO::GetBuffer(int * pnBufferBytes)
{
    if (*pnBufferBytes >  m_nBufferBytes)
        return APE_NULL; // the request exceeded the size we stored, so return NULL
    ASSERT(*pnBufferBytes == m_nBufferBytes);
    *pnBufferBytes = m_nBufferBytes;
    return m_pBuffer;
}

int64 CMemoryIO::GetPosition()
{
    return m_nPosition;
}

int64 CMemoryIO::GetSize()
{
    return m_nBufferBytes;
}

int CMemoryIO::Create(const str_utfn *)
{
    return ERROR_UNDEFINED;
}

int CMemoryIO::Delete()
{
    return ERROR_UNDEFINED;
}

}
