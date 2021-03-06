/******************************************************************************
 * $Id: cpl_vsil_curl_streaming.cpp 27044 2014-03-16 23:41:27Z rouault $
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Implement VSI large file api for HTTP/FTP files in streaming mode
 * Author:   Even Rouault <even dot rouault at mines dash paris.org>
 *
 ******************************************************************************
 * Copyright (c) 2012-2013, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_vsi_virtual.h"
#include "cpl_string.h"
#include "cpl_multiproc.h"
#include "cpl_hash_set.h"
#include "cpl_time.h"

CPL_CVSID("$Id: cpl_vsil_curl_streaming.cpp 27044 2014-03-16 23:41:27Z rouault $");

#if !defined(HAVE_CURL) || defined(CPL_MULTIPROC_STUB)

void VSIInstallCurlStreamingFileHandler(void)
{
    /* not supported */
}

#else

#include <curl/curl.h>

void VSICurlSetOptions(CURL* hCurlHandle, const char* pszURL);

#include <map>

#define ENABLE_DEBUG        0

#define N_MAX_REGIONS       10

#define BKGND_BUFFER_SIZE   (1024 * 1024)


/************************************************************************/
/*                               RingBuffer                             */
/************************************************************************/

class RingBuffer
{
    GByte* pabyBuffer;
    size_t nCapacity;
    size_t nOffset;
    size_t nLength;

    public:
        RingBuffer(size_t nCapacity = BKGND_BUFFER_SIZE);
        ~RingBuffer();

        size_t GetCapacity() const { return nCapacity; }
        size_t GetSize() const { return nLength; }

        void Reset();
        void Write(void* pBuffer, size_t nSize);
        void Read(void* pBuffer, size_t nSize);
};

RingBuffer::RingBuffer(size_t nCapacityIn)
{
    pabyBuffer = (GByte*)CPLMalloc(nCapacityIn);
    nCapacity = nCapacityIn;
    nOffset = 0;
    nLength = 0;
}

RingBuffer::~RingBuffer()
{
    CPLFree(pabyBuffer);
}

void RingBuffer::Reset()
{
    nOffset = 0;
    nLength = 0;
}

void RingBuffer::Write(void* pBuffer, size_t nSize)
{
    CPLAssert(nLength + nSize <= nCapacity);

    size_t nEndOffset = (nOffset + nLength) % nCapacity;
    size_t nSz = MIN(nSize, nCapacity - nEndOffset);
    memcpy(pabyBuffer + nEndOffset, pBuffer, nSz);
    if (nSz < nSize)
        memcpy(pabyBuffer, (GByte*)pBuffer + nSz, nSize - nSz);

    nLength += nSize;
}

void RingBuffer::Read(void* pBuffer, size_t nSize)
{
    CPLAssert(nSize <= nLength);

    if (pBuffer)
    {
        size_t nSz = MIN(nSize, nCapacity - nOffset);
        memcpy(pBuffer, pabyBuffer + nOffset, nSz);
        if (nSz < nSize)
            memcpy((GByte*)pBuffer + nSz, pabyBuffer, nSize - nSz);
    }

    nOffset = (nOffset + nSize) % nCapacity;
    nLength -= nSize;
}

/************************************************************************/

typedef enum
{
    EXIST_UNKNOWN = -1,
    EXIST_NO,
    EXIST_YES,
} ExistStatus;

typedef struct
{
    ExistStatus     eExists;
    int             bHastComputedFileSize;
    vsi_l_offset    fileSize;
    int             bIsDirectory;
#ifdef notdef
    unsigned int    nChecksumOfFirst1024Bytes;
#endif
} CachedFileProp;

/************************************************************************/
/*                       VSICurlStreamingFSHandler                      */
/************************************************************************/

class VSICurlStreamingFSHandler : public VSIFilesystemHandler
{
    void           *hMutex;

    std::map<CPLString, CachedFileProp*>   cacheFileSize;

public:
    VSICurlStreamingFSHandler();
    ~VSICurlStreamingFSHandler();

    virtual VSIVirtualHandle *Open( const char *pszFilename,
                                    const char *pszAccess);
    virtual int      Stat( const char *pszFilename, VSIStatBufL *pStatBuf, int nFlags );

    void                AcquireMutex();
    void                ReleaseMutex();

    CachedFileProp*     GetCachedFileProp(const char*     pszURL);
};

/************************************************************************/
/*                        VSICurlStreamingHandle                        */
/************************************************************************/

class VSICurlStreamingHandle : public VSIVirtualHandle
{
  private:
    VSICurlStreamingFSHandler* poFS;

    char*           pszURL;

#ifdef notdef
    unsigned int    nRecomputedChecksumOfFirst1024Bytes;
#endif
    vsi_l_offset    curOffset;
    vsi_l_offset    fileSize;
    int             bHastComputedFileSize;
    ExistStatus     eExists;
    int             bIsDirectory;
    
    int             bCanTrustCandidateFileSize;
    int             bHasCandidateFileSize;
    vsi_l_offset    nCandidateFileSize;

    int             bEOF;

    size_t          nCachedSize;
    GByte          *pCachedData;

    CURL*           hCurlHandle;

    volatile int    bDownloadInProgress;
    volatile int    bDownloadStopped;
    volatile int    bAskDownloadEnd;
    vsi_l_offset    nRingBufferFileOffset;
    void           *hThread;
    void           *hRingBufferMutex;
    void           *hCondProducer;
    void           *hCondConsumer;
    RingBuffer      oRingBuffer;
    void            StartDownload();
    void            StopDownload();
    void            PutRingBufferInCache();

    GByte          *pabyHeaderData;
    size_t          nHeaderSize;
    vsi_l_offset    nBodySize;
    int             nHTTPCode;

    void                AcquireMutex();
    void                ReleaseMutex();

    void                AddRegion( vsi_l_offset    nFileOffsetStart,
                                   size_t          nSize,
                                   GByte          *pData );
  public:

    VSICurlStreamingHandle(VSICurlStreamingFSHandler* poFS, const char* pszURL);
    ~VSICurlStreamingHandle();

    virtual int          Seek( vsi_l_offset nOffset, int nWhence );
    virtual vsi_l_offset Tell();
    virtual size_t       Read( void *pBuffer, size_t nSize, size_t nMemb );
    virtual size_t       Write( const void *pBuffer, size_t nSize, size_t nMemb );
    virtual int          Eof();
    virtual int          Flush();
    virtual int          Close();

    void                 DownloadInThread();
    int                  ReceivedBytes(GByte *buffer, size_t count, size_t nmemb);
    int                  ReceivedBytesHeader(GByte *buffer, size_t count, size_t nmemb);

    int                  IsKnownFileSize() const { return bHastComputedFileSize; }
    vsi_l_offset         GetFileSize();
    int                  Exists();
    int                  IsDirectory() const { return bIsDirectory; }
};

/************************************************************************/
/*                       VSICurlStreamingHandle()                       */
/************************************************************************/

VSICurlStreamingHandle::VSICurlStreamingHandle(VSICurlStreamingFSHandler* poFS, const char* pszURL)
{
    this->poFS = poFS;
    this->pszURL = CPLStrdup(pszURL);

#ifdef notdef
    nRecomputedChecksumOfFirst1024Bytes = 0;
#endif
    curOffset = 0;

    poFS->AcquireMutex();
    CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
    eExists = cachedFileProp->eExists;
    fileSize = cachedFileProp->fileSize;
    bHastComputedFileSize = cachedFileProp->bHastComputedFileSize;
    bIsDirectory = cachedFileProp->bIsDirectory;
    poFS->ReleaseMutex();

    bCanTrustCandidateFileSize = TRUE;
    bHasCandidateFileSize = FALSE;
    nCandidateFileSize = 0;

    nCachedSize = 0;
    pCachedData = NULL;

    bEOF = FALSE;

    hCurlHandle = NULL;

    hThread = NULL;
    hRingBufferMutex = CPLCreateMutex();
    ReleaseMutex();
    hCondProducer = CPLCreateCond();
    hCondConsumer = CPLCreateCond();

    bDownloadInProgress = FALSE;
    bDownloadStopped = FALSE;
    bAskDownloadEnd = FALSE;
    nRingBufferFileOffset = 0;

    pabyHeaderData = NULL;
    nHeaderSize = 0;
    nBodySize = 0;
    nHTTPCode = 0;
}

/************************************************************************/
/*                       ~VSICurlStreamingHandle()                      */
/************************************************************************/

VSICurlStreamingHandle::~VSICurlStreamingHandle()
{
    StopDownload();

    CPLFree(pszURL);
    if (hCurlHandle != NULL)
        curl_easy_cleanup(hCurlHandle);

    CPLFree(pCachedData);

    CPLFree(pabyHeaderData);

    CPLDestroyMutex( hRingBufferMutex );
    CPLDestroyCond( hCondProducer );
    CPLDestroyCond( hCondConsumer );
}

/************************************************************************/
/*                         AcquireMutex()                               */
/************************************************************************/

void VSICurlStreamingHandle::AcquireMutex()
{
    CPLAcquireMutex(hRingBufferMutex, 1000.0);
}

/************************************************************************/
/*                          ReleaseMutex()                              */
/************************************************************************/

void VSICurlStreamingHandle::ReleaseMutex()
{
    CPLReleaseMutex(hRingBufferMutex);
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int VSICurlStreamingHandle::Seek( vsi_l_offset nOffset, int nWhence )
{
    if( curOffset >= BKGND_BUFFER_SIZE )
    {
        if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "Invalidating cache and file size due to Seek() beyond caching zone");
        CPLFree(pCachedData);
        pCachedData = NULL;
        nCachedSize = 0;
        AcquireMutex();
        bHastComputedFileSize = FALSE;
        fileSize = 0;
        ReleaseMutex();
    }

    if (nWhence == SEEK_SET)
    {
        curOffset = nOffset;
    }
    else if (nWhence == SEEK_CUR)
    {
        curOffset = curOffset + nOffset;
    }
    else
    {
        curOffset = GetFileSize() + nOffset;
    }
    bEOF = FALSE;
    return 0;
}

typedef struct
{
    char*           pBuffer;
    size_t          nSize;
    int             bIsHTTP;
    int             bIsInHeader;
    int             nHTTPCode;
    int             bDownloadHeaderOnly;
} WriteFuncStruct;

/************************************************************************/
/*                  VSICURLStreamingInitWriteFuncStruct()                */
/************************************************************************/

static void VSICURLStreamingInitWriteFuncStruct(WriteFuncStruct   *psStruct)
{
    psStruct->pBuffer = NULL;
    psStruct->nSize = 0;
    psStruct->bIsHTTP = FALSE;
    psStruct->bIsInHeader = TRUE;
    psStruct->nHTTPCode = 0;
    psStruct->bDownloadHeaderOnly = FALSE;
}

/************************************************************************/
/*                 VSICurlStreamingHandleWriteFuncForHeader()           */
/************************************************************************/

static int VSICurlStreamingHandleWriteFuncForHeader(void *buffer, size_t count, size_t nmemb, void *req)
{
    WriteFuncStruct* psStruct = (WriteFuncStruct*) req;
    size_t nSize = count * nmemb;

    char* pNewBuffer = (char*) VSIRealloc(psStruct->pBuffer,
                                          psStruct->nSize + nSize + 1);
    if (pNewBuffer)
    {
        psStruct->pBuffer = pNewBuffer;
        memcpy(psStruct->pBuffer + psStruct->nSize, buffer, nSize);
        psStruct->pBuffer[psStruct->nSize + nSize] = '\0';
        if (psStruct->bIsHTTP && psStruct->bIsInHeader)
        {
            char* pszLine = psStruct->pBuffer + psStruct->nSize;
            if (EQUALN(pszLine, "HTTP/1.0 ", 9) ||
                EQUALN(pszLine, "HTTP/1.1 ", 9))
                psStruct->nHTTPCode = atoi(pszLine + 9);

            if (pszLine[0] == '\r' || pszLine[0] == '\n')
            {
                if (psStruct->bDownloadHeaderOnly)
                {
                    /* If moved permanently/temporarily, go on. Otherwise stop now*/
                    if (!(psStruct->nHTTPCode == 301 || psStruct->nHTTPCode == 302))
                        return 0;
                }
                else
                {
                    psStruct->bIsInHeader = FALSE;
                }
            }
        }
        psStruct->nSize += nSize;
        return nmemb;
    }
    else
    {
        return 0;
    }
}


/************************************************************************/
/*                           GetFileSize()                              */
/************************************************************************/

vsi_l_offset VSICurlStreamingHandle::GetFileSize()
{
    WriteFuncStruct sWriteFuncData;
    WriteFuncStruct sWriteFuncHeaderData;

    AcquireMutex();
    if (bHastComputedFileSize)
    {
        vsi_l_offset nRet = fileSize;
        ReleaseMutex();
        return nRet;
    }
    ReleaseMutex();

#if LIBCURL_VERSION_NUM < 0x070B00
    /* Curl 7.10.X doesn't manage to unset the CURLOPT_RANGE that would have been */
    /* previously set, so we have to reinit the connection handle */
    if (hCurlHandle)
    {
        curl_easy_cleanup(hCurlHandle);
        hCurlHandle = curl_easy_init();
    }
#endif

    CURL* hLocalHandle = curl_easy_init();

    VSICurlSetOptions(hLocalHandle, pszURL);

    VSICURLStreamingInitWriteFuncStruct(&sWriteFuncHeaderData);

    /* HACK for mbtiles driver: proper fix would be to auto-detect servers that don't accept HEAD */
    /* http://a.tiles.mapbox.com/v3/ doesn't accept HEAD, so let's start a GET */
    /* and interrupt is as soon as the header is found */
    if (strstr(pszURL, ".tiles.mapbox.com/") != NULL)
    {
        curl_easy_setopt(hLocalHandle, CURLOPT_HEADERDATA, &sWriteFuncHeaderData);
        curl_easy_setopt(hLocalHandle, CURLOPT_HEADERFUNCTION, VSICurlStreamingHandleWriteFuncForHeader);

        sWriteFuncHeaderData.bIsHTTP = strncmp(pszURL, "http", 4) == 0;
        sWriteFuncHeaderData.bDownloadHeaderOnly = TRUE;
    }
    else
    {
        curl_easy_setopt(hLocalHandle, CURLOPT_NOBODY, 1);
        curl_easy_setopt(hLocalHandle, CURLOPT_HTTPGET, 0);
        curl_easy_setopt(hLocalHandle, CURLOPT_HEADER, 1);
    }

    /* We need that otherwise OSGEO4W's libcurl issue a dummy range request */
    /* when doing a HEAD when recycling connections */
    curl_easy_setopt(hLocalHandle, CURLOPT_RANGE, NULL);

    /* Bug with older curl versions (<=7.16.4) and FTP. See http://curl.haxx.se/mail/lib-2007-08/0312.html */
    VSICURLStreamingInitWriteFuncStruct(&sWriteFuncData);
    curl_easy_setopt(hLocalHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    curl_easy_setopt(hLocalHandle, CURLOPT_WRITEFUNCTION, VSICurlStreamingHandleWriteFuncForHeader);

    char szCurlErrBuf[CURL_ERROR_SIZE+1];
    szCurlErrBuf[0] = '\0';
    curl_easy_setopt(hLocalHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf );

    double dfSize = 0;
    curl_easy_perform(hLocalHandle);

    AcquireMutex();

    eExists = EXIST_UNKNOWN;
    bHastComputedFileSize = TRUE;

    if (strncmp(pszURL, "ftp", 3) == 0)
    {
        if (sWriteFuncData.pBuffer != NULL &&
            strncmp(sWriteFuncData.pBuffer, "Content-Length: ", strlen( "Content-Length: ")) == 0)
        {
            const char* pszBuffer = sWriteFuncData.pBuffer + strlen("Content-Length: ");
            eExists = EXIST_YES;
            fileSize = CPLScanUIntBig(pszBuffer, sWriteFuncData.nSize - strlen("Content-Length: "));
            if (ENABLE_DEBUG)
                CPLDebug("VSICURL", "GetFileSize(%s)=" CPL_FRMT_GUIB,
                        pszURL, fileSize);
        }
    }

    if (eExists != EXIST_YES)
    {
        CURLcode code = curl_easy_getinfo(hLocalHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &dfSize );
        if (code == 0)
        {
            eExists = EXIST_YES;
            if (dfSize < 0)
                fileSize = 0;
            else
                fileSize = (GUIntBig)dfSize;
        }
        else
        {
            eExists = EXIST_NO;
            fileSize = 0;
            CPLError(CE_Failure, CPLE_AppDefined,
                     "VSICurlStreamingHandle::GetFileSize failed");
        }

        long response_code = 0;
        curl_easy_getinfo(hLocalHandle, CURLINFO_HTTP_CODE, &response_code);
        if (response_code != 200)
        {
            eExists = EXIST_NO;
            fileSize = 0;
        }

        /* Try to guess if this is a directory. Generally if this is a directory, */
        /* curl will retry with an URL with slash added */
        char *pszEffectiveURL = NULL;
        curl_easy_getinfo(hLocalHandle, CURLINFO_EFFECTIVE_URL, &pszEffectiveURL);
        if (pszEffectiveURL != NULL &&
            strncmp(pszURL, pszEffectiveURL, strlen(pszURL)) == 0 &&
            pszEffectiveURL[strlen(pszURL)] == '/')
        {
            eExists = EXIST_YES;
            fileSize = 0;
            bIsDirectory = TRUE;
        }

        if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "GetFileSize(%s)=" CPL_FRMT_GUIB "  response_code=%d",
                    pszURL, fileSize, (int)response_code);
    }

    CPLFree(sWriteFuncData.pBuffer);
    CPLFree(sWriteFuncHeaderData.pBuffer);

    poFS->AcquireMutex();
    CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
    cachedFileProp->bHastComputedFileSize = TRUE;
#ifdef notdef
    cachedFileProp->nChecksumOfFirst1024Bytes = nRecomputedChecksumOfFirst1024Bytes;
#endif
    cachedFileProp->fileSize = fileSize;
    cachedFileProp->eExists = eExists;
    cachedFileProp->bIsDirectory = bIsDirectory;
    poFS->ReleaseMutex();

    vsi_l_offset nRet = fileSize;
    ReleaseMutex();

    if (hCurlHandle == NULL)
        hCurlHandle = hLocalHandle;
    else
        curl_easy_cleanup(hLocalHandle);

    return nRet;
}

/************************************************************************/
/*                                 Exists()                             */
/************************************************************************/

int VSICurlStreamingHandle::Exists()
{
    if (eExists == EXIST_UNKNOWN)
    {
        /* Consider that only the files whose extension ends up with one that is */
        /* listed in CPL_VSIL_CURL_ALLOWED_EXTENSIONS exist on the server */
        /* This can speeds up dramatically open experience, in case the server */
        /* cannot return a file list */
        /* For example : */
        /* gdalinfo --config CPL_VSIL_CURL_ALLOWED_EXTENSIONS ".tif" /vsicurl_streaming/http://igskmncngs506.cr.usgs.gov/gmted/Global_tiles_GMTED/075darcsec/bln/W030/30N030W_20101117_gmted_bln075.tif */
        const char* pszAllowedExtensions =
            CPLGetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", NULL);
        if (pszAllowedExtensions)
        {
            char** papszExtensions = CSLTokenizeString2( pszAllowedExtensions, ", ", 0 );
            int nURLLen = strlen(pszURL);
            int bFound = FALSE;
            for(int i=0;papszExtensions[i] != NULL;i++)
            {
                int nExtensionLen = strlen(papszExtensions[i]);
                if (nURLLen > nExtensionLen &&
                    EQUAL(pszURL + nURLLen - nExtensionLen, papszExtensions[i]))
                {
                    bFound = TRUE;
                    break;
                }
            }

            if (!bFound)
            {
                eExists = EXIST_NO;
                fileSize = 0;

                poFS->AcquireMutex();
                CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
                cachedFileProp->bHastComputedFileSize = TRUE;
                cachedFileProp->fileSize = fileSize;
                cachedFileProp->eExists = eExists;
                poFS->ReleaseMutex();

                CSLDestroy(papszExtensions);

                return 0;
            }

            CSLDestroy(papszExtensions);
        }

        char chFirstByte;
        int bExists = (Read(&chFirstByte, 1, 1) == 1);

        AcquireMutex();
        poFS->AcquireMutex();
        CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
        cachedFileProp->eExists = eExists = bExists ? EXIST_YES : EXIST_NO;
        poFS->ReleaseMutex();
        ReleaseMutex();

        Seek(0, SEEK_SET);
    }

    return eExists == EXIST_YES;
}

/************************************************************************/
/*                                  Tell()                              */
/************************************************************************/

vsi_l_offset VSICurlStreamingHandle::Tell()
{
    return curOffset;
}

/************************************************************************/
/*                         ReceivedBytes()                              */
/************************************************************************/

int VSICurlStreamingHandle::ReceivedBytes(GByte *buffer, size_t count, size_t nmemb)
{
    size_t nSize = count * nmemb;
    nBodySize += nSize;

    if (ENABLE_DEBUG)
        CPLDebug("VSICURL", "Receiving %d bytes...", (int)nSize);

    if( bHasCandidateFileSize && bCanTrustCandidateFileSize && !bHastComputedFileSize )
    {
        poFS->AcquireMutex();
        CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
        cachedFileProp->fileSize = fileSize = nCandidateFileSize;
        cachedFileProp->bHastComputedFileSize = bHastComputedFileSize = TRUE;
        if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "File size = " CPL_FRMT_GUIB, fileSize);
        poFS->ReleaseMutex();
    }

    AcquireMutex();
    if (eExists == EXIST_UNKNOWN)
    {
        poFS->AcquireMutex();
        CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
        cachedFileProp->eExists = eExists = EXIST_YES;
        poFS->ReleaseMutex();
    }
    else if (eExists == EXIST_NO)
    {
        ReleaseMutex();
        return 0;
    }

    while(TRUE)
    {
        size_t nFree = oRingBuffer.GetCapacity() - oRingBuffer.GetSize();
        if (nSize <= nFree)
        {
            oRingBuffer.Write(buffer, nSize);

            /* Signal to the consumer that we have added bytes to the buffer */
            CPLCondSignal(hCondProducer);

            if (bAskDownloadEnd)
            {
                if (ENABLE_DEBUG)
                    CPLDebug("VSICURL", "Download interruption asked");

                ReleaseMutex();
                return 0;
            }
            break;
        }
        else
        {
            oRingBuffer.Write(buffer, nFree);
            buffer += nFree;
            nSize -= nFree;

            /* Signal to the consumer that we have added bytes to the buffer */
            CPLCondSignal(hCondProducer);

            if (ENABLE_DEBUG)
                CPLDebug("VSICURL", "Waiting for reader to consume some bytes...");

            while(oRingBuffer.GetSize() == oRingBuffer.GetCapacity() && !bAskDownloadEnd)
            {
                CPLCondWait(hCondConsumer, hRingBufferMutex);
            }

            if (bAskDownloadEnd)
            {
                if (ENABLE_DEBUG)
                    CPLDebug("VSICURL", "Download interruption asked");

                ReleaseMutex();
                return 0;
            }
        }
    }

    ReleaseMutex();

    return nmemb;
}

/************************************************************************/
/*                 VSICurlStreamingHandleReceivedBytes()                */
/************************************************************************/

static int VSICurlStreamingHandleReceivedBytes(void *buffer, size_t count, size_t nmemb, void *req)
{
    return ((VSICurlStreamingHandle*)req)->ReceivedBytes((GByte*)buffer, count, nmemb);
}


/************************************************************************/
/*              VSICurlStreamingHandleReceivedBytesHeader()             */
/************************************************************************/

#define HEADER_SIZE 32768

int VSICurlStreamingHandle::ReceivedBytesHeader(GByte *buffer, size_t count, size_t nmemb)
{
    size_t nSize = count * nmemb;
    if (ENABLE_DEBUG)
        CPLDebug("VSICURL", "Receiving %d bytes for header...", (int)nSize);

    /* Reset buffer if we have followed link after a redirect */
    if (nSize >=9 && (nHTTPCode == 301 || nHTTPCode == 302) &&
        (EQUALN((const char*)buffer, "HTTP/1.0 ", 9) ||
         EQUALN((const char*)buffer, "HTTP/1.1 ", 9)))
    {
        nHeaderSize = 0;
        nHTTPCode = 0;
    }

    if (nHeaderSize < HEADER_SIZE)
    {
        size_t nSz = MIN(nSize, HEADER_SIZE - nHeaderSize);
        memcpy(pabyHeaderData + nHeaderSize, buffer, nSz);
        pabyHeaderData[nHeaderSize + nSz] = '\0';
        nHeaderSize += nSz;

        //CPLDebug("VSICURL", "Header : %s", pabyHeaderData);

        AcquireMutex();

        if (eExists == EXIST_UNKNOWN && nHTTPCode == 0 &&
            strchr((const char*)pabyHeaderData, '\n') != NULL &&
            (EQUALN((const char*)pabyHeaderData, "HTTP/1.0 ", 9) ||
                EQUALN((const char*)pabyHeaderData, "HTTP/1.1 ", 9)))
        {
            nHTTPCode = atoi((const char*)pabyHeaderData + 9);
            if (ENABLE_DEBUG)
                CPLDebug("VSICURL", "HTTP code = %d", nHTTPCode);

            /* If moved permanently/temporarily, go on */
            if( !(nHTTPCode == 301 || nHTTPCode == 302) )
            {
                poFS->AcquireMutex();
                CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
                cachedFileProp->eExists = eExists = (nHTTPCode == 200) ? EXIST_YES : EXIST_NO;
                poFS->ReleaseMutex();
            }
        }

        if ( !(nHTTPCode == 301 || nHTTPCode == 302) && !bHastComputedFileSize)
        {
            /* Caution: when gzip compression is enabled, the content-length is the compressed */
            /* size, which we are not interested in, so we must not take it into account. */

            const char* pszContentLength = strstr((const char*)pabyHeaderData, "Content-Length: ");
            const char* pszEndOfLine = pszContentLength ? strchr(pszContentLength, '\n') : NULL;
            if( bCanTrustCandidateFileSize && pszEndOfLine != NULL )
            {
                const char* pszVal = pszContentLength + strlen("Content-Length: ");
                bHasCandidateFileSize = TRUE;
                nCandidateFileSize = CPLScanUIntBig(pszVal, pszEndOfLine - pszVal);
                if (ENABLE_DEBUG)
                    CPLDebug("VSICURL", "Has found candidate file size = " CPL_FRMT_GUIB, nCandidateFileSize);
            }

            const char* pszContentEncoding = strstr((const char*)pabyHeaderData, "Content-Encoding: ");
            pszEndOfLine = pszContentEncoding ? strchr(pszContentEncoding, '\n') : NULL;
            if( bHasCandidateFileSize && pszEndOfLine != NULL )
            {
                const char* pszVal = pszContentEncoding + strlen("Content-Encoding: ");
                if( strncmp(pszVal, "gzip", 4) == 0 )
                {
                    if (ENABLE_DEBUG)
                        CPLDebug("VSICURL", "GZip compression enabled --> cannot trust candidate file size");
                    bCanTrustCandidateFileSize = FALSE;
                }
            }
        }

        ReleaseMutex();
    }

    return nmemb;
}

/************************************************************************/
/*                 VSICurlStreamingHandleReceivedBytesHeader()          */
/************************************************************************/

static int VSICurlStreamingHandleReceivedBytesHeader(void *buffer, size_t count, size_t nmemb, void *req)
{
    return ((VSICurlStreamingHandle*)req)->ReceivedBytesHeader((GByte*)buffer, count, nmemb);
}
/************************************************************************/
/*                       DownloadInThread()                             */
/************************************************************************/
static THR_LOCAL int bHasCheckVersion = FALSE;
static THR_LOCAL int bSupportGZip = FALSE;

void VSICurlStreamingHandle::DownloadInThread()
{
    VSICurlSetOptions(hCurlHandle, pszURL);

 
    if (!bHasCheckVersion)
    {
        bSupportGZip = strstr(curl_version(), "zlib/") != NULL;
        bHasCheckVersion = TRUE;
    }
    if (bSupportGZip && CSLTestBoolean(CPLGetConfigOption("CPL_CURL_GZIP", "YES")))
    {
        curl_easy_setopt(hCurlHandle, CURLOPT_ENCODING, "gzip");
    }

    if (pabyHeaderData == NULL)
        pabyHeaderData = (GByte*) CPLMalloc(HEADER_SIZE + 1);
    nHeaderSize = 0;
    nBodySize = 0;
    nHTTPCode = 0;

    curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION, VSICurlStreamingHandleReceivedBytesHeader);

    curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION, VSICurlStreamingHandleReceivedBytes);

    char szCurlErrBuf[CURL_ERROR_SIZE+1];
    szCurlErrBuf[0] = '\0';
    curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf );

    CURLcode eRet = curl_easy_perform(hCurlHandle);

    curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION, NULL);

    AcquireMutex();
    if (!bAskDownloadEnd && eRet == 0 && !bHastComputedFileSize)
    {
        poFS->AcquireMutex();
        CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
        cachedFileProp->fileSize = fileSize = nBodySize;
        cachedFileProp->bHastComputedFileSize = bHastComputedFileSize = TRUE;
        if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "File size = " CPL_FRMT_GUIB, fileSize);
        poFS->ReleaseMutex();
    }

    bDownloadInProgress = FALSE;
    bDownloadStopped = TRUE;

    /* Signal to the consumer that the download has ended */
    CPLCondSignal(hCondProducer);
    ReleaseMutex();
}

static void VSICurlDownloadInThread(void* pArg)
{
    ((VSICurlStreamingHandle*)pArg)->DownloadInThread();
}

/************************************************************************/
/*                            StartDownload()                            */
/************************************************************************/

void VSICurlStreamingHandle::StartDownload()
{
    if (bDownloadInProgress || bDownloadStopped)
        return;

    //if (ENABLE_DEBUG)
        CPLDebug("VSICURL", "Start download for %s", pszURL);

    if (hCurlHandle == NULL)
        hCurlHandle = curl_easy_init();
    oRingBuffer.Reset();
    bDownloadInProgress = TRUE;
    nRingBufferFileOffset = 0;
    hThread = CPLCreateJoinableThread(VSICurlDownloadInThread, this);
}

/************************************************************************/
/*                            StopDownload()                            */
/************************************************************************/

void VSICurlStreamingHandle::StopDownload()
{
    if (hThread)
    {
        //if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "Stop download for %s", pszURL);

        AcquireMutex();
        /* Signal to the producer that we ask for download interruption */
        bAskDownloadEnd = TRUE;
        CPLCondSignal(hCondConsumer);

        /* Wait for the producer to have finished */
        while(bDownloadInProgress)
            CPLCondWait(hCondProducer, hRingBufferMutex);

        bAskDownloadEnd = FALSE;

        ReleaseMutex();

        CPLJoinThread(hThread);
        hThread = NULL;

        curl_easy_cleanup(hCurlHandle);
        hCurlHandle = NULL;
    }

    oRingBuffer.Reset();
    bDownloadStopped = FALSE;
}

/************************************************************************/
/*                        PutRingBufferInCache()                        */
/************************************************************************/

void VSICurlStreamingHandle::PutRingBufferInCache()
{
    if (nRingBufferFileOffset >= BKGND_BUFFER_SIZE)
        return;

    AcquireMutex();

    /* Cache any remaining bytes available in the ring buffer */
    size_t nBufSize = oRingBuffer.GetSize();
    if ( nBufSize > 0 )
    {
        if (nRingBufferFileOffset + nBufSize > BKGND_BUFFER_SIZE)
            nBufSize = (size_t) (BKGND_BUFFER_SIZE - nRingBufferFileOffset);
        GByte* pabyTmp = (GByte*) CPLMalloc(nBufSize);
        oRingBuffer.Read(pabyTmp, nBufSize);

        /* Signal to the producer that we have ingested some bytes */
        CPLCondSignal(hCondConsumer);

        AddRegion(nRingBufferFileOffset, nBufSize, pabyTmp);
        nRingBufferFileOffset += nBufSize;
        CPLFree(pabyTmp);
    }

    ReleaseMutex();
}

/************************************************************************/
/*                                Read()                                */
/************************************************************************/

size_t VSICurlStreamingHandle::Read( void *pBuffer, size_t nSize, size_t nMemb )
{
    GByte* pabyBuffer = (GByte*)pBuffer;
    size_t nBufferRequestSize = nSize * nMemb;
    if (nBufferRequestSize == 0)
        return 0;
    size_t nRemaining = nBufferRequestSize;

    AcquireMutex();
    int bHastComputedFileSizeLocal = bHastComputedFileSize;
    vsi_l_offset fileSizeLocal = fileSize;
    ReleaseMutex();

    if (bHastComputedFileSizeLocal && curOffset >= fileSizeLocal)
    {
        CPLDebug("VSICURL", "Read attempt beyond end of file");
        bEOF = TRUE;
    }
    if (bEOF)
        return 0;

    if (curOffset < nRingBufferFileOffset)
        PutRingBufferInCache();

    if (ENABLE_DEBUG)
        CPLDebug("VSICURL", "Read [" CPL_FRMT_GUIB ", " CPL_FRMT_GUIB "[ in %s",
                 curOffset, curOffset + nBufferRequestSize, pszURL);

#ifdef notdef
    if( pCachedData != NULL && nCachedSize >= 1024 &&
        nRecomputedChecksumOfFirst1024Bytes == 0 )
    {
        for(size_t i = 0; i < 1024 / sizeof(int); i ++)
        {
            int nVal;
            memcpy(&nVal, pCachedData + i * sizeof(int), sizeof(int));
            nRecomputedChecksumOfFirst1024Bytes += nVal;
        }

        if( bHastComputedFileSizeLocal )
        {
            poFS->AcquireMutex();
            CachedFileProp* cachedFileProp = poFS->GetCachedFileProp(pszURL);
            if( cachedFileProp->nChecksumOfFirst1024Bytes == 0 )
            {
                cachedFileProp->nChecksumOfFirst1024Bytes = nRecomputedChecksumOfFirst1024Bytes;
            }
            else if( nRecomputedChecksumOfFirst1024Bytes != cachedFileProp->nChecksumOfFirst1024Bytes )
            {
                CPLDebug("VSICURL", "Invalidating previously cached file size. First bytes of file have changed!");
                AcquireMutex();
                bHastComputedFileSize = FALSE;
                cachedFileProp->bHastComputedFileSize = FALSE;
                cachedFileProp->nChecksumOfFirst1024Bytes = 0;
                ReleaseMutex();
            }
            poFS->ReleaseMutex();
        }
    }
#endif

    /* Can we use the cache ? */
    if( pCachedData != NULL && curOffset < nCachedSize )
    {
        size_t nSz = MIN(nRemaining, (size_t)(nCachedSize - curOffset));
        if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "Using cache for [%d, %d[ in %s",
                     (int)curOffset, (int)(curOffset + nSz), pszURL);
        memcpy(pabyBuffer, pCachedData + curOffset, nSz);
        pabyBuffer += nSz;
        curOffset += nSz;
        nRemaining -= nSz;
    }

    /* Is the request partially covered by the cache and going beyond file size ? */
    if ( pCachedData != NULL && bHastComputedFileSizeLocal &&
         curOffset <= nCachedSize &&
         curOffset + nRemaining > fileSizeLocal &&
         fileSize == nCachedSize )
    {
        size_t nSz = (size_t) (nCachedSize - curOffset);
        if (ENABLE_DEBUG && nSz != 0)
            CPLDebug("VSICURL", "Using cache for [%d, %d[ in %s",
                    (int)curOffset, (int)(curOffset + nSz), pszURL);
        memcpy(pabyBuffer, pCachedData + curOffset, nSz);
        pabyBuffer += nSz;
        curOffset += nSz;
        nRemaining -= nSz;
        bEOF = TRUE;
    }

    /* Has a Seek() being done since the last Read() ? */
    if (!bEOF && nRemaining > 0 && curOffset != nRingBufferFileOffset)
    {
        /* Backward seek : we need to restart the download from the start */
        if (curOffset < nRingBufferFileOffset)
            StopDownload();

        StartDownload();

#define SKIP_BUFFER_SIZE    32768
        GByte* pabyTmp = (GByte*)CPLMalloc(SKIP_BUFFER_SIZE);

        CPLAssert(curOffset >= nRingBufferFileOffset);
        vsi_l_offset nBytesToSkip = curOffset - nRingBufferFileOffset;
        while(nBytesToSkip > 0)
        {
            vsi_l_offset nBytesToRead = nBytesToSkip;

            AcquireMutex();
            if (nBytesToRead > oRingBuffer.GetSize())
                nBytesToRead = oRingBuffer.GetSize();
            if (nBytesToRead > SKIP_BUFFER_SIZE)
                nBytesToRead = SKIP_BUFFER_SIZE;
            oRingBuffer.Read(pabyTmp, (size_t)nBytesToRead);

            /* Signal to the producer that we have ingested some bytes */
            CPLCondSignal(hCondConsumer);
            ReleaseMutex();

            if (nBytesToRead)
                AddRegion(nRingBufferFileOffset, (size_t)nBytesToRead, pabyTmp);

            nBytesToSkip -= nBytesToRead;
            nRingBufferFileOffset += nBytesToRead;

            if (nBytesToRead == 0 && nBytesToSkip != 0)
            {
                if (ENABLE_DEBUG)
                    CPLDebug("VSICURL", "Waiting for writer to produce some bytes...");

                AcquireMutex();
                while(oRingBuffer.GetSize() == 0 && bDownloadInProgress)
                    CPLCondWait(hCondProducer, hRingBufferMutex);
                int bBufferEmpty = (oRingBuffer.GetSize() == 0);
                ReleaseMutex();

                if (bBufferEmpty && !bDownloadInProgress)
                    break;
            }
        }

        CPLFree(pabyTmp);

        if (nBytesToSkip != 0)
        {
            bEOF = TRUE;
            return 0;
        }
    }

    if (!bEOF && nRemaining > 0)
    {
        StartDownload();
        CPLAssert(curOffset == nRingBufferFileOffset);
    }

    /* Fill the destination buffer from the ring buffer */
    while(!bEOF && nRemaining > 0)
    {
        AcquireMutex();
        size_t nToRead = oRingBuffer.GetSize();
        if (nToRead > nRemaining)
            nToRead = nRemaining;
        oRingBuffer.Read(pabyBuffer, nToRead);

        /* Signal to the producer that we have ingested some bytes */
        CPLCondSignal(hCondConsumer);
        ReleaseMutex();

        if (nToRead)
            AddRegion(curOffset, nToRead, pabyBuffer);

        nRemaining -= nToRead;
        pabyBuffer += nToRead;
        curOffset += nToRead;
        nRingBufferFileOffset += nToRead;

        if (nToRead == 0 && nRemaining != 0)
        {
            if (ENABLE_DEBUG)
                CPLDebug("VSICURL", "Waiting for writer to produce some bytes...");

            AcquireMutex();
            while(oRingBuffer.GetSize() == 0 && bDownloadInProgress)
                CPLCondWait(hCondProducer, hRingBufferMutex);
            int bBufferEmpty = (oRingBuffer.GetSize() == 0);
            ReleaseMutex();

            if (bBufferEmpty && !bDownloadInProgress)
                break;
        }
    }

    if (ENABLE_DEBUG)
        CPLDebug("VSICURL", "Read(%d) = %d",
                (int)nBufferRequestSize, (int)(nBufferRequestSize - nRemaining));
    size_t nRet = (nBufferRequestSize - nRemaining) / nSize;
    if (nRet < nMemb)
        bEOF = TRUE;

    return nRet;
}

/************************************************************************/
/*                          AddRegion()                                 */
/************************************************************************/

void  VSICurlStreamingHandle::AddRegion( vsi_l_offset    nFileOffsetStart,
                                         size_t          nSize,
                                         GByte          *pData )
{
    if (nFileOffsetStart >= BKGND_BUFFER_SIZE)
        return;

        if (pCachedData == NULL)
            pCachedData = (GByte*) CPLMalloc(BKGND_BUFFER_SIZE);

    if (nFileOffsetStart <= nCachedSize &&
        nFileOffsetStart + nSize > nCachedSize)
    {
        size_t nSz = MIN(nSize, (size_t) (BKGND_BUFFER_SIZE - nFileOffsetStart));
        if (ENABLE_DEBUG)
            CPLDebug("VSICURL", "Writing [%d, %d[ in cache for %s",
                     (int)nFileOffsetStart, (int)(nFileOffsetStart + nSz), pszURL);
        memcpy(pCachedData + nFileOffsetStart, pData, nSz);
        nCachedSize = (size_t) (nFileOffsetStart + nSz);
    }
}
/************************************************************************/
/*                               Write()                                */
/************************************************************************/

size_t VSICurlStreamingHandle::Write( const void *pBuffer, size_t nSize, size_t nMemb )
{
    return 0;
}

/************************************************************************/
/*                                 Eof()                                */
/************************************************************************/


int       VSICurlStreamingHandle::Eof()
{
    return bEOF;
}

/************************************************************************/
/*                                 Flush()                              */
/************************************************************************/

int       VSICurlStreamingHandle::Flush()
{
    return 0;
}

/************************************************************************/
/*                                  Close()                             */
/************************************************************************/

int       VSICurlStreamingHandle::Close()
{
    return 0;
}


/************************************************************************/
/*                      VSICurlStreamingFSHandler()                     */
/************************************************************************/

VSICurlStreamingFSHandler::VSICurlStreamingFSHandler()
{
    hMutex = CPLCreateMutex();
    CPLReleaseMutex(hMutex);
}

/************************************************************************/
/*                      ~VSICurlStreamingFSHandler()                    */
/************************************************************************/

VSICurlStreamingFSHandler::~VSICurlStreamingFSHandler()
{
    std::map<CPLString, CachedFileProp*>::const_iterator iterCacheFileSize;

    for( iterCacheFileSize = cacheFileSize.begin();
         iterCacheFileSize != cacheFileSize.end();
         iterCacheFileSize++ )
    {
        CPLFree(iterCacheFileSize->second);
    }

    CPLDestroyMutex( hMutex );
    hMutex = NULL;
}

/************************************************************************/
/*                         AcquireMutex()                               */
/************************************************************************/

void VSICurlStreamingFSHandler::AcquireMutex()
{
    CPLAcquireMutex(hMutex, 1000.0);
}

/************************************************************************/
/*                         ReleaseMutex()                               */
/************************************************************************/

void VSICurlStreamingFSHandler::ReleaseMutex()
{
    CPLReleaseMutex(hMutex);
}

/************************************************************************/
/*                         GetCachedFileProp()                          */
/************************************************************************/

/* Should be called under the FS Lock */

CachedFileProp*  VSICurlStreamingFSHandler::GetCachedFileProp(const char* pszURL)
{
    CachedFileProp* cachedFileProp = cacheFileSize[pszURL];
    if (cachedFileProp == NULL)
    {
        cachedFileProp = (CachedFileProp*) CPLMalloc(sizeof(CachedFileProp));
        cachedFileProp->eExists = EXIST_UNKNOWN;
        cachedFileProp->bHastComputedFileSize = FALSE;
        cachedFileProp->fileSize = 0;
        cachedFileProp->bIsDirectory = FALSE;
#ifdef notdef
        cachedFileProp->nChecksumOfFirst1024Bytes = 0;
#endif
        cacheFileSize[pszURL] = cachedFileProp;
    }

    return cachedFileProp;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

VSIVirtualHandle* VSICurlStreamingFSHandler::Open( const char *pszFilename,
                                                   const char *pszAccess )
{
    if (strchr(pszAccess, 'w') != NULL ||
        strchr(pszAccess, '+') != NULL)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Only read-only mode is supported for /vsicurl_streaming");
        return NULL;
    }

    VSICurlStreamingHandle* poHandle = new VSICurlStreamingHandle(
        this, pszFilename + strlen("/vsicurl_streaming/"));
    /* If we didn't get a filelist, check that the file really exists */
    if (!poHandle->Exists())
    {
        delete poHandle;
        poHandle = NULL;
    }

    if( CSLTestBoolean( CPLGetConfigOption( "VSI_CACHE", "FALSE" ) ) )
        return VSICreateCachedFile( poHandle );
    else
        return poHandle;
}

/************************************************************************/
/*                                Stat()                                */
/************************************************************************/

int VSICurlStreamingFSHandler::Stat( const char *pszFilename,
                                     VSIStatBufL *pStatBuf,
                                     int nFlags )
{
    CPLString osFilename(pszFilename);

    memset(pStatBuf, 0, sizeof(VSIStatBufL));

    VSICurlStreamingHandle oHandle( this, osFilename + strlen("/vsicurl_streaming/"));

    if ( oHandle.IsKnownFileSize() ||
         ((nFlags & VSI_STAT_SIZE_FLAG) && !oHandle.IsDirectory() &&
           CSLTestBoolean(CPLGetConfigOption("CPL_VSIL_CURL_SLOW_GET_SIZE", "YES"))) )
        pStatBuf->st_size = oHandle.GetFileSize();

    int nRet = (oHandle.Exists()) ? 0 : -1;
    pStatBuf->st_mode = oHandle.IsDirectory() ? S_IFDIR : S_IFREG;
    return nRet;
}

/************************************************************************/
/*                   VSIInstallCurlFileHandler()                        */
/************************************************************************/

/**
 * \brief Install /vsicurl_streaming/ HTTP/FTP file system handler (requires libcurl)
 *
 * A special file handler is installed that allows on-the-fly reading of files
 * streamed through HTTP/FTP web protocols (typically dynamically generated files),
 * without downloading the entire file.
 *
 * Although this file handler is able seek to random offsets in the file, this will not
 * be efficient. If you need efficient random access and that the server supports range
 * dowloading, you should use the /vsicurl/ file system handler instead.
 *
 * Recognized filenames are of the form /vsicurl_streaming/http://path/to/remote/ressource or
 * /vsicurl_streaming/ftp://path/to/remote/ressource where path/to/remote/ressource is the
 * URL of a remote ressource.
 *
 * The GDAL_HTTP_PROXY, GDAL_HTTP_PROXYUSERPWD and GDAL_PROXY_AUTH configuration options can be
 * used to define a proxy server. The syntax to use is the one of Curl CURLOPT_PROXY,
 * CURLOPT_PROXYUSERPWD and CURLOPT_PROXYAUTH options.
 *
 * The file can be cached in RAM by setting the configuration option
 * VSI_CACHE to TRUE. The cache size defaults to 25 MB, but can be modified by setting
 * the configuration option VSI_CACHE_SIZE (in bytes).
 *
 * VSIStatL() will return the size in st_size member and file
 * nature- file or directory - in st_mode member (the later only reliable with FTP
 * resources for now).
 *
 * @since GDAL 1.10
 */
void VSIInstallCurlStreamingFileHandler(void)
{
    VSIFileManager::InstallHandler( "/vsicurl_streaming/", new VSICurlStreamingFSHandler );
}

#ifdef AS_PLUGIN
CPL_C_START
void CPL_DLL GDALRegisterMe();
void GDALRegisterMe()
{
    VSIInstallCurlStreamingFileHandler();
}
CPL_C_END
#endif

#endif /*  !defined(HAVE_CURL) || defined(CPL_MULTIPROC_STUB) */
