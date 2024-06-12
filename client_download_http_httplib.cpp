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
#include "httplib/httplib.h"
#include <assert.h>
#include "hsync_import_patch.h" // HSYNC_VERSION_STRING
#include "../HDiffPatch/file_for_patch.h"
#include "../HDiffPatch/libHDiffPatch/HDiff/private_diff/mem_buf.h"
#if (_IS_USED_MULTITHREAD)
#   include "../HDiffPatch/libParallel/parallel_import.h" //this_thread_yield
#   include "../HDiffPatch/libParallel/parallel_channel.h"
#endif
using namespace hdiff_private;

static const size_t kBestBufSize = hpatch_kFileIOBestMaxSize>>4;
static const size_t kBestRangesCacheSize=hpatch_kFileIOBestMaxSize;
static const int    kTimeout_s=10;
static const uint64_t kNullRangePos=~(uint64_t)0;
static const hpatch_StreamPos_t kEmptyEndPos=kNullRangePos;
static const char*  kHttpUserAgent="hsynz/" HSYNC_VERSION_STRING;

std::string ParseHostAddressFromURL(std::string InURL)
{
    std::string HostAddress;

    std::string http = InURL.substr(0, 7);
    std::string https = InURL.substr(0, 8);

    if(http == "http://" || https == "https://")
    {
        std::string TempStr = InURL.substr(9);
        int pos = TempStr.find('/');
        if(pos > 1)
        {
            HostAddress = InURL.substr(0, 9 + pos);
        }
        
    }
    return HostAddress;
}

class HsynzHttpClient
{
public:

    typedef std::pair<uint64_t,uint64_t> TRange;

    HsynzHttpClient(std::string InHostAddress)
    {
        HostAddress = ParseHostAddressFromURL(InHostAddress);
        FileURL = InHostAddress.substr(HostAddress.length());
        HttpClient = new httplib::Client(HostAddress);
    }

    ~HsynzHttpClient()
    {
        if(HttpClient != nullptr)
        {
            HttpClient->stop();
            delete HttpClient;
            HttpClient = nullptr;
        }
    }

   static hpatch_BOOL readSyncDataBegin(IReadSyncDataListener* listener,const TNeedSyncInfos* needSyncInfo,
                                         uint32_t blockIndex,hpatch_StreamPos_t posInNewSyncData,hpatch_StreamPos_t posInNeedSyncData)
    {
        HsynzHttpClient* self=(HsynzHttpClient*)listener->readSyncDataImport;
        self->nsi=needSyncInfo;
        try{
            size_t cacheSize=kBestRangesCacheSize;
            self->_cache.realloc(cacheSize);
            self->_writePos=0;
            self->_readPos=0;

            if (!self->_sendDownloads_init(blockIndex,posInNewSyncData)) //step by step send
                return hpatch_FALSE;
        }catch(...){
            return hpatch_FALSE;
        }

        return hpatch_TRUE;
    }

    static void readSyncDataEnd(IReadSyncDataListener* listener)
    { 
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


    bool _sendDownloads_init(uint32_t blockIndex,hpatch_StreamPos_t posInNewSyncData)
    {
        assert(nsi!=0);
        printf("\nhttp download from block index %d/%d\n",blockIndex,nsi->blockCount);
        curBlockIndex=blockIndex;
        curPosInNewSyncData=posInNewSyncData;
        return _sendDownloads_step();
    }

    bool _sendDownloads_step()
    {
        try{
            while (curBlockIndex<nsi->blockCount){
                makeRanges(RangeList,curBlockIndex,curPosInNewSyncData);
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
                                    unsigned char* out_syncDataBuf,uint32_t syncDataSize)
    {
        while ( false && syncDataSize>0)
        {
            size_t savedSize=_savedSize();
            if (savedSize>0)
            {
                size_t readSize=(savedSize<=syncDataSize)?savedSize:syncDataSize;
                _readFromCache(out_syncDataBuf,readSize);
                out_syncDataBuf+=readSize;
                syncDataSize-=(uint32_t)readSize;
                continue;
            }
        }

        int process = 0;

        if(HttpClient != nullptr)
        {
            HttpClient->Get(FileURL, 
                [&process, out_syncDataBuf, syncDataSize](const char *data, size_t data_length)
                {
                    int chunkSize = std::min<int>(syncDataSize - process, data_length);
                    memcpy(out_syncDataBuf + process, data, chunkSize);
                    process += chunkSize;
                    if(process == syncDataSize)
                    {
                        return false;
                    }
                    return true;
                },

                [](uint64_t current, uint64_t tota)
                {
                    return true;
                }
            );
        }


        return hpatch_TRUE;
    }

    size_t _savedSize() const
    {
        return ((_writePos>=_readPos)?_writePos:_writePos+_cache.size())-_readPos;
    }

    void _readFromCache(unsigned char* out_syncDataBuf,size_t readSize)
    {
        const size_t cacheSize=_cache.size();
        while (readSize>0){
            size_t rlen=cacheSize-_readPos;
            rlen=(rlen<=readSize)?rlen:readSize;
            memcpy(out_syncDataBuf,_cache.data()+_readPos,rlen);
            out_syncDataBuf+=rlen;
            readSize-=rlen;
            _readPos+=rlen;
            _readPos=(_readPos<cacheSize)?_readPos:(_readPos-cacheSize);
        }
    }

    void _closeAll()
    {
        if(HttpClient != nullptr)
        {
            HttpClient->stop();
        }
    }

    void makeRanges(std::vector<TRange>& out_ranges,uint32_t& blockIndex,
                    hpatch_StreamPos_t& posInNewSyncData){
        out_ranges.resize(kStepRangeNumber);
        size_t gotRangeCount=TNeedSyncInfos_getNextRanges(nsi,(hpatch_StreamPos_t*)out_ranges.data(),
                                                          kStepRangeNumber,&blockIndex,&posInNewSyncData);
        out_ranges.resize(gotRangeCount);
    }

protected:

    httplib::Client* HttpClient = nullptr;

    std::string HostAddress;
    std::string FileURL;

    //range download

    std::vector<TRange> RangeList;

    uint32_t            curBlockIndex;
    hpatch_StreamPos_t  curPosInNewSyncData;

    const std::string _file_url;
    TAutoMem          _cache;
    size_t            _readPos;
    size_t            _writePos;
    size_t            kStepRangeNumber = 1;
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
