#include "All.h"
#include "BufferIO.h"
#include "GlobalFunctions.h"

namespace APE
{

/**************************************************************************************************
CBufferIO
**************************************************************************************************/
CBufferIO::CBufferIO(IAPEIO * pSource, int nBufferBytes)
{
    m_spSource.Assign(pSource);
    m_nBufferBytes = 0;
    m_nBufferTotalBytes = nBufferBytes;
    m_spBuffer.AllocateArray(m_nBufferTotalBytes);
    m_bReadToBuffer = true;
}

CBufferIO::~CBufferIO()
{
    m_spSource->Close();
    m_spSource.Delete();
}

int CBufferIO::Open(const str_utfn * pName, bool bOpenReadOnly)
{
    return m_spSource->Open(pName, bOpenReadOnly);
}

int CBufferIO::Close()
{
    return m_spSource->Close();
}

int CBufferIO::Read(void * pBuffer, unsigned int nBytesToRead, unsigned int * pBytesRead)
{
    const int nResult = m_spSource->Read(pBuffer, nBytesToRead, pBytesRead);

    if (m_bReadToBuffer && (m_spBuffer != APE_NULL) && (*pBytesRead > 0))
    {
        const int nBufferBytes = APE_MIN(m_nBufferTotalBytes - m_nBufferBytes, *reinterpret_cast<int *>(pBytesRead));
        if (nBufferBytes <= 0)
        {
            m_bReadToBuffer = false;
        }
        else
        {
            memcpy(&m_spBuffer[m_nBufferBytes], pBuffer, static_cast<size_t>(nBufferBytes));
            m_nBufferBytes += *pBytesRead;
        }
    }

    return nResult;
}

int CBufferIO::Write(const void *, unsigned int, unsigned int *)
{
    return ERROR_IO_WRITE;
}

int CBufferIO::Seek(int64 nPosition, SeekMethod nMethod)
{
    m_bReadToBuffer = false; // we seeked, so stop buffering

    // perform the seek on the underlying source
    return m_spSource->Seek(nPosition, nMethod);
}

int CBufferIO::SetEOF()
{
    return m_spSource->SetEOF();
}

unsigned char * CBufferIO::GetBuffer(int * pnBufferBytes)
{
    if (*pnBufferBytes > m_nBufferTotalBytes)
        return APE_NULL; // the request exceeded the size we stored, so return NULL
    ASSERT(*pnBufferBytes == m_nBufferBytes);
    *pnBufferBytes = m_nBufferBytes;
    m_bReadToBuffer = false; // no longer needed
    return m_spBuffer;
}

int64 CBufferIO::GetPosition()
{
    if (m_bReadToBuffer)
    {
        // getting the position on a pipe doesn't work, so we need to do this
        return m_nBufferBytes;
    }
    else
    {
        return m_spSource->GetPosition();
    }
}

int64 CBufferIO::GetSize()
{
    return m_spSource->GetSize();
}

int CBufferIO::Create(const str_utfn * pName)
{
    return m_spSource->Create(pName);
}

int CBufferIO::Delete()
{
    return m_spSource->Delete();
}

/**************************************************************************************************
CHeaderIO
**************************************************************************************************/
CHeaderIO::CHeaderIO(IAPEIO * pSource)
{
    m_spSource.Assign(pSource);
    m_nPosition = 0; // start at position zero even though we read the header
    m_nHeaderBytes = 0;
    APE_CLEAR(m_aryHeader);
}

CHeaderIO::~CHeaderIO()
{
    m_spSource->Close();
    m_spSource.Delete();
}

bool CHeaderIO::ReadHeader(BYTE (& aryHeader)[64])
{
    // zero out the entire header object
    APE_CLEAR(aryHeader);

    // read but cap at the file size
    int64 nFileSize = GetSize();
    if ((nFileSize > 64) || (nFileSize == APE_FILE_SIZE_UNDEFINED))
        nFileSize = 64;
    m_nHeaderBytes = nFileSize;
    if (ReadSafe(m_spSource, m_aryHeader, static_cast<int>(m_nHeaderBytes)) != ERROR_SUCCESS)
        return false;

    // copy the header out
    memcpy(aryHeader, m_aryHeader, static_cast<size_t>(m_nHeaderBytes));
    return true;
}

int CHeaderIO::Open(const str_utfn * pName, bool bOpenReadOnly)
{
    return m_spSource->Open(pName, bOpenReadOnly);
}

int CHeaderIO::Close()
{
    return m_spSource->Close();
}

int CHeaderIO::Read(void * pBuffer, unsigned int nBytesToRead, unsigned int * pBytesRead)
{
    // if we're inside the header, just copy out of it for the first part of the read
    int nResult = ERROR_SUCCESS;
    if (m_nPosition < m_nHeaderBytes)
    {
        int64 nBytesFromBuffer = APE_MIN(m_nHeaderBytes - m_nPosition, nBytesToRead);
        memcpy(pBuffer, &m_aryHeader[m_nPosition], static_cast<size_t>(nBytesFromBuffer));
        char * pBufferChar = reinterpret_cast<char *>(pBuffer);
        unsigned int nBytesFromReader = static_cast<unsigned int>(static_cast<int64>(nBytesToRead) - nBytesFromBuffer);
        *pBytesRead = 0; // zero-out so if we read nothing it stays zero (and we pass a zero into read)
        if (nBytesFromReader > 0)
            nResult = m_spSource->Read(&pBufferChar[nBytesFromBuffer], nBytesFromReader, pBytesRead);
        *pBytesRead += static_cast<unsigned int>(nBytesFromBuffer); // add the number of bytes we read from the buffer
    }
    else
    {
        nResult = m_spSource->Read(pBuffer, nBytesToRead, pBytesRead);
    }

    // increment position
    m_nPosition += *pBytesRead;

    // return result
    return nResult;
}

int CHeaderIO::Write(const void *, unsigned int, unsigned int *)
{
    return ERROR_IO_WRITE;
}

int CHeaderIO::Seek(int64 nPosition, SeekMethod nMethod)
{
    if (nMethod == SeekFileCurrent)
    {
        m_nPosition += nPosition;
        if (m_nPosition > m_nHeaderBytes)
            m_spSource->Seek(m_nPosition, SeekFileBegin);
        return ERROR_SUCCESS;
    }
    else if (nMethod == SeekFileBegin)
    {
        m_nPosition = nPosition;
        if (m_nPosition > m_nHeaderBytes)
            m_spSource->Seek(m_nPosition, SeekFileBegin);
        else
            m_spSource->Seek(m_nHeaderBytes, SeekFileBegin);
        return ERROR_SUCCESS;
    }
    else if (nMethod == SeekFileEnd)
    {
        m_nPosition = GetSize() - abs(nPosition);
        if (m_nPosition > m_nHeaderBytes)
            m_spSource->Seek(m_nPosition, SeekFileBegin);
        else
            m_spSource->Seek(m_nHeaderBytes, SeekFileBegin);
        return ERROR_SUCCESS;
    }

    return ERROR_IO_READ;
}

int CHeaderIO::SetEOF()
{
    return m_spSource->SetEOF();
}

int64 CHeaderIO::GetPosition()
{
    return m_nPosition;
}

int64 CHeaderIO::GetSize()
{
    return m_spSource->GetSize();
}

int CHeaderIO::Create(const str_utfn * pName)
{
    return m_spSource->Create(pName);
}

int CHeaderIO::Delete()
{
    return m_spSource->Delete();
}

}
