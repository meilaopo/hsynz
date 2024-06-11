//  client_download_http.cpp
//  hsync_http: http(s) download demo by http(s)
//  Created by housisong on 2020-01-29.
/*
 The MIT License (MIT)
 Copyright (c) 2020-2023 HouSisong
 
 Permission is hereby granted, free of charge, to any person
 obtaining a copy of this software and associated documentation
 files (the "Software"), to deal in the Software without
 restriction, including without limitation the rights to use,
 copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following
 conditions:
 
 The above copyright notice and this permission notice shall be
 included in all copies of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.
 */
#include "client_download_http.h"
#include <assert.h>
#include "hsync_import_patch.h" // HSYNC_VERSION_STRING
#include "../HDiffPatch/file_for_patch.h"
#include "../HDiffPatch/libHDiffPatch/HDiff/private_diff/mem_buf.h"
#if (_IS_USED_MULTITHREAD)
#   include "../HDiffPatch/libParallel/parallel_import.h" //this_thread_yield
#   include "../HDiffPatch/libParallel/parallel_channel.h"
#endif
using namespace hdiff_private;

#include "httplib/httplib.h"
#if defined(_MSC_VER)
#	ifdef MINIHTTP_USE_MBEDTLS
#		pragma comment( lib, "Advapi32.lib" )
#	endif
#	if defined(_WIN32_WCE)
#		pragma comment( lib, "ws2.lib" )
#	else
#		pragma comment( lib, "ws2_32.lib" )
#	endif
#endif /* _MSC_VER */

static const size_t kBestBufSize = hpatch_kFileIOBestMaxSize>>4;
static const size_t kBestRangesCacheSize=hpatch_kFileIOBestMaxSize;
static const int    kTimeout_s=10;
static const uint64_t kNullRangePos=~(uint64_t)0;
static const hpatch_StreamPos_t kEmptyEndPos=kNullRangePos;
static const char*  kHttpUserAgent="hsynz/" HSYNC_VERSION_STRING;

std::string ParseHostAddressFromURL(const char* InURL)
{
    std::string HostAddress;

    if(strncmp(InURL, "http:://", 8) == 0
        || strncmp(InURL, "https:://", 9) == 0)
    {
        const char *quotPtr = strchr(InURL + 10, '/');
        if(quotPtr == NULL)
        {
            return HostAddress;
        }

        int position = quotPtr - (InURL + 10);
        HostAddress = InURL + position;
    }
    return HostAddress;
}

class HsynzHttpClient
{
public:

    HsynzHttpClient(const char* InHostAddress)
    {
        for(int i = 0; i < ParallelNum; ++i)
        {
            if(HttpClientList[i] != nullptr)
            {
                HttpClientList[i] = new httplib::Client(InHostAddress); ;
            }
        }
    }

    ~HsynzHttpClient()
    {
        for(int i = 0; i < ParallelNum; ++i)
        {
            if(HttpClientList[i] != nullptr)
            {
                delete HttpClientList[i];
                HttpClientList[i] = nullptr;
            }
        }
    }

   static hpatch_BOOL readSyncDataBegin(IReadSyncDataListener* listener,const TNeedSyncInfos* needSyncInfo,
                                         uint32_t blockIndex,hpatch_StreamPos_t posInNewSyncData,hpatch_StreamPos_t posInNeedSyncData){
        HsynzHttpClient* self=(HsynzHttpClient*)listener->readSyncDataImport;
        self->nsi=needSyncInfo;
        try{
            size_t cacheSize=kBestRangesCacheSize;
            self->_cache.realloc(cacheSize);
            self->_writePos=0;
            self->_readPos=0;
            //if (!self->_sendDownloads_all(blockIndex,posInNewSyncData))
            //    return hpatch_FALSE;
            if (!self->_sendDownloads_init(blockIndex,posInNewSyncData)) //step by step send
                return hpatch_FALSE;
        }catch(...){
            return hpatch_FALSE;
        }

        return hpatch_TRUE;
    }
    static void readSyncDataEnd(IReadSyncDataListener* listener){ 
        HsynzHttpClient* self=(HsynzHttpClient*)listener->readSyncDataImport;
        self->_closeAll(); 
    }
    
    static hpatch_BOOL readSyncData(IReadSyncDataListener* listener,uint32_t blockIndex,
                                    hpatch_StreamPos_t posInNewSyncData,hpatch_StreamPos_t posInNeedSyncData,
                                    unsigned char* out_syncDataBuf,uint32_t syncDataSize){
        HsynzHttpClient* self=(HsynzHttpClient*)listener->readSyncDataImport;
        try{
            return self->readSyncData(blockIndex,posInNewSyncData,posInNeedSyncData,out_syncDataBuf,syncDataSize);
        }catch(...){
            return hpatch_FALSE;
        }
    }

protected:


    bool _sendDownloads_init(uint32_t blockIndex,hpatch_StreamPos_t posInNewSyncData){
        assert(nsi!=0);
        printf("\nhttp download from block index %d/%d\n",blockIndex,nsi->blockCount);
        curBlockIndex=blockIndex;
        curPosInNewSyncData=posInNewSyncData;
        return _sendDownloads_step();
    }

    bool _sendDownloads_step(){
        try{
            while (curBlockIndex<nsi->blockCount){
                //makeRanges(_hd.Ranges(),curBlockIndex,curPosInNewSyncData);
                //if (!_hd.Ranges().empty())
                    //return _hd.doDownload(_file_url);
            }
        }catch(...){
            return false;
        }
        return true;
    }

    hpatch_BOOL readSyncData(uint32_t blockIndex,hpatch_StreamPos_t posInNewSyncData,
                                    hpatch_StreamPos_t posInNeedSyncData,
                                    unsigned char* out_syncDataBuf,uint32_t syncDataSize){
        while (syncDataSize>0){
            size_t savedSize=_savedSize();
            if (savedSize>0){
                size_t readSize=(savedSize<=syncDataSize)?savedSize:syncDataSize;
                _readFromCache(out_syncDataBuf,readSize);
                out_syncDataBuf+=readSize;
                syncDataSize-=(uint32_t)readSize;
                continue;
            }

            while(isNeedUpdate()&&(_emptySize()>GetBufSize())){
                if (!update()){
    #if (_IS_USED_MULTITHREAD)
                    this_thread_yield();
    #else
                    //sleep?
    #endif
                }
            }
            savedSize=_savedSize();
            if (savedSize==0){
                if (!IsSuccess()||is_write_error)
                    return hpatch_FALSE;
                if ((!isNeedUpdate())&&(requestCount==0))
                    return hpatch_FALSE;
            }
        }
        return hpatch_TRUE;
    }

    void _closeAll(){
        for(int i = 0; i < ParallelNum; ++i)
        {
            if(HttpClientList[i] != nullptr)
            {
                HttpClientList[i]->stop();
            }
        }

    }

protected:

    static const int ParallelNum = 8;

    httplib::Client* HttpClientList[ParallelNum] = { nullptr };

    //range download

    uint32_t            curBlockIndex;
    hpatch_StreamPos_t  curPosInNewSyncData;

    const std::string _file_url;
    TAutoMem          _cache;
    size_t            _readPos;
    size_t            _writePos;
    size_t            kStepRangeNumber;
    const TNeedSyncInfos* nsi;

};


hpatch_BOOL download_range_by_http_open(IReadSyncDataListener* out_httpListener,
                                        const char* file_url,size_t kStepRangeNumber){
    HsynzHttpClient* self=0;
    try {
        self=new HsynzHttpClient(file_url);
    } catch (...) {
        if (self) delete self;
        return hpatch_FALSE;
    }
    out_httpListener->readSyncDataImport=self;
    out_httpListener->readSyncDataBegin=HsynzHttpClient::readSyncDataBegin;
    out_httpListener->readSyncData=HsynzHttpClient::readSyncData;
    out_httpListener->readSyncDataEnd=HsynzHttpClient::readSyncDataEnd;
    return hpatch_TRUE;
}

hpatch_BOOL download_range_by_http_close(IReadSyncDataListener* httpListener){
    if (httpListener==0) return hpatch_TRUE;

    HsynzHttpClient* self=(HsynzHttpClient*)httpListener->readSyncDataImport;
    httpListener->readSyncDataImport=0;
    if(self != nullptr)
    {
        delete self;
        self = nullptr;
    }
}

hpatch_BOOL download_file_by_http(const char* file_url,const hpatch_TStreamOutput* out_stream,
                                  hpatch_StreamPos_t continueDownloadPos){

    std::string HostAddress = ParseHostAddressFromURL(file_url);

    httplib::Client HttpClient(HostAddress);

    hpatch_StreamPos_t _cur_pos = continueDownloadPos;

    httplib::Result Error = HttpClient.Get(file_url + HostAddress.length(), 
        [out_stream, &_cur_pos](const char *data, size_t data_length)
        {
            out_stream->write(out_stream, _cur_pos, (const unsigned char*)data, (const unsigned char*)data + data_length);
            _cur_pos += data_length;
            return true;
        },
        [](uint64_t current, uint64_t total)
        {
            return true;
        }
    );

    return hpatch_TRUE;
}
