/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * File Created: Wednesday, 5th September 2018 8:04:03 pm
 * Author: yangyaokai
 */

#include <gflags/gflags.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>

#include "src/chunkserver/datastore/chunkserver_datastore.h"
#include "src/chunkserver/datastore/filename_operator.h"
#include "src/common/location_operator.h"

namespace curve {
namespace chunkserver {

CSDataStore::CSDataStore(std::shared_ptr<LocalFileSystem> lfs,
                         std::shared_ptr<FilePool> chunkFilePool,
                         const DataStoreOptions& options)
    : chunkSize_(options.chunkSize),
      pageSize_(options.pageSize),
      locationLimit_(options.locationLimit),
      baseDir_(options.baseDir),
      chunkFilePool_(chunkFilePool),
      lfs_(lfs),
      enableOdsyncWhenOpenChunkFile_(options.enableOdsyncWhenOpenChunkFile) {
    CHECK(!baseDir_.empty()) << "Create datastore failed";
    CHECK(lfs_ != nullptr) << "Create datastore failed";
    CHECK(chunkFilePool_ != nullptr) << "Create datastore failed";
}

CSDataStore::~CSDataStore() {
}

bool CSDataStore::Initialize() {
    // Make sure the baseDir directory exists
    if (!lfs_->DirExists(baseDir_.c_str())) {
        int rc = lfs_->Mkdir(baseDir_.c_str());
        if (rc < 0) {
            LOG(ERROR) << "Create " << baseDir_ << " failed.";
            return false;
        }
    }

    vector<string> files;
    int rc = lfs_->List(baseDir_, &files);
    if (rc < 0) {
        LOG(ERROR) << "List " << baseDir_ << " failed.";
        return false;
    }

    // If loaded before, reload here
    metaCache_.Clear();
    metric_ = std::make_shared<DataStoreMetric>();
    for (size_t i = 0; i < files.size(); ++i) {
        FileNameOperator::FileInfo info =
            FileNameOperator::ParseFileName(files[i]);
        if (info.type == FileNameOperator::FileType::CHUNK) {
            // If the chunk file has not been loaded yet, load it to metaCache
            CSErrorCode errorCode = loadChunkFile(info.id);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load chunk file failed: " << files[i];
                return false;
            }
        } else if (info.type == FileNameOperator::FileType::CLONE) {
            string chunkFilePath = baseDir_ + "/" +
                        FileNameOperator::GenerateChunkFileName(info.id);
            
            // If the chunk file does not exist, print the log
            if (!lfs_->FileExists(chunkFilePath)) {
                LOG(WARNING) << "Can't find clone "
                             << files[i] << "' chunk.";
                continue;
            }

            // If the chunk file exists, load the chunk file to metaCache first
            CSErrorCode errorCode = loadChunkFile(info.id);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load chunk file failed.";
                return false;
            }

            CSChunkFilePtr clonefile = nullptr;
            // load clone chunk file to memory
            errorCode = loadCloneChunkFile(info.id, info.cloneNo, &clonefile);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load clone chunk file failed.";
                return false;
            }
        } else if (info.type == FileNameOperator::FileType::CLONE_SNAPSHOT) {
            string chunkFilePath = baseDir_ + "/" +
                        FileNameOperator::GenerateChunkFileName(info.id);

            // If the chunk file does not exist, print the log
            if (!lfs_->FileExists(chunkFilePath)) {
                LOG(WARNING) << "Can't find snapshot "
                             << files[i] << "' chunk.";
                continue;
            }
            // If the chunk file exists, load the chunk file to metaCache first
            CSErrorCode errorCode = loadChunkFile(info.id);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load chunk file failed.";
                return false;
            }

            CSChunkFilePtr clonefile = nullptr;
            errorCode = loadCloneChunkFile(info.id, info.cloneNo, &clonefile);
            if (nullptr == clonefile) {
                LOG(ERROR) << "Load clone chunk file failed.";
                return false;
            }

            // Load snapshot to memory
            errorCode = clonefile->LoadSnapshot(info.sn);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load snapshot failed.";
                return false;
            }
        } else if (info.type == FileNameOperator::FileType::SNAPSHOT) {
            string chunkFilePath = baseDir_ + "/" +
                        FileNameOperator::GenerateChunkFileName(info.id);

            // If the chunk file does not exist, print the log
            if (!lfs_->FileExists(chunkFilePath)) {
                LOG(WARNING) << "Can't find snapshot "
                             << files[i] << "' chunk.";
                continue;
            }
            // If the chunk file exists, load the chunk file to metaCache first
            CSErrorCode errorCode = loadChunkFile(info.id);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load chunk file failed.";
                return false;
            }

            // Load snapshot to memory
            errorCode = metaCache_.Get(info.id)->LoadSnapshot(info.sn);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load snapshot failed.";
                return false;
            }
        } else {
            LOG(WARNING) << "Unknown file: " << files[i];
        }
    }
    LOG(INFO) << "Initialize data store success.";
    return true;
}

CSErrorCode CSDataStore::DeleteChunk(ChunkID id, SequenceNum sn, std::shared_ptr<SnapContext> ctx) {
    if (ctx != nullptr && !ctx->empty()) {
        LOG(WARNING) << "Delete chunk file failed: snapshot exists."
                     << "ChunkID = " << id;
        return CSErrorCode::SnapshotExistError;
    }

    auto chunkFile = metaCache_.Get(id);
    if (chunkFile != nullptr) {
        CSErrorCode errorCode = chunkFile->Delete(sn);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Delete chunk file failed."
                         << "ChunkID = " << id;
            return errorCode;
        }
        metaCache_.Remove(id);
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::DeleteSnapshotChunk(
    ChunkID id, SequenceNum snapSn, std::shared_ptr<SnapContext> ctx) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile != nullptr) {
        CSErrorCode errorCode = chunkFile->DeleteSnapshot(snapSn, ctx);  // NOLINT
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Delete snapshot chunk or correct sn failed."
                         << "ChunkID = " << id
                         << ", snapSn = " << snapSn;
            return errorCode;
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::ReadChunk(ChunkID id,
                                   SequenceNum sn,
                                   char * buf,
                                   off_t offset,
                                   size_t length) {
    (void)sn;
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }

    CSErrorCode errorCode = chunkFile->Read(buf, offset, length);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Read chunk file failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}


struct CloneInfos CSDataStore::getParentClone (std::vector<struct CloneInfos>& clones, uint64_t cloneNo) {
    struct CloneInfos prev_clone;
    //use iterator to traverse the vector
    prev_clone = *clones.begin();
    for (auto it = clones.begin(); it != clones.end(); it++) {
        if (it->cloneNo == cloneNo) {
            return prev_clone;
        }
        prev_clone = *it;
    }

    return prev_clone;
}

// searchChunkForObj is a func to search the obj to find the obj in < chunkfile, sn, snapshot>
void CSDataStore::searchChunkForObj (CSChunkFilePtr chunkfile, SequenceNum sn, 
                                    std::vector<ObjectInfo>& objInfos, 
                                    uint32_t beginIndex, uint32_t endIndex, 
                                    struct CloneContext& ctx) {

    std::vector<BitRange> bitRanges;
    std::vector<BitRange> notInMapBitRanges;

    CSChunkFilePtr cloneFile = nullptr;

    bool isFinish = false;

    BitRange objRange;
    objRange.beginIndex = beginIndex;
    objRange.endIndex = endIndex;
    bitRanges.push_back(objRange);

    SequenceNum cloneSn = sn;
    uint64_t cloneParentNo = 0;
    uint64_t cloneNo = ctx.cloneNo;
    struct CloneInfos tmpclone;

    if (0 != ctx.cloneNo) {
        cloneFile = std::shared_ptr<CSChunkFile> (chunkfile->getClone (ctx.cloneNo));
        while (nullptr == cloneFile) {
            tmpclone = getParentClone (ctx.clones, cloneNo);
            cloneParentNo = tmpclone.cloneNo;
            cloneSn = tmpclone.cloneSn;
            cloneNo = cloneParentNo;
            if (0 == cloneParentNo) {
                break;
            }
            cloneFile = std::shared_ptr<CSChunkFile> (chunkfile->getClone (cloneParentNo));
        }
    }

    if (nullptr == cloneFile) { //must be zero, not any clone chunk left, just search the chunkfile
        assert (0 == cloneParentNo);
        isFinish = chunkfile->DivideObjInfoByIndex (cloneSn, bitRanges, notInMapBitRanges, objInfos);
        if (true == isFinish) { //all the objInfos is in the map must be true
            assert (isFinish == true);
            return;
        }
    } else {
        while (true != isFinish) {
            isFinish = cloneFile->DivideObjInfoByIndex (cloneSn, bitRanges, notInMapBitRanges, objInfos);
            if (true == isFinish) { //all the objInfos is in the map
                return;
            }

            //initialize the bitranges and notInMapBitRanges
            bitRanges = notInMapBitRanges;
            notInMapBitRanges.clear();

            cloneFile = nullptr;
            struct CloneInfos tmpclone;
            while (nullptr == cloneFile) {
                tmpclone = getParentClone (ctx.clones, cloneNo);
                cloneParentNo = tmpclone.cloneNo;
                cloneSn = tmpclone.cloneSn;
                cloneNo = cloneParentNo;
                if (0 == cloneParentNo) {
                    break;
                }
                cloneFile = std::shared_ptr<CSChunkFile>(chunkfile->getClone (cloneParentNo));
            }

            if (nullptr == cloneFile) { //must be zero, not any clone chunk left, just search the chunkfile
                assert (0 == cloneParentNo);
                isFinish = chunkfile->DivideObjInfoByIndex (cloneSn, bitRanges, notInMapBitRanges, objInfos);
                if (true == isFinish) { //all the objInfos is in the map must be true
                    assert (isFinish == true);
                    return;
                }                
            }
        }
    }

    return;
}

//func which help to read from objInfo
CSErrorCode CSDataStore::ReadByObjInfo (char* buf, ObjectInfo* objInfo) {
    CSErrorCode errorCode;

    if ((nullptr == objInfo->snapptr) && (0 == objInfo->sn)) {
        errorCode = objInfo->fileptr->Read (buf + objInfo->bufOff, objInfo->offset, objInfo->length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                         << "ReadByObjInfo read sn = " << objInfo->sn;
            return errorCode;
        }
    } else if ((nullptr == objInfo->snapptr) && (0 != objInfo->sn)) {
        errorCode = objInfo->fileptr->ReadSpecifiedChunk (objInfo->sn, buf + objInfo->bufOff, objInfo->offset, objInfo->length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                         << "ReadByObjInfo read sn = " << objInfo->sn;
            return errorCode;
        }
    } else {
        errorCode = objInfo->fileptr->ReadSpecifiedSnap (objInfo->sn, objInfo->snapptr, buf + objInfo->bufOff, objInfo->offset, objInfo->length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                         << "ReadByObjInfo read sn = " << objInfo->sn;
            return errorCode;
        }
    }

    return CSErrorCode::Success;
}

/*
    build obj vector for the specified  offset and length 
    according to the OBJ_SIZE to split the offset and length into several parts
    asume that the clone chunk use the object unit is OBJ_SIZE which is multiple of page size
    and use the OBJ_SIZE to split the offset and length into several parts
    and to check if the parts is in the chunk by bitmap
    the default OBJ_SIZE is 64KB
*/
void CSDataStore::SplitDataIntoObjs (CSChunkFilePtr chunkfile, SequenceNum sn,
                                    std::vector<ObjectInfo>& objInfos, 
                                    off_t offset, size_t length,
                                    struct CloneContext& ctx) {
    //if the offset is align with OBJ_SIZE then the objNum is length / OBJ_SIZE
    //else the objNum is length / OBJ_SIZE + 1

    uint32_t beginIndex = offset >> OBJ_SIZE_SHIFT;    
    uint32_t endIndex = (offset + length - 1) >> OBJ_SIZE_SHIFT;
    
    searchChunkForObj (chunkfile, sn, objInfos, beginIndex, endIndex, ctx);
    
    return;
}

//another ReadChunk Interface for the clone chunk
CSErrorCode CSDataStore::ReadChunk(ChunkID id,
                                   SequenceNum sn,
                                   char * buf,
                                   off_t offset,
                                   size_t length,
                                   struct CloneContext& ctx) {
    
    CSChunkFilePtr chunkFile = nullptr;
    //if it is clone chunk, means tha the chunkid is the root of clone chunk
    //so we need to use the vector clone to get the parent clone chunk
    if (ctx.cloneNo > 0) { 

        chunkFile = metaCache_.Get(id);
        if (nullptr == chunkFile) {
            return CSErrorCode::ChunkNotExistError;
        }

        std::vector<ObjectInfo> objInfos;
        SplitDataIntoObjs (chunkFile, sn, objInfos, offset, length, ctx);

        CSErrorCode errorCode;
        for (auto& objInfo : objInfos) {
            errorCode = ReadByObjInfo (buf + ((objInfo.index << OBJ_SIZE_SHIFT) - offset), &objInfo);
            if (errorCode != CSErrorCode::Success) {
                LOG(WARNING) << "Read chunk file failed."
                             << "ChunkID = " << id;
                return errorCode;
            }
        }

    } else {
        chunkFile = metaCache_.Get(id);
        if (chunkFile == nullptr) {
            return CSErrorCode::ChunkNotExistError;
        }

        CSErrorCode errorCode = chunkFile->Read(buf, offset, length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }        
    }

    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::ReadChunkMetaPage(ChunkID id, SequenceNum sn,
                                           char * buf) {
    (void)sn;
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }

    CSErrorCode errorCode = chunkFile->ReadMetaPage(buf);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Read chunk meta page failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}

//another ReadSnapshotChunk Interface for the clone chunk
CSErrorCode CSDataStore::ReadSnapshotChunk(ChunkID id,
                                           SequenceNum sn,
                                           char * buf,
                                           off_t offset,
                                           size_t length,
                                           std::shared_ptr<SnapContext> ctx,
                                           std::shared_ptr<CloneContext> cloneCtx) {

    if (ctx != nullptr && !ctx->contains(sn)) {
        return CSErrorCode::SnapshotNotExistError;
    }

    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }

    //if the chunkfile exist and it is not a clone chunk
    if ((nullptr != chunkFile) && (0 == cloneCtx->cloneNo)) {
        CSErrorCode errorCode =
            chunkFile->ReadSpecifiedChunk(sn, buf, offset, length);
    if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read snapshot chunk failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    } else {
        std::vector<ObjectInfo> objInfos;
        SplitDataIntoObjs (chunkFile, sn, objInfos, offset, length, *cloneCtx);

        CSErrorCode errorCode;
        for (auto& objInfo : objInfos) {
            errorCode = ReadByObjInfo (buf + ((objInfo.index << OBJ_SIZE_SHIFT) - offset), &objInfo);
            if (errorCode != CSErrorCode::Success) {
                LOG(WARNING) << "Read chunk file failed."
                             << "ChunkID = " << id;
                return errorCode;
            }
        }
    }

    return CSErrorCode::Success;
}

// It is ensured that if snap chunk exists, the chunk must exist.
// 1. snap chunk is generated from COW, thus chunk must exist.
// 2. discard will not delete chunk if there is snapshot.
CSErrorCode CSDataStore::ReadSnapshotChunk(ChunkID id,
                                           SequenceNum sn,
                                           char * buf,
                                           off_t offset,
                                           size_t length,
                                           std::shared_ptr<SnapContext> ctx) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }
    if (ctx != nullptr && !ctx->contains(sn)) {
        return CSErrorCode::SnapshotNotExistError;
    }
    CSErrorCode errorCode =
        chunkFile->ReadSpecifiedChunk(sn, buf, offset, length);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Read snapshot chunk failed."
                     << "ChunkID = " << id;
    }
    return errorCode;
}

CSErrorCode CSDataStore::CreateChunkFile(const ChunkOptions & options,
                                         CSChunkFilePtr* chunkFile) {
        if (!options.location.empty() &&
            options.location.size() > locationLimit_) {
            LOG(ERROR) << "Location is too long."
                       << "ChunkID = " << options.id
                       << ", location = " << options.location
                       << ", location size = " << options.location.size()
                       << ", location limit size = " << locationLimit_;
            return CSErrorCode::InvalidArgError;
        }
        auto tempChunkFile = std::make_shared<CSChunkFile>(lfs_,
                                                  chunkFilePool_,
                                                  options);
        CSErrorCode errorCode = tempChunkFile->Open(true);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Create chunk file failed."
                         << "ChunkID = " << options.id
                         << ", ErrorCode = " << errorCode;
            return errorCode;
        }
        // If there are two operations concurrently to create a chunk file,
        // Then the chunkFile generated by one of the operations will be added
        // to metaCache first, the subsequent operation abandons the currently
        // generated chunkFile and uses the previously generated chunkFile
        *chunkFile = metaCache_.Set(options.id, tempChunkFile);
        return CSErrorCode::Success;
}

void CSDataStore::DebugPrint() {
    std::cout << "chunkSize_ " << chunkSize_ << std::endl;
    std::cout << "pageSize_ " << pageSize_ << std::endl;
    std::cout << "locationLimit_ " << locationLimit_ << std::endl;
    std::cout << "baseDir_ " << baseDir_ << std::endl;
    std::cout << "metaCache_ " << static_cast<void *>(&metaCache_) << std::endl;
    std::cout << "chunkFilePool_ " << chunkFilePool_ << std::endl;
    std::cout << "lfs_ " << lfs_ << std::endl;
    std::cout << "metric_ " << metric_ << std::endl;
    std::cout << "enableOdsyncWhenOpenChunkFile_ " << enableOdsyncWhenOpenChunkFile_ << std::endl;

    return;
}

CSErrorCode CSDataStore::CreateChunkFile(const ChunkOptions & options,
                                         CSChunkFilePtr chunkFile,
                                         CSChunkFilePtr* cloneChunkFile, uint64_t cloneNo) {
        if (!options.location.empty() &&
            options.location.size() > locationLimit_) {
            LOG(ERROR) << "Location is too long."
                       << "ChunkID = " << options.id
                       << ", location = " << options.location
                       << ", location size = " << options.location.size()
                       << ", location limit size = " << locationLimit_;
            return CSErrorCode::InvalidArgError;
        }

        if (nullptr == chunkFile) {
            LOG(ERROR) << "chunkFile is null in func CreateChunkFile."
                       << "cloneNo is " << cloneNo;
            return CSErrorCode::InvalidArgError;
        }
        auto tempChunkFile = std::make_shared<CSChunkFile>(lfs_,
                                                  chunkFilePool_,
                                                  options);
        CSErrorCode errorCode = tempChunkFile->Open(true, cloneNo);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Create chunk file failed."
                         << "ChunkID = " << options.id
                         << ", ErrorCode = " << errorCode;
            return errorCode;
        }
        // If there are two operations concurrently to create a chunk file,
        // Then the chunkFile generated by one of the operations will be added
        // to metaCache first, the subsequent operation abandons the currently
        // generated chunkFile and uses the previously generated chunkFile

        *cloneChunkFile = tempChunkFile;
        chunkFile->addClone (cloneNo, tempChunkFile.get());

        return CSErrorCode::Success;
}

//WriteChunk interface for the clone chunk
CSErrorCode CSDataStore::WriteChunk (ChunkID id, SequenceNum sn,
                                    const butil::IOBuf& buf, off_t offset, size_t length,
                                    uint32_t* cost, std::shared_ptr<SnapContext> ctx, 
                                    std::shared_ptr<CloneContext> cloneCtx) {
    // The requested sequence number is not allowed to be 0, when snapsn=0,
    // it will be used as the basis for judging that the snapshot does not exist
    if (sn == kInvalidSeq) {
        LOG(ERROR) << "Sequence num should not be zero."
                   << "ChunkID = " << id;
        return CSErrorCode::InvalidArgError;
    }

    auto chunkFile = metaCache_.Get(id);
    // If the chunk file does not exist, create the chunk file first
    if (chunkFile == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = sn;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        options.enableOdsyncWhenOpenChunkFile = enableOdsyncWhenOpenChunkFile_;
        CSErrorCode errorCode = CreateChunkFile(options, &chunkFile);
        if (errorCode != CSErrorCode::Success) {
            return errorCode;
        }
    }

    //if it is clone chunk
    if (0 == cloneCtx->cloneNo) {//not clone chunk
        // write chunk file
        CSErrorCode errorCode = chunkFile->Write(sn, buf, offset, length, cost, ctx);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Write chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }
    } else { //write clone chunk, to see if need to do some reading first, if it is not whole obj size

        assert (nullptr != chunkFile); //for clone file the orgin clone must be exists
        auto cloneChunkFile = std::shared_ptr<CSChunkFile>(chunkFile->getClone (cloneCtx->cloneNo));
        if (nullptr == cloneChunkFile) {
            ChunkOptions options;
            options.id = id;
            options.sn = sn;
            options.baseDir = baseDir_;
            options.chunkSize = chunkSize_;
            options.pageSize = pageSize_;
            options.metric = metric_;
            options.enableOdsyncWhenOpenChunkFile = enableOdsyncWhenOpenChunkFile_;
            CSErrorCode errorCode = CreateChunkFile(options, chunkFile, &cloneChunkFile, cloneCtx->cloneNo);
            if (errorCode != CSErrorCode::Success) {
                return errorCode;
            }
        }

        //asume that the clone chunk use the object unit is OBJ_SIZE which is multiple of page size
        //and use the OBJ_SIZE to split the offset and length into several parts
        //and to check if the parts is in the chunk by bitmap
        //the default OBJ_SIZE is 64KB

        //if the offset is align with OBJ_SIZE then the objNum is length / OBJ_SIZE
        //else the objNum is length / OBJ_SIZE + 1
        uint32_t beginIndex = offset >> OBJ_SIZE_SHIFT;
        uint32_t endIndex = (offset + length - 1) >> OBJ_SIZE_SHIFT;
        uint32_t objNum = endIndex - beginIndex + 1;
        uint32_t tmpIndex = offset >> PAGE_SIZE_SHIFT;
        uint32_t tmpOffset = offset;
        uint32_t tmpLength = endIndex > beginIndex ?((beginIndex + 1) << OBJ_SIZE_SHIFT) - offset : length;
        uint32_t tmpBufOff = 0;

        //find the header obj and the tail obj
        ObjectInfo objInfoHeader, objInfoEnd; 
        objInfoHeader.fileptr = nullptr;
        objInfoEnd.fileptr = nullptr;

        CSChunkFilePtr tmpfile = chunkFile;
        CSSnapshotPtr tmpsnap = nullptr;
        SequenceNum cloneSn = 0;

        std::vector<ObjectInfo> objHeaders;
        std::vector<ObjectInfo> objEnds;

        if (offset != beginIndex << OBJ_SIZE_SHIFT) { //not aligned with the objh eader
            objInfoHeader.seq = beginIndex;
            objInfoHeader.index = (beginIndex << OBJ_SIZE_SHIFT) >> PAGE_SIZE_SHIFT;
            objInfoHeader.offset = beginIndex << OBJ_SIZE_SHIFT;
            objInfoHeader.length = offset - (beginIndex << OBJ_SIZE_SHIFT);
            objInfoHeader.bufOff = 0;

            SplitDataIntoObjs (chunkFile, sn, objHeaders, objInfoHeader.offset, objInfoHeader.length, *cloneCtx);
        }

        tmpfile = chunkFile;
        tmpsnap = nullptr;
        cloneSn = 0;
        if (offset + length < ((endIndex << OBJ_SIZE_SHIFT) + OBJ_SIZE)) { //not aligned with the obj end
            objInfoEnd.seq = endIndex;
            objInfoEnd.index = (endIndex << OBJ_SIZE_SHIFT) >> PAGE_SIZE_SHIFT;
            objInfoEnd.offset = offset + length;
            objInfoEnd.length = (endIndex << OBJ_SIZE_SHIFT) + OBJ_SIZE - objInfoEnd.offset;
            objInfoEnd.bufOff = 0;
            objInfoEnd.fileptr = nullptr;

            SplitDataIntoObjs (chunkFile, sn, objEnds, objInfoEnd.offset, objInfoEnd.length, *cloneCtx);
            objInfoEnd.fileptr = tmpfile;
            objInfoEnd.sn = cloneSn;
            objInfoEnd.snapptr = tmpsnap;
        }

        //the above have some optimal that the in the same obj and merge the objinfo, read the whole obj
        //read the data for the cloneObjInfos into the buf
        char* tmpbuf = nullptr;
        CSErrorCode errorCode;

        butil::IOBuf* cloneBuf = new butil::IOBuf();

        if (objInfoHeader.fileptr != NULL) {//read header Data into the buf
            tmpbuf = (char *)malloc(objInfoHeader.length);
            if (nullptr == tmpbuf) {
                errorCode = CSErrorCode::InternalError;
                LOG(WARNING) << "Read chunk file failed not enough memory."
                             << "ChunkID = " << id;
                return errorCode;
            }

            for (auto& objHeader : objHeaders) {
                errorCode = ReadByObjInfo (tmpbuf + (int)((objHeader.index - objInfoHeader.index) << OBJ_SIZE_SHIFT), &objHeader);
                if (errorCode != CSErrorCode::Success) {
                    LOG(WARNING) << "Read chunk file failed."
                                << "ChunkID = " << id;
                    return errorCode;
                }
            }
            cloneBuf->append(tmpbuf, objInfoHeader.length);
        }

        cloneBuf->append(buf);

        if (objInfoEnd.fileptr != NULL) {//read header Data into the buf
            tmpbuf = (char*) malloc(objInfoEnd.length);
            if (nullptr == tmpbuf) {
                errorCode = CSErrorCode::InternalError;
                LOG(WARNING) << "Read chunk file failed not enough memory."
                             << "ChunkID = " << id;
                return errorCode;
            }

            for (auto& objEnd : objEnds) {
                errorCode = ReadByObjInfo (tmpbuf + (int)((objEnd.index - objInfoEnd.index) << OBJ_SIZE_SHIFT), &objEnd);
                if (errorCode != CSErrorCode::Success) {
                    LOG(WARNING) << "Read chunk file failed."
                                << "ChunkID = " << id;
                    return errorCode;
                }
            }
            cloneBuf->append(tmpbuf, objInfoEnd.length);
        }

        //now write the cloneBuf to the chunk
        errorCode = cloneChunkFile->Write(sn, *cloneBuf, offset, length, cost, ctx);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Write chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::WriteChunk(ChunkID id,
                            SequenceNum sn,
                            const butil::IOBuf& buf,
                            off_t offset,
                            size_t length,
                            uint32_t* cost,
                            std::shared_ptr<SnapContext> ctx,
                            const std::string & cloneSourceLocation)  {
    // The requested sequence number is not allowed to be 0, when snapsn=0,
    // it will be used as the basis for judging that the snapshot does not exist
    if (sn == kInvalidSeq) {
        LOG(ERROR) << "Sequence num should not be zero."
                   << "ChunkID = " << id;
        return CSErrorCode::InvalidArgError;
    }
    auto chunkFile = metaCache_.Get(id);
    // If the chunk file does not exist, create the chunk file first
    if (chunkFile == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = sn;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.location = cloneSourceLocation;
        options.pageSize = pageSize_;
        options.metric = metric_;
        options.enableOdsyncWhenOpenChunkFile = enableOdsyncWhenOpenChunkFile_;
        CSErrorCode errorCode = CreateChunkFile(options, &chunkFile);
        if (errorCode != CSErrorCode::Success) {
            return errorCode;
        }
    }
    // write chunk file
    CSErrorCode errorCode = chunkFile->Write(sn,
                                             buf,
                                             offset,
                                             length,
                                             cost,
                                             ctx);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Write chunk file failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::SyncChunk(ChunkID id) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        LOG(WARNING) << "Sync chunk not exist, ChunkID = " << id;
        return CSErrorCode::Success;
    }
    CSErrorCode errorCode = chunkFile->Sync();
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Sync chunk file failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::CreateCloneChunk(ChunkID id,
                                          SequenceNum sn,
                                          SequenceNum correctedSn,
                                          ChunkSizeType size,
                                          const string& location) {
    // Check the validity of the parameters
    if (size != chunkSize_
        || sn == kInvalidSeq
        || location.empty()) {
        LOG(ERROR) << "Invalid arguments."
                   << "ChunkID = " << id
                   << ", sn = " << sn
                   << ", correctedSn = " << correctedSn
                   << ", size = " << size
                   << ", location = " << location;
        return CSErrorCode::InvalidArgError;
    }
    auto chunkFile = metaCache_.Get(id);
    // If the chunk file does not exist, create the chunk file first
    if (chunkFile == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = sn;
        options.correctedSn = correctedSn;
        options.location = location;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        CSErrorCode errorCode = CreateChunkFile(options, &chunkFile);
        if (errorCode != CSErrorCode::Success) {
            return errorCode;
        }
    }
    // Determine whether the specified parameters match the information
    // in the existing Chunk
    // No need to put in else, because users may call this interface at the
    // same time
    // If different sequence or location information are specified in the
    // parameters, there may be concurrent conflicts, and judgments are also
    // required
    CSChunkInfo info;
    chunkFile->GetInfo(&info);
    if (info.location.compare(location) != 0
        || info.curSn != sn
        || info.correctedSn != correctedSn) {
        LOG(WARNING) << "Conflict chunk already exists."
                   << "sn in arg = " << sn
                   << ", correctedSn in arg = " << correctedSn
                   << ", location in arg = " << location
                   << ", sn in chunk = " << info.curSn
                   << ", location in chunk = " << info.location
                   << ", corrected sn in chunk = " << info.correctedSn;
        return CSErrorCode::ChunkConflictError;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::PasteChunk(ChunkID id,
                                    const char * buf,
                                    off_t offset,
                                    size_t length) {
    auto chunkFile = metaCache_.Get(id);
    // Paste Chunk requires Chunk must exist
    if (chunkFile == nullptr) {
        LOG(WARNING) << "Paste Chunk failed, Chunk not exists."
                     << "ChunkID = " << id;
        return CSErrorCode::ChunkNotExistError;
    }
    CSErrorCode errcode = chunkFile->Paste(buf, offset, length);
    if (errcode != CSErrorCode::Success) {
        LOG(WARNING) << "Paste Chunk failed, Chunk not exists."
                     << "ChunkID = " << id;
        return errcode;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::GetChunkInfo(ChunkID id,
                                      CSChunkInfo* chunkInfo) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        LOG(INFO) << "Get ChunkInfo failed, Chunk not exists."
                  << "ChunkID = " << id;
        return CSErrorCode::ChunkNotExistError;
    }
    chunkFile->GetInfo(chunkInfo);
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::GetChunkHash(ChunkID id,
                                      off_t offset,
                                      size_t length,
                                      std::string* hash) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        LOG(INFO) << "Get ChunkHash failed, Chunk not exists."
                  << "ChunkID = " << id;
        return CSErrorCode::ChunkNotExistError;
    }
    return chunkFile->GetHash(offset, length, hash);
}

DataStoreStatus CSDataStore::GetStatus() {
    DataStoreStatus status;
    status.chunkFileCount = metric_->chunkFileCount.get_value();
    status.cloneChunkCount = metric_->cloneChunkCount.get_value();
    status.snapshotCount = metric_->snapshotCount.get_value();
    return status;
}

CSErrorCode CSDataStore::loadChunkFile(ChunkID id) {
    // If the chunk file has not been loaded yet, load it into metaCache
    if (metaCache_.Get(id) == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = 0;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        CSChunkFilePtr chunkFilePtr =
            std::make_shared<CSChunkFile>(lfs_,
                                          chunkFilePool_,
                                          options);
        CSErrorCode errorCode = chunkFilePtr->Open(false);
        if (errorCode != CSErrorCode::Success)
            return errorCode;
        metaCache_.Set(id, chunkFilePtr);
    }
    return CSErrorCode::Success;
}

//use bool needOpen to indicate that whether need to open the clone chunk file
//if we need not to open the clone chunk file, just return the pointer of the clone chunk file
CSErrorCode CSDataStore::loadCloneChunkFile(ChunkID id, uint64_t cloneNo, CSChunkFilePtr* clonefile) {
    // If the chunk file has not been loaded yet, load it into metaCache
    if (metaCache_.Get(id) == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = 0;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        CSChunkFilePtr chunkFilePtr =
            std::make_shared<CSChunkFile>(lfs_,
                                          chunkFilePool_,
                                          options);
        CSErrorCode errorCode = chunkFilePtr->Open(false);
        if (errorCode != CSErrorCode::Success)
            return errorCode;
        metaCache_.Set(id, chunkFilePtr);
    }

    CSChunkFilePtr chunkFilePtr = metaCache_.Get(id);
    CSChunkFilePtr cloneChunkFilePtr = std::shared_ptr<CSChunkFile>(chunkFilePtr->getClone (cloneNo));
    if (nullptr == cloneChunkFilePtr) {
        ChunkOptions options;
        options.id = id;
        options.sn = 0;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        CSChunkFilePtr cloneChunkFilePtr =
            std::make_shared<CSChunkFile>(lfs_,
                                          chunkFilePool_,
                                          options);
        CSErrorCode errorCode = cloneChunkFilePtr->Open(false, cloneNo);
        chunkFilePtr->addClone (cloneNo, cloneChunkFilePtr.get());
        if (errorCode != CSErrorCode::Success)
            return errorCode;

        *clonefile = cloneChunkFilePtr;
    }

    return CSErrorCode::Success;
}

ChunkMap CSDataStore::GetChunkMap() {
    return metaCache_.GetMap();
}

SnapContext::SnapContext(const std::vector<SequenceNum>& snapIds) {
    std::copy(snapIds.begin(), snapIds.end(), std::back_inserter(snaps));
}

SequenceNum SnapContext::getPrev(SequenceNum snapSn) const {
    SequenceNum n = 0;
    for (long i = 0; i < snaps.size(); i++) {
        if (snaps[i] >= snapSn) {
            break;
        }
        n = snaps[i];
    }

    return n;
}

SequenceNum SnapContext::getNext(SequenceNum snapSn) const {
    auto it = std::find_if(snaps.begin(), snaps.end(), [&](SequenceNum n) {return n > snapSn;});
    return it == snaps.end() ? 0 : *it;
}

SequenceNum SnapContext::getLatest() const {
    return snaps.empty() ? 0 : *snaps.rbegin();
}

bool SnapContext::contains(SequenceNum snapSn) const {
    return std::find(snaps.begin(), snaps.end(), snapSn) != snaps.end();
}

bool SnapContext::empty() const {
    return snaps.empty();
}

}  // namespace chunkserver
}  // namespace curve
